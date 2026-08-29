export module dcc.sema.scope_dumper;

import std;
import dcc.sema.scope;

export namespace dcc::sema
{
    class ScopeDumper
    {
    public:
        [[nodiscard]] static std::string dump(ModuleInfo const& mod)
        {
            ScopeDumper d;
            d.print_module(mod);
            return d.m_out;
        }

        [[nodiscard]] static std::string dump_scope(Scope const& scope, std::string_view label = "Scope")
        {
            ScopeDumper d;
            d.line_fmt("{}", label);
            d.m_indent++;
            d.print_scope(scope);
            return d.m_out;
        }

    private:
        std::string m_out;
        std::size_t m_indent = 0;

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

        static std::string_view kind_str(SymbolKind k) noexcept
        {
            switch (k)
            {
                case SymbolKind::Struct:
                    return "struct";
                case SymbolKind::Union:
                    return "union";
                case SymbolKind::Enum:
                    return "enum";
                case SymbolKind::TypeAlias:
                    return "alias";
                case SymbolKind::TemplateParam:
                    return "template_param";
                case SymbolKind::Function:
                    return "function";
                case SymbolKind::Variable:
                    return "variable";
                case SymbolKind::EnumVariant:
                    return "variant";
                case SymbolKind::ValueAlias:
                    return "value_alias";
                case SymbolKind::Module:
                    return "module";
                case SymbolKind::UsingGroup:
                    return "group";
            }
            return "?";
        }

        std::string flags_str(Symbol const& s) const
        {
            std::string r;
            if (s.is_exported)
                r += " exported";
            if (s.is_spilled)
                r += " spilled";
            if (s.via_using)
                r += " via-using";
            if (s.is_ambiguous)
                r += " ambiguous";
            return r;
        }

        void print_module(ModuleInfo const& mod)
        {
            line_fmt("Module {}", mod.canonical_path.str());
            ++m_indent;

            line("OwnScope");
            ++m_indent;
            if (mod.own_scope)
                print_scope(*mod.own_scope);
            else
                line("<unset>");
            --m_indent;

            line("ExportScope");
            ++m_indent;
            if (mod.export_scope)
                print_scope(*mod.export_scope);
            else
                line("<unset>");
            --m_indent;

            --m_indent;
        }

        void print_scope(Scope const& scope)
        {
            std::vector<std::string_view> keys;
            keys.reserve(scope.bindings().size());
            for (auto const& [name, _] : scope.bindings())
                keys.push_back(name);

            std::ranges::sort(keys);

            for (auto key : keys)
            {
                auto const& b = scope.bindings().at(key);
                line_fmt("Binding {}", key);
                ++m_indent;

                if (b.has_type)
                    line_fmt("Type: {}{}", kind_str(b.type_sym.kind), flags_str(b.type_sym));

                for (auto const& v : b.value_syms)
                    line_fmt("Value: {}{}", kind_str(v.kind), flags_str(v));

                if (b.has_namespace)
                {
                    line_fmt("Namespace: {}{}", kind_str(b.namespace_sym.kind), flags_str(b.namespace_sym));

                    if (b.namespace_scope && b.namespace_scope->parent() == &scope)
                    {
                        ++m_indent;
                        print_scope(*b.namespace_scope);
                        --m_indent;
                    }
                    else if (b.namespace_scope)
                    {
                        ++m_indent;
                        line("(foreign-scope)");
                        --m_indent;
                    }
                }
                --m_indent;
            }
        }
    };

} // namespace dcc::sema
