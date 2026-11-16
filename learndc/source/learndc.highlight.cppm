export module learndc.highlight;

import std;

export namespace learndc::highlight
{
    enum class Language
    {
        Plain,
        Dc,
        Dcon,
        Toml,
        Markdown,
    };

    enum class Kind
    {
        Plain,
        Keyword,
        Type,
        Number,
        String,
        Char,
        Comment,
        Attribute,
        Punct,
        Heading,
        Key,
    };

    struct Span
    {
        std::size_t begin = 0;
        std::size_t end = 0;
        Kind kind = Kind::Plain;
        friend bool operator==(Span const&, Span const&) = default;
    };

    struct State
    {
        bool in_block_comment = false;
        bool in_multiline_string = false;
        char delimiter = 0;
        std::size_t delimiter_len = 0;
        friend bool operator==(State const&, State const&) = default;
    };

    [[nodiscard]] inline Language language_for(std::string_view path)
    {
        auto const dot = path.rfind('.');
        auto const slash = path.find_last_of("/\\");
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
            return Language::Plain;
        auto const ext = path.substr(dot);
        if (ext == ".dc")
            return Language::Dc;
        if (ext == ".dcon")
            return Language::Dcon;
        if (ext == ".toml")
            return Language::Toml;
        if (ext == ".md")
            return Language::Markdown;
        return Language::Plain;
    }

    namespace detail
    {
        struct Out
        {
            std::vector<Span> spans;
            std::size_t pos = 0;

            void push(std::size_t begin, std::size_t end, Kind kind)
            {
                if (end <= begin)
                    return;
                if (begin > pos)
                    add(pos, begin, Kind::Plain);
                add(begin, end, kind);
                pos = end;
            }

            void finish(std::size_t size)
            {
                if (pos < size)
                    add(pos, size, Kind::Plain);
                pos = size;
            }

            void add(std::size_t begin, std::size_t end, Kind kind)
            {
                if (!spans.empty() && spans.back().kind == kind && spans.back().end == begin)
                    spans.back().end = end;
                else
                    spans.push_back({begin, end, kind});
            }
        };

        [[nodiscard]] inline bool is_alpha(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        [[nodiscard]] inline bool is_digit(char c)
        {
            return c >= '0' && c <= '9';
        }

        [[nodiscard]] inline bool is_ident(char c)
        {
            return is_alpha(c) || is_digit(c);
        }

        [[nodiscard]] inline bool is_blank(char c)
        {
            return c == ' ' || c == '\t';
        }

        [[nodiscard]] inline bool is_keyword(std::string_view w)
        {
            static constexpr auto table = std::to_array<std::string_view>({
                "alignof", "as",     "asm",    "break",  "compiles", "const",  "continue", "defer", "do",       "else",   "enum",
                "extern",  "false",  "for",    "if",     "import",   "match",  "module",   "null",  "offsetof", "public", "restrict",
                "return",  "sizeof", "static", "struct", "true",     "typeof", "union",    "using", "volatile", "while",
            });
            return std::ranges::binary_search(table, w);
        }

        [[nodiscard]] inline bool is_primitive(std::string_view w)
        {
            static constexpr auto table = std::to_array<std::string_view>({
                "bool",
                "char",
                "f32",
                "f64",
                "i16",
                "i32",
                "i64",
                "i8",
                "isize",
                "null_t",
                "u16",
                "u32",
                "u64",
                "u8",
                "usize",
                "void",
            });
            return std::ranges::binary_search(table, w);
        }

        [[nodiscard]] inline std::size_t skip_blank(std::string_view s, std::size_t i)
        {
            while (i < s.size() && is_blank(s[i]))
                ++i;
            return i;
        }

        [[nodiscard]] inline bool followed_by_equals(std::string_view s, std::size_t i)
        {
            i = skip_blank(s, i);
            return i < s.size() && s[i] == '=' && (i + 1 >= s.size() || s[i + 1] != '=');
        }

        [[nodiscard]] inline std::size_t scan_quoted(std::string_view s, std::size_t i, char quote, bool escapes)
        {
            ++i;
            while (i < s.size())
            {
                if (escapes && s[i] == '\\')
                {
                    i += 2;
                    continue;
                }
                if (s[i] == quote)
                    return i + 1;
                ++i;
            }
            return s.size();
        }

        [[nodiscard]] inline std::size_t scan_number(std::string_view s, std::size_t i)
        {
            auto const n = s.size();
            if (s[i] == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'b' || s[i + 1] == 'B' || s[i + 1] == 'o' || s[i + 1] == 'O'))
            {
                i += 2;
                while (i < n && (is_ident(s[i])))
                    ++i;
                return i;
            }
            while (i < n && (is_digit(s[i]) || s[i] == '_'))
                ++i;
            if (i + 1 < n && s[i] == '.' && is_digit(s[i + 1]))
            {
                ++i;
                while (i < n && (is_digit(s[i]) || s[i] == '_'))
                    ++i;
            }
            if (i < n && (s[i] == 'e' || s[i] == 'E'))
            {
                auto j = i + 1;
                if (j < n && (s[j] == '+' || s[j] == '-'))
                    ++j;
                if (j < n && is_digit(s[j]))
                {
                    i = j;
                    while (i < n && (is_digit(s[i]) || s[i] == '_'))
                        ++i;
                }
            }
            return i;
        }

