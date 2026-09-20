export module dcdoc.builder;

import std;
import dcc.sm;
import dcc.si;
import dcc.lex.tokens;
import dcc.diag;
import dcc.ast;
import dcc.sema;
import dcc.session;
import dcc.target;
import dcc.config;
import dcdoc.model;

export namespace dcdoc
{
    struct TypeRef
    {
        std::string text;
        std::vector<CrossRef> refs;
    };

    class Builder
    {
    public:
        explicit Builder(std::filesystem::path entry_file, std::vector<std::filesystem::path> roots = {}, std::string argv0 = {})
            : m_entry{std::move(entry_file)}, m_roots{std::move(roots)}, m_argv0{std::move(argv0)}
        {
        }

        Project build()
        {
            Project project;
            auto files = discover();
            if (files.empty())
                return project;

            dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
            m_interner = &session.interner();
            dcc::session::CompileOptions opts;
            for (auto const& r : m_roots)
                opts.import_roots.push_back(r);
            std::string arg = m_argv0;
            char* av[2] = {arg.data(), nullptr};
            auto prefix = dcc::config::current_prefix(m_argv0.empty() ? nullptr : av).path;
            std::error_code ec;
            auto std_root = prefix / "include";
            if (std::filesystem::is_directory(std_root, ec) && !ec)
                opts.import_roots.push_back(std::move(std_root));

            opts.enable_doc_comments = true;
            auto result = session.analyze_resolve_only_files(files, opts);
            if (auto* sema = session.sema_context())
                m_instantiation_count = sema->spec_registry().instantiation_count();

            collect_file_errors(session, files, project);
            assemble(session, result.entries, project);
            return project;
        }

        [[nodiscard]] std::uint64_t instantiation_count() const noexcept { return m_instantiation_count; }

    private:
        std::filesystem::path m_entry;
        std::vector<std::filesystem::path> m_roots;
        std::string m_argv0;
        std::uint64_t m_instantiation_count{};
        dcc::si::string_interner* m_interner{};
        std::unordered_map<dcc::ast::Decl const*, std::string> m_decl_ids;
        std::unordered_map<dcc::ast::TemplateParam const*, std::string> m_tparam_ids;
        std::unordered_set<std::string> m_miss_set;

        [[nodiscard]] std::vector<std::filesystem::path> discover() const
        {
            std::vector<std::filesystem::path> out;
            std::error_code ec;
            auto consider = [&](std::filesystem::path const& root) {
                for (auto it = std::filesystem::recursive_directory_iterator(root, ec); it != std::filesystem::recursive_directory_iterator{}; it.increment(ec))
                {
                    if (ec)
                        break;

                    if (it->is_regular_file(ec) && it->path().extension() == ".dc")
                        out.push_back(it->path());
                }
            };
            for (auto const& r : m_roots)
                consider(r);
            if (!m_entry.empty())
            {
                std::error_code ec2;
                auto canon = std::filesystem::weakly_canonical(m_entry, ec2);
                if (!ec2 && std::filesystem::is_regular_file(canon, ec2))
                    out.push_back(canon);
            }
            std::ranges::sort(out);
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }

        [[nodiscard]] static std::uint32_t line_of(dcc::sm::SourceManager const& sm, dcc::sm::Location loc) noexcept
        {
            auto lc = sm.line_col(loc);
            return lc ? lc->line : 0;
        }

        [[nodiscard]] static std::string file_string(dcc::sm::SourceManager const& sm, dcc::sm::Location loc)
        {
            auto const* f = sm.get(loc.fileId);
            return f ? f->path().string() : std::string{};
        }

        void collect_file_errors(dcc::session::CompilerSession& session, std::vector<std::filesystem::path> const& files, Project& project)
        {
            auto const& sm = session.source_manager();
            for (auto const& d : session.diagnostics().diagnostics())
            {
                for (auto const& label : d.labels())
                {
                    project.file_errors.push_back({.file = file_string(sm, label.range.begin), .message = d.message()});
                    break;
                }
            }

            if (auto* sema = session.sema_context())
            {
                for (auto const& f : files)
                {
                    bool known = false;
                    for (auto const& m : sema->graph().all())
                    {
                        std::error_code ec;
                        if (std::filesystem::equivalent(m->file_path, f, ec) && !ec)
                        {
                            known = true;
                            break;
                        }
                    }
                    if (known)
                        continue;

                    bool has_error = false;
                    std::string canon = std::filesystem::weakly_canonical(f).string();
                    for (auto const& e : project.file_errors)
                    {
                        std::string ec2 = std::filesystem::weakly_canonical(e.file).string();
                        if (e.file == f.string() || e.file == canon || ec2 == canon)
                        {
                            has_error = true;
                            break;
                        }
                    }
                    if (!has_error)
                        project.file_errors.push_back({.file = f.string(), .message = "duplicate module claim, kept first"});
                }
            }
        }

        void note_miss(std::string_view raw, Project& project)
        {
            std::string s{raw};
            if (m_miss_set.insert(s).second)
                project.misses.push_back(std::move(s));
        }

        [[nodiscard]] std::string path_text(dcc::ast::Path const& p)
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

