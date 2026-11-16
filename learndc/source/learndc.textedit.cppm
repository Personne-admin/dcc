export module learndc.textedit;

import std;
import learndc.text;

export namespace learndc::textedit
{
    struct Pos
    {
        std::size_t line = 0;
        std::size_t col = 0;
        friend constexpr auto operator<=>(Pos const&, Pos const&) = default;
    };

    enum class Eol
    {
        Lf,
        Crlf,
    };

    struct Limits
    {
        std::size_t max_bytes = 1024 * 1024;
        std::size_t max_lines = 20000;
        std::size_t max_undo = 1000;
        std::size_t max_undo_bytes = 8 * 1024 * 1024;
    };

    struct LoadResult
    {
        bool editable = true;
        std::string reason;
    };

    struct View
    {
        std::size_t top = 0;
        std::size_t left = 0;
    };

    namespace detail
    {
        [[nodiscard]] inline bool is_cont(char c)
        {
            return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
        }

        [[nodiscard]] inline bool is_blank(char c)
        {
            return c == ' ' || c == '\t';
        }

        [[nodiscard]] inline std::size_t prev_boundary(std::string_view l, std::size_t col)
        {
            if (col == 0)
                return 0;
            --col;
            while (col > 0 && is_cont(l[col]))
                --col;
            return col;
        }

        [[nodiscard]] inline std::size_t next_boundary(std::string_view l, std::size_t col)
        {
            if (col >= l.size())
                return l.size();
            ++col;
            while (col < l.size() && is_cont(l[col]))
                ++col;
            return col;
        }

        [[nodiscard]] inline std::size_t leading_blank(std::string_view l)
        {
            std::size_t i = 0;
            while (i < l.size() && is_blank(l[i]))
                ++i;
            return i;
        }

        [[nodiscard]] inline Pos end_of(Pos start, std::string_view text)
        {
            std::size_t newlines = 0;
            std::size_t last = 0;
            for (std::size_t i = 0; i < text.size(); ++i)
                if (text[i] == '\n')
                {
                    ++newlines;
                    last = i + 1;
                }
            if (newlines == 0)
                return {start.line, start.col + text.size()};
            return {start.line + newlines, text.size() - last};
        }

