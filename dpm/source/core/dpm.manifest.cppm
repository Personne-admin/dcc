export module dpm.manifest;

import std;
import dcc.sm;
import dcc.si;
import dcc.diag;
import dcc.ast;
import dcc.sema;
import dcc.session;
import dcc.types;
import dcc.comptime;
import dcc.ctfe;

export namespace dpm::manifest
{
    struct SpikeResult
    {
        bool ok{false};
        std::string tree;
        std::vector<std::string> errors;
    };

    inline constexpr std::string_view k_spike_header = "module dpm_manifest; public import dpm::v1; public const dpm::v1::Project __manifest = ";
    inline constexpr std::string_view k_spike_virtual_path = "/dpm-virtual/project.dcon";

    namespace detail
    {
        inline void dump_value(dcc::comptime::Value const& value, dcc::types::Type const* declared, int depth, std::string& out);

        inline dcc::types::Type const* canonical_of(dcc::ast::TypeExpr const* type)
        {
            return type ? reinterpret_cast<dcc::types::Type const*>(type->sema.canonical) : nullptr;
        }

        inline std::string indent(int depth)
        {
            return std::string(static_cast<std::size_t>(depth) * 2, ' ');
        }

        inline std::string quote(std::string_view s)
        {
            std::string r = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                    r += '\\';
                r += c;
            }
            r += '"';
            return r;
        }

        inline void dump_struct(dcc::comptime::Value const& value, dcc::types::StructType const& st, int depth, std::string& out)
        {
            auto const* decl = reinterpret_cast<dcc::ast::StructDecl const*>(st.decl);
            out += "{\n";
            std::size_t slot = 0;
            for (auto const& field : decl->fields)
            {
                if (field.name.empty())
                    continue;
                if (slot >= value.size())
                    break;
                out += indent(depth + 1);
                out += field.name;
                out += " = ";
                dump_value(value.at(slot), canonical_of(field.type), depth + 1, out);
                out += ",\n";
                ++slot;
            }
            out += indent(depth);
            out += "}";
        }

        inline void dump_value(dcc::comptime::Value const& value, dcc::types::Type const* declared, int depth, std::string& out)
        {
            auto const* type = declared ? declared : value.type;
            using Kind = dcc::comptime::Value::Kind;
            switch (value.kind())
            {
                case Kind::Int: {
                    if (auto const* et = dcc::types::type_cast<dcc::types::EnumType>(type))
                    {
                        auto const* decl = reinterpret_cast<dcc::ast::EnumDecl const*>(et->decl);
                        for (auto const& variant : decl->variants)
                            if (variant.discriminant == value.get_int())
                            {
                                out += decl->name;
                                out += "::";
                                out += variant.name;
                                return;
                            }
                    }
                    out += std::to_string(value.get_int());
                    return;
                }
                case Kind::Bool:
                    out += value.get_bool() ? "true" : "false";
                    return;
                case Kind::String:
                    out += quote(value.get_string());
                    return;
                case Kind::Aggregate: {
                    if (auto const* st = dcc::types::type_cast<dcc::types::StructType>(type))
                    {
                        dump_struct(value, *st, depth, out);
                        return;
                    }
                    [[fallthrough]];
                }
                case Kind::Slice: {
                    dcc::types::Type const* element = nullptr;
                    if (auto const* slice = dcc::types::type_cast<dcc::types::SliceType>(type))
                        element = slice->element;
                    out += "[";
                    for (std::size_t i = 0; i < value.size(); ++i)
                    {
                        out += "\n";
                        out += indent(depth + 1);
                        dump_value(value.at(i), element, depth + 1, out);
                        out += ",";
                    }
                    if (value.size() != 0)
                    {
                        out += "\n";
                        out += indent(depth);
                    }
                    out += "]";
                    return;
                }
                default:
                    out += "<unsupported>";
                    return;
            }
        }
    } // namespace detail

    inline SpikeResult spike_evaluate(std::string_view text, std::filesystem::path const& schema_root)
    {
        SpikeResult result;
        dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};

        std::string source;
        source.append(k_spike_header);
        source.append(text);
        source.append(";\n");

        std::string uri = "file://" + std::string{k_spike_virtual_path};
        (void)session.open_in_memory(uri, std::move(source));

        dcc::session::CompileOptions options;
        options.import_roots.push_back(schema_root);
        auto compiled = session.analyze_entry(std::filesystem::path{k_spike_virtual_path}, options);

        auto const& sm = session.source_manager();
        for (auto const& diagnostic : session.diagnostics().diagnostics())
        {
            if (diagnostic.severity() != dcc::diag::Severity::Error)
                continue;
            std::string where;
            if (!diagnostic.labels().empty())
            {
                auto const& range = diagnostic.labels().front().range;
                if (auto lc = sm.line_col(range.begin))
                {
                    auto column = lc->column;
                    if (lc->line == 1 && column > k_spike_header.size())
                        column -= static_cast<std::uint32_t>(k_spike_header.size());
                    where = std::format("{}:{}: ", lc->line, column);
                }
            }
            result.errors.push_back(where + diagnostic.message());
        }
        if (compiled.has_errors || !compiled.module || !compiled.module->tu)
            return result;

        dcc::ast::VarDecl const* var = nullptr;
        for (auto* decl : compiled.module->tu->decls)
            if (auto* v = dcc::ast::node_cast<dcc::ast::VarDecl>(decl); v && v->name == "__manifest")
                var = v;
        if (!var || !var->init)
        {
            result.errors.push_back("manifest declaration not found");
            return result;
        }

        dcc::ctfe::Context context;
        context.call_site = var->init->range;
        context.types = &session.sema_context()->types();
        context.source_manager = &session.source_manager();
        context.step_limit = 10000;
        context.memory_limit = 1 << 20;
        auto evaluated = dcc::ctfe::Evaluator(std::move(context), dcc::ctfe::Mode::Required).evaluate(*var->init);
        if (evaluated.failed() || !evaluated.value)
        {
            result.errors.push_back("evaluation failed: " + evaluated.message);
            return result;
        }

        detail::dump_value(*evaluated.value, detail::canonical_of(var->type), 0, result.tree);
        result.tree += "\n";
        result.ok = true;
        return result;
    }
} // namespace dpm::manifest