        [[nodiscard]] std::string const_expr_text(dcc::ast::Expr const* e)
        {
            if (!e)
                return "?";

            using K = dcc::ast::ExprKind;
            switch (e->kind)
            {
                case K::IntLiteral:
                    return std::string{static_cast<dcc::ast::IntLiteralExpr const*>(e)->spelling};
                case K::FloatLiteral:
                    return std::string{static_cast<dcc::ast::FloatLiteralExpr const*>(e)->spelling};
                case K::BoolLiteral:
                    return static_cast<dcc::ast::BoolLiteralExpr const*>(e)->value ? "true" : "false";
                default:
                    return "?";
            }
        }

        [[nodiscard]] CrossRef unresolved(std::string_view raw, std::string_view via, Project& project)
        {
            note_miss(raw, project);
            return {.target = {}, .resolved = false, .via = std::string{via}, .ambiguous = false, .raw = std::string{raw}, .display = {}};
        }

        [[nodiscard]] CrossRef to_decl(dcc::ast::Decl const* d, std::string_view raw, std::string_view via)
        {
            auto it = m_decl_ids.find(d);
            if (it == m_decl_ids.end())
                return {.target = {}, .resolved = false, .via = std::string{via}, .ambiguous = false, .raw = std::string{raw}, .display = {}};

            return {.target = it->second, .resolved = true, .via = std::string{via}, .ambiguous = false, .raw = std::string{raw}, .display = {}};
        }

        [[nodiscard]] CrossRef to_tparam(dcc::ast::TemplateParam const* tp, std::string_view raw, std::string_view via)
        {
            auto it = m_tparam_ids.find(tp);
            if (it == m_tparam_ids.end())
                return {.target = {}, .resolved = false, .via = std::string{via}, .ambiguous = false, .raw = std::string{raw}, .display = {}};

            return {.target = it->second, .resolved = true, .via = std::string{via}, .ambiguous = false, .raw = std::string{raw}, .display = {}};
        }

        TypeRef render_type(dcc::ast::TypeExpr const* node, dcc::sema::Scope const* scope, std::span<dcc::ast::TemplateParam const> tparams,
                            std::string_view via, Project& project, bool collect_refs = true)
        {
            TypeRef out;
            if (!node)
            {
                out.text = "?";
                return out;
            }

            using K = dcc::ast::TypeKind;
            switch (node->kind)
            {
                case K::Primitive: {
                    auto const* t = static_cast<dcc::ast::PrimitiveType const*>(node);
                    out.text = std::string{dcc::lex::to_string(t->which)};
                    break;
                }
                case K::Named: {
                    auto const* t = static_cast<dcc::ast::NamedType const*>(node);
                    std::string base = path_text(t->path);
                    std::string args;
                    std::vector<CrossRef> arefs;
                    for (auto const& a : t->template_args)
                    {
                        TypeRef ar = a.type ? render_type(a.type, scope, tparams, "TemplateArg", project, collect_refs)
                                            : TypeRef{.text = const_expr_text(a.expr), .refs = {}};
                        if (!a.type && collect_refs)
                            arefs.push_back(unresolved(ar.text, "TemplateArg", project));
                        else
                            arefs.insert(arefs.end(), ar.refs.begin(), ar.refs.end());

                        if (!args.empty())
                            args += ", ";

                        args += ar.text;
                    }
                    out.text = args.empty() ? base : base + "(" + args + ")";
                    out.refs = std::move(arefs);
                    if (collect_refs)
                    {
                        CrossRef self = resolve_named(t->path, scope, tparams, via, project);
                        out.refs.push_back(std::move(self));
                    }
                    break;
                }
                case K::Pointer: {
                    auto const* t = static_cast<dcc::ast::PointerType const*>(node);
                    TypeRef inner = render_type(t->pointee, scope, tparams, via, project, collect_refs);
                    out.text = inner.text + "*";
                    out.refs = std::move(inner.refs);
                    break;
                }
                case K::Array: {
                    auto const* t = static_cast<dcc::ast::ArrayType const*>(node);
                    TypeRef inner = render_type(t->element, scope, tparams, via, project, collect_refs);
                    out.text = inner.text + "[" + const_expr_text(t->size) + "]";
                    out.refs = std::move(inner.refs);
                    break;
                }
                case K::Slice: {
                    auto const* t = static_cast<dcc::ast::SliceType const*>(node);
                    TypeRef inner = render_type(t->element, scope, tparams, via, project, collect_refs);
                    out.text = std::string("[]") + inner.text;
                    out.refs = std::move(inner.refs);
                    break;
                }
                case K::Fam: {
                    auto const* t = static_cast<dcc::ast::FamType const*>(node);
                    TypeRef inner = render_type(t->element, scope, tparams, via, project, collect_refs);
                    out.text = inner.text + "[]";
                    out.refs = std::move(inner.refs);
                    break;
                }
                case K::FuncPtr: {
                    auto const* t = static_cast<dcc::ast::FuncPtrType const*>(node);
                    TypeRef rr = render_type(t->return_type, scope, tparams, "ReturnType", project);
                    out.refs.insert(out.refs.end(), rr.refs.begin(), rr.refs.end());
                    std::string s = rr.text + "(*)(";
                    bool first = true;
                    for (auto const& p : t->params)
                    {
                        if (!first)
                            s += ", ";

                        first = false;
                        TypeRef pr = render_type(p.type, scope, tparams, via, project, collect_refs);
                        s += pr.text;
                        if (!p.name.empty())
                        {
                            s += " ";
                            s += std::string{p.name};
                        }

                        out.refs.insert(out.refs.end(), pr.refs.begin(), pr.refs.end());
                    }
                    out.text = s + ")";
                    break;
                }
                case K::Qualified: {
                    auto const* t = static_cast<dcc::ast::QualifiedType const*>(node);
                    TypeRef inner = render_type(t->inner, scope, tparams, via, project, collect_refs);
                    std::string q;
                    if (dcc::ast::has_qual(t->quals, dcc::ast::Qual::Const))
                        q += "const ";
                    if (dcc::ast::has_qual(t->quals, dcc::ast::Qual::Volatile))
                        q += "volatile ";
                    if (dcc::ast::has_qual(t->quals, dcc::ast::Qual::Restrict))
                        q += "restrict ";

                    out.text = q + inner.text;
                    out.refs = std::move(inner.refs);
                    break;
                }
                case K::Restricted: {
                    auto const* t = static_cast<dcc::ast::RestrictedType const*>(node);
                    TypeRef inner = render_type(t->underlying, scope, tparams, via, project, collect_refs);
                    std::string s = inner.text + "{";
                    bool first = true;
                    for (auto const* e : t->elements)
                    {
                        if (!first)
                            s += ", ";
                        first = false;
                        s += const_expr_text(e);
                    }
                    out.text = s + "}";
                    out.refs = std::move(inner.refs);
                    break;
                }
                case K::PackIndex: {
                    auto const* t = static_cast<dcc::ast::PackIndexType const*>(node);
                    TypeRef inner = render_type(t->base, scope, tparams, via, project, collect_refs);
                    out.text = inner.text + "." + const_expr_text(t->index);
                    out.refs = std::move(inner.refs);
                    break;
                }
            }
            return out;
        }

