module;

#ifndef DCC_VERSION
#define DCC_VERSION "unknown"
#endif

export module dccd.dcon;

import std;
import dcc.sm;
import dccd.protocol;
import dpm.analyze;
import dpm.schema;

export namespace dccd::dcon
{
    [[nodiscard]] inline bool is_dcon_uri(std::string_view uri)
    {
        auto end = uri.find_first_of("?#");
        if (end != std::string_view::npos)
            uri = uri.substr(0, end);
        return uri.ends_with(".dcon");
    }

    [[nodiscard]] inline std::vector<std::size_t> line_starts(std::string_view text)
    {
        std::vector<std::size_t> starts{0};
        for (std::size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n')
                starts.push_back(i + 1);
        return starts;
    }

    [[nodiscard]] inline protocol::LspPosition position_of(std::string_view text, std::vector<std::size_t> const& starts, std::size_t offset)
    {
        offset = std::min(offset, text.size());
        auto it = std::upper_bound(starts.begin(), starts.end(), offset);
        auto line = static_cast<std::size_t>(it - starts.begin()) - 1;
        std::uint32_t units = 0;
        std::size_t i = starts[line];
        while (i < offset)
        {
            auto lead = static_cast<unsigned char>(text[i]);
            std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
            units += length == 4 ? 2 : 1;
            i += length;
        }
        return {static_cast<std::uint32_t>(line), units};
    }

    [[nodiscard]] inline std::size_t offset_of(std::string_view text, std::vector<std::size_t> const& starts, protocol::LspPosition position)
    {
        if (position.line >= starts.size())
            return text.size();
        std::size_t i = starts[position.line];
        std::uint32_t units = 0;
        while (i < text.size() && text[i] != '\n' && units < position.character)
        {
            auto lead = static_cast<unsigned char>(text[i]);
            std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
            units += length == 4 ? 2 : 1;
            i += length;
        }
        return std::min(i, text.size());
    }

    [[nodiscard]] inline protocol::LspRange range_of(std::string_view text, std::vector<std::size_t> const& starts, std::size_t begin, std::size_t end)
    {
        return {position_of(text, starts, begin), position_of(text, starts, end)};
    }

    class Analyzer
    {
    public:
        explicit Analyzer(std::filesystem::path schema_root) : schema_root_{std::move(schema_root)} {}

        void set_schema_root(std::filesystem::path root)
        {
            if (root != schema_root_)
            {
                schema_root_ = std::move(root);
                schema_.reset();
                schema_loaded_ = false;
            }
        }

        [[nodiscard]] std::filesystem::path const& schema_root() const { return schema_root_; }

        [[nodiscard]] protocol::PublishDiagnosticsParams diagnostics(std::string const& uri, std::optional<std::int64_t> version, std::string_view text,
                                                                     std::filesystem::path const& file)
        {
            protocol::PublishDiagnosticsParams params;
            params.uri = uri;
            params.version = version;
            auto options = make_options(file);
            auto starts = line_starts(text);
            if (!schema())
            {
                protocol::LspDiagnostic d;
                d.source = "dpm";
                d.severity = protocol::DiagnosticSeverity::Warning;
                d.message = std::format("the dpm schema was not found under {}; analysis of this file is unavailable", schema_root_.string());
                params.diagnostics.push_back(std::move(d));
                return params;
            }
            auto result = dpm::analyze::analyze(text, options);
            for (auto const& f : result.findings)
            {
                protocol::LspDiagnostic d;
                d.source = "dpm";
                d.message = f.message;
                d.range = range_of(text, starts, f.begin, f.end);
                switch (f.severity)
                {
                    case dpm::analyze::Severity::Error:
                        d.severity = protocol::DiagnosticSeverity::Error;
                        break;
                    case dpm::analyze::Severity::Warning:
                        d.severity = protocol::DiagnosticSeverity::Warning;
                        break;
                    case dpm::analyze::Severity::Information:
                        d.severity = protocol::DiagnosticSeverity::Information;
                        break;
                    case dpm::analyze::Severity::Hint:
                        d.severity = protocol::DiagnosticSeverity::Hint;
                        break;
                }
                params.diagnostics.push_back(std::move(d));
            }
            return params;
        }

        [[nodiscard]] std::optional<protocol::Hover> hover(std::string_view text, std::filesystem::path const& file, protocol::LspPosition position)
        {
            auto const* sch = schema();
            if (!sch)
                return std::nullopt;
            auto starts = line_starts(text);
            auto offset = static_cast<std::uint32_t>(offset_of(text, starts, position));
            auto h = dpm::analyze::hover(text, make_options(file), *sch, offset);
            if (!h)
                return std::nullopt;
            protocol::Hover out;
            out.contents.value = h->markdown;
            out.range = range_of(text, starts, h->begin, h->end);
            return out;
        }

        [[nodiscard]] protocol::CompletionList completion(std::string_view text, std::filesystem::path const& file, protocol::LspPosition position)
        {
            protocol::CompletionList out;
            auto const* sch = schema();
            if (!sch)
                return out;
            auto starts = line_starts(text);
            auto offset = static_cast<std::uint32_t>(offset_of(text, starts, position));
            auto list = dpm::analyze::complete(text, make_options(file), *sch, offset);
            auto replace = range_of(text, starts, list.begin, list.end);
            for (auto const& c : list.items)
            {
                protocol::CompletionItem item;
                item.label = c.label;
                item.detail = c.detail.empty() ? std::nullopt : std::optional<std::string>{c.detail};
                if (!c.documentation.empty())
                    item.documentation = c.documentation;
                switch (c.kind)
                {
                    case dpm::analyze::Completion::Kind::Field:
                        item.kind = protocol::CompletionItemKind::Field;
                        break;
                    case dpm::analyze::Completion::Kind::Variant:
                        item.kind = protocol::CompletionItemKind::EnumMember;
                        break;
                    case dpm::analyze::Completion::Kind::Constant:
                        item.kind = protocol::CompletionItemKind::Constant;
                        break;
                    case dpm::analyze::Completion::Kind::Alias:
                        item.kind = protocol::CompletionItemKind::Module;
                        break;
                }
                protocol::TextEdit edit;
                edit.range = replace;
                if (c.kind == dpm::analyze::Completion::Kind::Alias)
                    edit.range = range_of(text, starts, std::min<std::size_t>(list.begin + 1, offset), offset);
                edit.newText = c.insert;
                item.textEdit = std::move(edit);
                item.insertTextFormat = c.snippet ? 2 : 1;
                out.items.push_back(std::move(item));
            }
            return out;
        }

        [[nodiscard]] std::optional<protocol::LspLocation> definition(std::string_view text, std::filesystem::path const& file, protocol::LspPosition position)
        {
            auto const* sch = schema();
            if (!sch)
                return std::nullopt;
            auto starts = line_starts(text);
            auto offset = static_cast<std::uint32_t>(offset_of(text, starts, position));
            auto d = dpm::analyze::definition(text, make_options(file), *sch, offset);
            if (!d)
                return std::nullopt;
            std::ifstream in{d->file, std::ios::binary};
            std::string schema_text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            auto schema_starts = line_starts(schema_text);
            if (d->line >= schema_starts.size())
                return std::nullopt;
            auto line_begin = schema_starts[d->line];
            auto line_end = d->line + 1 < schema_starts.size() ? schema_starts[d->line + 1] : schema_text.size();
            auto line_text = std::string_view{schema_text}.substr(line_begin, line_end - line_begin);
            auto column = line_text.find(d->name);
            protocol::LspLocation loc;
            loc.uri = dcc::sm::SourceManager::to_file_uri(d->file);
            auto begin = line_begin + (column == std::string_view::npos ? 0 : column);
            auto end = begin + (column == std::string_view::npos ? 0 : d->name.size());
            loc.range = range_of(schema_text, schema_starts, begin, end);
            return loc;
        }

    private:
        [[nodiscard]] dpm::analyze::Options make_options(std::filesystem::path const& file) const
        {
            dpm::analyze::Options options;
            options.schema_root = schema_root_;
            options.file = file;
            std::string_view version{DCC_VERSION};
            if (version != "unknown")
                options.dcc_version = std::string{version};
            return options;
        }

        [[nodiscard]] dpm::schema::Schema const* schema()
        {
            if (!schema_loaded_)
            {
                schema_loaded_ = true;
                schema_ = dpm::analyze::load_schema(schema_root_);
            }
            return schema_ ? &*schema_ : nullptr;
        }

        std::filesystem::path schema_root_;
        std::optional<dpm::schema::Schema> schema_;
        bool schema_loaded_{};
    };
} // namespace dccd::dcon
