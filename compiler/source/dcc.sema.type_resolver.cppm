module;

#include <algorithm>

export module dcc.sema.type_resolver;

import std;
import dcc.ast;
import dcc.comptime;
import dcc.diag;
import dcc.lex.tokens;
import dcc.si;
import dcc.sm;
import dcc.types;
import dcc.sema.scope;
import dcc.sema.type_helpers;

export namespace dcc::sema
{
    void resolve_signature_types(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, types::TypeContext& type_ctx,
                                 std::pmr::polymorphic_allocator<> alloc);

    namespace detail
    {
        struct ResolvedType
        {
            types::TypePtr type{};
            types::Qual quals{types::Qual::None};
        };

        [[nodiscard]] constexpr types::Qual qual_or(types::Qual a, types::Qual b) noexcept
        {
            return static_cast<types::Qual>(std::to_underlying(a) | std::to_underlying(b));
        }

        [[nodiscard]] std::string_view decl_name(ast::Decl const* d) noexcept
        {
            if (!d)
                return "<null>";

            switch (d->kind)
            {
                case ast::DeclKind::Struct:
                    return static_cast<ast::StructDecl const*>(d)->name;
                case ast::DeclKind::Union:
                    return static_cast<ast::UnionDecl const*>(d)->name;
                case ast::DeclKind::Enum:
                    return static_cast<ast::EnumDecl const*>(d)->name;
                case ast::DeclKind::Func:
                    return static_cast<ast::FuncDecl const*>(d)->name;
                case ast::DeclKind::Var:
                    return static_cast<ast::VarDecl const*>(d)->name;
                case ast::DeclKind::Using:
                    return static_cast<ast::UsingDecl const*>(d)->alias_path.is_empty()
                               ? std::string_view{"<using>"}
                               : static_cast<ast::UsingDecl const*>(d)->alias_path.segments.back().name;
                case ast::DeclKind::Module:
                    return "module";
                case ast::DeclKind::Import:
                    return "import";
                case ast::DeclKind::StaticIfGroup:
                    return "static if";
            }
            return "<decl>";
        }

        struct TemplateEnv
        {
            si::InternedHashMap<types::TypePtr> types;

            [[nodiscard]] types::TypePtr find(std::string_view name) const
            {
                auto it = types.find(name);
                return it == types.end() ? nullptr : it->second;
            }
        };

    } // namespace detail

    class TypeResolver
    {
    public:
        TypeResolver(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, types::TypeContext& type_ctx,
                     std::pmr::polymorphic_allocator<> alloc)
            : m_modules{modules}, m_diag{diag}, m_types{type_ctx}, m_alloc{alloc}
        {
        }

        void run()
        {
            prepare_placeholders();

            for (auto const& m : m_modules)
            {
                if (!m->tu)
                    continue;

                resolve_module(*m);
                if (m->state < ModuleState::TypesResolved)
                    m->state = ModuleState::TypesResolved;
            }
        }

    private:
        std::span<std::unique_ptr<ModuleInfo> const> m_modules;
        diag::DiagnosticEngine& m_diag;
        types::TypeContext& m_types;
        std::pmr::polymorphic_allocator<> m_alloc;

        std::unordered_map<ast::Decl const*, types::TypePtr> m_decl_types;
        std::unordered_map<ast::UsingDecl const*, types::TypePtr> m_alias_cache;
        std::unordered_map<ast::TypeExpr const*, detail::ResolvedType> m_type_cache;
        std::unordered_set<ast::UsingDecl const*> m_alias_resolving;
        std::unordered_set<ast::TypeExpr const*> m_type_resolving;

        struct FinalizeFrame
        {
            ast::Decl const* decl;
            std::string_view field_name;
        };
        std::vector<FinalizeFrame> m_finalizing_stack;
        std::unordered_set<ast::Decl const*> m_cycle_reported;

        [[nodiscard]] bool is_finalizing(ast::Decl const* decl) const noexcept
        {
            return std::ranges::find_if(m_finalizing_stack, [decl](FinalizeFrame const& fr) { return fr.decl == decl; }) != m_finalizing_stack.end();
        }

        void prepare_placeholders()
        {
            for (auto const& m : m_modules)
            {
                if (!m->tu)
                    continue;

                for (auto* d : m->tu->decls)
                    switch (d->kind)
                    {
                        case ast::DeclKind::Struct:
                            m_decl_types.emplace(d, m_types.nominal_t(types::TypeKind::Struct, d));
                            break;
                        case ast::DeclKind::Union:
                            m_decl_types.emplace(d, m_types.nominal_t(types::TypeKind::Union, d));
                            break;
                        case ast::DeclKind::Enum:
                            m_decl_types.emplace(d, m_types.nominal_t(types::TypeKind::Enum, d));
                            break;
                        default:
                            break;
                    }

                for (auto* d : m->tu->decls)
                {
                    if (auto* s = ast::node_cast<ast::StructDecl>(d))
                        prepare_template_params(s->template_params);
                    else if (auto* e = ast::node_cast<ast::EnumDecl>(d))
                        prepare_template_params(e->template_params);
                    else if (auto* f = ast::node_cast<ast::FuncDecl>(d))
                        prepare_template_params(f->template_params);
                    else if (auto* u = ast::node_cast<ast::UsingDecl>(d))
                        prepare_template_params(u->template_params);
                }
            }
        }

        void prepare_template_params(std::pmr::vector<ast::TemplateParam>& params)
        {
            for (std::size_t i = 0; i < params.size(); ++i)
            {
                auto& tp = params[i];
                if (!tp.value_type)
                    std::ignore = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
            }
        }

        void resolve_module(ModuleInfo& mod)
        {
            for (auto* d : mod.tu->decls)
                switch (d->kind)
                {
                    case ast::DeclKind::Struct:
                        resolve_struct(mod, *static_cast<ast::StructDecl*>(d));
                        break;
                    case ast::DeclKind::Union:
                        resolve_union(mod, *static_cast<ast::UnionDecl*>(d));
                        break;
                    case ast::DeclKind::Enum:
                        resolve_enum(mod, *static_cast<ast::EnumDecl*>(d));
                        break;
                    case ast::DeclKind::Func:
                        resolve_func(mod, *static_cast<ast::FuncDecl*>(d));
                        break;
                    case ast::DeclKind::Var:
                        resolve_var(mod, *static_cast<ast::VarDecl*>(d));
                        break;
                    case ast::DeclKind::Using:
                        resolve_using(mod, *static_cast<ast::UsingDecl*>(d));
                        break;
                    case ast::DeclKind::Module:
                    case ast::DeclKind::Import:
                    case ast::DeclKind::StaticIfGroup:
                        break;
                }
        }

        void resolve_struct(ModuleInfo& mod, ast::StructDecl& d)
        {
            auto env = make_env(d.template_params);
            for (auto& tp : d.template_params)
                if (tp.value_type)
                    std::ignore = resolve_type_expr(tp.value_type, mod, env);

            for (auto& f : d.fields)
                std::ignore = resolve_type_expr(f.type, mod, env);

            finalize_struct(d);
        }

        void resolve_union(ModuleInfo& mod, ast::UnionDecl& d)
        {
            detail::TemplateEnv env;
            for (auto& f : d.fields)
                std::ignore = resolve_type_expr(f.type, mod, env);

            finalize_union(d);
        }

        void resolve_enum(ModuleInfo& mod, ast::EnumDecl& d)
        {
            auto env = make_env(d.template_params);
            for (auto& tp : d.template_params)
                if (tp.value_type)
                    std::ignore = resolve_type_expr(tp.value_type, mod, env);

            if (d.backing_type)
            {
                auto backing = resolve_type_expr(d.backing_type, mod, env);
                auto* et = const_cast<types::EnumType*>(types::type_cast<types::EnumType>(type_of(d)));
                if (auto* en = et)
                    en->backing = backing.type;
            }

            for (auto& v : d.variants)
                for (auto* p : v.payload)
                    std::ignore = resolve_type_expr(p, mod, env);

            finalize_enum(d, mod);

            if (auto forced_align = decl_align_val(d))
            {
                auto* et = const_cast<types::EnumType*>(types::type_cast<types::EnumType>(type_of(d)));
                if (et && et->is_complete)
                {
                    if (forced_align < et->byte_align)
                        m_diag.error(d.range, "alignment {} is less than the natural alignment {} of enum `{}`", forced_align, et->byte_align, d.name);

                    et->byte_align = forced_align;
                    et->byte_size = align_up(et->byte_size, forced_align);
                    et->layout_is_default = false;
                }
            }
        }

        void resolve_func(ModuleInfo& mod, ast::FuncDecl& d)
        {
            auto env = make_env(d.template_params);
            for (auto& tp : d.template_params)
                if (tp.value_type)
                    std::ignore = resolve_type_expr(tp.value_type, mod, env);

            if (d.return_type)
                std::ignore = resolve_type_expr(d.return_type, mod, env);

            for (auto& p : d.params)
                std::ignore = resolve_type_expr(p.type, mod, env);
        }