        [[nodiscard]] inline std::string sanitize_input(std::string_view raw)
        {
            std::string out;
            out.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size();)
            {
                auto const c = static_cast<unsigned char>(raw[i]);
                if (c == '\r')
                {
                    out += '\n';
                    ++i;
                    if (i < raw.size() && raw[i] == '\n')
                        ++i;
                    continue;
                }
                if (c < 0x80)
                {
                    if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F))
                        out += static_cast<char>(c);
                    ++i;
                    continue;
                }
                auto const d = text::decode_utf8(raw, i);
                if (d.valid)
                    out.append(raw.substr(i, d.length));
                i += d.length;
            }
            return out;
        }

        [[nodiscard]] inline std::string sanitize_preview(std::string_view raw)
        {
            std::string out;
            for (std::size_t i = 0; i < raw.size();)
            {
                auto const c = static_cast<unsigned char>(raw[i]);
                if (c < 0x80)
                {
                    if (c == 0)
                        text::append_utf8(out, 0xFFFD);
                    else
                        out += static_cast<char>(c);
                    ++i;
                    continue;
                }
                auto const d = text::decode_utf8(raw, i);
                if (d.valid)
                    out.append(raw.substr(i, d.length));
                else
                    text::append_utf8(out, 0xFFFD);
                i += d.length;
            }
            return out;
        }

        [[nodiscard]] inline bool is_word_byte(char c)
        {
            auto const u = static_cast<unsigned char>(c);
            return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u >= 0x80;
        }

        enum class CharClass
        {
            Space,
            Word,
            Punct,
        };

        [[nodiscard]] inline CharClass classify(char c)
        {
            if (is_blank(c))
                return CharClass::Space;
            return is_word_byte(c) ? CharClass::Word : CharClass::Punct;
        }

        [[nodiscard]] inline bool match_at(std::string_view line, std::size_t i, std::string_view needle, bool match_case)
        {
            if (i + needle.size() > line.size())
                return false;
            for (std::size_t k = 0; k < needle.size(); ++k)
            {
                auto a = line[i + k];
                auto b = needle[k];
                if (!match_case)
                {
                    if (a >= 'A' && a <= 'Z')
                        a = static_cast<char>(a - 'A' + 'a');
                    if (b >= 'A' && b <= 'Z')
                        b = static_cast<char>(b - 'A' + 'a');
                }
                if (a != b)
                    return false;
            }
            return true;
        }
    } // namespace detail

    class Buffer
    {
    public:
        std::vector<std::string> lines{std::string{}};
        Pos cursor;
        std::optional<Pos> anchor;
        int indent_width = 4;
        bool insert_spaces = true;
        bool read_only = false;
        bool bom = false;
        Eol eol = Eol::Lf;
        bool final_newline = false;
        Limits limits;

        LoadResult load(std::string_view bytes, Limits new_limits = {})
        {
            limits = new_limits;
            reset_state();
            lines.assign(1, std::string{});
            cursor = {};
            anchor.reset();
            read_only = false;
            bom = false;
            eol = Eol::Lf;
            final_newline = false;

            LoadResult result;
            if (bytes.size() > limits.max_bytes)
                result = {false, std::format("the file is larger than {} bytes", limits.max_bytes)};
            else if (bytes.find('\0') != std::string_view::npos)
                result = {false, "the file contains NUL bytes and is not text"};
            else
            {
                bool valid = true;
                for (std::size_t i = 0; i < bytes.size();)
                {
                    auto const d = text::decode_utf8(bytes, i);
                    if (!d.valid)
                    {
                        valid = false;
                        break;
                    }
                    i += d.length;
                }
                if (!valid)
                    result = {false, "the file is not valid UTF-8"};
            }

            if (!result.editable)
            {
                std::string_view view = bytes;
                if (view.size() > limits.max_bytes)
                {
                    view = view.substr(0, limits.max_bytes);
                    auto const nl = view.rfind('\n');
                    if (nl != std::string_view::npos)
                        view = view.substr(0, nl + 1);
                }
                auto clean = detail::sanitize_preview(view);
                auto split = split_text(clean);
                if (split.size() > limits.max_lines)
                    split.resize(limits.max_lines);
                lines = std::move(split);
                if (lines.empty())
                    lines.emplace_back();
                read_only = true;
                return result;
            }

            std::string_view body = bytes;
            if (body.starts_with("\xEF\xBB\xBF"))
            {
                bom = true;
                body.remove_prefix(3);
            }
            std::size_t crlf = 0;
            std::size_t lf = 0;
            for (std::size_t i = 0; i < body.size(); ++i)
                if (body[i] == '\n')
                {
                    if (i > 0 && body[i - 1] == '\r')
                        ++crlf;
                    else
                        ++lf;
                }
            eol = crlf > lf ? Eol::Crlf : Eol::Lf;
            std::vector<std::string> split;
            std::size_t start = 0;
            while (true)
            {
                auto const nl = body.find('\n', start);
                if (nl == std::string_view::npos)
                {
                    split.emplace_back(body.substr(start));
                    break;
                }
                auto end = nl;
                if (eol == Eol::Crlf && end > start && body[end - 1] == '\r')
                    --end;
                split.emplace_back(body.substr(start, end - start));
                start = nl + 1;
            }
            if (split.size() > 1 && split.back().empty())
            {
                split.pop_back();
                final_newline = true;
            }
            else if (body.ends_with('\n'))
                final_newline = true;
            if (split.size() > limits.max_lines)
            {
                result = {false, std::format("the file has more than {} lines", limits.max_lines)};
                split.resize(limits.max_lines);
                read_only = true;
            }
            lines = std::move(split);
            return result;
        }

        [[nodiscard]] std::string text() const
        {
            std::string out;
            if (bom)
                out += "\xEF\xBB\xBF";
            std::string_view const sep = eol == Eol::Crlf ? "\r\n" : "\n";
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (i > 0)
                    out += sep;
                out += lines[i];
            }
            if (final_newline)
                out += sep;
            return out;
        }

        [[nodiscard]] std::size_t line_count() const { return lines.size(); }

        [[nodiscard]] bool has_selection() const { return anchor && *anchor != cursor; }

        [[nodiscard]] std::pair<Pos, Pos> selection() const
        {
            if (!anchor)
                return {cursor, cursor};
            return *anchor < cursor ? std::pair{*anchor, cursor} : std::pair{cursor, *anchor};
        }

        [[nodiscard]] Pos selection_start() const { return selection().first; }

        [[nodiscard]] Pos selection_end() const { return selection().second; }

        void select_range(Pos a, Pos c)
        {
            break_group();
            a = clamp(a);
            cursor = clamp(c);
            anchor = a;
            update_desired();
        }

        [[nodiscard]] Pos clamp(Pos p) const
        {
            if (lines.empty())
                return {};
            if (p.line >= lines.size())
                return {lines.size() - 1, lines.back().size()};
            auto const& l = lines[p.line];
            if (p.col >= l.size())
                return {p.line, l.size()};
            while (p.col > 0 && detail::is_cont(l[p.col]))
                --p.col;
            return p;
        }

        [[nodiscard]] std::size_t display_col(Pos p) const
        {
            p = clamp(p);
            auto const& l = lines[p.line];
            auto const tw = static_cast<std::size_t>(tab_width());
            std::size_t x = 0;
            for (std::size_t i = 0; i < p.col;)
            {
                if (l[i] == '\t')
                {
                    x += tw - (x % tw);
                    ++i;
                    continue;
                }
                auto const d = text::decode_utf8(l, i);
                x += d.valid ? static_cast<std::size_t>(text::char_width(d.cp)) : 1u;
                i += d.length;
            }
            return x;
        }

        [[nodiscard]] Pos pos_at_display(std::size_t line, std::size_t dc) const
        {
            if (lines.empty())
                return {};
            line = std::min(line, lines.size() - 1);
            auto const& l = lines[line];
            auto const tw = static_cast<std::size_t>(tab_width());
            std::size_t x = 0;
            for (std::size_t i = 0; i < l.size();)
            {
                std::size_t w = 0;
                std::size_t len = 1;
                if (l[i] == '\t')
                    w = tw - (x % tw);
                else
                {
                    auto const d = text::decode_utf8(l, i);
                    len = d.length;
                    w = d.valid ? static_cast<std::size_t>(text::char_width(d.cp)) : 1u;
                }
                if (dc < x + w)
                    return {line, (dc - x) * 2 < w ? i : i + len};
                x += w;
                i += len;
            }
            return {line, l.size()};
        }

        void ensure_visible(View& v, int height, int width) const
        {
            if (height <= 0 || width <= 0)
                return;
            auto const h = static_cast<std::size_t>(height);
            auto const w = static_cast<std::size_t>(width);
            auto const c = clamp(cursor);
            auto const margin_wanted = h >= 3 ? std::size_t{1} : std::size_t{0};
            auto const top_margin = std::min(margin_wanted, c.line);
            auto const bottom_margin = std::min(margin_wanted, lines.size() - 1 - c.line);
            if (c.line < v.top + top_margin)
                v.top = c.line - top_margin;
            else if (c.line + bottom_margin >= v.top + h)
                v.top = c.line + bottom_margin + 1 - h;
            if (v.top >= lines.size())
                v.top = lines.size() - 1;
            auto const dc = display_col(c);
            if (dc < v.left)
                v.left = dc;
            else if (dc >= v.left + w)
                v.left = dc + 1 - w;
        }

        void set_cursor(Pos p, bool select = false)
        {
            break_group();
            begin_move(select);
            cursor = clamp(p);
            end_move();
        }

        void goto_line(std::size_t line, std::size_t col = 1)
        {
            line = line == 0 ? 0 : line - 1;
            col = col == 0 ? 0 : col - 1;
            set_cursor({line, col});
        }

        void left(bool select = false)
        {
            break_group();
            if (!select && has_selection())
            {
                cursor = selection().first;
                anchor.reset();
                update_desired();
                return;
            }
            begin_move(select);
            if (cursor.col > 0)
                cursor.col = detail::prev_boundary(lines[cursor.line], cursor.col);
            else if (cursor.line > 0)
            {
                --cursor.line;
                cursor.col = lines[cursor.line].size();
            }
            end_move();
        }

        void right(bool select = false)
        {
            break_group();
            if (!select && has_selection())
            {
                cursor = selection().second;
                anchor.reset();
                update_desired();
                return;
            }
            begin_move(select);
            auto const& l = lines[cursor.line];
            if (cursor.col < l.size())
                cursor.col = detail::next_boundary(l, cursor.col);
            else if (cursor.line + 1 < lines.size())
            {
                ++cursor.line;
                cursor.col = 0;
            }
            end_move();
        }

        void up(bool select = false) { vertical(-1, select); }

        void down(bool select = false) { vertical(1, select); }

        void page_up(std::size_t n, bool select = false) { vertical(-static_cast<std::ptrdiff_t>(std::max<std::size_t>(n, 1)), select); }

        void page_down(std::size_t n, bool select = false) { vertical(static_cast<std::ptrdiff_t>(std::max<std::size_t>(n, 1)), select); }

        void home(bool select = false)
        {
            break_group();
            begin_move(select);
            auto const first = detail::leading_blank(lines[cursor.line]);
            cursor.col = (cursor.col == first && first != 0) ? 0 : first;
            end_move();
        }

        void end(bool select = false)
        {
            break_group();
            begin_move(select);
            cursor.col = lines[cursor.line].size();
            end_move();
        }

        void buffer_start(bool select = false)
        {
            break_group();
            begin_move(select);
            cursor = {};
            end_move();
        }

        void buffer_end(bool select = false)
        {
            break_group();
            begin_move(select);
            cursor = {lines.size() - 1, lines.back().size()};
            end_move();
        }

        void select_all()
        {
            break_group();
            anchor = Pos{};
            cursor = {lines.size() - 1, lines.back().size()};
            update_desired();
        }

        void word_left(bool select = false)
        {
            break_group();
            begin_move(select);
            auto const& l = lines[cursor.line];
            if (cursor.col == 0)
            {
                if (cursor.line > 0)
                {
                    --cursor.line;
                    cursor.col = lines[cursor.line].size();
                }
            }
            else
            {
                auto c = cursor.col;
                while (c > 0 && detail::classify(l[c - 1]) == detail::CharClass::Space)
                    --c;
                if (c > 0)
                {
                    auto const cls = detail::classify(l[c - 1]);
                    while (c > 0 && detail::classify(l[c - 1]) == cls)
                        --c;
                }
                cursor.col = c;
            }
            end_move();
        }

        void word_right(bool select = false)
        {
            break_group();
            begin_move(select);
            auto const& l = lines[cursor.line];
            if (cursor.col >= l.size())
            {
                if (cursor.line + 1 < lines.size())
                {
                    ++cursor.line;
                    cursor.col = 0;
                }
            }
            else
            {
                auto c = cursor.col;
                while (c < l.size() && detail::classify(l[c]) == detail::CharClass::Space)
                    ++c;
                if (c < l.size())
                {
                    auto const cls = detail::classify(l[c]);
                    while (c < l.size() && detail::classify(l[c]) == cls)
                        ++c;
                }
                cursor.col = c;
            }
            end_move();
        }

        [[nodiscard]] std::string selected_text() const
        {
            if (!has_selection())
                return {};
            auto const [a, b] = selection();
            return extract(a, b);
        }

        std::string cut()
        {
            if (read_only || !has_selection())
                return {};
            auto out = selected_text();
            auto& s = open(Kind::Other);
            delete_selection(s);
            close(s, Kind::Other);
            return out;
        }

        bool paste(std::string_view s) { return insert_text(s, true); }

        bool insert(std::string_view s) { return insert_text(s, false); }

        bool newline()
        {
            if (read_only)
                return false;
            auto& s = open(Kind::Other);
            delete_selection(s);
            auto const& line = lines[cursor.line];
            auto const before = std::string_view(line).substr(0, cursor.col);
            auto const after = std::string_view(line).substr(cursor.col);
            auto const ws = detail::leading_blank(before);
            std::string indent(before.substr(0, ws));
            auto trimmed = before;
            while (!trimmed.empty() && detail::is_blank(trimmed.back()))
                trimmed.remove_suffix(1);
            auto const lead_after = detail::leading_blank(after);
            auto const unit = indent_unit();
            bool const brace_block = !trimmed.empty() && trimmed.back() == '{' && lead_after < after.size() && after[lead_after] == '}';
            bool const opener = !trimmed.empty() && (trimmed.back() == '{' || trimmed.back() == '(' || trimmed.back() == '[');
            std::string text;
            Pos final_pos;
            if (brace_block)
            {
                text = "\n" + indent + unit + "\n" + indent;
                final_pos = {cursor.line + 1, indent.size() + unit.size()};
            }
            else
            {
                text = "\n" + indent + (opener ? unit : std::string{});
                final_pos = {cursor.line + 1, text.size() - 1};
            }
            replace(s, cursor, {cursor.line, cursor.col + lead_after}, text);
            cursor = final_pos;
            anchor.reset();
            close(s, Kind::Other);
            return true;
        }

        bool backspace()
        {
            if (read_only)
                return false;
            if (has_selection())
            {
                auto& s = open(Kind::Other);
                delete_selection(s);
                close(s, Kind::Other);
                return true;
            }
            if (cursor.line == 0 && cursor.col == 0)
                return false;
            auto& s = open(Kind::Backspace);
            anchor.reset();
            if (cursor.col == 0)
            {
                Pos const prev{cursor.line - 1, lines[cursor.line - 1].size()};
                replace(s, prev, cursor, "");
                cursor = prev;
            }
            else
            {
                auto const& l = lines[cursor.line];
                std::size_t target = detail::prev_boundary(l, cursor.col);
                bool all_spaces = true;
                for (std::size_t i = 0; i < cursor.col; ++i)
                    if (l[i] != ' ')
                    {
                        all_spaces = false;
                        break;
                    }
                if (all_spaces)
                {
                    auto const w = static_cast<std::size_t>(tab_width());
                    target = (cursor.col - 1) / w * w;
                }
                Pos const from{cursor.line, target};
                replace(s, from, cursor, "");
                cursor = from;
            }
            close(s, Kind::Backspace);
            return true;
        }

        bool del()
        {
            if (read_only)
                return false;
            if (has_selection())
            {
                auto& s = open(Kind::Other);
                delete_selection(s);
                close(s, Kind::Other);
                return true;
            }
            auto const& l = lines[cursor.line];
            if (cursor.col >= l.size() && cursor.line + 1 >= lines.size())
                return false;
            auto& s = open(Kind::Delete);
            anchor.reset();
            Pos to = cursor.col >= l.size() ? Pos{cursor.line + 1, 0} : Pos{cursor.line, detail::next_boundary(l, cursor.col)};
            replace(s, cursor, to, "");
            close(s, Kind::Delete);
            return true;
        }

        bool tab()
        {
            if (read_only)
                return false;
            if (has_selection() && selection().first.line != selection().second.line)
                return indent_lines();
            std::string text;
            if (insert_spaces)
            {
                auto const w = static_cast<std::size_t>(tab_width());
                auto const at = has_selection() ? selection().first : cursor;
                text.assign(w - display_col(at) % w, ' ');
            }
            else
                text = "\t";
            return insert_text(text, true);
        }

        bool untab()
        {
            if (read_only)
                return false;
            auto const [lo, hi] = line_range();
            std::vector<std::size_t> removed(hi - lo + 1, 0);
            bool any = false;
            for (std::size_t i = lo; i <= hi; ++i)
            {
                auto const& l = lines[i];
                if (!l.empty() && l[0] == '\t')
                    removed[i - lo] = 1;
                else
                {
                    std::size_t n = 0;
                    while (n < l.size() && n < static_cast<std::size_t>(tab_width()) && l[n] == ' ')
                        ++n;
                    removed[i - lo] = n;
                }
                any = any || removed[i - lo] > 0;
            }
            if (!any)
                return false;
            auto& s = open(Kind::Other);
            auto const adjust = [&](Pos p) {
                if (p.line >= lo && p.line <= hi)
                {
                    auto const r = removed[p.line - lo];
                    p.col = p.col >= r ? p.col - r : 0;
                }
                return p;
            };
            auto const new_cursor = adjust(cursor);
            auto const new_anchor = anchor ? std::optional<Pos>(adjust(*anchor)) : std::nullopt;
            for (std::size_t i = lo; i <= hi; ++i)
                if (removed[i - lo] > 0)
                    replace(s, {i, 0}, {i, removed[i - lo]}, "");
            cursor = new_cursor;
            anchor = new_anchor;
            close(s, Kind::Other);
            return true;
        }

        bool delete_line()
        {
            if (read_only)
                return false;
            auto const [lo, hi] = line_range();
            auto& s = open(Kind::Other);
            std::size_t new_line = lo;
            if (lo == 0 && hi + 1 == lines.size())
                replace(s, {0, 0}, {hi, lines[hi].size()}, "");
            else if (hi + 1 == lines.size())
            {
                replace(s, {lo - 1, lines[lo - 1].size()}, {hi, lines[hi].size()}, "");
                new_line = lo - 1;
            }
            else
                replace(s, {lo, 0}, {hi + 1, 0}, "");
            anchor.reset();
            cursor = pos_at_display(new_line, desired);
            close(s, Kind::Other);
            return true;
        }

        bool duplicate_line()
        {
            if (read_only)
                return false;
            auto const [lo, hi] = line_range();
            std::string text;
            for (std::size_t i = lo; i <= hi; ++i)
            {
                text += '\n';
                text += lines[i];
            }
            auto& s = open(Kind::Other);
            replace(s, {hi, lines[hi].size()}, {hi, lines[hi].size()}, text);
            auto const shift = hi - lo + 1;
            cursor.line += shift;
            if (anchor)
                anchor->line += shift;
            close(s, Kind::Other);
            return true;
        }

        bool undo()
        {
            if (read_only || undo_.empty())
                return false;
            Step s = std::move(undo_.back());
            undo_.pop_back();
            undo_bytes_ -= s.bytes;
            for (auto it = s.ops.rbegin(); it != s.ops.rend(); ++it)
                raw_replace(it->start, detail::end_of(it->start, it->inserted), it->removed);
            cursor = clamp(s.cursor_before);
            anchor = s.anchor_before ? std::optional<Pos>(clamp(*s.anchor_before)) : std::nullopt;
            revision_ = s.rev_before;
            redo_.push_back(std::move(s));
            last_kind_ = Kind::None;
            update_desired();
            return true;
        }

        bool redo()
        {
            if (read_only || redo_.empty())
                return false;
            Step s = std::move(redo_.back());
            redo_.pop_back();
            for (auto const& op : s.ops)
                raw_replace(op.start, detail::end_of(op.start, op.removed), op.inserted);
            cursor = clamp(s.cursor_after);
            anchor = s.anchor_after ? std::optional<Pos>(clamp(*s.anchor_after)) : std::nullopt;
            revision_ = s.rev_after;
            undo_bytes_ += s.bytes;
            undo_.push_back(std::move(s));
            last_kind_ = Kind::None;
            trim_history();
            update_desired();
            return true;
        }

        [[nodiscard]] std::size_t undo_depth() const { return undo_.size(); }

        [[nodiscard]] std::size_t redo_depth() const { return redo_.size(); }

        [[nodiscard]] std::uint64_t revision() const { return revision_; }

        [[nodiscard]] bool modified() const { return revision_ != saved_revision_; }

        void mark_saved()
        {
            saved_revision_ = revision_;
            break_group();
        }

        [[nodiscard]] std::optional<std::pair<Pos, Pos>> find(std::string_view needle, Pos from, bool forward, bool match_case) const
        {
            if (needle.empty() || needle.find('\n') != std::string_view::npos)
                return std::nullopt;
            from = clamp(from);
            auto const n = lines.size();
            for (std::size_t k = 0; k <= n; ++k)
            {
                auto const li = forward ? (from.line + k) % n : (from.line + n - k % n) % n;
                auto const& l = lines[li];
                if (forward)
                {
                    std::size_t begin = k == 0 ? from.col : 0;
                    for (std::size_t i = begin; i + needle.size() <= l.size(); ++i)
                        if (detail::match_at(l, i, needle, match_case))
                            return std::pair{Pos{li, i}, Pos{li, i + needle.size()}};
                }
                else if (l.size() >= needle.size())
                {
                    for (std::size_t i = l.size() - needle.size() + 1; i-- > 0;)
                    {
                        if (k == 0 && i >= from.col)
                            continue;
                        if (detail::match_at(l, i, needle, match_case))
                            return std::pair{Pos{li, i}, Pos{li, i + needle.size()}};
                    }
                }
            }
            return std::nullopt;
        }

        bool find_next(std::string_view needle, bool match_case)
        {
            auto const from = has_selection() ? selection().second : cursor;
            return select_match(find(needle, from, true, match_case));
        }

        bool find_prev(std::string_view needle, bool match_case)
        {
            auto const from = has_selection() ? selection().first : cursor;
            return select_match(find(needle, from, false, match_case));
        }

        [[nodiscard]] std::size_t count_matches(std::string_view needle, bool match_case) const
        {
            if (needle.empty() || needle.find('\n') != std::string_view::npos)
                return 0;
            std::size_t count = 0;
            for (auto const& l : lines)
                for (std::size_t i = 0; i + needle.size() <= l.size();)
                {
                    if (detail::match_at(l, i, needle, match_case))
                    {
                        ++count;
                        i += needle.size();
                    }
                    else
                        ++i;
                }
            return count;
        }

    private:
        enum class Kind
        {
            None,
            Other,
            Typing,
            Backspace,
            Delete,
        };

        struct Op
        {
            Pos start;
            std::string removed;
            std::string inserted;
        };

        struct Step
        {
            std::vector<Op> ops;
            Pos cursor_before;
            std::optional<Pos> anchor_before;
            Pos cursor_after;
            std::optional<Pos> anchor_after;
            std::uint64_t rev_before = 0;
            std::uint64_t rev_after = 0;
            std::size_t bytes = 0;
        };

        std::deque<Step> undo_;
        std::vector<Step> redo_;
        std::size_t undo_bytes_ = 0;
        std::uint64_t revision_ = 0;
        std::uint64_t saved_revision_ = 0;
        std::uint64_t next_rev_ = 0;
        Kind last_kind_ = Kind::None;
        bool fresh_ = false;
        std::size_t ops_before_ = 0;
        std::size_t desired = 0;
        std::size_t desired_keep = 0;

        [[nodiscard]] int tab_width() const { return std::clamp(indent_width, 1, 8); }

        [[nodiscard]] std::string indent_unit() const { return insert_spaces ? std::string(static_cast<std::size_t>(tab_width()), ' ') : std::string("\t"); }

        void reset_state()
        {
            undo_.clear();
            redo_.clear();
            undo_bytes_ = 0;
            revision_ = 0;
            saved_revision_ = 0;
            next_rev_ = 0;
            last_kind_ = Kind::None;
            desired = 0;
        }

        static std::vector<std::string> split_text(std::string_view t)
        {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (true)
            {
                auto const nl = t.find('\n', start);
                if (nl == std::string_view::npos)
                {
                    out.emplace_back(t.substr(start));
                    break;
                }
                out.emplace_back(t.substr(start, nl - start));
                start = nl + 1;
            }
            return out;
        }

        void break_group() { last_kind_ = Kind::None; }

        void update_desired()
        {
            cursor = clamp(cursor);
            if (anchor)
                anchor = clamp(*anchor);
            desired = display_col(cursor);
            desired_keep = desired;
        }

        void begin_move(bool select)
        {
            cursor = clamp(cursor);
            if (select)
            {
                if (!anchor)
                    anchor = cursor;
            }
            else
                anchor.reset();
        }

        void end_move() { update_desired(); }

        void vertical(std::ptrdiff_t delta, bool select)
        {
            break_group();
            begin_move(select);
            auto const keep = desired;
            auto const target = static_cast<std::ptrdiff_t>(cursor.line) + delta;
            if (target < 0)
            {
                cursor = {};
                update_desired();
                return;
            }
            if (static_cast<std::size_t>(target) >= lines.size())
            {
                cursor = {lines.size() - 1, lines.back().size()};
                update_desired();
                return;
            }
            cursor = pos_at_display(static_cast<std::size_t>(target), keep);
            desired = keep;
            desired_keep = keep;
        }

        [[nodiscard]] std::string extract(Pos a, Pos b) const
        {
            if (a.line == b.line)
                return lines[a.line].substr(a.col, b.col - a.col);
            std::string out = lines[a.line].substr(a.col);
            for (std::size_t i = a.line + 1; i < b.line; ++i)
            {
                out += '\n';
                out += lines[i];
            }
            out += '\n';
            out += lines[b.line].substr(0, b.col);
            return out;
        }

        void raw_replace(Pos a, Pos b, std::string_view text)
        {
            auto const head = lines[a.line].substr(0, a.col);
            auto const tail = lines[b.line].substr(b.col);
            auto pieces = split_text(text);
            pieces.front().insert(0, head);
            pieces.back() += tail;
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(a.line), lines.begin() + static_cast<std::ptrdiff_t>(b.line) + 1);
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(a.line), std::make_move_iterator(pieces.begin()), std::make_move_iterator(pieces.end()));
        }

        Pos replace(Step& s, Pos a, Pos b, std::string text)
        {
            Op op{a, extract(a, b), std::move(text)};
            raw_replace(a, b, op.inserted);
            auto const end = detail::end_of(a, op.inserted);
            s.bytes += op.removed.size() + op.inserted.size();
            undo_bytes_ += op.removed.size() + op.inserted.size();
            s.ops.push_back(std::move(op));
            return end;
        }

        Step& open(Kind kind)
        {
            cursor = clamp(cursor);
            if (anchor)
                anchor = clamp(*anchor);
            bool const merge = kind != Kind::Other && last_kind_ == kind && !undo_.empty() && undo_.back().cursor_after == cursor && !has_selection();
            if (merge)
            {
                fresh_ = false;
                ops_before_ = undo_.back().ops.size();
                return undo_.back();
            }
            Step s;
            s.cursor_before = cursor;
            s.anchor_before = anchor;
            s.rev_before = revision_;
            undo_.push_back(std::move(s));
            fresh_ = true;
            ops_before_ = 0;
            return undo_.back();
        }

        void close(Step& s, Kind kind)
        {
            if (s.ops.size() == ops_before_)
            {
                if (fresh_)
                    undo_.pop_back();
                return;
            }
            redo_.clear();
            s.cursor_after = cursor;
            s.anchor_after = anchor;
            s.rev_after = revision_ = ++next_rev_;
            last_kind_ = kind;
            trim_history();
            if (undo_.empty())
                last_kind_ = Kind::None;
            update_desired();
        }

        void trim_history()
        {
            while (!undo_.empty() && (undo_.size() > limits.max_undo || undo_bytes_ > limits.max_undo_bytes))
            {
                undo_bytes_ -= undo_.front().bytes;
                undo_.pop_front();
            }
        }

        void delete_selection(Step& s)
        {
            if (!has_selection())
            {
                anchor.reset();
                return;
            }
            auto const [a, b] = selection();
            replace(s, a, b, "");
            cursor = a;
            anchor.reset();
        }

        bool insert_text(std::string_view raw, bool force_other)
        {
            if (read_only)
                return false;
            auto text = detail::sanitize_input(raw);
            if (text.empty())
                return false;
            bool const sel = has_selection();
            bool const single = !force_other && !sel && text != "\n" && text::decode_utf8(text, 0).length == text.size();
            if (single && text.size() == 1 && (text[0] == '}' || text[0] == ')' || text[0] == ']') && cursor.col > 0)
            {
                auto const& l = lines[cursor.line];
                if (detail::leading_blank(l) >= cursor.col)
                {
                    std::size_t remove = 0;
                    if (l[0] == '\t')
                        remove = 1;
                    else
                        while (remove < cursor.col && remove < static_cast<std::size_t>(tab_width()) && l[remove] == ' ')
                            ++remove;
                    auto& s = open(Kind::Other);
                    replace(s, {cursor.line, 0}, {cursor.line, remove}, "");
                    cursor.col -= remove;
                    cursor = replace(s, cursor, cursor, text);
                    anchor.reset();
                    close(s, Kind::Other);
                    return true;
                }
            }
            auto const kind = single ? Kind::Typing : Kind::Other;
            auto& s = open(kind);
            delete_selection(s);
            cursor = replace(s, cursor, cursor, std::move(text));
            anchor.reset();
            close(s, kind);
            return true;
        }

        [[nodiscard]] std::pair<std::size_t, std::size_t> line_range() const
        {
            if (has_selection())
            {
                auto const [a, b] = selection();
                auto hi = b.line;
                if (b.col == 0 && hi > a.line)
                    --hi;
                return {a.line, hi};
            }
            return {cursor.line, cursor.line};
        }

        bool indent_lines()
        {
            auto const [lo, hi] = line_range();
            auto const unit = indent_unit();
            auto const adjust = [&](Pos p) {
                if (p.line >= lo && p.line <= hi && p.col > 0 && !lines[p.line].empty())
                    p.col += unit.size();
                return p;
            };
            auto const new_cursor = adjust(cursor);
            auto const new_anchor = anchor ? std::optional<Pos>(adjust(*anchor)) : std::nullopt;
            auto& s = open(Kind::Other);
            for (std::size_t i = lo; i <= hi; ++i)
                if (!lines[i].empty())
                    replace(s, {i, 0}, {i, 0}, unit);
            cursor = new_cursor;
            anchor = new_anchor;
            close(s, Kind::Other);
            return true;
        }

        bool select_match(std::optional<std::pair<Pos, Pos>> m)
        {
            break_group();
            if (!m)
                return false;
            anchor = m->first;
            cursor = m->second;
            update_desired();
            return true;
        }
    };
} // namespace learndc::textedit