        inline void highlight_dc(std::string_view line, State& state, Out& out, bool dcon)
        {
            auto const n = line.size();
            std::size_t i = 0;
            while (i < n)
            {
                if (state.in_block_comment)
                {
                    auto const close = line.find("*/", i);
                    if (close == std::string_view::npos)
                    {
                        out.push(i, n, Kind::Comment);
                        return;
                    }
                    out.push(i, close + 2, Kind::Comment);
                    i = close + 2;
                    state.in_block_comment = false;
                    continue;
                }
                auto const c = line[i];
                if (is_blank(c))
                {
                    ++i;
                    continue;
                }
                if (c == '\\' && i + 1 < n && line[i + 1] == '\\' && skip_blank(line, 0) == i)
                {
                    out.push(i, n, Kind::String);
                    return;
                }
                if (c == '/' && i + 1 < n && line[i + 1] == '/')
                {
                    out.push(i, n, Kind::Comment);
                    return;
                }
                if (c == '/' && i + 1 < n && line[i + 1] == '*')
                {
                    state.in_block_comment = true;
                    auto const close = line.find("*/", i + 2);
                    if (close == std::string_view::npos)
                    {
                        out.push(i, n, Kind::Comment);
                        return;
                    }
                    out.push(i, close + 2, Kind::Comment);
                    i = close + 2;
                    state.in_block_comment = false;
                    continue;
                }
                auto const prefixed = c == 'u' && i + 1 < n && (line[i + 1] == '"' || line[i + 1] == '\'');
                auto const q = prefixed ? line[i + 1] : c;
                if (q == '"' || q == '\'')
                {
                    auto const start = i;
                    auto const quote_at = prefixed ? i + 1 : i;
                    if (q == '"')
                    {
                        auto const end = scan_quoted(line, quote_at, '"', true);
                        auto kind = Kind::String;
                        if (dcon && followed_by_equals(line, end))
                            kind = Kind::Key;
                        out.push(start, end, kind);
                        i = end;
                        continue;
                    }
                    auto const end = scan_quoted(line, quote_at, '\'', true);
                    bool const closed = end <= n && end > quote_at + 1 && line[end - 1] == '\'';
                    if (closed)
                    {
                        out.push(start, end, Kind::Char);
                        i = end;
                        continue;
                    }
                    out.push(i, i + 1, Kind::Punct);
                    ++i;
                    continue;
                }
                if (is_digit(c))
                {
                    auto const end = scan_number(line, i);
                    out.push(i, end, Kind::Number);
                    i = end;
                    continue;
                }
                if (c == '@')
                {
                    auto end = i + 1;
                    while (end < n && is_ident(line[end]))
                        ++end;
                    out.push(i, end, end > i + 1 ? Kind::Attribute : Kind::Punct);
                    i = end;
                    continue;
                }
                if (is_alpha(c))
                {
                    auto end = i + 1;
                    while (end < n && is_ident(line[end]))
                        ++end;
                    auto word = line.substr(i, end - i);
                    if (dcon)
                    {
                        auto path_end = end;
                        bool path = false;
                        while (path_end + 2 < n && line[path_end] == ':' && line[path_end + 1] == ':' && is_alpha(line[path_end + 2]))
                        {
                            path = true;
                            path_end += 2;
                            while (path_end < n && is_ident(line[path_end]))
                                ++path_end;
                        }
                        if (path)
                        {
                            out.push(i, path_end, Kind::Type);
                            i = path_end;
                            continue;
                        }
                        if (followed_by_equals(line, end))
                        {
                            out.push(i, end, Kind::Key);
                            i = end;
                            continue;
                        }
                    }
                    Kind kind = Kind::Plain;
                    if (is_primitive(word))
                        kind = Kind::Type;
                    else if (is_keyword(word))
                        kind = Kind::Keyword;
                    else if (word[0] >= 'A' && word[0] <= 'Z')
                        kind = Kind::Type;
                    out.push(i, end, kind);
                    i = end;
                    continue;
                }
                if (static_cast<unsigned char>(c) >= 0x80)
                {
                    ++i;
                    continue;
                }
                out.push(i, i + 1, Kind::Punct);
                ++i;
            }
        }

