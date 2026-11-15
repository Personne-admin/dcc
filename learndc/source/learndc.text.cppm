export module learndc.text;

import std;

export namespace learndc::text
{
    struct Error
    {
        std::string message;
    };

    struct Decoded
    {
        char32_t cp = 0xFFFD;
        std::size_t length = 1;
        bool valid = false;
    };

    [[nodiscard]] inline Decoded decode_utf8(std::string_view s, std::size_t i)
    {
        auto const b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80)
            return {b0, 1, true};
        std::size_t need = 0;
        char32_t cp = 0;
        unsigned lo = 0x80;
        unsigned hi = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF)
        {
            need = 1;
            cp = b0 & 0x1Fu;
        }
        else if (b0 >= 0xE0 && b0 <= 0xEF)
        {
            need = 2;
            cp = b0 & 0x0Fu;
            if (b0 == 0xE0)
                lo = 0xA0;
            if (b0 == 0xED)
                hi = 0x9F;
        }
        else if (b0 >= 0xF0 && b0 <= 0xF4)
        {
            need = 3;
            cp = b0 & 0x07u;
            if (b0 == 0xF0)
                lo = 0x90;
            if (b0 == 0xF4)
                hi = 0x8F;
        }
        else
            return {0xFFFD, 1, false};
        for (std::size_t k = 1; k <= need; ++k)
        {
            if (i + k >= s.size())
                return {0xFFFD, k, false};
            auto const b = static_cast<unsigned>(static_cast<unsigned char>(s[i + k]));
            unsigned const low = k == 1 ? lo : 0x80u;
            unsigned const high = k == 1 ? hi : 0xBFu;
            if (b < low || b > high)
                return {0xFFFD, k, false};
            cp = (cp << 6) | (b & 0x3Fu);
        }
        return {cp, need + 1, true};
    }

    inline void append_utf8(std::string& out, char32_t cp)
    {
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    [[nodiscard]] inline bool utf8_valid(std::string_view s)
    {
        for (std::size_t i = 0; i < s.size();)
        {
            auto const d = decode_utf8(s, i);
            if (!d.valid)
                return false;
            i += d.length;
        }
        return true;
    }

    [[nodiscard]] inline std::size_t skip_escape(std::string_view s, std::size_t i)
    {
        auto const n = s.size();
        std::size_t j = i + 1;
        if (j >= n)
            return n;
        auto const c = s[j];
        if (c == '[')
        {
            ++j;
            while (j < n && s[j] >= 0x30 && s[j] <= 0x3F)
                ++j;
            while (j < n && s[j] >= 0x20 && s[j] <= 0x2F)
                ++j;
            if (j < n && s[j] >= 0x40 && s[j] <= 0x7E)
                ++j;
            return j;
        }
        if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_')
        {
            bool const osc = c == ']';
            ++j;
            while (j < n)
            {
                if (osc && s[j] == 0x07)
                    return j + 1;
                if (s[j] == 0x1B)
                {
                    if (j + 1 < n && s[j + 1] == '\\')
                        return j + 2;
                    return j;
                }
                ++j;
            }
            return n;
        }
        while (j < n && s[j] >= 0x20 && s[j] <= 0x2F)
            ++j;
        if (j < n && s[j] >= 0x30 && s[j] <= 0x7E)
            ++j;
        return j;
    }

    [[nodiscard]] inline std::string strip_ansi(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size();)
        {
            if (s[i] == 0x1B)
            {
                i = skip_escape(s, i);
                continue;
            }
            out += s[i++];
        }
        return out;
    }

    [[nodiscard]] inline std::string sanitize(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size();)
        {
            auto const b = static_cast<unsigned char>(s[i]);
            if (b == 0x1B)
            {
                i = skip_escape(s, i);
                continue;
            }
            if (b == '\n' || b == '\t')
            {
                out += static_cast<char>(b);
                ++i;
                continue;
            }
            if (b == '\r')
            {
                if (i + 1 < s.size() && s[i + 1] == '\n')
                {
                    ++i;
                    continue;
                }
                out += "^M";
                ++i;
                continue;
            }
            if (b < 0x20)
            {
                out += '^';
                out += static_cast<char>(b + 64);
                ++i;
                continue;
            }
            if (b == 0x7F)
            {
                out += "^?";
                ++i;
                continue;
            }
            auto const d = decode_utf8(s, i);
            i += d.length;
            if (!d.valid)
            {
                append_utf8(out, 0xFFFD);
                continue;
            }
            if (d.cp >= 0x80 && d.cp <= 0x9F)
            {
                out += std::format("\\x{:02x}", static_cast<unsigned>(d.cp));
                continue;
            }
            if ((d.cp >= 0x202A && d.cp <= 0x202E) || (d.cp >= 0x2066 && d.cp <= 0x2069))
            {
                out += std::format("\\u{:04x}", static_cast<unsigned>(d.cp));
                continue;
            }
            append_utf8(out, d.cp);
        }
        return out;
    }

    [[nodiscard]] inline std::string normalize_newlines(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\r')
            {
                out += '\n';
                if (i + 1 < s.size() && s[i + 1] == '\n')
                    ++i;
            }
            else
                out += s[i];
        }
        return out;
    }

    namespace detail
    {
        struct Range
        {
            char32_t lo;
            char32_t hi;
        };

        constexpr Range zero_width[] = {
            {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5},   {0x05C7, 0x05C7}, {0x0610, 0x061A},
            {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED},   {0x0711, 0x0711}, {0x0730, 0x074A},
            {0x07A6, 0x07B0}, {0x0900, 0x0902}, {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},   {0x0951, 0x0957}, {0x0E31, 0x0E31},
            {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E},   {0x2060, 0x2064}, {0x2066, 0x206F},
            {0x20D0, 0x20FF}, {0x3099, 0x309A}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF}, {0x0000, 0x001F}, {0x007F, 0x009F},
            {0x0345, 0x0345}, {0x0653, 0x0655}, };

        constexpr Range wide[] = {
            {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653},
            {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
            {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705},
            {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
            {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E}, {0x3041, 0x33FF},
            {0x3400, 0x4DBF}, {0x4E00, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
        };

        constexpr Range wide_high[] = {
            {0xFF00, 0xFF60},
            {0xFFE0, 0xFFE6},
            {0x1F300, 0x1F64F},
            {0x1F680, 0x1F6FF},
            {0x1F900, 0x1F9FF},
            {0x1FA70, 0x1FAFF},
            {0x20000, 0x2FFFD},
            {0x30000, 0x3FFFD},
            {0x1F1E6, 0x1F1FF},
        };

        [[nodiscard]] constexpr bool in_ranges(char32_t cp, auto const& ranges)
        {
            for (auto const& r : ranges)
                if (cp >= r.lo && cp <= r.hi)
                    return true;
            return false;
        }
    } // namespace detail

    [[nodiscard]] inline int char_width(char32_t cp)
    {
        if (cp >= 0x20 && cp < 0x7F)
            return 1;
        if (detail::in_ranges(cp, detail::zero_width))
            return 0;
        if (detail::in_ranges(cp, detail::wide) || detail::in_ranges(cp, detail::wide_high))
            return 2;
        return 1;
    }

    [[nodiscard]] inline std::size_t display_width(std::string_view s)
    {
        std::size_t width = 0;
        for (std::size_t i = 0; i < s.size();)
        {
            auto const d = decode_utf8(s, i);
            i += d.length;
            width += d.valid ? static_cast<std::size_t>(char_width(d.cp)) : 1u;
        }
        return width;
    }

    [[nodiscard]] inline std::vector<std::string> split_lines(std::string_view s)
    {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < s.size())
        {
            auto const end = s.find('\n', start);
            if (end == std::string_view::npos)
            {
                lines.emplace_back(s.substr(start));
                break;
            }
            lines.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
        return lines;
    }

    [[nodiscard]] inline std::vector<std::string> split(std::string_view s, char sep)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true)
        {
            auto const end = s.find(sep, start);
            if (end == std::string_view::npos)
            {
                parts.emplace_back(s.substr(start));
                break;
            }
            parts.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    }

    [[nodiscard]] inline std::string_view trim(std::string_view s)
    {
        constexpr std::string_view ws = " \t\r\n\f\v";
        auto const first = s.find_first_not_of(ws);
        if (first == std::string_view::npos)
            return {};
        auto const last = s.find_last_not_of(ws);
        return s.substr(first, last - first + 1);
    }

    [[nodiscard]] inline std::string lower(std::string_view s)
    {
        std::string out(s);
        for (auto& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    [[nodiscard]] inline bool starts_with_ci(std::string_view s, std::string_view prefix)
    {
        return s.size() >= prefix.size() && lower(s.substr(0, prefix.size())) == lower(prefix);
    }

    [[nodiscard]] inline bool contains_ci(std::string_view s, std::string_view needle)
    {
        return lower(s).find(lower(needle)) != std::string::npos;
    }

    template <std::ranges::input_range R> [[nodiscard]] std::string join(R&& items, std::string_view sep)
    {
        std::string out;
        bool first = true;
        for (auto const& item : items)
        {
            if (!first)
                out += sep;
            first = false;
            out += std::string_view(item);
        }
        return out;
    }

    [[nodiscard]] inline std::string replace_all(std::string_view s, std::string_view from, std::string_view to)
    {
        if (from.empty())
            return std::string(s);
        std::string out;
        std::size_t pos = 0;
        while (true)
        {
            auto const hit = s.find(from, pos);
            if (hit == std::string_view::npos)
            {
                out.append(s.substr(pos));
                break;
            }
            out.append(s.substr(pos, hit - pos));
            out.append(to);
            pos = hit + from.size();
        }
        return out;
    }

    namespace detail
    {
        inline void place_word(std::vector<std::string>& lines, std::string& current, std::size_t& current_width, std::string const& indent,
                               std::size_t indent_width, std::string_view word, std::size_t width)
        {
            auto const word_width = display_width(word);
            bool const at_start = current_width == indent_width;
            if (!at_start && current_width + 1 + word_width <= width)
            {
                current += ' ';
                current.append(word);
                current_width += 1 + word_width;
                return;
            }
            if (!at_start)
            {
                lines.push_back(current);
                current = indent;
                current_width = indent_width;
            }
            if (current_width + word_width <= width)
            {
                current.append(word);
                current_width += word_width;
                return;
            }
            for (std::size_t i = 0; i < word.size();)
            {
                auto const d = decode_utf8(word, i);
                auto const cw = d.valid ? static_cast<std::size_t>(char_width(d.cp)) : 1u;
                if (current_width + cw > width && current_width > indent_width)
                {
                    lines.push_back(current);
                    current = indent;
                    current_width = indent_width;
                }
                current.append(word.substr(i, d.length));
                current_width += cw;
                i += d.length;
            }
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<std::string> wrap(std::string_view text, std::size_t width)
    {
        if (width == 0)
            width = 1;
        std::vector<std::string> lines;
        std::size_t start = 0;
        bool more = true;
        while (more)
        {
            auto const end = text.find('\n', start);
            std::string_view paragraph;
            if (end == std::string_view::npos)
            {
                paragraph = text.substr(start);
                more = false;
            }
            else
            {
                paragraph = text.substr(start, end - start);
                start = end + 1;
            }
            if (!more && paragraph.empty() && start >= text.size() && !text.empty() && text.back() == '\n')
                break;
            if (trim(paragraph).empty())
            {
                lines.emplace_back();
                continue;
            }
            auto const lead = paragraph.find_first_not_of(' ');
            std::size_t indent_width = lead == std::string_view::npos ? 0 : lead;
            if (indent_width > width / 2)
                indent_width = 0;
            std::string const indent(indent_width, ' ');
            std::string current = indent;
            std::size_t current_width = indent_width;
            std::size_t i = 0;
            while (i < paragraph.size())
            {
                while (i < paragraph.size() && paragraph[i] == ' ')
                    ++i;
                auto const stop = paragraph.find(' ', i);
                auto const word = paragraph.substr(i, stop == std::string_view::npos ? std::string_view::npos : stop - i);
                if (word.empty())
                    break;
                detail::place_word(lines, current, current_width, indent, indent_width, word, width);
                i += word.size();
            }
            lines.push_back(current);
        }
        return lines;
    }

    [[nodiscard]] inline std::string truncate_middle(std::string_view s, std::size_t width)
    {
        if (display_width(s) <= width)
            return std::string(s);
        if (width <= 3)
            return std::string(width, '.');
        struct Piece
        {
            std::size_t start;
            std::size_t length;
            std::size_t width;
        };
        std::vector<Piece> pieces;
        for (std::size_t i = 0; i < s.size();)
        {
            auto const d = decode_utf8(s, i);
            pieces.push_back({i, d.length, d.valid ? static_cast<std::size_t>(char_width(d.cp)) : 1u});
            i += d.length;
        }
        auto const keep = width - 3;
        auto const head_budget = (keep + 1) / 2;
        auto const tail_budget = keep / 2;
        std::size_t head_end = 0;
        std::size_t used = 0;
        std::size_t index = 0;
        while (index < pieces.size() && used + pieces[index].width <= head_budget)
        {
            used += pieces[index].width;
            head_end = pieces[index].start + pieces[index].length;
            ++index;
        }
        std::size_t tail_start = s.size();
        used = 0;
        for (std::size_t k = pieces.size(); k > index; --k)
        {
            auto const& p = pieces[k - 1];
            if (used + p.width > tail_budget)
                break;
            used += p.width;
            tail_start = p.start;
        }
        return std::string(s.substr(0, head_end)) + "..." + std::string(s.substr(tail_start));
    }

    [[nodiscard]] inline std::string clamp_bytes(std::string_view s, std::size_t max)
    {
        if (s.size() <= max)
            return std::string(s);
        std::size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        std::string out(s.substr(0, cut));
        out += std::format("\n[... {} more bytes]", s.size() - cut);
        return out;
    }

    namespace detail
    {
        [[nodiscard]] inline std::string visible(std::string_view s, std::string_view space, std::string_view tab, std::string_view cr,
                                                 std::string_view newline)
        {
            std::string out;
            std::size_t pos = 0;
            while (pos < s.size())
            {
                auto const nl = s.find('\n', pos);
                bool const has_nl = nl != std::string_view::npos;
                auto const line = s.substr(pos, has_nl ? nl - pos : std::string_view::npos);
                std::size_t end = line.size();
                while (end > 0 && line[end - 1] == '\r')
                    --end;
                std::size_t trailing = end;
                while (trailing > 0 && line[trailing - 1] == ' ')
                    --trailing;
                for (std::size_t i = 0; i < line.size(); ++i)
                {
                    char const c = line[i];
                    if (c == ' ' && i >= trailing && i < end)
                        out += space;
                    else if (c == '\t')
                        out += tab;
                    else if (c == '\r')
                        out += cr;
                    else
                        out += c;
                }
                if (has_nl)
                {
                    if (nl + 1 == s.size())
                        out += newline;
                    out += '\n';
                    pos = nl + 1;
                }
                else
                    break;
            }
            return out;
        }
    } // namespace detail

    [[nodiscard]] inline std::string visible_whitespace(std::string_view s)
    {
        return detail::visible(s, "\xC2\xB7", "\xE2\x86\x92", "\xE2\x90\x8D", "\xE2\x8F\x8E");
    }

    [[nodiscard]] inline std::string visible_whitespace_ascii(std::string_view s)
    {
        return detail::visible(s, ".", "->", "\\r", "$");
    }
} // namespace learndc::text