        [[nodiscard]] CrossRef resolve_named(dcc::ast::Path const& path, dcc::sema::Scope const* scope, std::span<dcc::ast::TemplateParam const> tparams,
                                             std::string_view via, Project& project)
        {
            std::string raw = path_text(path);
            if (path.is_simple())
                for (auto const& tp : tparams)
                    if (tp.name == path.simple_name())
                        return to_tparam(&tp, raw, via);

            if (scope)
                if (auto const* sym = dcc::sema::resolve_type_path(*scope, path))
                    if (sym->decl)
                        return to_decl(sym->decl, raw, via);

            return unresolved(raw, via, project);
        }

        [[nodiscard]] static std::vector<std::string> split_path(std::string_view s)
        {
            std::vector<std::string> out;
            std::size_t i = 0;
            while (i <= s.size())
            {
                auto j = s.find("::", i);
                if (j == std::string_view::npos)
                {
                    out.emplace_back(s.substr(i));
                    break;
                }
                out.emplace_back(s.substr(i, j - i));
                i = j + 2;
            }
            return out;
        }

        [[nodiscard]] CrossRef resolve_doc_path(std::string_view path_str, dcc::sema::ModuleInfo const& mod, std::span<dcc::ast::TemplateParam const> tparams,
                                                std::span<dcc::sema::ModuleInfo const* const> others, Project& project)
        {
            std::string raw{path_str};
            auto segs = split_path(path_str);
            if (segs.empty() || segs.front().empty())
                return unresolved(raw, "DocLink", project);

            auto const* own = mod.own_scope;
            auto const* exp = mod.export_scope;
            std::pmr::monotonic_buffer_resource pool;

            dcc::ast::Path apath{&pool};
            for (auto const& s : segs)
            {
                std::string_view name = m_interner ? m_interner->intern(s) : std::string_view{s};
                apath.segments.push_back({name, dcc::sm::SourceRange{}});
            }

            if (segs.size() == 1)
            {
                for (auto const& tp : tparams)
                {
                    if (tp.name == segs.front())
                        return to_tparam(&tp, raw, "DocLink");
                }
            }

            for (auto const* scope : {own, exp})
            {
                if (!scope)
                    continue;

                if (auto const* sym = dcc::sema::resolve_type_path(*scope, apath))
                {
                    if (sym->decl)
                    {
                        CrossRef r = to_decl(sym->decl, raw, "DocLink");
                        r.ambiguous = sym->is_ambiguous;
                        return r;
                    }
                }

                auto overloads = dcc::sema::resolve_value_overloads(*scope, apath);
                if (!overloads.empty() && overloads.front().decl)
                {
                    CrossRef r = to_decl(overloads.front().decl, raw, "DocLink");
                    r.ambiguous = overloads.size() > 1;
                    return r;
                }
            }

            std::vector<std::pair<std::string, dcc::ast::Decl const*>> hits;
            for (auto const* other : others)
            {
                if (other == &mod || !other->export_scope)
                    continue;

                if (auto const* sym = dcc::sema::resolve_type_path(*other->export_scope, apath))
                {
                    if (sym->decl)
                        hits.emplace_back(other->canonical_path.str(), sym->decl);
                }
                else
                {
                    auto overloads = dcc::sema::resolve_value_overloads(*other->export_scope, apath);
                    if (!overloads.empty() && overloads.front().decl)
                        hits.emplace_back(other->canonical_path.str(), overloads.front().decl);
                }
            }
            std::ranges::sort(hits, {}, &std::pair<std::string, dcc::ast::Decl const*>::first);
            if (!hits.empty())
            {
                CrossRef r = to_decl(hits.front().second, raw, "DocLink");
                r.ambiguous = hits.size() > 1;
                return r;
            }
            return unresolved(raw, "DocLink", project);
        }

