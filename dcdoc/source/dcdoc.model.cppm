export module dcdoc.model;

import std;

export namespace dcdoc
{
    struct CrossRef
    {
        std::string target;
        bool resolved{false};
        std::string via;
        bool ambiguous{false};
        std::string raw;
        std::string display;
        std::size_t start{std::size_t(-1)};
        std::size_t length{};
    };

    struct Signature
    {
        std::string text;
        std::vector<CrossRef> refs;
    };

    struct Item
    {
        std::string id;
        std::string kind;
        std::string name;
        std::string parent;
        std::string doc;
        Signature sig;
        std::string file;
        std::uint32_t line{};
        bool is_public{false};
        bool public_surface{true};
        std::vector<CrossRef> doc_refs;
    };

    struct Section
    {
        std::string title;
        std::string body;
        std::vector<std::string> items;
    };

    struct Module
    {
        std::string id;
        std::string file;
        std::string overview;
        std::vector<Section> sections;
    };

    struct FileError
    {
        std::string file;
        std::string message;
    };

    struct Project
    {
        std::string entry_module;
        std::string overview;
        std::vector<Module> modules;
        std::vector<Item> items;
        std::vector<FileError> file_errors;
        std::vector<std::string> misses;
    };

    struct DocLink
    {
        std::string display;
        std::string path;
        std::size_t start{};
        std::size_t length{};
    };

    [[nodiscard]] std::string json_escape(std::string_view s)
    {
        constexpr char kQuote = 34;
        constexpr char kBackslash = 92;
        constexpr char kNewline = 10;
        constexpr char kReturn = 13;
        constexpr char kTab = 9;
        std::string out;
        for (char c : s)
        {
            switch (c)
            {
                case kQuote:
                    out += "\\\"";
                    break;
                case kBackslash:
                    out += "\\\\";
                    break;
                case kNewline:
                    out += "\\n";
                    break;
                case kReturn:
                    out += "\\r";
                    break;
                case kTab:
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                    else
                        out += c;
            }
        }
        return out;
    }

    [[nodiscard]] std::string json_string(std::string_view s)
    {
        constexpr char kQuote = 34;
        std::string out;
        out += kQuote;
        out += json_escape(s);
        out += kQuote;
        return out;
    }

    [[nodiscard]] std::string dump_ref(CrossRef const& r)
    {
        return std::format("target:{} resolved:{} via:{} ambiguous:{} raw:{} display:{} start:{} length:{}", json_string(r.target), r.resolved,
                           json_string(r.via), r.ambiguous, json_string(r.raw), json_string(r.display), r.start, r.length);
    }

    [[nodiscard]] std::string dump_refs(std::vector<CrossRef> const& refs)
    {
        std::string out;
        out += '[';
        bool first = true;
        for (auto const& r : refs)
        {
            if (!first)
                out += ',';
            first = false;
            out += dump_ref(r);
        }

        out += ']';
        return out;
    }

    [[nodiscard]] std::string dump_item(Item const& it)
    {
        std::string out = "id:";
        out += json_string(it.id);
        out += " kind:";
        out += json_string(it.kind);
        out += " name:";
        out += json_string(it.name);
        out += " parent:";
        out += json_string(it.parent);
        out += " doc:";
        out += json_string(it.doc);
        out += " signature:";
        out += json_string(it.sig.text);
        out += " sig_refs:";
        out += dump_refs(it.sig.refs);
        out += " doc_refs:";
        out += dump_refs(it.doc_refs);
        return out;
    }

    [[nodiscard]] std::string dump(Project const& p)
    {
        std::string out = "entry:";
        out += json_string(p.entry_module);
        out += "\noverview:";
        out += json_string(p.overview);
        out += "\nmodules:\n";
        for (auto const& m : p.modules)
        {
            out += "- module ";
            out += json_string(m.id);
            out += " file:";
            out += json_string(m.file);
            out += " overview:";
            out += json_string(m.overview);
            out += "\n";
            for (auto const& s : m.sections)
            {
                out += "  section ";
                out += json_string(s.title);
                out += " body:";
                out += json_string(s.body);
                out += " items:";
                bool first = true;
                for (auto const& id : s.items)
                {
                    if (!first)
                        out += ',';
                    first = false;
                    out += json_string(id);
                }
                out += "\n";
            }
        }
        out += "items:\n";
        for (auto const& it : p.items)
        {
            out += "- ";
            out += dump_item(it);
            out += "\n";
        }
        out += "misses:";
        bool first = true;
        for (auto const& m : p.misses)
        {
            if (!first)
                out += ',';
            first = false;
            out += json_string(m);
        }
        out += "\nerrors:\n";
        for (auto const& e : p.file_errors)
        {
            out += "- ";
            out += json_string(e.file);
            out += " message:";
            out += json_string(e.message);
            out += "\n";
        }
        return out;
    }

    [[nodiscard]] std::vector<DocLink> scan_doc_links(std::string_view text)
    {
        constexpr char lb = '[';
        constexpr char rb = ']';
        constexpr char bar = static_cast<char>(124);
        constexpr char tick = static_cast<char>(96);
        std::vector<DocLink> out;
        std::size_t i = 0;
        while (i < text.size())
        {
            if (text[i] != lb)
            {
                ++i;
                continue;
            }
            std::size_t open = i++;
            if (i < text.size() && text[i] == tick)
            {
                ++i;
                std::size_t ps = i;
                while (i < text.size() && text[i] != tick)
                    ++i;
                if (i + 1 >= text.size() || text[i] != tick || text[i + 1] != rb)
                    continue;
                std::string_view path = text.substr(ps, i - ps);
                if (path.empty())
                    continue;
                out.push_back({.display = std::string{path}, .path = std::string{path}, .start = open, .length = i + 2 - open});
                i += 2;
                continue;
            }
            std::size_t ds = i;
            while (i < text.size() && text[i] != bar && text[i] != rb)
                ++i;
            if (i >= text.size() || text[i] != bar)
                continue;
            std::string_view display = text.substr(ds, i - ds);
            ++i;
            if (i >= text.size() || text[i] != tick)
                continue;
            ++i;
            std::size_t ps = i;
            while (i < text.size() && text[i] != tick)
                ++i;
            if (i + 1 >= text.size() || text[i] != tick || text[i + 1] != rb)
                continue;
            std::string_view path = text.substr(ps, i - ps);
            if (path.empty() || display.empty())
                continue;
            out.push_back({.display = std::string{display}, .path = std::string{path}, .start = open, .length = i + 2 - open});
            i += 2;
        }
        return out;
    }

    [[nodiscard]] std::string fn_suffix(std::vector<std::string> const& param_types)
    {
        std::string s;
        s += '(';
        for (std::size_t i = 0; i < param_types.size(); ++i)
        {
            if (i > 0)
                s += ',';
            s += param_types[i];
        }
        s += ')';
        return s;
    }

} // namespace dcdoc