        inline void highlight_toml(std::string_view line, State& state, Out& out)
        {
            auto const n = line.size();
            std::size_t i = 0;
            if (state.in_multiline_string)
            {
                auto const q = state.delimiter;
                while (i < n)
                {
                    if (q == '"' && line[i] == '\\')
                    {
                        i += 2;
                        continue;
                    }
                    if (line[i] == q && i + 2 < n + 0 && line[i + 1] == q && line[i + 2] == q)
                    {
                        i += 3;
                        while (i < n && line[i] == q)
                            ++i;
                        state.in_multiline_string = false;
                        break;
                    }
                    ++i;
                }
                if (state.in_multiline_string)
                {
                    out.push(0, n, Kind::String);
                    return;
                }
                out.push(0, std::min(i, n), Kind::String);
            }
            auto const first = skip_blank(line, i);
            if (first < n && line[first] == '[' && i == 0)
            {
                bool const array = first + 1 < n && line[first + 1] == '[';
                auto const close = line.find(array ? "]]" : "]", first);
                auto const end = close == std::string_view::npos ? n : close + (array ? 2 : 1);
                out.push(first, end, Kind::Heading);
                i = end;
            }
            while (i < n)
            {
                auto const c = line[i];
                if (is_blank(c))
                {
                    ++i;
                    continue;
                }
                if (c == '#')
                {
                    out.push(i, n, Kind::Comment);
                    return;
                }
                if (c == '"' || c == '\'')
                {
                    if (i + 2 < n && line[i + 1] == c && line[i + 2] == c)
                    {
                        auto j = i + 3;
                        bool closed = false;
                        while (j < n)
                        {
                            if (c == '"' && line[j] == '\\')
                            {
                                j += 2;
                                continue;
                            }
                            if (j + 2 < n + 0 && line[j] == c && line[j + 1] == c && line[j + 2] == c)
                            {
                                j += 3;
                                while (j < n && line[j] == c)
                                    ++j;
                                closed = true;
                                break;
                            }
                            ++j;
                        }
                        if (!closed)
                        {
                            state.in_multiline_string = true;
                            state.delimiter = c;
                            state.delimiter_len = 3;
                            out.push(i, n, Kind::String);
                            return;
                        }
                        out.push(i, j, Kind::String);
                        i = j;
                        continue;
                    }
                    auto const end = scan_quoted(line, i, c, c == '"');
                    out.push(i, end, followed_by_equals(line, end) ? Kind::Key : Kind::String);
                    i = end;
                    continue;
                }
                auto const key_char = [](char ch) { return is_ident(ch) || ch == '-' || ch == '.'; };
                if (key_char(c))
                {
                    auto end = i;
                    while (end < n && key_char(line[end]))
                        ++end;
                    if (followed_by_equals(line, end))
                    {
                        out.push(i, end, Kind::Key);
                        i = end;
                        continue;
                    }
                    if (is_digit(c))
                    {
                        end = i;
                        while (end < n && (is_ident(line[end]) || line[end] == '.' || line[end] == ':' || line[end] == '-'))
                            ++end;
                        out.push(i, end, Kind::Number);
                        i = end;
                        continue;
                    }
                    if (is_alpha(c))
                    {
                        end = i;
                        while (end < n && is_ident(line[end]))
                            ++end;
                        auto const word = line.substr(i, end - i);
                        Kind kind = Kind::Plain;
                        if (word == "true" || word == "false")
                            kind = Kind::Keyword;
                        else if (word == "inf" || word == "nan")
                            kind = Kind::Number;
                        out.push(i, end, kind);
                        i = end;
                        continue;
                    }
                    if (c == '-' && i + 1 < n && is_digit(line[i + 1]))
                    {
                        end = i + 1;
                        while (end < n && (is_ident(line[end]) || line[end] == '.' || line[end] == ':' || line[end] == '-'))
                            ++end;
                        out.push(i, end, Kind::Number);
                        i = end;
                        continue;
                    }
                }
                if (c == '+' && i + 1 < n && is_digit(line[i + 1]))
                {
                    auto end = i + 1;
                    while (end < n && (is_ident(line[end]) || line[end] == '.' || line[end] == ':' || line[end] == '-'))
                        ++end;
                    out.push(i, end, Kind::Number);
                    i = end;
                    continue;
                }
                if (static_cast<unsigned char>(c) >= 0x80)
                {
                    ++i;
                    continue;
                }
                out.push(i, i + 1, Kind::Punct);
                ++i;
            }
        }