        [[nodiscard]] static dcc::sm::Location decl_begin(dcc::ast::Decl const* d) noexcept
        {
            if (!d)
                return {};

            using K = dcc::ast::DeclKind;
            auto named = [](dcc::sm::SourceRange nr, dcc::sm::SourceRange r) { return (nr.valid() ? nr : r).begin; };
            switch (d->kind)
            {
                case K::Func: {
                    auto const* f = static_cast<dcc::ast::FuncDecl const*>(d);
                    return named(f->name_range, f->range);
                }
                case K::Struct: {
                    auto const* s = static_cast<dcc::ast::StructDecl const*>(d);
                    return named(s->name_range, s->range);
                }
                case K::Union: {
                    auto const* u = static_cast<dcc::ast::UnionDecl const*>(d);
                    return named(u->name_range, u->range);
                }
                case K::Enum: {
                    auto const* e = static_cast<dcc::ast::EnumDecl const*>(d);
                    return named(e->name_range, e->range);
                }
                case K::Var: {
                    auto const* v = static_cast<dcc::ast::VarDecl const*>(d);
                    return named(v->name_range, v->range);
                }
                case K::Using: {
                    auto const* u = static_cast<dcc::ast::UsingDecl const*>(d);
                    return named(u->name_range, u->range);
                }
                default:
                    return d->range.begin;
            }
        }

        [[nodiscard]] static std::string decl_simple_name(dcc::ast::Decl const* d)
        {
            using K = dcc::ast::DeclKind;
            switch (d->kind)
            {
                case K::Func:
                    return std::string{static_cast<dcc::ast::FuncDecl const*>(d)->name};
                case K::Struct:
                    return std::string{static_cast<dcc::ast::StructDecl const*>(d)->name};
                case K::Union:
                    return std::string{static_cast<dcc::ast::UnionDecl const*>(d)->name};
                case K::Enum:
                    return std::string{static_cast<dcc::ast::EnumDecl const*>(d)->name};
                case K::Var:
                    return std::string{static_cast<dcc::ast::VarDecl const*>(d)->name};
                case K::Using: {
                    auto const* u = static_cast<dcc::ast::UsingDecl const*>(d);
                    if (!u->alias_path.segments.empty())
                        return std::string{u->alias_path.segments.back().name};
                    return "using";
                }
                default:
                    return "decl";
            }
        }

        [[nodiscard]] static std::string decl_kind_name(dcc::ast::Decl const* d)
        {
            using K = dcc::ast::DeclKind;
            switch (d->kind)
            {
                case K::Func:
                    return "fn";
                case K::Struct:
                    return "struct";
                case K::Union:
                    return "union";
                case K::Enum:
                    return "enum";
                case K::Var:
                    return "var";
                case K::Using: {
                    auto const* u = static_cast<dcc::ast::UsingDecl const*>(d);
                    if (u->using_kind == dcc::ast::UsingKind::ValueAlias)
                        return "valuealias";
                    if (u->using_kind == dcc::ast::UsingKind::Concept)
                        return "concept";
                    return "alias";
                }
                default:
                    return "decl";
            }
        }

        [[nodiscard]] static std::string pub_prefix(dcc::ast::Decl const* d) { return d && d->is_public ? "public " : ""; }

        struct TparamText
        {
            std::string text;
            std::vector<CrossRef> refs;
        };

        [[nodiscard]] TparamText render_tparam(dcc::ast::TemplateParam const& tp, dcc::sema::Scope const* scope,
                                               std::span<dcc::ast::TemplateParam const> tparams, Project& project)
        {
            TparamText out;
            std::string head;
            if (tp.value_type)
            {
                TypeRef vt = render_type(tp.value_type, scope, tparams, "TemplateArg", project);
                head = vt.text + " " + std::string{tp.name};
                out.refs = std::move(vt.refs);
            }
            else
                head = std::string{tp.name};
            if (tp.is_pack)
                head += "...";
            if (tp.value_type)
            {
                if (tp.default_value)
                    head += " = " + const_expr_text(tp.default_value);
            }
            else if (tp.default_type)
            {
                TypeRef dt = render_type(tp.default_type, scope, tparams, "TemplateArg", project);
                head += " = " + dt.text;
                out.refs.insert(out.refs.end(), dt.refs.begin(), dt.refs.end());
            }
            out.text = std::move(head);
            return out;
        }

