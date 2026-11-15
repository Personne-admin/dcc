export module learndc.markdown;

import std;
import learndc.text;

export namespace learndc::markdown
{
    enum class Kind
    {
        Text,
        Strong,
        Emphasis,
        StrongEmphasis,
        Code,
        Heading,
        CodeBlock,
        Border,
        TableHead,
        Marker,
        Link,
        Url,
    };

    struct Span
    {
        std::string text;
        Kind kind = Kind::Text;

        friend bool operator==(Span const&, Span const&) = default;
    };

    using Line = std::vector<Span>;

    struct Options
    {
        int width = 80;
        bool ascii = false;
        bool plain = false;
        bool skip_title = false;
    };

    namespace detail
    {
        struct Glyphs
        {
            std::string_view bullet, ell, h, v, tl, tm, tr, ml, mm, mr, bl, bm, br;
        };

        [[nodiscard]] inline Glyphs glyphs_for(bool ascii)
        {
            if (ascii)
                return Glyphs{"-", "~", "-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+"};
            return Glyphs{"\xE2\x80\xA2", "\xE2\x80\xA6", "\xE2\x94\x80", "\xE2\x94\x82", "\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90",
                          "\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4", "\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98"};
        }

        [[nodiscard]] inline int width_of(std::string_view s)
        {
            return static_cast<int>(text::display_width(s));
        }

        [[nodiscard]] inline int line_width(Line const& line)
        {
            int w = 0;
            for (auto const& s : line)
                w += width_of(s.text);
            return w;
        }

        [[nodiscard]] inline std::size_t to_size(int n)
        {
            return n < 0 ? 0u : static_cast<std::size_t>(n);
        }

        inline void push(Line& line, std::string value, Kind kind)
        {
            if (value.empty())
                return;
            if (!line.empty() && line.back().kind == kind)
                line.back().text += value;
            else
                line.push_back(Span{std::move(value), kind});
        }

        inline void push_line(Line& dst, Line const& src)
        {
            for (auto const& s : src)
                push(dst, s.text, s.kind);
        }

        [[nodiscard]] inline Kind combine(Kind base, Kind add)
        {
            if (add == Kind::Text)
                return base;
            if (base == Kind::Text)
                return add;
            if (base == add)
                return base;
            bool const base_strong = base == Kind::Strong || base == Kind::StrongEmphasis;
            bool const base_emph = base == Kind::Emphasis || base == Kind::StrongEmphasis;
            bool const add_strong = add == Kind::Strong || add == Kind::StrongEmphasis;
            bool const add_emph = add == Kind::Emphasis || add == Kind::StrongEmphasis;
            if ((base == Kind::Strong || base == Kind::Emphasis || base == Kind::StrongEmphasis) &&
                (add == Kind::Strong || add == Kind::Emphasis || add == Kind::StrongEmphasis))
                return (base_strong || add_strong) && (base_emph || add_emph) ? Kind::StrongEmphasis : base;
            return base;
        }

        [[nodiscard]] inline bool is_punct(char c)
        {
            return std::ispunct(static_cast<unsigned char>(c)) != 0;
        }