        void resolve_var(ModuleInfo& mod, ast::VarDecl& d)
        {
            detail::TemplateEnv env;
            if (d.type)
                std::ignore = resolve_type_expr(d.type, mod, env);
        }

        void resolve_concept_target_expr(ast::Expr* expr, ModuleInfo const& mod, detail::TemplateEnv const& env)
        {
            if (!expr)
                return;

            switch (expr->kind)
            {
                case ast::ExprKind::Compiles: {
                    auto& c = *static_cast<ast::CompilesExpr*>(expr);
                    for (auto& p : c.params)
                        if (p.type)
                            std::ignore = resolve_type_expr(p.type, mod, env);

                    break;
                }
                case ast::ExprKind::Unary:
                    resolve_concept_target_expr(static_cast<ast::UnaryExpr*>(expr)->operand, mod, env);
                    break;
                case ast::ExprKind::Postfix:
                    resolve_concept_target_expr(static_cast<ast::PostfixExpr*>(expr)->operand, mod, env);
                    break;
                case ast::ExprKind::Binary: {
                    auto& b = *static_cast<ast::BinaryExpr*>(expr);
                    resolve_concept_target_expr(b.lhs, mod, env);
                    resolve_concept_target_expr(b.rhs, mod, env);
                    break;
                }
                case ast::ExprKind::Call: {
                    auto& c = *static_cast<ast::CallExpr*>(expr);
                    resolve_concept_target_expr(c.callee, mod, env);
                    for (auto* arg : c.args)
                        resolve_concept_target_expr(arg, mod, env);

                    break;
                }
                case ast::ExprKind::FieldAccess:
                    resolve_concept_target_expr(static_cast<ast::FieldAccessExpr*>(expr)->object, mod, env);
                    break;
                case ast::ExprKind::Index: {
                    auto& i = *static_cast<ast::IndexExpr*>(expr);
                    resolve_concept_target_expr(i.object, mod, env);
                    resolve_concept_target_expr(i.index, mod, env);
                    break;
                }
                case ast::ExprKind::Cast: {
                    auto& c = *static_cast<ast::CastExpr*>(expr);
                    resolve_concept_target_expr(c.operand, mod, env);
                    if (c.target)
                        std::ignore = resolve_type_expr(c.target, mod, env);

                    break;
                }
                case ast::ExprKind::StructLiteral: {
                    auto& s = *static_cast<ast::StructLiteralExpr*>(expr);
                    if (s.type)
                        std::ignore = resolve_type_expr(s.type, mod, env);

                    for (auto& f : s.fields)
                        resolve_concept_target_expr(f.value, mod, env);

                    break;
                }
                case ast::ExprKind::Sizeof:
                    if (auto* t = static_cast<ast::SizeofExpr*>(expr)->target)
                        std::ignore = resolve_type_expr(t, mod, env);
                    break;
                case ast::ExprKind::Alignof:
                    if (auto* t = static_cast<ast::AlignofExpr*>(expr)->target)
                        std::ignore = resolve_type_expr(t, mod, env);
                    break;
                case ast::ExprKind::Offsetof:
                    if (auto* t = static_cast<ast::OffsetofExpr*>(expr)->target)
                        std::ignore = resolve_type_expr(t, mod, env);
                    break;
                case ast::ExprKind::TypeAST:
                    if (auto* t = static_cast<ast::TypeASTExpr*>(expr)->type_node)
                        std::ignore = resolve_type_expr(t, mod, env);
                    break;
                case ast::ExprKind::Range: {
                    auto& r = *static_cast<ast::RangeExpr*>(expr);
                    resolve_concept_target_expr(r.start, mod, env);
                    resolve_concept_target_expr(r.end, mod, env);
                    break;
                }
                case ast::ExprKind::TemplateInst: {
                    auto& t = *static_cast<ast::TemplateInstExpr*>(expr);
                    resolve_concept_target_expr(t.callee, mod, env);
                    for (auto& arg : t.template_args)
                    {
                        if (arg.type)
                            std::ignore = resolve_type_expr(arg.type, mod, env);
                        if (arg.expr)
                            resolve_concept_target_expr(arg.expr, mod, env);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        void resolve_using(ModuleInfo& mod, ast::UsingDecl& d)
        {
            auto env = make_env(d.template_params);
            for (auto& tp : d.template_params)
                if (tp.value_type)
                    std::ignore = resolve_type_expr(tp.value_type, mod, env);

            switch (d.using_kind)
            {
                case ast::UsingKind::Alias:
                    if (d.target_type)
                    {
                        if (auto const* nt = ast::node_cast<ast::NamedType>(d.target_type);
                            nt && d.alias_path.is_simple() && nt->path.is_simple() && nt->path.simple_name() == d.alias_path.segments.back().name)
                            m_diag.error(d.target_type->range, "cyclic type alias");
                        else
                        {
                            bool quiet = ast::node_cast<ast::NamedType>(d.target_type) != nullptr;
                            std::ignore = resolve_type_expr(d.target_type, mod, env, quiet);
                        }
                    }
                    break;
                case ast::UsingKind::Concept:
                    resolve_concept_target_expr(d.target_expr, mod, env);
                    break;
                case ast::UsingKind::BareImport:
                case ast::UsingKind::Wildcard:
                case ast::UsingKind::List:
                    break;
            }
        }

        [[nodiscard]] detail::TemplateEnv make_env(std::pmr::vector<ast::TemplateParam>& params)
        {
            detail::TemplateEnv env;
            for (std::size_t i = 0; i < params.size(); ++i)
            {
                auto& tp = params[i];
                if (!tp.value_type)
                    env.types.emplace(tp.name,
                                      m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i)));
            }
            return env;
        }

        [[nodiscard]] detail::ResolvedType resolve_type_expr(ast::TypeExpr* node, ModuleInfo const& mod, detail::TemplateEnv const& env,
                                                             bool quiet_unknown = false)
        {
            if (!node)
                return {m_types.m_errort(), types::Qual::None};

            if (auto it = m_type_cache.find(node); it != m_type_cache.end())
                return it->second;

            if (!m_type_resolving.insert(node).second)
            {
                if (!m_alias_resolving.empty())
                    m_diag.error(node->range, "cyclic type alias");
                return {m_types.m_errort(), types::Qual::None};
            }

            detail::ResolvedType out{};
            switch (node->kind)
            {
                case ast::TypeKind::Primitive:
                    out.type = resolve_primitive(static_cast<ast::PrimitiveType*>(node)->which);
                    break;
                case ast::TypeKind::Named:
                    out = resolve_named(static_cast<ast::NamedType*>(node), mod, env, quiet_unknown);
                    break;
                case ast::TypeKind::Pointer: {
                    auto inner = resolve_type_expr(static_cast<ast::PointerType*>(node)->pointee, mod, env, quiet_unknown);
                    out.type = m_types.pointer_to(inner.type, inner.quals);
                    break;
                }
                case ast::TypeKind::Array: {
                    auto* t = static_cast<ast::ArrayType*>(node);
                    auto inner = resolve_type_expr(t->element, mod, env, quiet_unknown);
                    if (inner.type && is_concrete_void_type(inner.type))
                    {
                        m_diag.error(t->element->range, "array element type cannot be void");
                        out.type = m_types.m_errort();
                        break;
                    }
                    auto count = resolve_const_uint(t->size, mod, env);
                    out.type = m_types.array_t(inner.type, count.value_or(0));
                    break;
                }
                case ast::TypeKind::Slice: {
                    auto inner = resolve_type_expr(static_cast<ast::SliceType*>(node)->element, mod, env, quiet_unknown);
                    if (inner.type && is_concrete_void_type(inner.type))
                    {
                        m_diag.error(node->range, "slice element type cannot be void");
                        out.type = m_types.m_errort();
                        break;
                    }
                    out.type = m_types.slice_t(inner.type, inner.quals);
                    break;
                }
                case ast::TypeKind::Fam: {
                    auto inner = resolve_type_expr(static_cast<ast::FamType*>(node)->element, mod, env, quiet_unknown);
                    out.type = m_types.fam_t(inner.type);
                    break;
                }
                case ast::TypeKind::FuncPtr: {
                    auto* t = static_cast<ast::FuncPtrType*>(node);
                    auto ret = resolve_type_expr(t->return_type, mod, env, quiet_unknown);
                    std::vector<types::TypePtr> params;
                    params.reserve(t->params.size());
                    for (auto* p : t->params)
                    {
                        auto rp = resolve_type_expr(p, mod, env, quiet_unknown);
                        params.push_back(rp.type);
                    }
                    out.type = m_types.funcptr_t(ret.type, params);
                    break;
                }
                case ast::TypeKind::Qualified: {
                    auto* t = static_cast<ast::QualifiedType*>(node);
                    out = resolve_type_expr(t->inner, mod, env, quiet_unknown);
                    out.quals = detail::qual_or(out.quals, ast_to_type_qual(t->quals));
                    break;
                }
                case ast::TypeKind::PackIndex: {
                    auto* pi = static_cast<ast::PackIndexType*>(node);
                    auto base = resolve_type_expr(pi->base, mod, env, quiet_unknown);
                    if (!base.type || base.type->kind == types::TypeKind::Error)
                    {
                        out.type = m_types.m_errort();
                        break;
                    }

                    if (!is_pack_indexable_type(base.type))
                    {
                        m_diag.error(pi->range, "cannot index non-pack type with .N syntax");
                        out.type = m_types.m_errort();
                        break;
                    }

                    auto idx_val = resolve_const_uint(pi->index, mod, env);
                    if (!idx_val)
                    {
                        out.type = m_types.m_errort();
                        m_diag.error(pi->index->range, "pack index must be a compile-time constant");
                        break;
                    }

                    if (types::type_cast<types::TemplateParamType>(base.type))
                    {
                        out.type = m_types.type_pack_t(base.type, static_cast<std::uint32_t>(*idx_val));
                    }
                    else if (auto const* type_pack = types::type_cast<types::TypePackType>(base.type))
                    {
                        if (!types::type_cast<types::TemplateParamType>(type_pack->element))
                            out.type = type_pack->element;
                        else
                            out.type = m_types.type_pack_t(type_pack->element, type_pack->pack_index + static_cast<std::uint32_t>(*idx_val));
                    }
                    break;
                }
            }

            apply_sema(*node, out);
            m_type_resolving.erase(node);
            m_type_cache.emplace(node, out);
            return out;
        }

        [[nodiscard]] detail::ResolvedType resolve_named(ast::NamedType* node, ModuleInfo const& mod, detail::TemplateEnv const& env, bool quiet_unknown)
        {
            detail::ResolvedType out{};

            if (node->path.is_simple())
            {
                if (auto t = env.find(node->path.simple_name()))
                {
                    out.type = t;
                    if (!node->template_args.empty())
                        m_diag.error(node->range, "template parameter `{}` cannot take template arguments", node->path.simple_name());

                    return out;
                }
            }

            auto const* sym = resolve_type_path(*mod.own_scope, node->path);
            if (!sym)
            {
                if (!quiet_unknown)
                    m_diag.error(node->range, "unknown type `{}`", path_str(node->path));

                out.type = m_types.m_errort();
                return out;
            }

            out = resolve_symbol_type(*sym, mod, env, node->template_args, node->range, quiet_unknown);
            return out;
        }

        [[nodiscard]] detail::ResolvedType resolve_symbol_type(Symbol const& sym, ModuleInfo const& mod, detail::TemplateEnv const& env,
                                                               std::pmr::vector<ast::TemplateArg> const& args, sm::SourceRange range, bool quiet_unknown)
        {
            switch (sym.kind)
            {
                case SymbolKind::Struct:
                case SymbolKind::Union:
                case SymbolKind::Enum:
                    return resolve_nominal(sym.decl, mod, env, args, range, quiet_unknown);
                case SymbolKind::TypeAlias:
                    if (auto const* u = ast::node_cast<ast::UsingDecl>(sym.decl))
                    {
                        if (u->using_kind == ast::UsingKind::Alias)
                            return resolve_alias(*u, mod, env, args, range, quiet_unknown);

                        if (!quiet_unknown)
                            m_diag.error(range, "`{}` is not a type", sym.name);
                        return {m_types.m_errort(), types::Qual::None};
                    }
                    break;
                case SymbolKind::TemplateParam:
                    break;
                default:
                    break;
            }

            if (!quiet_unknown)
                m_diag.error(range, "`{}` is not a type", sym.name);

            return {m_types.m_errort(), types::Qual::None};
        }

        [[nodiscard]] detail::ResolvedType resolve_nominal(ast::Decl const* decl, ModuleInfo const& mod, detail::TemplateEnv const& env,
                                                           std::pmr::vector<ast::TemplateArg> const& args, sm::SourceRange range, bool quiet_unknown)
        {
            if (!type_of(*decl))
                return {m_types.m_errort(), types::Qual::None};

            std::vector<types::TypePtr> resolved_args;
            resolved_args.reserve(args.size());
            for (auto const& arg : args)
                if (arg.type)
                {
                    auto arg_resolved = resolve_type_expr(arg.type, mod, env, quiet_unknown);
                    resolved_args.push_back(arg_resolved.type);

                    if (arg_resolved.type && is_concrete_void_type(arg_resolved.type))
                    {
                        auto decl_kind = decl ? decl->kind : ast::DeclKind::Module;
                        if (decl_kind != ast::DeclKind::Enum)
                        {
                            if (!quiet_unknown)
                                m_diag.error(arg.type->range, "void is not allowed as a type argument to `{}`", detail::decl_name(decl));
                            resolved_args.back() = m_types.m_errort();
                        }
                    }
                }
                else
                {
                    if (!quiet_unknown)
                        m_diag.error(arg.range, "value template arguments are not supported in signature types");

                    resolved_args.push_back(m_types.m_errort());
                }

            auto expected = template_param_count(*decl);
            if (!resolved_args.empty() && expected == 0)
            {
                if (!quiet_unknown)
                    m_diag.error(range, "`{}` does not take template arguments", detail::decl_name(decl));

                return {m_types.m_errort(), types::Qual::None};
            }

            if (expected > 0 && resolved_args.size() != expected)
            {
                if (!quiet_unknown)
                    m_diag.error(range, "template argument count mismatch for `{}`", detail::decl_name(decl));

                return {m_types.m_errort(), types::Qual::None};
            }

            if (!resolved_args.empty())
            {
                if (auto kind = nominal_kind(*decl); kind != types::TypeKind::Error)
                    return {m_types.nominal_t(kind, const_cast<ast::Decl*>(decl), resolved_args), types::Qual::None};
            }

            if (auto kind = nominal_kind(*decl); kind != types::TypeKind::Error)
                return {m_types.nominal_t(kind, const_cast<ast::Decl*>(decl)), types::Qual::None};

            return {m_types.m_errort(), types::Qual::None};
        }

        [[nodiscard]] detail::ResolvedType resolve_alias(ast::UsingDecl const& u, ModuleInfo const& mod, detail::TemplateEnv const& env,
                                                         std::pmr::vector<ast::TemplateArg> const& args, sm::SourceRange range, bool quiet_unknown)
        {
            bool cacheable = u.template_params.empty() && args.empty();
            if (cacheable)
            {
                if (auto it = m_alias_cache.find(&u); it != m_alias_cache.end())
                    return {it->second, types::Qual::None};
            }

            if (!m_alias_resolving.insert(&u).second)
            {
                m_diag.error(range, "cyclic type alias `{}`", detail::decl_name(&u));
                return {m_types.m_errort(), types::Qual::None};
            }

            detail::ResolvedType out{m_types.m_errort(), types::Qual::None};
            auto inst_env = env;
            if (!u.template_params.empty() || !args.empty())
            {
                if (u.template_params.size() != args.size())
                {
                    if (!quiet_unknown)
                        m_diag.error(range, "template argument count mismatch for `{}`", detail::decl_name(&u));

                    m_alias_resolving.erase(&u);
                    return {m_types.m_errort(), types::Qual::None};
                }

                for (std::size_t i = 0; i < args.size(); ++i)
                {
                    auto const& tp = u.template_params[i];
                    auto const& arg = args[i];

                    if (!arg.type)
                    {
                        if (!quiet_unknown)
                            m_diag.error(arg.range, "value template arguments are not supported in signature types");
                        continue;
                    }

                    inst_env.types[tp.name] = resolve_type_expr(arg.type, mod, env, quiet_unknown).type;
                }
            }

            if (u.target_type)
                out = resolve_type_expr(u.target_type, mod, inst_env, quiet_unknown);
            else
            {
                if (!quiet_unknown)
                    m_diag.error(range, "malformed type alias `{}`", detail::decl_name(&u));
            }

            if (out.type && out.type->kind != types::TypeKind::Error && decl_has_attr(u, "nominal"))
                out.type = m_types.nominal_alias_t(out.type, &u);

            m_alias_resolving.erase(&u);
            if (cacheable)
                m_alias_cache.emplace(&u, out.type);
            return out;
        }

        void apply_sema(ast::TypeExpr& node, detail::ResolvedType resolved)
        {
            set_canonical(node.sema, resolved.type);
            node.sema.resolved_decl = resolved_decl(node, resolved.type);
            node.sema.byte_size = resolved.type ? resolved.type->byte_size : 0;
            node.sema.byte_align = resolved.type ? resolved.type->byte_align : 0;
            node.sema.is_complete = resolved.type ? resolved.type->is_complete : false;
            node.sema.is_zero_sized = resolved.type ? resolved.type->is_zero_sized : false;
            node.sema.layout_is_default = resolved.type ? resolved.type->layout_is_default : true;
        }

        [[nodiscard]] ast::Decl const* resolved_decl([[maybe_unused]] ast::TypeExpr const& node, [[maybe_unused]] types::TypePtr type) const noexcept
        {
            return nullptr;
        }

        [[nodiscard]] types::TypePtr resolve_primitive(lex::TokenKind kind)
        {
            switch (kind)
            {
                case lex::TokenKind::KwVoid:
                    return m_types.m_voidt();
                case lex::TokenKind::KwBool:
                    return m_types.m_boolt();
                case lex::TokenKind::Kwi8:
                    return m_types.int_t(8, true);
                case lex::TokenKind::Kwi16:
                    return m_types.int_t(16, true);
                case lex::TokenKind::Kwi32:
                    return m_types.int_t(32, true);
                case lex::TokenKind::Kwi64:
                    return m_types.int_t(64, true);
                case lex::TokenKind::Kwu8:
                    return m_types.int_t(8, false);
                case lex::TokenKind::Kwu16:
                    return m_types.int_t(16, false);
                case lex::TokenKind::Kwu32:
                    return m_types.int_t(32, false);
                case lex::TokenKind::Kwu64:
                    return m_types.int_t(64, false);
                case lex::TokenKind::Kwf32:
                    return m_types.float_t(32);
                case lex::TokenKind::Kwf64:
                    return m_types.float_t(64);
                case lex::TokenKind::KwChar:
                    return m_types.m_chart();
                case lex::TokenKind::KwNullT:
                    return m_types.m_nullt();
                case lex::TokenKind::KwUsize:
                    return m_types.usize_t();
                case lex::TokenKind::KwIsize:
                    return m_types.isize_t();
                default:
                    return m_types.m_errort();
            }
        }

        [[nodiscard]] types::Qual ast_to_type_qual(ast::Qual q) const noexcept
        {
            types::Qual out = types::Qual::None;
            if (ast::has_qual(q, ast::Qual::Const))
                out = detail::qual_or(out, types::Qual::Const);
            if (ast::has_qual(q, ast::Qual::Volatile))
                out = detail::qual_or(out, types::Qual::Volatile);
            if (ast::has_qual(q, ast::Qual::Restrict))
                out = detail::qual_or(out, types::Qual::Restrict);

            return out;
        }

        [[nodiscard]] std::optional<std::uint64_t> resolve_const_uint(ast::Expr* expr, ModuleInfo const&, detail::TemplateEnv const&)
        {
            if (!expr)
                return std::nullopt;

            if (auto* lit = ast::node_cast<ast::IntLiteralExpr>(expr); lit && lit->value >= 0)
                return static_cast<std::uint64_t>(lit->value);

            return std::nullopt;
        }

        [[nodiscard]] std::string path_str(ast::Path const& p) const
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

        [[nodiscard]] std::size_t template_param_count(ast::Decl const& d) const noexcept
        {
            switch (d.kind)
            {
                case ast::DeclKind::Struct:
                    return static_cast<ast::StructDecl const&>(d).template_params.size();
                case ast::DeclKind::Enum:
                    return static_cast<ast::EnumDecl const&>(d).template_params.size();
                case ast::DeclKind::Func:
                    return static_cast<ast::FuncDecl const&>(d).template_params.size();
                case ast::DeclKind::Using:
                    return static_cast<ast::UsingDecl const&>(d).template_params.size();
                default:
                    return 0;
            }
        }

        [[nodiscard]] types::TypeKind nominal_kind(ast::Decl const& d) const noexcept
        {
            switch (d.kind)
            {
                case ast::DeclKind::Struct:
                    return types::TypeKind::Struct;
                case ast::DeclKind::Union:
                    return types::TypeKind::Union;
                case ast::DeclKind::Enum:
                    return types::TypeKind::Enum;
                default:
                    return types::TypeKind::Error;
            }
        }

        [[nodiscard]] types::TypePtr type_of(ast::Decl const& d) const noexcept
        {
            auto it = m_decl_types.find(&d);
            return it == m_decl_types.end() ? nullptr : it->second;
        }

        [[nodiscard]] ast::TypeSema sema_of(ast::TypeExpr const& node) const noexcept { return node.sema; }

        [[nodiscard]] static bool decl_has_attr(ast::Decl const& d, std::string_view name) noexcept
        {
            for (auto const& a : d.attrs)
                if (a.name == name)
                    return true;

            return false;
        }

        [[nodiscard]] static std::uint32_t decl_align_val(ast::Decl const& d) noexcept
        {
            for (auto const& a : d.attrs)
                if (a.name == "align" && !a.args.empty())
                    if (auto* lit = ast::node_cast<ast::IntLiteralExpr>(a.args[0]))
                        if (lit->value > 0)
                            return static_cast<std::uint32_t>(lit->value);

            return 0;
        }

        void ensure_type_finalized(ast::TypeSema& ts)
        {
            auto* raw = const_cast<types::Type*>(get_canonical(ts));
            if (!raw || raw->is_complete)
                return;

            if (auto* arr = const_cast<types::ArrayType*>(types::type_cast<types::ArrayType>(raw)))
            {
                auto* elem = const_cast<types::Type*>(arr->element);
                if (elem && !elem->is_complete)
                {
                    if (auto* st = const_cast<types::StructType*>(types::type_cast<types::StructType>(elem)))
                        ensure_decl_finalized(reinterpret_cast<ast::Decl const*>(st->decl));
                    else if (auto* ut = const_cast<types::UnionType*>(types::type_cast<types::UnionType>(elem)))
                        ensure_decl_finalized(reinterpret_cast<ast::Decl const*>(ut->decl));
                }
                if (elem && elem->is_complete)
                {
                    arr->byte_align = elem->byte_align;
                    arr->byte_size = elem->byte_size * arr->count;
                    arr->is_zero_sized = (arr->byte_size == 0);
                    arr->is_complete = true;
                }

                ts.byte_size = raw->byte_size;
                ts.byte_align = raw->byte_align;
                ts.is_complete = raw->is_complete;
                ts.is_zero_sized = raw->is_zero_sized;
                return;
            }

            ast::Decl const* decl = nullptr;
            if (auto* st = types::type_cast<types::StructType>(raw))
                decl = reinterpret_cast<ast::Decl const*>(st->decl);
            else if (auto* ut = types::type_cast<types::UnionType>(raw))
                decl = reinterpret_cast<ast::Decl const*>(ut->decl);
            else if (auto* et = types::type_cast<types::EnumType>(raw))
                decl = reinterpret_cast<ast::Decl const*>(et->decl);
            else
                return;

            ensure_decl_finalized(decl);

            ts.byte_size = raw->byte_size;
            ts.byte_align = raw->byte_align;
            ts.is_complete = raw->is_complete;
            ts.is_zero_sized = raw->is_zero_sized;
        }

        void ensure_decl_finalized(ast::Decl const* decl)
        {
            if (!decl)
                return;

            ModuleInfo const* mod = nullptr;
            for (auto const& m : m_modules)
            {
                if (!m->tu)
                    continue;
                for (auto* d : m->tu->decls)
                    if (d == decl)
                    {
                        mod = m.get();
                        break;
                    }
                if (mod)
                    break;
            }
            if (!mod)
                return;

            if (decl->kind == ast::DeclKind::Struct)
            {
                auto& d = const_cast<ast::StructDecl&>(static_cast<ast::StructDecl const&>(*decl));
                auto env = make_env(d.template_params);
                for (auto& tp : d.template_params)
                    if (tp.value_type)
                        std::ignore = resolve_type_expr(tp.value_type, const_cast<ModuleInfo&>(*mod), env);

                for (auto& f : d.fields)
                    std::ignore = resolve_type_expr(f.type, const_cast<ModuleInfo&>(*mod), env);

                finalize_struct(d);
            }
            else if (decl->kind == ast::DeclKind::Union)
            {
                auto& d = const_cast<ast::UnionDecl&>(static_cast<ast::UnionDecl const&>(*decl));

                detail::TemplateEnv env;
                for (auto& f : d.fields)
                    std::ignore = resolve_type_expr(f.type, const_cast<ModuleInfo&>(*mod), env);

                finalize_union(d);
            }
            else if (decl->kind == ast::DeclKind::Enum)
            {
                auto& d = const_cast<ast::EnumDecl&>(static_cast<ast::EnumDecl const&>(*decl));
                auto env = make_env(d.template_params);
                for (auto& tp : d.template_params)
                    if (tp.value_type)
                        std::ignore = resolve_type_expr(tp.value_type, const_cast<ModuleInfo&>(*mod), env);

                if (d.backing_type)
                    std::ignore = resolve_type_expr(d.backing_type, const_cast<ModuleInfo&>(*mod), env);

                for (auto& v : d.variants)
                    for (auto* p : v.payload)
                        std::ignore = resolve_type_expr(p, const_cast<ModuleInfo&>(*mod), env);

                finalize_enum(d, *mod);
            }
        }

        [[nodiscard]] std::string format_cycle_path(std::vector<FinalizeFrame>::const_iterator first, ast::Decl const* repeated) const
        {
            std::string path;
            for (auto it = first; it != m_finalizing_stack.cend(); ++it)
            {
                if (!path.empty())
                    path += " -> ";

                if (auto* sd = ast::node_cast<ast::StructDecl>(it->decl))
                    path += sd->name;
                else if (auto* ud = ast::node_cast<ast::UnionDecl>(it->decl))
                    path += ud->name;
                else
                    path += "<decl>";

                if (!it->field_name.empty())
                {
                    path += '.';
                    path += it->field_name;
                }
            }

            if (!path.empty())
                path += " -> ";

            if (auto* sd = ast::node_cast<ast::StructDecl>(repeated))
                path += sd->name;
            else if (auto* ud = ast::node_cast<ast::UnionDecl>(repeated))
                path += ud->name;
            else
                path += "<decl>";

            return path;
        }

        void finalize_struct(ast::StructDecl& d)
        {
            auto* st = const_cast<types::StructType*>(types::type_cast<types::StructType>(type_of(d)));
            if (!st)
                return;

            if (st->is_complete)
                return;

            auto it = std::ranges::find_if(m_finalizing_stack, [&d](FinalizeFrame const& fr) { return fr.decl == &d; });
            if (it != m_finalizing_stack.end())
            {
                bool already_reported = m_cycle_reported.contains(&d);
                if (!already_reported)
                    for (auto cur = it; cur != m_finalizing_stack.cend(); ++cur)
                        if (m_cycle_reported.contains(cur->decl))
                        {
                            already_reported = true;
                            break;
                        }

                if (!already_reported)
                {
                    for (auto cur = it; cur != m_finalizing_stack.cend(); ++cur)
                        m_cycle_reported.insert(cur->decl);
                    m_cycle_reported.insert(&d);
                    m_diag.error(d.range, "cyclic struct layout: {}", format_cycle_path(it, &d));
                }

                st->is_complete = false;
                return;
            }

            m_finalizing_stack.push_back({&d, {}});

            bool is_packed = decl_has_attr(d, "packed");
            std::uint32_t forced_align = decl_align_val(d);

            bool complete = true;
            std::uint64_t size = 0;
            std::uint32_t natural_align = 1;
            bool has_fam = false;

            for (std::size_t i = 0; i < d.fields.size(); ++i)
            {
                auto& f = d.fields[i];
                f.index = static_cast<std::uint32_t>(i);

                if (!f.type)
                {
                    complete = false;
                    break;
                }

                m_finalizing_stack.back().field_name = f.name;
                ensure_type_finalized(f.type->sema);
                m_finalizing_stack.back().field_name = {};

                auto const& ts = f.type->sema;
                auto* can = ts.canonical ? get_canonical(ts) : nullptr;
                if (can && is_concrete_void_type(can))
                {
                    m_diag.error(f.type->range, "struct field `{}` cannot have void type", f.name);
                    complete = false;
                    break;
                }
                bool is_fam = can && types::is_fam_type(can);

                if (is_fam)
                {
                    if (has_fam)
                    {
                        m_diag.error(f.type->range, "struct can have at most one flexible array member");
                        m_diag.error(f.type->range, "flexible array member must be the last field of a struct");
                        complete = false;
                        break;
                    }

                    if (i != d.fields.size() - 1)
                    {
                        m_diag.error(f.type->range, "flexible array member must be the last field of a struct");
                        complete = false;
                        break;
                    }

                    has_fam = true;

                    auto* elem_type = types::fam_element(can);
                    if (!elem_type)
                    {
                        complete = false;
                        break;
                    }

                    std::uint32_t elem_align = is_packed ? 1 : elem_type->byte_align;
                    if (elem_align > natural_align)
                        natural_align = elem_align;

                    if (!is_packed)
                        size = align_up(size, elem_align);

                    f.byte_offset = static_cast<std::uint32_t>(size);
                    continue;
                }

                if (!ts.canonical || !ts.is_complete)
                {
                    complete = false;
                    break;
                }

                if (can && types::type_has_fam_struct(can))
                {
                    m_diag.error(f.type->range, "struct field `{}` has a flexible array member and cannot be embedded by value", f.name);
                    complete = false;
                    break;
                }

                std::uint32_t field_align = is_packed ? 1 : ts.byte_align;
                natural_align = std::max(field_align, natural_align);

                if (!is_packed)
                    size = align_up(size, field_align);

                f.byte_offset = static_cast<std::uint32_t>(size);
                size += ts.byte_size;
            }

            if (complete)
            {
                std::uint32_t agg_align = forced_align ? forced_align : (is_packed ? 1 : natural_align);

                if (forced_align && forced_align < natural_align)
                    m_diag.error(d.range, "alignment {} is less than the natural alignment {} of struct `{}`", forced_align, natural_align, d.name);

                st->byte_size = has_fam ? size : align_up(size, agg_align);
                st->byte_align = agg_align;
                st->is_zero_sized = (st->byte_size == 0 && !has_fam);
                st->is_complete = true;
                st->has_fam = has_fam;

                if (is_packed || forced_align || has_fam)
                    st->layout_is_default = false;
            }
            else
                st->is_complete = false;

            m_finalizing_stack.pop_back();
        }

        void finalize_union(ast::UnionDecl& d)
        {
            auto* ut = const_cast<types::UnionType*>(types::type_cast<types::UnionType>(type_of(d)));
            if (!ut)
                return;

            if (ut->is_complete)
                return;

            auto it = std::ranges::find_if(m_finalizing_stack, [&d](FinalizeFrame const& fr) { return fr.decl == &d; });
            if (it != m_finalizing_stack.end())
            {
                bool already_reported = m_cycle_reported.contains(&d);
                if (!already_reported)
                    for (auto cur = it; cur != m_finalizing_stack.cend(); ++cur)
                        if (m_cycle_reported.contains(cur->decl))
                        {
                            already_reported = true;
                            break;
                        }

                if (!already_reported)
                {
                    for (auto cur = it; cur != m_finalizing_stack.cend(); ++cur)
                        m_cycle_reported.insert(cur->decl);
                    m_cycle_reported.insert(&d);
                    m_diag.error(d.range, "cyclic struct layout: {}", format_cycle_path(it, &d));
                }

                ut->is_complete = false;
                return;
            }

            m_finalizing_stack.push_back({&d, {}});

            bool is_packed = decl_has_attr(d, "packed");
            std::uint32_t forced_align = decl_align_val(d);

            bool complete = true;
            std::uint64_t size = 0;
            std::uint32_t natural_align = 1;

            for (std::size_t i = 0; i < d.fields.size(); ++i)
            {
                auto& f = d.fields[i];
                f.index = static_cast<std::uint32_t>(i);
                f.byte_offset = 0;

                m_finalizing_stack.back().field_name = f.name;
                ensure_type_finalized(f.type->sema);
                m_finalizing_stack.back().field_name = {};

                auto const& ts = f.type->sema;
                if (ts.canonical && is_concrete_void_type(get_canonical(ts)))
                {
                    m_diag.error(f.type->range, "union field `{}` cannot have void type", f.name);
                    complete = false;
                    break;
                }
                if (!ts.canonical || !ts.is_complete)
                {
                    complete = false;
                    break;
                }

                if (ts.byte_align > natural_align)
                    natural_align = ts.byte_align;

                if (ts.byte_size > size)
                    size = ts.byte_size;
            }

            if (complete)
            {
                std::uint32_t agg_align = forced_align ? forced_align : (is_packed ? 1 : natural_align);

                if (forced_align && forced_align < natural_align)
                    m_diag.error(d.range, "alignment {} is less than the natural alignment {} of union `{}`", forced_align, natural_align, d.name);

                ut->byte_size = align_up(size, agg_align);
                ut->byte_align = agg_align;
                ut->is_zero_sized = (ut->byte_size == 0);
                ut->is_complete = true;

                if (is_packed || forced_align)
                    ut->layout_is_default = false;
            }
            else
                ut->is_complete = false;

            m_finalizing_stack.pop_back();
        }

        [[nodiscard]] static bool is_const_variable(ast::VarDecl const* vd) noexcept
        {
            if (!vd->type)
                return false;

            if (auto* qt = ast::node_cast<ast::QualifiedType>(vd->type))
                return (qt->quals & ast::Qual::Const) != ast::Qual::None;

            return false;
        }

        struct DiscriminantValue
        {
            std::uint64_t bits{};
            bool is_negative{};
        };

        [[nodiscard]] static DiscriminantValue make_disc_from_raw(std::uint64_t raw_bits, bool is_negative)
        {
            if (!is_negative)
                return DiscriminantValue{raw_bits, false};

            if (raw_bits == 0)
                return DiscriminantValue{0, false};
            std::uint64_t mag = (~raw_bits) + 1;
            return DiscriminantValue{mag, true};
        }

        [[nodiscard]] static DiscriminantValue disc_from_stored(std::int64_t stored_val, bool is_negative)
        {
            if (!is_negative)
                return DiscriminantValue{static_cast<std::uint64_t>(stored_val), false};

            if (stored_val == std::numeric_limits<std::int64_t>::min())
                return DiscriminantValue{static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1, true};

            return DiscriminantValue{static_cast<std::uint64_t>(-stored_val), true};
        }

        [[nodiscard]] static std::string disc_value_str(DiscriminantValue const& dv)
        {
            if (dv.is_negative)
                return std::format("-{}", dv.bits);

            return std::format("{}", dv.bits);
        }

        [[nodiscard]] static std::optional<std::int64_t> disc_to_int64(DiscriminantValue const& dv)
        {
            if (dv.is_negative)
            {
                if (dv.bits > std::uint64_t(std::numeric_limits<std::int64_t>::max()) + 1)
                    return std::nullopt;
                if (dv.bits == std::uint64_t(std::numeric_limits<std::int64_t>::max()) + 1)
                    return std::numeric_limits<std::int64_t>::min();
                return -static_cast<std::int64_t>(dv.bits);
            }
            return static_cast<std::int64_t>(dv.bits);
        }

        [[nodiscard]] static DiscriminantValue int64_to_disc(std::int64_t val)
        {
            if (val < 0)
            {
                if (val == std::numeric_limits<std::int64_t>::min())
                    return DiscriminantValue{std::uint64_t(std::numeric_limits<std::int64_t>::max()) + 1, true};

                return DiscriminantValue{static_cast<std::uint64_t>(-val), true};
            }
            return DiscriminantValue{static_cast<std::uint64_t>(val), false};
        }

        [[nodiscard]] std::optional<DiscriminantValue> evaluate_enum_discriminant(ast::Expr* expr, ModuleInfo const& mod,
                                                                                  si::InternedHashMap<DiscriminantValue>& prior_variants,
                                                                                  std::unordered_set<ast::VarDecl const*>& evaluating_consts,
                                                                                  ast::EnumDecl const* current_enum = nullptr)
        {
            if (!expr)
                return std::nullopt;

            switch (expr->kind)
            {
                case ast::ExprKind::IntLiteral: {
                    auto* lit = static_cast<ast::IntLiteralExpr*>(expr);
                    auto sp = lit->spelling;

                    bool is_radix =
                        sp.size() > 2 && sp[0] == '0' && (sp[1] == 'x' || sp[1] == 'X' || sp[1] == 'o' || sp[1] == 'O' || sp[1] == 'b' || sp[1] == 'B');

                    if (is_radix)
                        return DiscriminantValue{static_cast<std::uint64_t>(lit->value), false};

                    return DiscriminantValue{static_cast<std::uint64_t>(lit->value), false};
                }

                case ast::ExprKind::BoolLiteral:
                    return DiscriminantValue{static_cast<ast::BoolLiteralExpr*>(expr)->value ? 1ULL : 0ULL, false};

                case ast::ExprKind::CharLiteral:
                    return DiscriminantValue{static_cast<ast::CharLiteralExpr*>(expr)->codepoint, false};

                case ast::ExprKind::U16CharLiteral:
                    return DiscriminantValue{static_cast<ast::U16CharLiteralExpr*>(expr)->value, false};

                case ast::ExprKind::Ident: {
                    auto* ident = static_cast<ast::IdentExpr*>(expr);

                    if (auto it = prior_variants.find(ident->name); it != prior_variants.end())
                        return it->second;

                    if (mod.own_scope)
                    {
                        auto vs = mod.own_scope->lookup_values(ident->name);
                        for (auto const& sym : vs)
                        {
                            if (!sym.decl)
                                continue;

                            auto const* vd = ast::node_cast<ast::VarDecl>(sym.decl);
                            if (!vd || !vd->init)
                                continue;

                            if (!is_const_variable(vd))
                            {
                                m_diag.error(expr->range, "discriminant expression references non-const variable `{}`", ident->name);
                                return std::nullopt;
                            }

                            if (!evaluating_consts.insert(vd).second)
                            {
                                m_diag.error(expr->range, "cyclic reference to `{}` in enum discriminant", ident->name);
                                return std::nullopt;
                            }

                            auto val = evaluate_enum_discriminant(vd->init, mod, prior_variants, evaluating_consts, current_enum);
                            evaluating_consts.erase(vd);
                            if (val)
                                return *val;
                        }
                    }

                    m_diag.error(expr->range, "unknown identifier `{}` in enum discriminant", ident->name);
                    return std::nullopt;
                }

                case ast::ExprKind::PathExpr: {
                    auto* pe = static_cast<ast::PathExpr*>(expr);
                    if (pe->path.is_simple())
                    {
                        std::string_view name = pe->path.simple_name();
                        if (auto it = prior_variants.find(name); it != prior_variants.end())
                            return it->second;

                        m_diag.error(expr->range, "unknown name `{}` in enum discriminant", name);
                        return std::nullopt;
                    }

                    if (mod.own_scope)
                    {
                        auto const* sym = resolve_value_path(*mod.own_scope, pe->path);
                        if (sym && sym->kind == SymbolKind::EnumVariant && sym->decl && sym->decl->kind == ast::DeclKind::Enum)
                        {
                            auto const* enum_decl = static_cast<ast::EnumDecl const*>(sym->decl);

                            if (enum_decl == current_enum)
                            {
                                if (auto it = prior_variants.find(sym->name); it != prior_variants.end())
                                    return it->second;

                                m_diag.error(expr->range, "discriminant cannot reference later variant `{}`", sym->name);
                                return std::nullopt;
                            }

                            for (auto const& v : enum_decl->variants)
                                if (v.name == sym->name)
                                    return disc_from_stored(v.discriminant, v.discriminant_is_negative);
                        }
                    }

                    m_diag.error(expr->range, "unresolved path `{}` in enum discriminant", path_str(pe->path));
                    return std::nullopt;
                }

                case ast::ExprKind::Unary: {
                    auto* u = static_cast<ast::UnaryExpr*>(expr);
                    auto operand = evaluate_enum_discriminant(u->operand, mod, prior_variants, evaluating_consts, current_enum);
                    if (!operand)
                        return std::nullopt;

                    auto uop = token_to_unary_op(u->op);
                    if (!uop)
                    {
                        m_diag.error(expr->range, "unary operator not supported in enum discriminant");
                        return std::nullopt;
                    }

                    auto i64_opt = disc_to_int64(*operand);
                    if (!i64_opt)
                    {
                        m_diag.error(expr->range, "overflow in unary operation in enum discriminant");
                        return std::nullopt;
                    }

                    auto* i64t = m_types.int_t(64, true);
                    auto val = comptime::Value::make_int(*i64_opt, i64t);
                    auto result = val.fold_unary(*uop, i64t);
                    if (!result)
                    {
                        m_diag.error(expr->range, "overflow in unary operation in enum discriminant");
                        return std::nullopt;
                    }

                    auto result_i64 = result->get_int();
                    switch (*uop)
                    {
                        case comptime::UnaryOp::Plus:
                            return *operand;
                        case comptime::UnaryOp::Minus:
                            return int64_to_disc(result_i64);
                        case comptime::UnaryOp::BitNot:
                            return DiscriminantValue{static_cast<std::uint64_t>(result_i64), false};
                        case comptime::UnaryOp::Not:
                            return DiscriminantValue{static_cast<std::uint64_t>(result_i64), false};
                    }
                    return std::nullopt;
                }

                case ast::ExprKind::Binary: {
                    auto* b = static_cast<ast::BinaryExpr*>(expr);
                    auto lhs_val = evaluate_enum_discriminant(b->lhs, mod, prior_variants, evaluating_consts, current_enum);
                    if (!lhs_val)
                        return std::nullopt;

                    auto rhs_val = evaluate_enum_discriminant(b->rhs, mod, prior_variants, evaluating_consts, current_enum);
                    if (!rhs_val)
                        return std::nullopt;

                    auto bop = token_to_disc_binop(b->op);
                    if (!bop)
                    {
                        m_diag.error(expr->range, "binary operator not supported in enum discriminant");
                        return std::nullopt;
                    }

                    switch (*bop)
                    {
                        case comptime::BinaryOp::Eq:
                        case comptime::BinaryOp::Ne:
                        case comptime::BinaryOp::Lt:
                        case comptime::BinaryOp::Le:
                        case comptime::BinaryOp::Gt:
                        case comptime::BinaryOp::Ge:
                            m_diag.error(expr->range, "comparison operators are not valid in enum discriminant expressions");
                            return std::nullopt;
                        default:
                            break;
                    }

                    auto lhs_i64 = disc_to_int64(*lhs_val);
                    auto rhs_i64 = disc_to_int64(*rhs_val);
                    if (!lhs_i64 || !rhs_i64)
                    {
                        m_diag.error(expr->range, "integer overflow in enum discriminant");
                        return std::nullopt;
                    }

                    auto* i64t = m_types.int_t(64, true);
                    auto result = comptime::Value::fold_int_binary(*bop, *lhs_i64, *rhs_i64, i64t);
                    if (!result)
                    {
                        if (*bop == comptime::BinaryOp::Div || *bop == comptime::BinaryOp::Rem)
                            m_diag.error(expr->range, "division by zero in enum discriminant");
                        else
                            m_diag.error(expr->range, "integer overflow in enum discriminant");

                        return std::nullopt;
                    }

                    auto result_i64 = result->get_int();
                    switch (*bop)
                    {
                        case comptime::BinaryOp::Add:
                        case comptime::BinaryOp::Sub:
                        case comptime::BinaryOp::Mul:
                        case comptime::BinaryOp::Div:
                        case comptime::BinaryOp::Rem:
                            return int64_to_disc(result_i64);
                        case comptime::BinaryOp::BitAnd:
                        case comptime::BinaryOp::BitOr:
                        case comptime::BinaryOp::BitXor: {
                            bool neg = (lhs_val->is_negative || rhs_val->is_negative) && (result_i64 < 0);
                            return DiscriminantValue{static_cast<std::uint64_t>(result_i64), neg};
                        }
                        case comptime::BinaryOp::Shl:
                        case comptime::BinaryOp::Shr:
                            return DiscriminantValue{static_cast<std::uint64_t>(result_i64), lhs_val->is_negative};
                        default:
                            return int64_to_disc(result_i64);
                    }
                }

                case ast::ExprKind::Cast: {
                    auto* c = static_cast<ast::CastExpr*>(expr);
                    auto operand = evaluate_enum_discriminant(c->operand, mod, prior_variants, evaluating_consts, current_enum);
                    if (!operand)
                        return std::nullopt;

                    if (!c->target)
                    {
                        m_diag.error(expr->range, "cast target type not resolved in enum discriminant");
                        return std::nullopt;
                    }

                    auto* target_type = get_canonical(c->target->sema);
                    if (!types::type_cast<types::IntType>(target_type))
                    {
                        m_diag.error(expr->range, "cast to non-integer type in enum discriminant");
                        return std::nullopt;
                    }

                    auto i64_opt = disc_to_int64(*operand);
                    if (!i64_opt)
                    {
                        m_diag.error(expr->range, "integer overflow in enum discriminant");
                        return std::nullopt;
                    }

                    auto* i64t = m_types.int_t(64, true);
                    auto val = comptime::Value::make_int(*i64_opt, i64t);
                    auto result = val.fold_cast(target_type);
                    if (!result)
                        return std::nullopt;

                    auto* ti = types::type_cast<types::IntType>(target_type);
                    if (ti && !ti->is_signed)
                        return DiscriminantValue{static_cast<std::uint64_t>(result->get_int()), false};

                    return int64_to_disc(result->get_int());
                }

                default:
                    m_diag.error(expr->range, "expression is not a valid enum discriminant");
                    return std::nullopt;
            }
        }

        [[nodiscard]] static bool fits_enum_backing(DiscriminantValue const& dv, std::uint8_t bits, bool is_signed) noexcept
        {
            if (is_signed)
            {
                if (bits >= 64)
                {
                    if (dv.is_negative)
                        return dv.bits <= (std::uint64_t(1) << 63);
                    else
                        return dv.bits <= (std::uint64_t(1) << 63) - 1;
                }
                else
                {
                    std::uint64_t max_magnitude = std::uint64_t(1) << (bits - 1);
                    if (dv.is_negative)
                        return dv.bits <= max_magnitude;
                    else
                        return dv.bits <= max_magnitude - 1;
                }
            }
            else
            {
                if (dv.is_negative)
                    return false;

                if (bits >= 64)
                    return true;

                std::uint64_t const max = (std::uint64_t(1) << bits) - 1;
                return dv.bits <= max;
            }
        }

        [[nodiscard]] static std::optional<comptime::UnaryOp> token_to_unary_op(lex::TokenKind op) noexcept
        {
            using UO = comptime::UnaryOp;
            switch (op)
            {
                case lex::TokenKind::Plus:
                    return UO::Plus;
                case lex::TokenKind::Minus:
                    return UO::Minus;
                case lex::TokenKind::Tilde:
                    return UO::BitNot;
                default:
                    return std::nullopt;
            }
        }

        [[nodiscard]] static std::optional<comptime::BinaryOp> token_to_disc_binop(lex::TokenKind op) noexcept
        {
            using BO = comptime::BinaryOp;
            switch (op)
            {
                case lex::TokenKind::Plus:
                    return BO::Add;
                case lex::TokenKind::Minus:
                    return BO::Sub;
                case lex::TokenKind::Star:
                    return BO::Mul;
                case lex::TokenKind::Slash:
                    return BO::Div;
                case lex::TokenKind::Percent:
                    return BO::Rem;
                case lex::TokenKind::Amp:
                    return BO::BitAnd;
                case lex::TokenKind::Pipe:
                    return BO::BitOr;
                case lex::TokenKind::Caret:
                    return BO::BitXor;
                case lex::TokenKind::LtLt:
                    return BO::Shl;
                case lex::TokenKind::GtGt:
                    return BO::Shr;
                case lex::TokenKind::EqEq:
                    return BO::Eq;
                case lex::TokenKind::BangEq:
                    return BO::Ne;
                case lex::TokenKind::Lt:
                    return BO::Lt;
                case lex::TokenKind::LtEq:
                    return BO::Le;
                case lex::TokenKind::Gt:
                    return BO::Gt;
                case lex::TokenKind::GtEq:
                    return BO::Ge;
                default:
                    return std::nullopt;
            }
        }

        void finalize_enum(ast::EnumDecl& d, ModuleInfo const& mod)
        {
            auto* et = const_cast<types::EnumType*>(types::type_cast<types::EnumType>(type_of(d)));
            if (!et)
                return;

            if (et->is_complete)
                return;

            et->is_tagged = d.is_tagged;

            if (is_finalizing(&d))
            {
                m_diag.error(d.range, "cyclic enum layout");
                return;
            }

            m_finalizing_stack.push_back({&d, {}});

            DiscriminantValue next_implicit{0, false};
            si::InternedHashMap<DiscriminantValue> prior_variants;
            std::unordered_map<std::uint64_t, std::string_view> used_values;
            std::unordered_set<ast::VarDecl const*> evaluating_consts;
            bool has_disc_error = false;

            for (auto& v : d.variants)
            {
                if (v.explicit_value)
                {
                    auto val = evaluate_enum_discriminant(v.explicit_value, mod, prior_variants, evaluating_consts, &d);
                    if (val)
                    {
                        auto i64_opt = disc_to_int64(*val);
                        if (i64_opt)
                        {
                            v.discriminant = *i64_opt;
                            v.discriminant_is_negative = val->is_negative;
                        }
                        else
                        {
                            v.discriminant = static_cast<std::int64_t>(val->bits);
                            v.discriminant_is_negative = val->is_negative;
                        }

                        if (val->is_negative)
                        {
                            if (val->bits == 1)
                                next_implicit = DiscriminantValue{0, false};
                            else
                                next_implicit = DiscriminantValue{val->bits - 1, true};
                        }
                        else
                            next_implicit = DiscriminantValue{val->bits + 1, false};
                    }
                    else
                    {
                        v.discriminant = 0;
                        v.discriminant_is_negative = false;
                        has_disc_error = true;
                    }
                }
                else
                {
                    auto i64_opt = disc_to_int64(next_implicit);
                    if (i64_opt)
                    {
                        v.discriminant = *i64_opt;
                        v.discriminant_is_negative = next_implicit.is_negative;
                    }
                    else
                    {
                        v.discriminant = static_cast<std::int64_t>(next_implicit.bits);
                        v.discriminant_is_negative = next_implicit.is_negative;
                    }

                    if (next_implicit.is_negative)
                    {
                        if (next_implicit.bits == 1)
                            next_implicit = DiscriminantValue{0, false};
                        else
                            next_implicit = DiscriminantValue{next_implicit.bits - 1, true};
                    }
                    else
                        next_implicit = DiscriminantValue{next_implicit.bits + 1, false};
                }

                auto dup_bits = static_cast<std::uint64_t>(v.discriminant);
                if (auto it = used_values.find(dup_bits); it != used_values.end())
                {
                    auto dv = disc_from_stored(v.discriminant, v.discriminant_is_negative);
                    m_diag.error(v.range, "duplicate discriminant value {} for `{}` (previously used by `{}`)", disc_value_str(dv), v.name, it->second);
                    has_disc_error = true;
                }
                else
                    used_values.emplace(dup_bits, v.name);

                prior_variants.emplace(v.name, disc_from_stored(v.discriminant, v.discriminant_is_negative));
            }

            if (d.is_tagged)
            {
                bool has_payload_error = false;
                for (auto& v : d.variants)
                {
                    if (v.payload.size() > 1)
                    {
                        m_diag.error(v.range, "variant `{}` has multiple payload types; wrap them in a struct", v.name);
                        has_payload_error = true;
                    }
                }

                if (has_disc_error || has_payload_error)
                {
                    m_finalizing_stack.pop_back();
                    return;
                }

                auto variant_count = d.variants.size();

                std::uint64_t max_positive = 0;
                std::uint64_t max_negative_magnitude = 0;
                bool has_negative_disc = false;
                for (auto& v : d.variants)
                {
                    auto dv = disc_from_stored(v.discriminant, v.discriminant_is_negative);
                    if (v.discriminant_is_negative)
                    {
                        has_negative_disc = true;
                        if (dv.bits > max_negative_magnitude)
                            max_negative_magnitude = dv.bits;
                    }
                    else
                    {
                        if (dv.bits > max_positive)
                            max_positive = dv.bits;
                    }
                }

                bool needs_signed = has_negative_disc;

                types::IntType const* disc_type = nullptr;
                std::uint64_t disc_size = 0;

                if (needs_signed)
                {
                    auto neg_dv = DiscriminantValue{max_negative_magnitude, true};
                    auto pos_dv = DiscriminantValue{max_positive, false};
                    if (fits_enum_backing(neg_dv, 8, true) && fits_enum_backing(pos_dv, 8, true))
                    {
                        disc_type = static_cast<types::IntType const*>(m_types.int_t(8, true));
                        disc_size = 1;
                    }
                    else if (fits_enum_backing(neg_dv, 16, true) && fits_enum_backing(pos_dv, 16, true))
                    {
                        disc_type = static_cast<types::IntType const*>(m_types.int_t(16, true));
                        disc_size = 2;
                    }
                    else if (fits_enum_backing(neg_dv, 32, true) && fits_enum_backing(pos_dv, 32, true))
                    {
                        disc_type = static_cast<types::IntType const*>(m_types.int_t(32, true));
                        disc_size = 4;
                    }
                    else
                    {
                        disc_type = static_cast<types::IntType const*>(m_types.int_t(64, true));
                        disc_size = 8;
                    }
                }
                else
                {
                    std::uint64_t needed =
                        max_positive > static_cast<std::uint64_t>(variant_count) - 1 ? max_positive : static_cast<std::uint64_t>(variant_count) - 1;

                    if (needed <= 255)
                    {
                        disc_type = static_cast<types::IntType const*>(m_types.int_t(8, false));
                        disc_size = 1;
                    }
                    else if (needed <= 65535)
                    {
                        disc_type = static_cast<types::IntType const*>(m_types.int_t(16, false));
                        disc_size = 2;
                    }
                    else if (needed <= 4294967295)
                    {
                        disc_type = static_cast<types::IntType const*>(m_types.int_t(32, false));
                        disc_size = 4;
                    }
                    else
                    {
                        disc_type = static_cast<types::IntType const*>(m_types.int_t(64, false));
                        disc_size = 8;
                    }
                }

                std::uint32_t disc_align = static_cast<std::uint32_t>(disc_size);

                std::uint32_t max_payload_align = 1;
                std::uint64_t max_payload_size = 0;

                auto* variant_layouts = static_cast<types::TaggedEnumVariantLayout*>(
                    m_alloc.resource()->allocate(sizeof(types::TaggedEnumVariantLayout) * variant_count, alignof(types::TaggedEnumVariantLayout)));

                for (std::size_t i = 0; i < variant_count; ++i)
                {
                    auto& v = d.variants[i];
                    types::TypePtr payload_type = nullptr;
                    if (v.payload.size() == 1)
                    {
                        auto const& ts = v.payload[0]->sema;
                        ensure_type_finalized(v.payload[0]->sema);
                        payload_type = get_canonical(ts);
                        if (payload_type && is_concrete_void_type(payload_type))
                            payload_type = nullptr;

                        if (payload_type && payload_type->is_complete)
                        {
                            if (payload_type->byte_align > max_payload_align)
                                max_payload_align = payload_type->byte_align;
                            if (payload_type->byte_size > max_payload_size)
                                max_payload_size = payload_type->byte_size;
                        }
                    }

                    ::new (&variant_layouts[i]) types::TaggedEnumVariantLayout{v.name, payload_type, v.discriminant};
                }

                std::uint64_t disc_offset = 0;
                std::uint64_t payload_offset = align_up(disc_size, max_payload_align);
                std::uint64_t total_size_raw = payload_offset + max_payload_size;

                std::uint32_t total_align = disc_align > max_payload_align ? disc_align : max_payload_align;
                if (total_align < 1)
                    total_align = 1;

                std::uint64_t total_size = align_up(total_size_raw, total_align);

                void* p = m_alloc.resource()->allocate(sizeof(types::TaggedEnumLayout), alignof(types::TaggedEnumLayout));
                auto* layout = ::new (p) types::TaggedEnumLayout{
                    .discriminant_offset = disc_offset,
                    .discriminant_size = disc_size,
                    .discriminant_type = disc_type,
                    .payload_offset = payload_offset,
                    .payload_size = max_payload_size,
                    .total_size = total_size,
                    .total_align = total_align,
                    .variants = variant_layouts,
                    .variant_count = variant_count,
                };

                et->tagged_layout = layout;
                et->byte_size = total_size;
                et->byte_align = total_align;
                et->is_zero_sized = (total_size == 0);
                et->is_complete = true;
            }
            else
            {
                if (has_disc_error)
                {
                    m_finalizing_stack.pop_back();
                    return;
                }

                if (d.backing_type && d.backing_type->sema.is_complete)
                {
                    auto* bt = get_canonical(d.backing_type->sema);
                    if (!types::type_cast<types::IntType>(bt))
                    {
                        m_diag.error(d.backing_type->range, "backing type of plain enum must be an integer type");
                        m_finalizing_stack.pop_back();
                        return;
                    }
                    et->backing = bt;
                }

                if (et->backing)
                {
                    auto const* int_ty = types::type_cast<types::IntType>(et->backing);
                    for (auto& v : d.variants)
                    {
                        auto dv = disc_from_stored(v.discriminant, v.discriminant_is_negative);
                        if (!fits_enum_backing(dv, int_ty->bits, int_ty->is_signed))
                            m_diag.error(v.range, "discriminant value {} does not fit in backing type {}", disc_value_str(dv),
                                         int_ty->is_signed ? std::format("i{}", int_ty->bits) : std::format("u{}", int_ty->bits));
                    }

                    et->byte_size = int_ty->byte_size;
                    et->byte_align = int_ty->byte_align;
                }
                else
                {
                    types::TypePtr inferred_backing{};
                    bool has_negative = false;
                    std::uint64_t max_val = 0;
                    for (auto& v : d.variants)
                    {
                        if (v.discriminant_is_negative)
                        {
                            has_negative = true;

                            std::uint64_t mag;
                            if (v.discriminant == std::numeric_limits<std::int64_t>::min())
                                mag = std::uint64_t(std::numeric_limits<std::int64_t>::max()) + 1;
                            else
                                mag = static_cast<std::uint64_t>(-v.discriminant);
                            if (mag > max_val)
                                max_val = mag;
                        }
                        else
                        {
                            auto ubits = static_cast<std::uint64_t>(v.discriminant);
                            if (ubits > max_val)
                                max_val = ubits;
                        }
                    }

                    std::uint8_t bits = 8;
                    if (has_negative)
                    {
                        while (bits < 64)
                        {
                            bool ok = true;
                            for (auto& v : d.variants)
                            {
                                auto dv = disc_from_stored(v.discriminant, v.discriminant_is_negative);
                                if (!fits_enum_backing(dv, bits, true))
                                {
                                    ok = false;
                                    break;
                                }
                            }
                            if (ok)
                                break;

                            bits *= 2;
                        }
                        inferred_backing = m_types.int_t(bits, true);
                    }
                    else
                    {
                        while (bits < 64)
                        {
                            auto const limit = (std::uint64_t(1) << bits) - 1;
                            if (max_val <= limit)
                                break;

                            bits *= 2;
                        }
                        inferred_backing = m_types.int_t(bits, false);
                    }

                    et->backing = inferred_backing;
                    auto const* int_ty = types::type_cast<types::IntType>(inferred_backing);
                    et->byte_size = int_ty->byte_size;
                    et->byte_align = int_ty->byte_align;
                }

                et->is_complete = true;
            }

            m_finalizing_stack.pop_back();
        }

        [[nodiscard]] static std::uint64_t align_up(std::uint64_t n, std::uint32_t align) noexcept
        {
            if (align <= 1)
                return n;

            auto rem = n % align;
            return rem ? (n + (align - rem)) : n;
        }
    };

    void resolve_signature_types(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, types::TypeContext& type_ctx,
                                 std::pmr::polymorphic_allocator<> alloc)
    {
        TypeResolver{modules, diag, type_ctx, alloc}.run();
    }

} // namespace dcc::sema