        [[nodiscard]] static bool eligible_kind(dcc::ast::Decl const* d) noexcept
        {
            using K = dcc::ast::DeclKind;
            switch (d->kind)
            {
                case K::Func:
                case K::Struct:
                case K::Union:
                case K::Enum:
                case K::Var:
                    return true;
                case K::Using: {
                    auto k = static_cast<dcc::ast::UsingDecl const*>(d)->using_kind;
                    return k == dcc::ast::UsingKind::Alias || k == dcc::ast::UsingKind::ValueAlias || k == dcc::ast::UsingKind::Concept;
                }
                default:
                    return false;
            }
        }

        void preregister(dcc::ast::Decl const* d, std::string const& mod_id, dcc::sema::Scope const* scope, Project& project)
        {
            if (!d || !eligible_kind(d))
                return;

            std::string name = decl_simple_name(d);
            std::string id = mod_id + "::" + name;
            std::span<dcc::ast::TemplateParam const> tps;
            if (d->kind == dcc::ast::DeclKind::Func)
            {
                auto const* f = static_cast<dcc::ast::FuncDecl const*>(d);
                tps = f->template_params;
                std::vector<std::string> ptypes;
                for (auto const& fp : f->params)
                {
                    TypeRef tr = render_type(fp.type, scope, tps, "ParamType", project, false);
                    ptypes.push_back(tr.text);
                }
                id += "#fn" + fn_suffix(ptypes);
            }
            else if (d->kind == dcc::ast::DeclKind::Struct)
            {
                auto const* s = static_cast<dcc::ast::StructDecl const*>(d);
                tps = s->template_params;
                id += "#struct";
            }
            else if (d->kind == dcc::ast::DeclKind::Union)
            {
                id += "#union";
            }
            else if (d->kind == dcc::ast::DeclKind::Enum)
            {
                auto const* e = static_cast<dcc::ast::EnumDecl const*>(d);
                tps = e->template_params;
                id += "#enum";
            }
            else if (d->kind == dcc::ast::DeclKind::Var)
            {
                id += "#var";
            }
            else if (d->kind == dcc::ast::DeclKind::Using)
            {
                auto const* u = static_cast<dcc::ast::UsingDecl const*>(d);
                tps = u->template_params;
                std::string kind = decl_kind_name(d);
                id += (kind == "valuealias" ? "#valuealias" : (kind == "concept" ? "#concept" : "#alias"));
            }

            m_decl_ids[d] = id;
            std::string owner_path = mod_id + "::" + name;
            for (auto const& tp : tps)
                m_tparam_ids[&tp] = owner_path + "::" + std::string{tp.name} + "#tparam";
        }

        std::size_t add_item(Project& project, std::string id)
        {
            std::size_t idx = project.items.size();
            project.items.push_back(Item{});
            project.items.back().id = std::move(id);
            return idx;
        }

        void attach_doc_links(Item& item, dcc::sema::ModuleInfo const& mod, std::span<dcc::ast::TemplateParam const> tparams,
                              std::span<dcc::sema::ModuleInfo const* const> others, Project& project)
        {
            if (item.doc.empty())
                return;

            for (auto const& link : scan_doc_links(item.doc))
            {
                CrossRef r = resolve_doc_path(link.path, mod, tparams, others, project);
                r.raw = link.path;
                r.display = link.display;
                r.start = link.start;
                r.length = link.length;
                item.doc_refs.push_back(std::move(r));
            }
        }

        void itemize_tparams(std::span<dcc::ast::TemplateParam const> tps, dcc::sema::ModuleInfo const& mod, std::string const& parent_id,
                             std::string const& parent_path, bool is_public, Project& project, std::span<dcc::sema::ModuleInfo const* const> others,
                             std::vector<std::string>& rendered)
        {
            for (auto const& tp : tps)
            {
                std::string id = parent_path + "::" + std::string{tp.name} + "#tparam";
                m_tparam_ids[&tp] = id;
                Item& item = project.items[add_item(project, id)];
                item.kind = "tparam";
                item.name = std::string{tp.name};
                item.parent = parent_id;
                item.is_public = is_public;
                if (tp.doc)
                    item.doc = std::string{tp.doc->text};

                TparamText tt = render_tparam(tp, mod.own_scope, tps, project);
                item.sig.text = std::move(tt.text);
                item.sig.refs = std::move(tt.refs);

                attach_doc_links(item, mod, tps, others, project);
                rendered.push_back(id);
            }
        }

        void itemize_fparams(std::span<dcc::ast::FuncParam const> fps, dcc::sema::ModuleInfo const& mod, std::string const& parent_id,
                             std::string const& parent_path, bool is_public, std::span<dcc::ast::TemplateParam const> tps, Project& project,
                             std::span<dcc::sema::ModuleInfo const* const> others, std::vector<std::string>& rendered_types)
        {
            for (auto const& fp : fps)
            {
                std::string id = parent_path + "::" + std::string{fp.name} + "#fparam";
                Item& item = project.items[add_item(project, id)];
                item.kind = "fparam";
                item.name = std::string{fp.name};
                item.parent = parent_id;
                item.is_public = is_public;
                if (fp.doc)
                    item.doc = std::string{fp.doc->text};

                TypeRef tr = render_type(fp.type, mod.own_scope, tps, "ParamType", project);
                item.sig.text = tr.text;
                if (!fp.name.empty())
                    item.sig.text += " " + std::string{fp.name};
                item.sig.refs = std::move(tr.refs);
                rendered_types.push_back(item.sig.text);
                attach_doc_links(item, mod, tps, others, project);
            }
        }