        [[nodiscard]] inline bool is_space(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        [[nodiscard]] inline bool alnum_at(std::string_view s, std::size_t i)
        {
            auto const b = static_cast<unsigned char>(s[i]);
            return b >= 0x80 || std::isalnum(b) != 0;
        }

        [[nodiscard]] inline std::size_t run_length(std::string_view s, std::size_t i, char c)
        {
            std::size_t n = 0;
            while (i + n < s.size() && s[i + n] == c)
                ++n;
            return n;
        }

        [[nodiscard]] inline std::optional<std::size_t> find_code_end(std::string_view s, std::size_t from, std::size_t n)
        {
            for (std::size_t j = from; j < s.size();)
            {
                if (s[j] == '`')
                {
                    auto const k = run_length(s, j, '`');
                    if (k == n)
                        return j;
                    j += k;
                }
                else
                    ++j;
            }
            return std::nullopt;
        }

        [[nodiscard]] inline std::optional<std::size_t> find_bracket_end(std::string_view s, std::size_t open)
        {
            int depth = 0;
            for (std::size_t j = open; j < s.size();)
            {
                char const c = s[j];
                if (c == '\\')
                {
                    j += 2;
                    continue;
                }
                if (c == '`')
                {
                    auto const k = run_length(s, j, '`');
                    if (auto end = find_code_end(s, j + k, k))
                        j = *end + k;
                    else
                        j += k;
                    continue;
                }
                if (c == '[')
                    ++depth;
                else if (c == ']')
                {
                    --depth;
                    if (depth == 0)
                        return j;
                }
                ++j;
            }
            return std::nullopt;
        }

        [[nodiscard]] inline std::optional<std::size_t> find_paren_end(std::string_view s, std::size_t open)
        {
            int depth = 0;
            for (std::size_t j = open; j < s.size(); ++j)
            {
                if (s[j] == '(')
                    ++depth;
                else if (s[j] == ')')
                {
                    --depth;
                    if (depth == 0)
                        return j;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] inline std::string line_text(Line const& line)
        {
            std::string out;
            for (auto const& s : line)
                out += s.text;
            return out;
        }

        [[nodiscard]] inline std::optional<std::size_t> find_closer(std::string_view s, std::size_t from, char c, std::size_t n)
        {
            for (std::size_t j = from; j < s.size();)
            {
                if (s[j] == '\\')
                {
                    j += 2;
                    continue;
                }
                if (s[j] == '`')
                {
                    auto const k = run_length(s, j, '`');
                    if (auto end = find_code_end(s, j + k, k))
                        j = *end + k;
                    else
                        j += k;
                    continue;
                }
                if (s[j] == c)
                {
                    auto const m = run_length(s, j, c);
                    if (m == n && j > from && !is_space(s[j - 1]) && (j + m >= s.size() || !alnum_at(s, j + m)))
                        return j;
                    j += m;
                    continue;
                }
                ++j;
            }
            return std::nullopt;
        }

        inline void parse_inline(std::string_view s, Kind base, Line& out)
        {
            std::string literal;
            auto flush = [&] {
                push(out, std::move(literal), base);
                literal.clear();
            };
            std::size_t i = 0;
            while (i < s.size())
            {
                char const c = s[i];
                if (c == '\\' && i + 1 < s.size() && is_punct(s[i + 1]))
                {
                    literal += s[i + 1];
                    i += 2;
                    continue;
                }
                if (c == '`')
                {
                    auto const n = run_length(s, i, '`');
                    if (auto end = find_code_end(s, i + n, n))
                    {
                        std::string code(s.substr(i + n, *end - i - n));
                        if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos)
                            code = code.substr(1, code.size() - 2);
                        flush();
                        push(out, std::move(code), Kind::Code);
                        i = *end + n;
                        continue;
                    }
                    literal.append(n, '`');
                    i += n;
                    continue;
                }
                if (c == '[' || (c == '!' && i + 1 < s.size() && s[i + 1] == '['))
                {
                    std::size_t const open = c == '!' ? i + 1 : i;
                    if (auto close = find_bracket_end(s, open); close && *close + 1 < s.size() && s[*close + 1] == '(')
                    {
                        if (auto end = find_paren_end(s, *close + 1))
                        {
                            auto url = std::string(text::trim(s.substr(*close + 2, *end - *close - 2)));
                            if (auto sp = url.find(' '); sp != std::string::npos)
                                url.resize(sp);
                            Line label;
                            parse_inline(s.substr(open + 1, *close - open - 1), Kind::Link, label);
                            flush();
                            auto const label_text = line_text(label);
                            if (label_text.empty())
                                push(out, url, Kind::Url);
                            else
                            {
                                push_line(out, label);
                                if (!url.empty() && url != label_text)
                                    push(out, " (" + url + ")", Kind::Url);
                            }
                            i = *end + 1;
                            continue;
                        }
                    }
                }
                if (c == '*' || c == '_')
                {
                    auto const n = run_length(s, i, c);
                    bool const opens = n <= 3 && i + n < s.size() && !is_space(s[i + n]) && (i == 0 || !alnum_at(s, i - 1));
                    if (opens)
                        if (auto close = find_closer(s, i + n, c, n))
                        {
                            flush();
                            Kind const add = n == 1 ? Kind::Emphasis : (n == 2 ? Kind::Strong : Kind::StrongEmphasis);
                            parse_inline(s.substr(i + n, *close - i - n), combine(base, add), out);
                            i = *close + n;
                            continue;
                        }
                    literal.append(n, c);
                    i += n;
                    continue;
                }
                literal += c;
                ++i;
            }
            flush();
        }

        struct Atom
        {
            Line spans;
            Kind gap = Kind::Text;
        };

        [[nodiscard]] inline std::vector<Atom> split_atoms(Line const& line)
        {
            std::vector<Atom> atoms;
            Atom cur;
            bool have = false;
            bool space_pending = false;
            std::size_t space_span = 0;
            std::size_t last_span = static_cast<std::size_t>(-1);
            for (std::size_t si = 0; si < line.size(); ++si)
            {
                auto const& sp = line[si];
                std::size_t i = 0;
                while (i < sp.text.size())
                {
                    if (sp.text[i] == ' ' || sp.text[i] == '\t')
                    {
                        if (have)
                        {
                            atoms.push_back(std::move(cur));
                            cur = Atom{};
                            have = false;
                        }
                        space_pending = true;
                        space_span = si;
                        ++i;
                        continue;
                    }
                    std::size_t j = i;
                    while (j < sp.text.size() && sp.text[j] != ' ' && sp.text[j] != '\t')
                        ++j;
                    if (!have)
                    {
                        cur.gap = space_pending && space_span == si && last_span == si ? sp.kind : Kind::Text;
                        have = true;
                    }
                    push(cur.spans, sp.text.substr(i, j - i), sp.kind);
                    last_span = si;
                    space_pending = false;
                    i = j;
                }
            }
            if (have)
                atoms.push_back(std::move(cur));
            return atoms;
        }

        [[nodiscard]] inline std::pair<Line, Line> split_at_width(Line const& spans, int room)
        {
            Line head;
            Line tail;
            int used = 0;
            bool full = false;
            for (auto const& sp : spans)
            {
                std::size_t i = 0;
                while (i < sp.text.size())
                {
                    auto const d = text::decode_utf8(sp.text, i);
                    int const cw = d.valid ? text::char_width(d.cp) : 1;
                    auto const glyph = sp.text.substr(i, d.length);
                    i += d.length;
                    if (!full && (used + cw <= room || used == 0))
                    {
                        push(head, std::string(glyph), sp.kind);
                        used += cw;
                    }
                    else
                    {
                        full = true;
                        push(tail, std::string(glyph), sp.kind);
                    }
                }
            }
            return {std::move(head), std::move(tail)};
        }

        [[nodiscard]] inline std::vector<Line> layout(Line const& content, int width, Line const& first, Line const& rest)
        {
            std::vector<Line> out;
            Line cur = first;
            int used = line_width(first);
            bool has_word = false;
            auto const start_next = [&] {
                out.push_back(std::move(cur));
                cur = rest;
                used = line_width(rest);
                has_word = false;
            };
            for (auto& atom : split_atoms(content))
            {
                int const aw = line_width(atom.spans);
                if (has_word && used + 1 + aw <= width)
                {
                    push(cur, " ", atom.gap);
                    push_line(cur, atom.spans);
                    used += 1 + aw;
                    continue;
                }
                if (!has_word && used + aw <= width)
                {
                    push_line(cur, atom.spans);
                    used += aw;
                    has_word = true;
                    continue;
                }
                if (has_word)
                {
                    start_next();
                    if (used + aw <= width)
                    {
                        push_line(cur, atom.spans);
                        used += aw;
                        has_word = true;
                        continue;
                    }
                }
                Line remain = atom.spans;
                for (;;)
                {
                    int const room = std::max(1, width - used);
                    if (line_width(remain) <= room)
                    {
                        used += line_width(remain);
                        push_line(cur, remain);
                        has_word = true;
                        break;
                    }
                    auto [head, tail] = split_at_width(remain, room);
                    push_line(cur, head);
                    start_next();
                    remain = std::move(tail);
                }
            }
            if (has_word || out.empty() || line_width(cur) > 0)
                out.push_back(std::move(cur));
            return out;
        }

        [[nodiscard]] inline std::string spaces(int n)
        {
            return std::string(to_size(n), ' ');
        }

        [[nodiscard]] inline std::vector<std::string> split_row(std::string_view row)
        {
            auto t = text::trim(row);
            if (t.starts_with('|'))
                t.remove_prefix(1);
            if (t.ends_with('|') && !t.ends_with("\\|"))
                t.remove_suffix(1);
            std::vector<std::string> cells;
            std::string cur;
            for (std::size_t i = 0; i < t.size();)
            {
                char const c = t[i];
                if (c == '\\' && i + 1 < t.size())
                {
                    cur += c;
                    cur += t[i + 1];
                    i += 2;
                    continue;
                }
                if (c == '`')
                {
                    auto const n = run_length(t, i, '`');
                    if (auto end = find_code_end(t, i + n, n))
                    {
                        cur += t.substr(i, *end + n - i);
                        i = *end + n;
                        continue;
                    }
                    cur.append(n, '`');
                    i += n;
                    continue;
                }
                if (c == '|')
                {
                    cells.emplace_back(text::trim(cur));
                    cur.clear();
                    ++i;
                    continue;
                }
                cur += c;
                ++i;
            }
            cells.emplace_back(text::trim(cur));
            return cells;
        }

        [[nodiscard]] inline bool alignment_cell(std::string_view cell)
        {
            auto t = text::trim(cell);
            if (t.starts_with(':'))
                t.remove_prefix(1);
            if (t.ends_with(':'))
                t.remove_suffix(1);
            return !t.empty() && std::ranges::all_of(t, [](char c) { return c == '-'; });
        }

        [[nodiscard]] inline bool alignment_row(std::string_view row, std::vector<char>* aligns = nullptr)
        {
            if (row.find('-') == std::string_view::npos)
                return false;
            auto const cells = split_row(row);
            if (cells.empty())
                return false;
            for (auto const& c : cells)
                if (!alignment_cell(c))
                    return false;
            if (aligns)
            {
                aligns->clear();
                for (auto const& c : cells)
                {
                    bool const l = c.starts_with(':');
                    bool const r = c.ends_with(':');
                    aligns->push_back(l && r ? 'c' : (r ? 'r' : 'l'));
                }
            }
            return true;
        }

        [[nodiscard]] inline bool is_rule(std::string_view t)
        {
            t = text::trim(t);
            if (t.size() < 3)
                return false;
            char const c = t[0];
            if (c != '-' && c != '*' && c != '_')
                return false;
            std::size_t count = 0;
            for (char x : t)
            {
                if (x == c)
                    ++count;
                else if (x != ' ')
                    return false;
            }
            return count >= 3;
        }

        struct ListMarker
        {
            int indent = 0;
            std::string marker;
            bool numbered = false;
            std::string body;
        };

        [[nodiscard]] inline std::optional<ListMarker> list_marker(std::string_view raw)
        {
            std::size_t i = 0;
            while (i < raw.size() && raw[i] == ' ')
                ++i;
            if (i >= raw.size())
                return std::nullopt;
            ListMarker m;
            m.indent = static_cast<int>(i);
            if ((raw[i] == '-' || raw[i] == '*' || raw[i] == '+') && i + 1 < raw.size() && raw[i + 1] == ' ')
            {
                if (is_rule(raw))
                    return std::nullopt;
                m.body = std::string(text::trim(raw.substr(i + 2)));
                return m;
            }
            std::size_t d = i;
            while (d < raw.size() && std::isdigit(static_cast<unsigned char>(raw[d])))
                ++d;
            if (d > i && d - i <= 9 && d + 1 < raw.size() && (raw[d] == '.' || raw[d] == ')') && raw[d + 1] == ' ')
            {
                m.numbered = true;
                m.marker = std::string(raw.substr(i, d - i + 1));
                m.body = std::string(text::trim(raw.substr(d + 2)));
                return m;
            }
            return std::nullopt;
        }

        [[nodiscard]] inline std::optional<std::pair<int, std::string>> heading_of(std::string_view raw)
        {
            std::size_t i = 0;
            while (i < raw.size() && raw[i] == ' ' && i < 3)
                ++i;
            std::size_t h = i;
            while (h < raw.size() && raw[h] == '#')
                ++h;
            auto const level = h - i;
            if (level < 1 || level > 6 || (h < raw.size() && raw[h] != ' '))
                return std::nullopt;
            auto body = std::string(text::trim(raw.substr(h)));
            while (!body.empty() && body.back() == '#')
                body.pop_back();
            return std::pair<int, std::string>{static_cast<int>(level), std::string(text::trim(body))};
        }

        [[nodiscard]] inline std::optional<std::pair<char, std::size_t>> fence_of(std::string_view raw)
        {
            auto const t = text::trim(raw);
            if (t.size() < 3 || (t[0] != '`' && t[0] != '~'))
                return std::nullopt;
            auto const n = run_length(t, 0, t[0]);
            if (n < 3)
                return std::nullopt;
            if (t[0] == '`' && t.find('`', n) != std::string_view::npos)
                return std::nullopt;
            return std::pair<char, std::size_t>{t[0], n};
        }

        class Renderer
        {
        public:
            explicit Renderer(Options const& options) : m_opt(options), m_gl(glyphs_for(options.ascii || options.plain)), m_width(std::max(4, options.width)) {}

            [[nodiscard]] std::vector<Line> run(std::string_view markdown)
            {
                auto const lines = text::split_lines(text::normalize_newlines(text::sanitize(markdown)));
                bool first_block = true;
                for (std::size_t i = 0; i < lines.size(); ++i)
                {
                    auto const& raw = lines[i];
                    auto const t = text::trim(raw);
                    if (auto fence = fence_of(raw))
                    {
                        flush_text();
                        std::vector<std::string> body;
                        ++i;
                        for (; i < lines.size(); ++i)
                        {
                            auto const ct = text::trim(lines[i]);
                            if (ct.size() >= fence->second && run_length(ct, 0, fence->first) == ct.size())
                                break;
                            body.push_back(lines[i]);
                        }
                        emit_code(body);
                        first_block = false;
                        continue;
                    }
                    if (t.empty())
                    {
                        flush_text();
                        continue;
                    }
                    if (auto h = heading_of(raw))
                    {
                        flush_text();
                        if (first_block && m_opt.skip_title && h->first == 1)
                        {
                            first_block = false;
                            continue;
                        }
                        emit_heading(h->first, h->second);
                        first_block = false;
                        continue;
                    }
                    if (is_rule(raw))
                    {
                        flush_text();
                        emit_rule();
                        first_block = false;
                        continue;
                    }
                    if (raw.find('|') != std::string::npos && i + 1 < lines.size() && alignment_row(lines[i + 1]) &&
                        split_row(raw).size() == split_row(lines[i + 1]).size())
                    {
                        flush_text();
                        std::vector<std::string> rows;
                        rows.push_back(raw);
                        std::size_t j = i + 2;
                        while (j < lines.size() && !text::trim(lines[j]).empty() && lines[j].find('|') != std::string::npos)
                            rows.push_back(lines[j++]);
                        emit_table(rows, lines[i + 1]);
                        i = j - 1;
                        first_block = false;
                        continue;
                    }
                    if (auto item = list_marker(raw))
                    {
                        flush_text();
                        m_item = std::move(item);
                        first_block = false;
                        continue;
                    }
                    if (!m_text.empty())
                        m_text += ' ';
                    m_text += std::string(t);
                    first_block = false;
                }
                flush_text();
                while (!m_out.empty() && m_out.back().empty())
                    m_out.pop_back();
                return std::move(m_out);
            }

        private:
            Options m_opt;
            Glyphs m_gl;
            int m_width;
            std::vector<Line> m_out;
            std::string m_text;
            std::optional<ListMarker> m_item;
            bool m_in_list = false;
            bool m_tight = false;

            void inline_into(std::string_view s, Kind base, Line& out) const
            {
                Line parsed;
                parse_inline(s, base, parsed);
                for (auto& sp : parsed)
                    if (m_opt.plain && sp.kind == Kind::Code)
                        sp.text = "`" + sp.text + "`";
                push_line(out, parsed);
            }

            void blank()
            {
                if (m_tight)
                {
                    m_tight = false;
                    return;
                }
                if (!m_out.empty() && !m_out.back().empty())
                    m_out.push_back({});
            }

            void add_all(std::vector<Line> lines)
            {
                for (auto& l : lines)
                    m_out.push_back(std::move(l));
            }

            void flush_text()
            {
                if (m_item)
                {
                    auto item = std::move(*m_item);
                    m_item.reset();
                    auto body = std::move(m_text);
                    m_text.clear();
                    if (!body.empty())
                        item.body += (item.body.empty() ? "" : " ") + body;
                    emit_item(item);
                    return;
                }
                if (m_text.empty())
                    return;
                auto body = std::move(m_text);
                m_text.clear();
                blank();
                m_in_list = false;
                Line content;
                inline_into(body, Kind::Text, content);
                add_all(layout(content, m_width, {}, {}));
            }

            void emit_item(ListMarker const& item)
            {
                if (!m_in_list)
                    blank();
                m_in_list = true;
                int const level = std::min(4, (item.indent + 1) / 2);
                std::string marker = item.numbered ? item.marker : std::string(m_gl.bullet);
                Line first;
                push(first, spaces(level * 2), Kind::Text);
                push(first, marker + " ", Kind::Marker);
                Line rest;
                push(rest, spaces(line_width(first)), Kind::Text);
                Line content;
                inline_into(item.body, Kind::Text, content);
                add_all(layout(content, m_width, first, rest));
            }

            void emit_heading(int level, std::string const& body)
            {
                m_tight = false;
                blank();
                m_in_list = false;
                Line first;
                if (m_opt.plain)
                    push(first, std::string(to_size(level), '#') + " ", Kind::Heading);
                Line content;
                inline_into(body, Kind::Heading, content);
                for (auto& s : content)
                    if (s.kind != Kind::Code)
                        s.kind = Kind::Heading;
                Line rest;
                push(rest, spaces(line_width(first)), Kind::Text);
                add_all(layout(content, m_width, first, rest));
                m_tight = !m_opt.plain;
            }

            void emit_rule()
            {
                blank();
                m_in_list = false;
                Line l;
                push(
                    l,
                    [&] {
                        std::string s;
                        for (int i = 0; i < std::min(m_width, 40); ++i)
                            s += m_gl.h;
                        return s;
                    }(),
                    Kind::Border);
                m_out.push_back(std::move(l));
            }

            void emit_code(std::vector<std::string> const& body)
            {
                blank();
                m_in_list = false;
                int const indent = m_opt.plain ? 4 : 2;
                for (auto const& raw : body)
                {
                    auto code = text::replace_all(raw, "\t", "    ");
                    Line l;
                    push(l, spaces(indent), Kind::Text);
                    int const room = std::max(1, m_width - indent);
                    if (!m_opt.plain && width_of(code) > room)
                    {
                        auto [head, tail] = split_at_width({Span{code, Kind::CodeBlock}}, std::max(1, room - 1));
                        (void)tail;
                        push(l, line_text(head), Kind::CodeBlock);
                        push(l, std::string(m_gl.ell), Kind::CodeBlock);
                    }
                    else
                        push(l, std::move(code), Kind::CodeBlock);
                    if (l.size() == 1 && l[0].kind == Kind::Text)
                        l.clear();
                    m_out.push_back(std::move(l));
                }
                if (!body.empty())
                    blank();
            }

            void emit_table(std::vector<std::string> const& rows, std::string const& align_row)
            {
                blank();
                m_in_list = false;
                std::vector<char> aligns;
                (void)alignment_row(align_row, &aligns);
                std::size_t const cols = aligns.size();
                std::vector<std::vector<Line>> cells;
                for (std::size_t r = 0; r < rows.size(); ++r)
                {
                    auto raw = split_row(rows[r]);
                    raw.resize(cols);
                    std::vector<Line> row;
                    for (auto const& c : raw)
                    {
                        Line l;
                        inline_into(text::replace_all(c, "<br>", " "), r == 0 ? Kind::TableHead : Kind::Text, l);
                        row.push_back(std::move(l));
                    }
                    cells.push_back(std::move(row));
                }
                std::vector<int> widths(cols, 1);
                for (auto const& row : cells)
                    for (std::size_t c = 0; c < cols; ++c)
                        widths[c] = std::max(widths[c], line_width(row[c]));
                int total = 0;
                for (int w : widths)
                    total += w;
                int const needed = m_opt.plain ? total + 2 * (static_cast<int>(cols) - 1) : total + 3 * static_cast<int>(cols) + 1;
                if (needed > m_width)
                {
                    emit_stacked(cells);
                    return;
                }
                if (m_opt.plain)
                    emit_plain_table(cells, widths, aligns);
                else
                    emit_box_table(cells, widths, aligns);
            }

            static void emit_cell(Line& l, Line const& cell, int width, char align, bool pad_end)
            {
                int const pad = width - line_width(cell);
                int const left = align == 'r' ? pad : (align == 'c' ? pad / 2 : 0);
                push(l, spaces(left), Kind::Text);
                push_line(l, cell);
                if (pad_end)
                    push(l, spaces(pad - left), Kind::Text);
            }

            void emit_plain_table(std::vector<std::vector<Line>> const& cells, std::vector<int> const& widths, std::vector<char> const& aligns)
            {
                for (std::size_t r = 0; r < cells.size(); ++r)
                {
                    Line l;
                    for (std::size_t c = 0; c < widths.size(); ++c)
                    {
                        if (c > 0)
                            push(l, "  ", Kind::Text);
                        emit_cell(l, cells[r][c], widths[c], aligns[c], c + 1 < widths.size());
                    }
                    m_out.push_back(std::move(l));
                    if (r == 0)
                    {
                        Line rule;
                        for (std::size_t c = 0; c < widths.size(); ++c)
                        {
                            if (c > 0)
                                push(rule, "  ", Kind::Text);
                            push(rule, std::string(to_size(widths[c]), '-'), Kind::Border);
                        }
                        m_out.push_back(std::move(rule));
                    }
                }
                blank();
            }

            [[nodiscard]] Line border_line(std::vector<int> const& widths, std::string_view left, std::string_view mid, std::string_view right) const
            {
                Line l;
                std::string s(left);
                for (std::size_t c = 0; c < widths.size(); ++c)
                {
                    for (int i = 0; i < widths[c] + 2; ++i)
                        s += m_gl.h;
                    s += c + 1 < widths.size() ? mid : right;
                }
                push(l, std::move(s), Kind::Border);
                return l;
            }

            void emit_box_table(std::vector<std::vector<Line>> const& cells, std::vector<int> const& widths, std::vector<char> const& aligns)
            {
                m_out.push_back(border_line(widths, m_gl.tl, m_gl.tm, m_gl.tr));
                for (std::size_t r = 0; r < cells.size(); ++r)
                {
                    Line l;
                    push(l, std::string(m_gl.v), Kind::Border);
                    for (std::size_t c = 0; c < widths.size(); ++c)
                    {
                        push(l, " ", Kind::Text);
                        emit_cell(l, cells[r][c], widths[c], aligns[c], true);
                        push(l, " ", Kind::Text);
                        push(l, std::string(m_gl.v), Kind::Border);
                    }
                    m_out.push_back(std::move(l));
                    if (r == 0)
                        m_out.push_back(border_line(widths, m_gl.ml, m_gl.mm, m_gl.mr));
                }
                m_out.push_back(border_line(widths, m_gl.bl, m_gl.bm, m_gl.br));
                blank();
            }

            void emit_stacked(std::vector<std::vector<Line>> const& cells)
            {
                for (std::size_t r = 1; r < cells.size(); ++r)
                {
                    if (r > 1)
                        m_out.push_back({});
                    for (std::size_t c = 0; c < cells[r].size(); ++c)
                    {
                        Line first;
                        auto const head = line_text(cells[0][c]);
                        if (!head.empty())
                            push(first, head + ": ", Kind::TableHead);
                        Line rest;
                        push(rest, "  ", Kind::Text);
                        add_all(layout(cells[r][c], m_width, first, rest));
                    }
                }
                blank();
            }
        };
    } // namespace detail

    [[nodiscard]] inline int width_of(Line const& line)
    {
        return detail::line_width(line);
    }

    [[nodiscard]] inline std::string line_text(Line const& line)
    {
        return detail::line_text(line);
    }

    [[nodiscard]] inline std::vector<Line> render(std::string_view markdown, Options const& options = {})
    {
        detail::Renderer renderer(options);
        return renderer.run(markdown);
    }

    [[nodiscard]] inline std::string to_text(std::vector<Line> const& lines)
    {
        std::string out;
        for (auto const& l : lines)
        {
            out += detail::line_text(l);
            out += '\n';
        }
        return out;
    }

    [[nodiscard]] inline std::string render_plain(std::string_view markdown, int width = 80)
    {
        Options o;
        o.width = width;
        o.plain = true;
        o.ascii = true;
        return to_text(render(markdown, o));
    }
} // namespace learndc::markdown
