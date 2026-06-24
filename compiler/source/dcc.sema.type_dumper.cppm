export module dcc.sema.type_dumper;

import std;
import dcc.ast;
import dcc.types;
import dcc.sema.scope;
import dcc.sema.type_helpers;

export namespace dcc::sema
{
    class TypeDumper
    {
    public:
        [[nodiscard]] static std::string dump(ModuleInfo const& mod)
        {
            TypeDumper d;
            d.line_fmt("Module {}", mod.canonical_path.str());
            d.m_indent++;
            if (mod.tu)
                d.print_tu(*mod.tu);

            return d.m_out;
        }

    private:
        std::string m_out;
        std::size_t m_indent{};

        void pad() { m_out.append(m_indent * 2, ' '); }

        void line(std::string_view s)
        {
            pad();
            m_out += s;
            m_out += '\n';
        }

        template <typename... A> void line_fmt(std::format_string<A...> fmt, A&&... args)
        {
            pad();
            std::format_to(std::back_inserter(m_out), fmt, std::forward<A>(args)...);
            m_out += '\n';
        }

        void print_tu(ast::TranslationUnit const& tu)
        {
            for (auto* d : tu.decls)
                print_decl(*d);
        }

        void print_decl(ast::Decl const& d)
        {
            switch (d.kind)
            {
                case ast::DeclKind::Struct:
                    print_struct(static_cast<ast::StructDecl const&>(d));
                    break;
                case ast::DeclKind::Union:
                    print_union(static_cast<ast::UnionDecl const&>(d));
                    break;
                case ast::DeclKind::Enum:
                    print_enum(static_cast<ast::EnumDecl const&>(d));
                    break;
                case ast::DeclKind::Func:
                    print_func(static_cast<ast::FuncDecl const&>(d));
                    break;
                case ast::DeclKind::Var:
                    print_var(static_cast<ast::VarDecl const&>(d));
                    break;
                case ast::DeclKind::Using:
                    print_using(static_cast<ast::UsingDecl const&>(d));
                    break;
                case ast::DeclKind::Module:
                case ast::DeclKind::Import:
                case ast::DeclKind::StaticIfGroup:
                    break;
            }
        }

        void print_template_params(std::pmr::vector<ast::TemplateParam> const& params)
        {
            if (params.empty())
                return;

            std::string joined;
            for (std::size_t i = 0; i < params.size(); ++i)
            {
                if (i > 0)
                    joined += ", ";

                joined += params[i].name;
                if (params[i].value_type)
                {
                    joined += ": ";
                    joined += type_str(params[i].value_type->sema.canonical);
                }
            }
            line_fmt("TemplateParams {}", joined);
        }

        void print_struct(ast::StructDecl const& d)
        {
            line_fmt("Struct {}", d.name);
            m_indent++;
            print_template_params(d.template_params);
            for (auto const& f : d.fields)
                line_fmt("Field {}: {}", f.name, type_str(f.type ? f.type->sema.canonical : nullptr));

            m_indent--;
        }

        void print_union(ast::UnionDecl const& d)
        {
            line_fmt("Union {}", d.name);
            m_indent++;
            for (auto const& f : d.fields)
                line_fmt("Field {}: {}", f.name, type_str(f.type ? f.type->sema.canonical : nullptr));

            m_indent--;
        }

        void print_enum(ast::EnumDecl const& d)
        {
            line_fmt("Enum {}", d.name);
            m_indent++;
            print_template_params(d.template_params);
            if (d.backing_type)
                line_fmt("Backing: {}", type_str(d.backing_type->sema.canonical));

            for (auto const& v : d.variants)
                if (!v.payload.empty())
                {
                    std::string joined;
                    for (std::size_t i = 0; i < v.payload.size(); ++i)
                    {
                        if (i > 0)
                            joined += ", ";

                        joined += type_str(v.payload[i] ? v.payload[i]->sema.canonical : nullptr);
                    }
                    line_fmt("Variant {}({})", v.name, joined);
                }
                else
                    line_fmt("Variant {}", v.name);
            m_indent--;
        }

        void print_func(ast::FuncDecl const& d)
        {
            line_fmt("Func {}", d.name);
            m_indent++;
            print_template_params(d.template_params);
            line_fmt("Return: {}", type_str(d.return_type ? d.return_type->sema.canonical : nullptr));
            for (auto const& p : d.params)
                line_fmt("Param {}: {}", p.name, type_str(p.type ? p.type->sema.canonical : nullptr));

            m_indent--;
        }

        void print_var(ast::VarDecl const& d) { line_fmt("Var {}: {}", d.name, type_str(d.type ? d.type->sema.canonical : nullptr)); }

        void print_using(ast::UsingDecl const& d)
        {
            line_fmt("Using {}", d.alias_path.is_empty() ? std::string_view{"<anon>"} : d.alias_path.segments.back().name);
            m_indent++;
            print_template_params(d.template_params);
            switch (d.using_kind)
            {
                case ast::UsingKind::Alias:
                    line_fmt("Alias: {}", type_str(d.target_type ? d.target_type->sema.canonical : nullptr));
                    break;
                case ast::UsingKind::Concept:
                    if (auto* c = ast::node_cast<ast::CompilesExpr>(d.target_expr))
                        for (auto const& p : c->params)
                            line_fmt("CompilesParam {}: {}", p.name, type_str(p.type ? p.type->sema.canonical : nullptr));
                    break;
                case ast::UsingKind::BareImport:
                case ast::UsingKind::Wildcard:
                case ast::UsingKind::List:
                    break;
            }
            m_indent--;
        }

        [[nodiscard]] std::string type_str(void const* t) const { return format_dcc_type(reinterpret_cast<types::TypePtr>(t)); }

        [[nodiscard]] static std::string decl_name(void const* d)
        {
            if (!d)
                return "<null>";

            auto const* dd = reinterpret_cast<ast::Decl const*>(d);

            switch (dd->kind)
            {
                case ast::DeclKind::Struct:
                    return std::string{static_cast<ast::StructDecl const*>(dd)->name};
                case ast::DeclKind::Union:
                    return std::string{static_cast<ast::UnionDecl const*>(dd)->name};
                case ast::DeclKind::Enum:
                    return std::string{static_cast<ast::EnumDecl const*>(dd)->name};
                case ast::DeclKind::Func:
                    return std::string{static_cast<ast::FuncDecl const*>(dd)->name};
                case ast::DeclKind::Var:
                    return std::string{static_cast<ast::VarDecl const*>(dd)->name};
                case ast::DeclKind::Using:
                    return static_cast<ast::UsingDecl const*>(dd)->alias_path.is_empty()
                               ? std::string{"<using>"}
                               : std::string{static_cast<ast::UsingDecl const*>(dd)->alias_path.segments.back().name};
                case ast::DeclKind::Module:
                    return "module";
                case ast::DeclKind::Import:
                    return "import";
                case ast::DeclKind::StaticIfGroup:
                    return "static if";
            }
            return "<decl>";
        }
    };

} // namespace dcc::sema