        void itemize_decl(dcc::ast::Decl const* d, dcc::ast::TranslationUnit const& tu, dcc::sema::ModuleInfo const& mod, std::string const& mod_id,
                          std::string const& file, Project& project, std::string& out_id, std::size_t& out_idx)
        {
            out_id.clear();
            out_idx = static_cast<std::size_t>(-1);
            if (!d || !eligible_kind(d))
                return;

            bool is_public = d->is_public;
            std::string name = decl_simple_name(d);
            std::string kind = decl_kind_name(d);
            std::string id = mod_id + "::" + name;
            std::string sig_text;
            std::vector<CrossRef> sig_refs;
            std::span<dcc::ast::TemplateParam const> tparam_view;
            auto const* fscope = mod.own_scope;
            if (d->kind == dcc::ast::DeclKind::Func)
                tparam_view = static_cast<dcc::ast::FuncDecl const*>(d)->template_params;
            else if (d->kind == dcc::ast::DeclKind::Struct)
                tparam_view = static_cast<dcc::ast::StructDecl const*>(d)->template_params;
            else if (d->kind == dcc::ast::DeclKind::Enum)
                tparam_view = static_cast<dcc::ast::EnumDecl const*>(d)->template_params;
            else if (d->kind == dcc::ast::DeclKind::Using)
                tparam_view = static_cast<dcc::ast::UsingDecl const*>(d)->template_params;
            std::string pub = pub_prefix(d);
            auto render_tps = [&](std::span<dcc::ast::TemplateParam const> tps) {
                std::string list;
                for (auto const& tp : tps)
                {
                    TparamText tt = render_tparam(tp, fscope, tparam_view, project);
                    if (!list.empty())
                        list += ", ";

                    list += tt.text;
                    sig_refs.insert(sig_refs.end(), tt.refs.begin(), tt.refs.end());
                }

                return list;
            };
            if (d->kind == dcc::ast::DeclKind::Func)
            {
                auto const* f = static_cast<dcc::ast::FuncDecl const*>(d);
                std::string tp_list = render_tps(f->template_params);
                std::string plist;
                std::vector<std::string> ptypes;
                for (auto const& fp : f->params)
                {
                    TypeRef tr = render_type(fp.type, fscope, tparam_view, "ParamType", project);
                    if (!plist.empty())
                        plist += ", ";

                    plist += tr.text;
                    if (!fp.name.empty())
                    {
                        plist += " ";
                        plist += std::string{fp.name};
                    }

                    ptypes.push_back(tr.text);
                    sig_refs.insert(sig_refs.end(), tr.refs.begin(), tr.refs.end());
                }

                TypeRef rr = render_type(f->return_type, fscope, tparam_view, "ReturnType", project);
                sig_refs.insert(sig_refs.end(), rr.refs.begin(), rr.refs.end());
                id += "#fn" + fn_suffix(ptypes);
                sig_text = pub + rr.text + " " + name;
                if (!tp_list.empty())
                    sig_text += "(" + tp_list + ")";
                sig_text += "(" + plist + ")";
            }
            else if (d->kind == dcc::ast::DeclKind::Struct)
            {
                auto const* s = static_cast<dcc::ast::StructDecl const*>(d);
                std::string tp_list = render_tps(s->template_params);
                id += "#struct";
                sig_text = pub + "struct " + name;
                if (!tp_list.empty())
                    sig_text += "(" + tp_list + ")";
            }
            else if (d->kind == dcc::ast::DeclKind::Union)
            {
                id += "#union";
                sig_text = pub + "union " + name;
            }
            else if (d->kind == dcc::ast::DeclKind::Enum)
            {
                auto const* e = static_cast<dcc::ast::EnumDecl const*>(d);
                id += "#enum";
                sig_text = pub + "enum " + name;
                if (e->backing_type)
                {
                    TypeRef bt = render_type(e->backing_type, fscope, tparam_view, "FieldType", project);
                    sig_refs.insert(sig_refs.end(), bt.refs.begin(), bt.refs.end());
                    sig_text += " : " + bt.text;
                }
            }
            else if (d->kind == dcc::ast::DeclKind::Var)
            {
                auto const* v = static_cast<dcc::ast::VarDecl const*>(d);
                TypeRef tr = render_type(v->type, fscope, tparam_view, "FieldType", project);
                sig_refs = std::move(tr.refs);
                id += "#var";
                sig_text = pub + tr.text + " " + name;
                if (v->init)
                    sig_text += " = " + const_expr_text(v->init);
                sig_text += ";";
            }
            else if (d->kind == dcc::ast::DeclKind::Using)
            {
                auto const* u = static_cast<dcc::ast::UsingDecl const*>(d);
                if (kind == "concept")
                {
                    std::string tp_list = render_tps(u->template_params);
                    id += "#concept";
                    sig_text = pub + "using " + name;
                    if (!tp_list.empty())
                        sig_text += "(" + tp_list + ")";
                    if (u->target_expr)
                        sig_text += " = " + const_expr_text(u->target_expr);
                    sig_text += ";";
                }
                else if (kind == "valuealias")
                {
                    TypeRef tr = render_type(u->target_type, fscope, tparam_view, "AliasTarget", project);
                    sig_refs = std::move(tr.refs);
                    id += "#valuealias";
                    sig_text = pub + "using " + tr.text + " " + name;
                    if (u->target_expr)
                        sig_text += " = " + const_expr_text(u->target_expr);
                    sig_text += ";";
                }
                else
                {
                    TypeRef tr = render_type(u->target_type, fscope, tparam_view, "AliasTarget", project);
                    sig_refs = std::move(tr.refs);
                    id += "#alias";
                    sig_text = pub + "using " + name + " = " + tr.text + ";";
                }
            }

            m_decl_ids[d] = id;
            std::size_t idx = add_item(project, id);
            out_idx = idx;
            Item& item = project.items[idx];
            item.kind = kind;
            item.name = name;
            item.is_public = is_public;
            item.file = file;
            item.sig.text = std::move(sig_text);
            item.sig.refs = std::move(sig_refs);
            if (auto const* doc = tu.doc_for(d))
                item.doc = std::string{doc->text};

            out_id = id;
        }

