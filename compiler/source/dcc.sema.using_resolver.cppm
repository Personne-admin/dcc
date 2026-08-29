export module dcc.sema.using_resolver;

import std;
import dcc.ast;
import dcc.diag;
import dcc.sm;
import dcc.sema.scope;

export namespace dcc::sema
{
    class UsingResolver
    {
    public:
        UsingResolver(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, std::pmr::polymorphic_allocator<> alloc)
            : m_modules{modules}, m_diag{diag}, m_alloc{alloc}
        {
        }

        void run()
        {
            constexpr int max_iters = 64;
            int iter = 0;

            for (; iter < max_iters; ++iter)
            {
                bool progress = false;
                for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it)
                {
                    auto const& m = *it;
                    if (!m->tu)
                        continue;

                    progress = bind_imports(*m) || progress;
                    progress = resolve_pending_usings(*m) || progress;
                }

                if (!progress)
                    break;
            }

            if (iter == max_iters)
                m_diag.error("internal compiler error: using resolution did not converge after {} iterations", max_iters);

            for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it)
            {
                auto const& m = *it;
                if (!m->tu)
                    continue;

                for (auto* u : m->using_worklist)
                    if (!m_resolved.contains(u))
                        diagnose_unresolved(*u, *m);

                if (m->state < ModuleState::UsingResolved)
                    m->state = ModuleState::UsingResolved;
            }
        }

    private:
        std::span<std::unique_ptr<ModuleInfo> const> m_modules;
        diag::DiagnosticEngine& m_diag;
        std::pmr::polymorphic_allocator<> m_alloc;
        std::unordered_set<ast::UsingDecl const*> m_resolved;

        bool bind_imports(ModuleInfo& mod)
        {
            bool progress = false;
            for (auto const& imp : mod.imports)
            {
                if (!imp.target || !imp.target->export_scope)
                    continue;

                bool publicly_imported = imp.decl->is_public;

                if (imp.alias_prefix.empty())
                    progress = inject_ancestor_exports(mod, *imp.target, publicly_imported) || progress;
                else
                    progress = install_module_alias(mod, imp.alias_prefix, *imp.target, publicly_imported) || progress;

                progress = absorb_spills(mod, *imp.target, publicly_imported) || progress;

                if (publicly_imported)
                    progress = absorb_all_public_exports(mod, *imp.target) || progress;
            }

            return progress;
        }

        static bool merge_symbol(Symbol& dst, Symbol const& src) noexcept
        {
            bool changed = false;

            if (src.is_spilled && !dst.is_spilled)
            {
                dst.is_spilled = true;
                changed = true;
            }
            if (src.is_exported && !dst.is_exported)
            {
                dst.is_exported = true;
                changed = true;
            }
            if (src.is_ambiguous && !dst.is_ambiguous)
            {
                dst.is_ambiguous = true;
                changed = true;
            }
            if (!dst.via_using)
            {
                dst.via_using = src.via_using;
                changed = changed || (src.via_using != nullptr);
            }

            return changed;
        }
        bool install_module_alias(ModuleInfo& mod, ModulePath const& prefix, ModuleInfo const& target, bool also_export)
        {
            if (prefix.empty())
                return false;

            auto write_path = [&](Scope& root) -> bool {
                auto segs = prefix.segments();
                Scope* cur = &root;

                for (std::size_t i = 0; i + 1 < segs.size(); ++i)
                {
                    Symbol anchor{};
                    anchor.name = segs[i];
                    anchor.kind = SymbolKind::UsingGroup;
                    cur = cur->ensure_namespace(segs[i], anchor, ScopeKind::UsingGroup);
                }

                Symbol leaf{};
                leaf.name = segs.back();
                leaf.kind = SymbolKind::Module;
                leaf.module = &target;
                leaf.is_exported = also_export;

                return cur->bind_namespace(segs.back(), target.export_scope, leaf) == DefineResult::Ok;
            };

            bool progress = write_path(*mod.own_scope);
            if (also_export)
                progress = write_path(*mod.export_scope) || progress;

            return progress;
        }

        bool absorb_spills(ModuleInfo& mod, ModuleInfo const& target, bool propagate)
        {
            if (!target.export_scope)
                return false;
            if (&target == &mod)
                return false;

            bool progress = false;
            for (auto const& [name, src] : target.export_scope->bindings())
            {
                if (!has_any_spill(src))
                    continue;

                progress = copy_spilled_slots(src, name, *mod.own_scope) || progress;
                if (propagate)
                    progress = copy_spilled_slots(src, name, *mod.export_scope) || progress;
            }
            return progress;
        }

        bool absorb_all_public_exports(ModuleInfo& mod, ModuleInfo const& target)
        {
            if (!target.export_scope)
                return false;
            if (&target == &mod)
                return false;

            bool progress = false;
            for (auto const& [name, src] : target.export_scope->bindings())
            {
                if (src.empty())
                    continue;
                if (src.has_namespace && !src.has_type && src.value_syms.empty())
                    continue;

                progress = install_binding_in_scope(src, name, *mod.export_scope, true) || progress;
            }
            return progress;
        }

        bool inject_ancestor_exports(ModuleInfo& mod, ModuleInfo const& target, bool publicly_imported)
        {
            if (!target.export_scope)
                return false;
            if (&target == &mod)
                return false;

            bool progress = false;
            for (auto const& [name, src] : target.export_scope->bindings())
            {
                if (src.empty())
                    continue;

                if (src.has_namespace && !src.has_type && src.value_syms.empty())
                    continue;

                progress = install_binding_in_scope(src, name, *mod.own_scope, publicly_imported) || progress;
            }

            return progress;
        }

        static bool has_any_spill(NameBinding const& b) noexcept
        {
            if (b.has_type && b.type_sym.is_spilled)
                return true;

            for (auto const& v : b.value_syms)
                if (v.is_spilled)
                    return true;

            return b.has_namespace && b.namespace_sym.is_spilled;
        }

        static bool is_variable_like(SymbolKind k) noexcept { return k == SymbolKind::Variable || k == SymbolKind::ValueAlias; }

        static bool install_binding_in_scope(NameBinding const& src, std::string_view target_name, Scope& dst, bool is_exported)
        {
            bool added = false;
            auto* existing = dst.find_binding_local(target_name);

            if (src.has_type)
            {
                Symbol s = src.type_sym;
                s.name = target_name;
                s.is_exported = is_exported;
                if (existing && existing->has_type && existing->type_sym.decl == s.decl)
                    added = merge_symbol(existing->type_sym, s) || added;
                else if ((!existing || !existing->has_type) && dst.define_type(s) == DefineResult::Ok)
                    added = true;
            }

            for (auto const& vs : src.value_syms)
            {
                Symbol s = vs;
                s.name = target_name;
                s.is_exported = is_exported;
                auto* binding = dst.find_binding_local(target_name);

                if (is_variable_like(vs.kind))
                {
                    if (binding)
                    {
                        auto it = std::ranges::find_if(binding->value_syms, [&](Symbol& e) { return e.kind == s.kind && e.decl == s.decl; });
                        if (it != binding->value_syms.end())
                            added = merge_symbol(*it, s) || added;
                        else if (binding->value_syms.empty() && dst.define_variable(s) == DefineResult::Ok)
                            added = true;
                    }
                    else if (dst.define_variable(s) == DefineResult::Ok)
                        added = true;
                }
                else
                {
                    if (binding)
                    {
                        auto it = std::ranges::find_if(binding->value_syms, [&](Symbol& e) { return e.kind == s.kind && e.decl == s.decl; });
                        if (it != binding->value_syms.end())
                            added = merge_symbol(*it, s) || added;
                        else if (dst.add_function_overload(s) == DefineResult::Ok)
                            added = true;
                    }
                    else if (dst.add_function_overload(s) == DefineResult::Ok)
                        added = true;
                }
            }

            if (src.has_namespace)
            {
                Symbol s = src.namespace_sym;
                s.name = target_name;
                s.is_exported = is_exported;
                auto* binding = dst.find_binding_local(target_name);
                if (binding && binding->has_namespace && binding->namespace_scope == src.namespace_scope)
                    added = merge_symbol(binding->namespace_sym, s) || added;
                else if ((!binding || !binding->has_namespace) && dst.bind_namespace(target_name, src.namespace_scope, s) == DefineResult::Ok)
                    added = true;
            }

            return added;
        }

        bool resolve_pending_usings(ModuleInfo& mod)
        {
            bool progress = false;
            for (auto* u : mod.using_worklist)
            {
                if (m_resolved.contains(u))
                    continue;

                if (try_resolve_using(mod, *u))
                {
                    m_resolved.insert(u);
                    progress = true;
                }
            }
            return progress;
        }

        bool try_resolve_using(ModuleInfo& mod, ast::UsingDecl& u)
        {
            switch (u.using_kind)
            {
                case ast::UsingKind::Alias:
                    return resolve_alias(mod, u);
                case ast::UsingKind::ValueAlias:
                    return resolve_value_alias(mod, u);
                case ast::UsingKind::BareImport:
                    return resolve_bare(mod, u);
                case ast::UsingKind::Wildcard:
                    return resolve_wildcard(mod, u);
                case ast::UsingKind::List:
                    return resolve_list(mod, u);
                case ast::UsingKind::Concept:
                    return resolve_concept(mod, u);
            }

            return false;
        }

        bool install_path_alias_from(ModuleInfo& mod, ast::UsingDecl& u, ast::Path const& path)
        {
            auto resolved = resolve_target_binding(*mod.own_scope, path);
            if (!resolved)
                return false;

            std::string_view tail = u.alias_path.segments.back().name;
            auto write = [&](Scope& root, bool spill) {
                Scope* cur = walk_alias_prefix(root, u.alias_path, u, spill);
                copy_all_slots(*resolved->binding, tail, *cur, &u, spill);
            };

            write(*mod.own_scope, u.is_spill);
            if (u.is_public || u.is_spill)
                write(*mod.export_scope, u.is_spill);

            record_resolved(u, tail, primary_decl_of(*resolved->binding));
            return true;
        }

        static bool has_nominal_attr(ast::UsingDecl const& u) noexcept
        {
            for (auto const& attr : u.attrs)
                if (attr.name == "nominal")
                    return true;
            return false;
        }

        bool resolve_alias(ModuleInfo& mod, ast::UsingDecl& u)
        {
            if (u.alias_path.is_empty())
            {
                m_diag.error(u.range, "type alias missing name");
                m_resolved.insert(&u);
                return true;
            }

            if (u.target_type)
            {
                if (has_nominal_attr(u))
                    return install_type_alias_marker(mod, u);

                if (auto const* nt = ast::node_cast<ast::NamedType>(u.target_type); nt && nt->template_args.empty())
                {
                    if (u.alias_path.is_simple() && nt->path.is_simple() && nt->path.simple_name() == u.alias_path.segments.back().name)
                        return install_type_alias_marker(mod, u);

                    return install_path_alias_from(mod, u, nt->path);
                }

                return install_type_alias_marker(mod, u);
            }

            if (!u.target_path.is_empty())
                return install_path_alias(mod, u);

            m_diag.error(u.range, "malformed using declaration");
            m_resolved.insert(&u);
            return true;
        }

        bool install_type_alias_marker(ModuleInfo& mod, ast::UsingDecl& u)
        {
            auto write = [&](Scope& root, bool spill) -> bool {
                Scope* cur = walk_alias_prefix(root, u.alias_path, u, spill);
                Symbol s{};

                s.name = u.alias_path.segments.back().name;
                s.kind = SymbolKind::TypeAlias;
                s.decl = &u;
                s.via_using = &u;
                s.is_exported = u.is_public;
                s.is_spilled = spill;
                s.definition_range = u.range;

                return cur->define_type(s) == DefineResult::Ok;
            };

            write(*mod.own_scope, u.is_spill);
            if (u.is_public || u.is_spill)
                write(*mod.export_scope, u.is_spill);

            record_resolved(u, u.alias_path.segments.back().name, &u);
            return true;
        }

        bool resolve_value_alias(ModuleInfo& mod, ast::UsingDecl& u)
        {
            if (u.alias_path.is_empty())
            {
                m_diag.error(u.range, "value alias missing name");
                m_resolved.insert(&u);
                return true;
            }

            std::string_view name = u.alias_path.segments.back().name;

            auto write = [&](Scope& root, bool spill) -> bool {
                Scope* cur = walk_alias_prefix(root, u.alias_path, u, spill);
                Symbol s{};
                s.name = name;
                s.kind = SymbolKind::ValueAlias;
                s.decl = &u;
                s.via_using = &u;
                s.is_exported = u.is_public;
                s.is_spilled = spill;
                s.definition_range = u.name_range.valid() ? u.name_range : u.range;
                Symbol const* existing = nullptr;
                auto r = cur->define_variable(s, &existing);
                if (r == DefineResult::Conflict)
                {
                    auto diag_obj = diag::Diagnostic{diag::Severity::Error, std::format("redefinition of `{}`", name)}.primary(
                        u.name_range.valid() ? u.name_range : u.range);
                    if (existing && existing->definition_range.valid())
                        std::move(diag_obj).secondary(existing->definition_range, "previous definition was here");
                    m_diag.emit(std::move(diag_obj));
                }
                return r == DefineResult::Ok;
            };

            write(*mod.own_scope, u.is_spill);
            if (u.is_public || u.is_spill)
                write(*mod.export_scope, u.is_spill);

            record_resolved(u, name, &u);
            return true;
        }

        bool install_path_alias(ModuleInfo& mod, ast::UsingDecl& u)
        {
            auto resolved = resolve_target_binding(*mod.own_scope, u.target_path);
            if (!resolved)
                return false;

            std::string_view tail = u.alias_path.segments.back().name;

            auto write = [&](Scope& root, bool spill) {
                Scope* cur = walk_alias_prefix(root, u.alias_path, u, spill);
                copy_all_slots(*resolved->binding, tail, *cur, &u, spill);
            };

            write(*mod.own_scope, u.is_spill);
            if (u.is_public || u.is_spill)
                write(*mod.export_scope, u.is_spill);

            record_resolved(u, tail, primary_decl_of(*resolved->binding));
            return true;
        }

        bool resolve_bare(ModuleInfo& mod, ast::UsingDecl& u)
        {
            if (u.target_path.is_empty())
            {
                m_diag.error(u.range, "bare using requires a path");
                m_resolved.insert(&u);
                return true;
            }

            auto resolved = resolve_target_binding(*mod.own_scope, u.target_path);
            if (!resolved)
                return false;

            std::string_view name = resolved->name;

            copy_all_slots(*resolved->binding, name, *mod.own_scope, &u, u.is_spill);
            if (u.is_public || u.is_spill)
                copy_all_slots(*resolved->binding, name, *mod.export_scope, &u, u.is_spill);

            record_resolved(u, name, primary_decl_of(*resolved->binding));
            return true;
        }

        bool resolve_wildcard(ModuleInfo& mod, ast::UsingDecl& u)
        {
            Scope const* src = resolve_namespace_path(*mod.own_scope, u.target_path);
            if (!src)
                return false;

            if (u.alias_path.is_empty())
            {
                splat_scope(*src, *mod.own_scope, u, u.is_spill);
                if (u.is_public || u.is_spill)
                    splat_scope(*src, *mod.export_scope, u, u.is_spill);

                record_resolved(u, "*", nullptr);
                return true;
            }

            std::string_view grp = u.alias_path.segments.back().name;
            auto write = [&](Scope& dst, bool spill) -> bool {
                Symbol anchor{};
                anchor.name = grp;
                anchor.kind = SymbolKind::UsingGroup;
                anchor.via_using = &u;
                anchor.is_exported = u.is_public;
                anchor.is_spilled = spill;
                return dst.bind_namespace(grp, const_cast<Scope*>(src), anchor) == DefineResult::Ok;
            };

            write(*mod.own_scope, u.is_spill);
            if (u.is_public || u.is_spill)
                write(*mod.export_scope, u.is_spill);

            record_resolved(u, grp, nullptr);
            return true;
        }

        std::vector<ast::Path> expand_using_items(ast::Path const& prefix, std::pmr::vector<ast::UsingItem*> const& items)
        {
            std::vector<ast::Path> result;
            for (auto const* item : items)
                expand_item(prefix, item, result);

            return result;
        }

        void expand_item(ast::Path const& prefix, ast::UsingItem const* item, std::vector<ast::Path>& out)
        {
            ast::Path full{m_alloc};
            for (auto const& seg : prefix.segments)
                full.segments.push_back(seg);
            for (auto const& seg : item->path.segments)
                full.segments.push_back(seg);
            if (!full.segments.empty())
                full.range = sm::SourceRange{.begin = full.segments.front().range.begin, .end = full.segments.back().range.end};

            if (item->children.empty())
                out.push_back(std::move(full));
            else
                for (auto const* child : item->children)
                    expand_item(full, child, out);
        }

        bool resolve_list(ModuleInfo& mod, ast::UsingDecl& u)
        {
            Scope const* target_scope = nullptr;
            if (!u.target_path.is_empty())
            {
                target_scope = resolve_namespace_path(*mod.own_scope, u.target_path);
                if (!target_scope)
                    return false;
            }

            Scope const& start_scope = target_scope ? *target_scope : *mod.own_scope;

            std::vector<ast::Path> resolved_paths;
            if (!u.target_items.empty())
            {
                ast::Path empty_prefix{m_alloc};
                resolved_paths = expand_using_items(empty_prefix, u.target_items);
            }
            else if (!u.target_list.empty())
                resolved_paths.assign(u.target_list.begin(), u.target_list.end());

            std::vector<std::pair<NameBinding const*, std::string_view>> resolved;
            resolved.reserve(resolved_paths.size());
            for (auto const& tp : resolved_paths)
            {
                auto r = resolve_target_binding(start_scope, tp);
                if (!r)
                    return false;

                resolved.emplace_back(r->binding, r->name);
            }

            if (u.alias_path.is_empty())
            {
                auto splat = [&](Scope& dst, bool spill) {
                    for (auto const& [b, name] : resolved)
                        copy_all_slots(*b, name, dst, &u, spill);
                };

                splat(*mod.own_scope, u.is_spill);
                if (u.is_public || u.is_spill)
                    splat(*mod.export_scope, u.is_spill);

                record_resolved(u, "{...}", nullptr);
                return true;
            }

            std::string_view grp = u.alias_path.segments.back().name;
            auto into_group = [&](Scope& dst, bool spill) {
                Symbol anchor{};

                anchor.name = grp;
                anchor.kind = SymbolKind::UsingGroup;
                anchor.via_using = &u;
                anchor.is_exported = u.is_public;
                anchor.is_spilled = spill;
                Scope* group = dst.ensure_namespace(grp, anchor);

                for (auto const& [b, name] : resolved)
                    copy_all_slots(*b, name, *group, &u, false);
            };

            into_group(*mod.own_scope, u.is_spill);
            if (u.is_public || u.is_spill)
                into_group(*mod.export_scope, u.is_spill);

            record_resolved(u, grp, nullptr);
            return true;
        }

        bool resolve_concept(ModuleInfo& mod, ast::UsingDecl& u)
        {
            if (u.alias_path.is_empty())
            {
                m_diag.error(u.range, "concept missing name");
                m_resolved.insert(&u);
                return true;
            }

            std::string_view name = u.alias_path.segments.back().name;

            auto write = [&](Scope& dst, bool spill) {
                Symbol s{};
                s.name = name;
                s.kind = SymbolKind::TypeAlias;
                s.decl = &u;
                s.via_using = &u;
                s.is_exported = u.is_public;
                s.is_spilled = spill;
                s.definition_range = u.range;
                dst.define_type(s);
            };

            write(*mod.own_scope, u.is_spill);
            if (u.is_public || u.is_spill)
                write(*mod.export_scope, u.is_spill);

            record_resolved(u, name, &u);
            return true;
        }

        struct ResolvedTarget
        {
            NameBinding const* binding;
            Scope const* scope;
            std::string_view name;
        };

        std::optional<ResolvedTarget> resolve_target_binding(Scope const& start, ast::Path const& path)
        {
            if (path.is_empty())
                return std::nullopt;

            auto const& segs = path.segments;

            if (segs.size() == 1)
            {
                for (auto const* s = &start; s; s = s->parent())
                    if (auto const* b = s->find_binding_local(segs.front().name); b && !b->empty())
                        return ResolvedTarget{b, s, segs.front().name};

                return std::nullopt;
            }

            Scope const* cur = start.lookup_namespace(segs.front().name);
            if (!cur)
                return std::nullopt;

            for (std::size_t i = 1; i + 1 < segs.size(); ++i)
            {
                cur = cur->lookup_namespace_local(segs[i].name);
                if (!cur)
                    return std::nullopt;
            }

            auto const* b = cur->find_binding_local(segs.back().name);
            if (!b || b->empty())
                return std::nullopt;

            return ResolvedTarget{b, cur, segs.back().name};
        }

        Scope* walk_alias_prefix(Scope& root, ast::Path const& alias, ast::UsingDecl const& u, bool spill)
        {
            Scope* cur = &root;
            auto const& segs = alias.segments;
            for (std::size_t i = 0; i + 1 < segs.size(); ++i)
            {
                Symbol anchor{};
                anchor.name = segs[i].name;
                anchor.kind = SymbolKind::UsingGroup;
                anchor.via_using = &u;
                anchor.is_exported = u.is_public;
                anchor.is_spilled = spill;
                cur = cur->ensure_namespace(segs[i].name, anchor);
            }
            return cur;
        }

        bool copy_all_slots(NameBinding const& src, std::string_view target_name, Scope& dst, ast::UsingDecl const* via, bool spill,
                            bool preserve_exported = false)
        {
            bool added = false;
            bool exported = via->is_public;
            auto* existing = dst.find_binding_local(target_name);

            auto apply_using_flags = [&](Symbol& s) {
                s.via_using = via;
                s.is_spilled = spill;
                if (preserve_exported)
                    s.is_exported |= exported;
                else
                    s.is_exported = exported;
            };

            if (src.has_type)
            {
                Symbol s = src.type_sym;
                s.name = target_name;
                apply_using_flags(s);
                if (existing && existing->has_type && existing->type_sym.decl == s.decl)
                    added = merge_symbol(existing->type_sym, s) || added;
                else if (!existing || !existing->has_type)
                {
                    if (dst.define_type(s) == DefineResult::Ok)
                        added = true;
                }
            }

            for (auto const& vs : src.value_syms)
            {
                Symbol s = vs;
                s.name = target_name;
                apply_using_flags(s);
                auto* binding = dst.find_binding_local(target_name);

                if (is_variable_like(vs.kind))
                {
                    if (binding)
                    {
                        auto it = std::ranges::find_if(binding->value_syms, [&](Symbol& e) { return e.kind == s.kind && e.decl == s.decl; });
                        if (it != binding->value_syms.end())
                            added = merge_symbol(*it, s) || added;
                        else if (binding->value_syms.empty() && dst.define_variable(s) == DefineResult::Ok)
                            added = true;
                    }
                    else if (dst.define_variable(s) == DefineResult::Ok)
                        added = true;
                }
                else
                {
                    if (binding)
                    {
                        auto it = std::ranges::find_if(binding->value_syms, [&](Symbol& e) { return e.kind == s.kind && e.decl == s.decl; });
                        if (it != binding->value_syms.end())
                            added = merge_symbol(*it, s) || added;
                        else if (dst.add_function_overload(s) == DefineResult::Ok)
                            added = true;
                    }
                    else if (dst.add_function_overload(s) == DefineResult::Ok)
                        added = true;
                }
            }

            if (src.has_namespace)
            {
                Symbol s = src.namespace_sym;
                s.name = target_name;
                apply_using_flags(s);

                if (src.namespace_sym.kind == SymbolKind::Module)
                {
                    auto* binding = dst.find_binding_local(target_name);
                    if (binding && binding->has_namespace && binding->namespace_scope == src.namespace_scope)
                        added = merge_symbol(binding->namespace_sym, s) || added;
                    else if (dst.bind_namespace(target_name, src.namespace_scope, s) == DefineResult::Ok)
                        added = true;
                }
                else
                {
                    auto* before = dst.find_binding_local(target_name);
                    bool had_namespace = before && before->has_namespace;
                    Scope* group = dst.ensure_namespace(target_name, s, src.namespace_scope ? src.namespace_scope->kind() : ScopeKind::UsingGroup);
                    auto* after = dst.find_binding_local(target_name);

                    if (!had_namespace)
                        added = true;
                    else if (after && after->has_namespace && after->namespace_scope == group)
                        added = merge_symbol(after->namespace_sym, s) || added;

                    if (src.namespace_scope)
                        added = splat_scope(*src.namespace_scope, *group, *via, spill, true) || added;
                }
            }

            return added;
        }

        bool copy_spilled_slots(NameBinding const& src, std::string_view name, Scope& dst)
        {
            bool added = false;
            auto* existing = dst.find_binding_local(name);

            if (src.has_type && src.type_sym.is_spilled)
            {
                Symbol s = src.type_sym;
                s.name = name;
                if (existing && existing->has_type && existing->type_sym.decl == s.decl)
                    added = merge_symbol(existing->type_sym, s) || added;
                else if ((!existing || !existing->has_type) && dst.define_type(s) == DefineResult::Ok)
                    added = true;
            }

            for (auto const& vs : src.value_syms)
            {
                if (!vs.is_spilled)
                    continue;

                Symbol s = vs;
                s.name = name;
                auto* binding = dst.find_binding_local(name);

                if (is_variable_like(vs.kind))
                {
                    if (binding)
                    {
                        auto it = std::ranges::find_if(binding->value_syms, [&](Symbol& e) { return e.kind == s.kind && e.decl == s.decl; });
                        if (it != binding->value_syms.end())
                            added = merge_symbol(*it, s) || added;
                        else if (binding->value_syms.empty() && dst.define_variable(s) == DefineResult::Ok)
                            added = true;
                    }
                    else if (dst.define_variable(s) == DefineResult::Ok)
                        added = true;
                }
                else
                {
                    if (binding)
                    {
                        auto it = std::ranges::find_if(binding->value_syms, [&](Symbol& e) { return e.kind == s.kind && e.decl == s.decl; });
                        if (it != binding->value_syms.end())
                            added = merge_symbol(*it, s) || added;
                        else if (dst.add_function_overload(s) == DefineResult::Ok)
                            added = true;
                    }
                    else if (dst.add_function_overload(s) == DefineResult::Ok)
                        added = true;
                }
            }

            if (src.has_namespace && src.namespace_sym.is_spilled)
            {
                Symbol s = src.namespace_sym;
                s.name = name;
                auto* binding = dst.find_binding_local(name);
                if (binding && binding->has_namespace && binding->namespace_scope == src.namespace_scope)
                    added = merge_symbol(binding->namespace_sym, s) || added;
                else if ((!binding || !binding->has_namespace) && dst.bind_namespace(name, src.namespace_scope, s) == DefineResult::Ok)
                    added = true;
            }

            return added;
        }

        bool splat_scope(Scope const& src, Scope& dst, ast::UsingDecl const& u, bool spill, bool preserve_exported = false)
        {
            bool added = false;
            for (auto const& [name, b] : src.bindings())
                added = copy_all_slots(b, name, dst, &u, spill, preserve_exported) || added;

            return added;
        }

        static ast::Decl const* primary_decl_of(NameBinding const& b) noexcept
        {
            if (b.has_type)
                return b.type_sym.decl;

            if (!b.value_syms.empty())
                return b.value_syms.front().decl;

            if (b.has_namespace)
                return b.namespace_sym.decl;

            return nullptr;
        }

        void record_resolved(ast::UsingDecl& u, std::string_view name, ast::Decl const* decl)
        {
            if (!u.resolved_bindings.empty())
                return;

            ast::UsingDecl::ResolvedBinding rb{};
            rb.name = name;
            rb.decl = decl;
            u.resolved_bindings.push_back(rb);
        }

        void diagnose_unresolved(ast::UsingDecl const& u, ModuleInfo const& mod) const
        {
            auto target_range = u.target_path.range.valid() ? u.target_path.range : u.range;
            if (!u.target_path.is_empty())
            {
                if (u.using_kind == ast::UsingKind::List)
                {
                    auto const* target_scope = resolve_namespace_path(*mod.own_scope, u.target_path);
                    if (!target_scope)
                        m_diag.error(target_range, "could not resolve `{}`", path_str(u.target_path));
                    else
                        m_diag.error(target_range, "could not resolve all paths in using-list `{}`", path_str(u.target_path));
                }
                else
                    m_diag.error(target_range, "could not resolve `{}`", path_str(u.target_path));
            }
            else if (!u.target_list.empty())
                m_diag.error(u.range, "could not resolve all paths in using-list");
            else
                m_diag.error(u.range, "could not resolve using declaration");
        }

        static std::string path_str(ast::Path const& p)
        {
            std::string s;
            for (std::size_t i = 0; i < p.segments.size(); ++i)
            {
                if (i > 0)
                    s += "::";

                s += p.segments[i].name;
            }
            return s;
        }
    };

    void resolve_usings(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, std::pmr::polymorphic_allocator<> alloc)
    {
        UsingResolver{modules, diag, alloc}.run();
    }

} // namespace dcc::sema