        inline void highlight_markdown(std::string_view line, State& state, Out& out)
        {
            auto const n = line.size();
            std::size_t lead = 0;
            while (lead < n && lead < 3 && line[lead] == ' ')
                ++lead;
            std::size_t run = 0;
            char fence = 0;
            if (lead < n && (line[lead] == '`' || line[lead] == '~'))
            {
                fence = line[lead];
                while (lead + run < n && line[lead + run] == fence)
                    ++run;
            }
            if (state.in_multiline_string)
            {
                if (run >= 3 && fence == state.delimiter && run >= state.delimiter_len && skip_blank(line, lead + run) == n)
                {
                    out.push(lead, n, Kind::Punct);
                    state.in_multiline_string = false;
                    state.delimiter = 0;
                    state.delimiter_len = 0;
                }
                return;
            }
            if (run >= 3)
            {
                bool const info_ok = fence != '`' || line.find('`', lead + run) == std::string_view::npos;
                if (info_ok)
                {
                    out.push(lead, n, Kind::Punct);
                    state.in_multiline_string = true;
                    state.delimiter = fence;
                    state.delimiter_len = run;
                    return;
                }
            }
            if (lead < n && line[lead] == '#')
            {
                std::size_t hashes = 0;
                while (lead + hashes < n && line[lead + hashes] == '#')
                    ++hashes;
                if (hashes <= 6 && (lead + hashes == n || line[lead + hashes] == ' ' || line[lead + hashes] == '\t'))
                {
                    out.push(lead, n, Kind::Heading);
                    return;
                }
            }
            std::size_t i = 0;
            while (i < n)
            {
                if (line[i] != '`')
                {
                    ++i;
                    continue;
                }
                std::size_t ticks = 0;
                while (i + ticks < n && line[i + ticks] == '`')
                    ++ticks;
                std::size_t search = i + ticks;
                std::size_t close = std::string_view::npos;
                while (search < n)
                {
                    auto const next = line.find('`', search);
                    if (next == std::string_view::npos)
                        break;
                    std::size_t len = 0;
                    while (next + len < n && line[next + len] == '`')
                        ++len;
                    if (len == ticks)
                    {
                        close = next + len;
                        break;
                    }
                    search = next + len;
                }
                if (close == std::string_view::npos)
                {
                    i += ticks;
                    continue;
                }
                out.push(i, close, Kind::String);
                i = close;
            }
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<Span> highlight_line(Language language, std::string_view line, State& state)
    {
        detail::Out out;
        switch (language)
        {
            case Language::Dc:
                detail::highlight_dc(line, state, out, false);
                break;
            case Language::Dcon:
                detail::highlight_dc(line, state, out, true);
                break;
            case Language::Toml:
                detail::highlight_toml(line, state, out);
                break;
            case Language::Markdown:
                detail::highlight_markdown(line, state, out);
                break;
            case Language::Plain:
                break;
        }
        out.finish(line.size());
        return std::move(out.spans);
    }
} // namespace learndc::highlight