        void itemize_members(dcc::ast::Decl const* d, dcc::sema::ModuleInfo const& mod, std::string const& file, std::string const& owner_id,
                             std::string const& owner_path, bool is_public, std::span<dcc::ast::TemplateParam const> tparams, Project& project,
                             std::span<dcc::sema::ModuleInfo const* const> others)
        {
            if (owner_id.empty())
                return;

            auto const* fscope = mod.own_scope;
            auto const* fields_begin = static_cast<dcc::ast::FieldDecl const*>(nullptr);
            std::size_t field_count = 0;
            if (d->kind == dcc::ast::DeclKind::Struct)
            {
                auto const& fs = static_cast<dcc::ast::StructDecl const*>(d)->fields;
                fields_begin = fs.data();
                field_count = fs.size();
            }
            else if (d->kind == dcc::ast::DeclKind::Union)
            {
                auto const& fs = static_cast<dcc::ast::UnionDecl const*>(d)->fields;
                fields_begin = fs.data();
                field_count = fs.size();
            }
            if (field_count > 0)
            {
                for (std::size_t fi = 0; fi < field_count; ++fi)
                {
                    auto const& fld = fields_begin[fi];
                    std::string id = owner_path + "::" + std::string{fld.name} + "#field";
                    Item& item = project.items[add_item(project, id)];
                    item.kind = "field";
                    item.name = std::string{fld.name};
                    item.parent = owner_id;
                    item.is_public = is_public;
                    item.file = file;
                    if (fld.doc)
                        item.doc = std::string{fld.doc->text};

                    TypeRef tr = render_type(fld.type, fscope, tparams, "FieldType", project);
                    item.sig.text = tr.text + " " + item.name + ";";
                    item.sig.refs = std::move(tr.refs);
                    attach_doc_links(item, mod, tparams, others, project);
                }
            }
            if (d->kind == dcc::ast::DeclKind::Enum)
            {
                auto const* e = static_cast<dcc::ast::EnumDecl const*>(d);
                for (auto const& v : e->variants)
                {
                    std::string id = owner_path + "::" + std::string{v.name} + "#variant";
                    Item& item = project.items[add_item(project, id)];
                    item.kind = "variant";
                    item.name = std::string{v.name};
                    item.parent = owner_id;
                    item.is_public = is_public;
                    item.file = file;
                    if (v.doc)
                        item.doc = std::string{v.doc->text};

                    std::string payload;
                    for (auto const* pt : v.payload)
                    {
                        TypeRef tr = render_type(pt, fscope, tparams, "VariantPayload", project);
                        if (!payload.empty())
                            payload += ", ";
                        payload += tr.text;
                        item.sig.refs.insert(item.sig.refs.end(), tr.refs.begin(), tr.refs.end());
                    }

                    std::string vsig = item.name;
                    if (!payload.empty())
                        vsig += "(" + payload + ")";
                    if (v.explicit_value)
                        vsig += " = " + const_expr_text(v.explicit_value);
                    item.sig.text = std::move(vsig);
                    attach_doc_links(item, mod, tparams, others, project);
                }
            }
            if (d->kind == dcc::ast::DeclKind::Func)
            {
                auto const* f = static_cast<dcc::ast::FuncDecl const*>(d);
                std::vector<std::string> rendered;
                itemize_fparams(f->params, mod, owner_id, owner_path, is_public, tparams, project, others, rendered);
            }

            std::vector<std::string> t_rendered;
            if (d->kind == dcc::ast::DeclKind::Func)
                itemize_tparams(static_cast<dcc::ast::FuncDecl const*>(d)->template_params, mod, owner_id, owner_path, is_public, project, others, t_rendered);
            else if (d->kind == dcc::ast::DeclKind::Struct)
                itemize_tparams(static_cast<dcc::ast::StructDecl const*>(d)->template_params, mod, owner_id, owner_path, is_public, project, others,
                                t_rendered);
            else if (d->kind == dcc::ast::DeclKind::Enum)
                itemize_tparams(static_cast<dcc::ast::EnumDecl const*>(d)->template_params, mod, owner_id, owner_path, is_public, project, others, t_rendered);
            else if (d->kind == dcc::ast::DeclKind::Using)
                itemize_tparams(static_cast<dcc::ast::UsingDecl const*>(d)->template_params, mod, owner_id, owner_path, is_public, project, others, t_rendered);
        }

        void assemble(dcc::session::CompilerSession& session, std::vector<dcc::sema::ModuleInfo*> const& entries, Project& project)
        {
            auto const& sm = session.source_manager();
            std::vector<dcc::sema::ModuleInfo const*> mods;
            if (auto* sema = session.sema_context())
                for (auto const& m : sema->graph().all())
                    mods.push_back(m.get());

            std::ranges::sort(mods, {}, [](dcc::sema::ModuleInfo const* m) { return m->canonical_path.str(); });
            std::error_code overview_ec;
            std::string entry_canon = std::filesystem::weakly_canonical(m_entry, overview_ec).string();
            if (overview_ec)
                entry_canon = m_entry.string();
            bool overview_set = false;
            auto entry_overview = [&](dcc::sema::ModuleInfo const* mod) {
                if (overview_set || !mod || !mod->tu)
                    return;
                std::error_code ec;
                bool is_entry = std::filesystem::equivalent(mod->file_path, m_entry, ec) && !ec;
                if (!is_entry && !mod->file_path.empty())
                {
                    std::string mod_canon = std::filesystem::weakly_canonical(mod->file_path, ec).string();
                    is_entry = !ec && mod_canon == entry_canon;
                }
                if (!is_entry)
                    return;
                if (project.entry_module.empty())
                    project.entry_module = mod->canonical_path.str();
                if (auto const* ov = mod->tu->overview_of())
                {
                    project.overview = std::string{ov->text};
                    overview_set = true;
                }
            };
            for (auto const* mod : mods)
                entry_overview(mod);
            if (!overview_set)
            {
                for (auto const* entry : entries)
                {
                    if (entry && entry->tu)
                    {
                        if (auto const* ov = entry->tu->overview_of())
                        {
                            project.overview = std::string{ov->text};
                            overview_set = true;
                            break;
                        }
                    }
                }
            }

            for (auto const* mod : mods)
            {
                if (!mod->tu)
                    continue;

                std::string mod_id = mod->canonical_path.str();
                for (auto const* d : mod->tu->decls)
                    preregister(d, mod_id, mod->own_scope, project);
            }

            for (auto const* mod : mods)
            {
                if (!mod->tu)
                    continue;

                auto const* tu = mod->tu;
                Module m;
                m.id = mod->canonical_path.str();
                m.file = mod->file_path.string();
                if (auto const* ov = tu->overview_of())
                    m.overview = std::string{ov->text};

                std::unordered_map<dcc::ast::Section const*, std::size_t> sec_index;
                for (auto const* sec : tu->sections)
                {
                    sec_index[sec] = m.sections.size();
                    m.sections.push_back({.title = std::string{sec->title}, .body = std::string{sec->body}, .items = {}});
                }

                for (auto const* d : tu->decls)
                {
                    std::string id;
                    std::size_t idx = static_cast<std::size_t>(-1);
                    itemize_decl(d, *tu, *mod, m.id, m.file, project, id, idx);
                    if (id.empty())
                        continue;

                    if (auto const* sec = tu->section_for(d))
                    {
                        auto it = sec_index.find(sec);
                        if (it != sec_index.end())
                            m.sections[it->second].items.push_back(id);
                    }

                    std::span<dcc::ast::TemplateParam const> tps;
                    if (d->kind == dcc::ast::DeclKind::Func)
                        tps = static_cast<dcc::ast::FuncDecl const*>(d)->template_params;
                    else if (d->kind == dcc::ast::DeclKind::Struct)
                        tps = static_cast<dcc::ast::StructDecl const*>(d)->template_params;
                    else if (d->kind == dcc::ast::DeclKind::Enum)
                        tps = static_cast<dcc::ast::EnumDecl const*>(d)->template_params;
                    else if (d->kind == dcc::ast::DeclKind::Using)
                        tps = static_cast<dcc::ast::UsingDecl const*>(d)->template_params;

                    std::string owner_path = m.id + "::" + decl_simple_name(d);
                    itemize_members(d, *mod, m.file, id, owner_path, d->is_public, tps, project, mods);
                    Item& top = project.items[idx];
                    top.line = line_of(sm, decl_begin(d));
                    attach_doc_links(top, *mod, tps, mods, project);
                }
                m.sections.erase(
                    std::remove_if(m.sections.begin(), m.sections.end(), [](Section const& s) { return s.title.empty() && s.body.empty() && s.items.empty(); }),
                    m.sections.end());

                project.modules.push_back(std::move(m));
            }
            std::ranges::sort(project.misses);
        }
    };

    [[nodiscard]] Project build_project(std::filesystem::path entry, std::vector<std::filesystem::path> roots = {})
    {
        Builder b{std::move(entry), std::move(roots)};
        return b.build();
    }

} // namespace dcdoc
