module;

#include "ftxui.hpp"

module learndc.tui;

import std;
import learndc.text;
import learndc.catalog;
import learndc.progress;
import learndc.runner;
import learndc.doctor;
import learndc.diff;
import learndc.settings;
import learndc.app;
import learndc.markdown;
import learndc.activity;
import learndc.tui.model;

namespace learndc::tui
{
    namespace
    {
        using model::LayoutMode;
        using model::Model;
        using model::Pane;
        using model::ThemeId;

        enum class Role
        {
            Text,
            Muted,
            Accent,
            Ok,
            Fail,
            Warn,
            Border,
            Focus,
            Code,
            Add,
            Del,
            Key,
            Inline,
            Watch,
            Boss,
        };

        struct Style
        {
            Role role = Role::Text;
            bool bold = false;
            bool inverse = false;
            bool underline = false;
            bool italic = false;

            friend bool operator==(Style const&, Style const&) = default;
        };

        [[nodiscard]] constexpr Style plain(Role r, bool bold = false)
        {
            return Style{r, bold, false, false};
        }

        struct Cell
        {
            std::string glyph = " ";
            Style style;
            bool cont = false;
        };

        struct Rect
        {
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;

            [[nodiscard]] int right() const { return x + w; }
            [[nodiscard]] int bottom() const { return y + h; }
            [[nodiscard]] bool empty() const { return w <= 0 || h <= 0; }
            [[nodiscard]] Rect inset(int n) const { return {x + n, y + n, std::max(0, w - 2 * n), std::max(0, h - 2 * n)}; }
        };

        [[nodiscard]] int to_int(std::size_t n)
        {
            return static_cast<int>(n);
        }

        [[nodiscard]] std::size_t to_size(int n)
        {
            return n < 0 ? 0u : static_cast<std::size_t>(n);
        }

        [[nodiscard]] int width_of(std::string_view s)
        {
            return to_int(text::display_width(s));
        }

        class Grid
        {
        public:
            Grid(int width, int height) : m_w(std::max(0, width)), m_h(std::max(0, height)), m_cells(to_size(m_w) * to_size(m_h)) {}

            [[nodiscard]] int width() const { return m_w; }
            [[nodiscard]] int height() const { return m_h; }

            [[nodiscard]] Cell& at(int x, int y) { return m_cells[to_size(y) * to_size(m_w) + to_size(x)]; }

            [[nodiscard]] Cell const& at(int x, int y) const { return m_cells[to_size(y) * to_size(m_w) + to_size(x)]; }

            void clear_wide(int x, int y)
            {
                if (x < 0 || x >= m_w || y < 0 || y >= m_h)
                    return;
                auto& c = at(x, y);
                if (c.cont && x > 0)
                {
                    auto& lead = at(x - 1, y);
                    lead.glyph = " ";
                    lead.cont = false;
                    c.cont = false;
                    c.glyph = " ";
                }
                else if (!c.cont && x + 1 < m_w && at(x + 1, y).cont)
                {
                    auto& tail = at(x + 1, y);
                    tail.cont = false;
                    tail.glyph = " ";
                    c.glyph = " ";
                }
            }

            int put(int x, int y, std::string_view s, Style style, Rect clip)
            {
                int cx = x;
                if (y < clip.y || y >= clip.bottom() || y < 0 || y >= m_h)
                    return 0;
                for (std::size_t i = 0; i < s.size();)
                {
                    auto const d = text::decode_utf8(s, i);
                    auto const glyph = s.substr(i, d.length);
                    i += d.length;
                    int const cw = d.valid ? text::char_width(d.cp) : 1;
                    if (cw == 0)
                    {
                        if (cx > clip.x && cx - 1 < m_w && cx - 1 >= 0)
                        {
                            auto& prev = at(cx - 1, y);
                            if (!prev.cont)
                                prev.glyph += std::string(glyph);
                        }
                        continue;
                    }
                    if (cx >= clip.x && cx + cw <= clip.right() && cx >= 0 && cx + cw <= m_w)
                    {
                        for (int k = 0; k < cw; ++k)
                            clear_wide(cx + k, y);
                        auto& c = at(cx, y);
                        c.glyph = d.valid ? std::string(glyph) : std::string("?");
                        c.style = style;
                        c.cont = false;
                        if (cw == 2)
                        {
                            auto& n = at(cx + 1, y);
                            n.glyph.clear();
                            n.style = style;
                            n.cont = true;
                        }
                    }
                    cx += cw;
                }
                return cx - x;
            }

            void fill(Rect r, Style style)
            {
                for (int y = std::max(0, r.y); y < std::min(m_h, r.bottom()); ++y)
                    for (int x = std::max(0, r.x); x < std::min(m_w, r.right()); ++x)
                    {
                        clear_wide(x, y);
                        auto& c = at(x, y);
                        c.glyph = " ";
                        c.style = style;
                        c.cont = false;
                    }
            }

        private:
            int m_w;
            int m_h;
            std::vector<Cell> m_cells;
        };

        struct Span
        {
            std::string text;
            Style style;
        };

        using Line = std::vector<Span>;

        [[nodiscard]] int line_width(Line const& line)
        {
            int w = 0;
            for (auto const& s : line)
                w += width_of(s.text);
            return w;
        }

        struct Glyphs
        {
            std::string_view tl, tr, bl, br, h, v, watch;
            std::string_view sel, done, active, todo, full, empty, ell, dot, pass, fail, warn, skip, thumb, bullet, skull, radio_on, radio_off;
            int status_width = 1;
        };

        [[nodiscard]] Glyphs glyphs_for(bool ascii)
        {
            Glyphs g;
            if (ascii)
            {
                g.tl = g.tr = g.bl = g.br = "+";
                g.h = "-";
                g.v = "|";
                g.sel = ">";
                g.done = "[x]";
                g.active = "[~]";
                g.todo = "[ ]";
                g.full = "#";
                g.empty = "-";
                g.ell = "~";
                g.dot = "-";
                g.pass = "[ok]";
                g.fail = "[x]";
                g.warn = "[!]";
                g.skip = "[-]";
                g.thumb = "#";
                g.bullet = "-";
                g.watch = "*";
                g.skull = "[X]";
                g.radio_on = "(*)";
                g.radio_off = "( )";
                g.status_width = 3;
            }
            else
            {
                g.tl = "\xE2\x95\xAD";
                g.tr = "\xE2\x95\xAE";
                g.bl = "\xE2\x95\xB0";
                g.br = "\xE2\x95\xAF";
                g.h = "\xE2\x94\x80";
                g.v = "\xE2\x94\x82";
                g.sel = "\xE2\x80\xBA";
                g.done = "\xE2\x9C\x93";
                g.active = "\xE2\x97\x90";
                g.todo = "\xE2\x97\x8B";
                g.full = "\xE2\x96\x88";
                g.empty = "\xE2\x96\x91";
                g.ell = "\xE2\x80\xA6";
                g.dot = "\xC2\xB7";
                g.pass = "\xE2\x9C\x93";
                g.fail = "\xE2\x9C\x97";
                g.warn = "!";
                g.skip = "\xE2\x97\x8B";
                g.thumb = "\xE2\x94\x83";
                g.bullet = "\xE2\x80\xA2";
                g.watch = "\xE2\x97\x89";
                g.skull = "\xE2\x98\xA0";
                g.radio_on = "\xE2\x97\x89";
                g.radio_off = "\xE2\x97\x8B";
                g.status_width = 1;
            }
            return g;
        }

        struct Ui
        {
            Grid& g;
            Model const& m;
            model::Size size;
            bool ascii;
            Glyphs gl;

            Ui(Grid& grid, Model const& model, model::Size sz)
                : g(grid), m(model), size(sz), ascii(model.appearance.ascii), gl(glyphs_for(model.appearance.ascii))
            {
            }

            [[nodiscard]] std::string sep() const { return std::format(" {} ", gl.dot); }
        };

        [[nodiscard]] std::string fit(Ui const& ui, std::string_view s, int width)
        {
            if (width <= 0)
                return {};
            if (width_of(s) <= width)
                return std::string(s);
            if (width == 1)
                return std::string(ui.gl.ell);
            std::string out;
            int used = 0;
            for (std::size_t i = 0; i < s.size();)
            {
                auto const d = text::decode_utf8(s, i);
                int const cw = d.valid ? text::char_width(d.cp) : 1;
                if (used + cw > width - 1)
                    break;
                out += s.substr(i, d.length);
                used += cw;
                i += d.length;
            }
            out += ui.gl.ell;
            return out;
        }

        [[nodiscard]] std::string pad_right(std::string_view s, int width)
        {
            auto const w = width_of(s);
            std::string out(s);
            if (w < width)
                out.append(to_size(width - w), ' ');
            return out;
        }

        [[nodiscard]] std::string pad_left(std::string_view s, int width)
        {
            auto const w = width_of(s);
            if (w >= width)
                return std::string(s);
            return std::string(to_size(width - w), ' ') + std::string(s);
        }

        [[nodiscard]] std::string clean_line(std::string_view s)
        {
            return text::replace_all(text::sanitize(s), "\t", "    ");
        }

        void draw_line(Ui& ui, Rect area, int y, Line const& line, bool selected = false)
        {
            if (y < area.y || y >= area.bottom())
                return;
            Rect const row{area.x, y, area.w, 1};
            if (selected)
                ui.g.fill(Rect{area.x - 1, y, area.w + 2, 1}, Style{Role::Text, true, true, false});
            int x = area.x;
            for (auto const& span : line)
            {
                Style s = span.style;
                if (selected)
                {
                    s.inverse = true;
                    if (s.role == Role::Muted)
                        s.role = Role::Text;
                }
                x += ui.g.put(x, y, span.text, s, row);
                if (x >= area.right())
                    break;
            }
        }

        void draw_lines(Ui& ui, Rect area, std::vector<Line> const& lines, int scroll)
        {
            for (int i = 0; i < area.h; ++i)
            {
                auto const idx = scroll + i;
                if (idx >= 0 && idx < to_int(lines.size()))
                    draw_line(ui, area, area.y + i, lines[to_size(idx)]);
            }
        }

        [[nodiscard]] int first_visible(int selected, int count, int height)
        {
            if (count <= height || height <= 0)
                return 0;
            return std::clamp(selected - height / 2, 0, count - height);
        }

        [[nodiscard]] int clamp_scroll(int scroll, int content, int viewport)
        {
            return std::clamp(scroll, 0, std::max(0, content - viewport));
        }

        void draw_scrollbar(Ui& ui, Rect box, int scroll, int content, int viewport)
        {
            int const track = box.h - 2;
            if (content <= viewport || track < 1 || viewport <= 0)
                return;
            int const thumb = std::max(1, track * viewport / content);
            int const span = std::max(1, content - viewport);
            int const pos = (track - thumb) * scroll / span;
            for (int i = 0; i < thumb; ++i)
                ui.g.put(box.right() - 1, box.y + 1 + pos + i, ui.gl.thumb, plain(Role::Accent), Rect{0, 0, ui.g.width(), ui.g.height()});
        }

        [[nodiscard]] Rect content_of(Rect box)
        {
            return Rect{box.x + 2, box.y + 1, std::max(0, box.w - 4), std::max(0, box.h - 2)};
        }

        void draw_rule(Ui& ui, Rect inner, int y)
        {
            Rect const span{inner.x - 1, y, inner.w + 2, 1};
            for (int x = span.x; x < span.right(); ++x)
                ui.g.put(x, y, ui.gl.h, plain(Role::Border), span);
        }

        Rect draw_box(Ui& ui, Rect r, std::string_view title, bool focused, std::string_view note = {}, std::optional<Role> accent = std::nullopt)
        {
            if (r.w < 3 || r.h < 2)
                return {};
            Rect const all{0, 0, ui.g.width(), ui.g.height()};
            Style const border = accent ? plain(*accent, true) : plain(focused ? Role::Focus : Role::Border);
            ui.g.put(r.x, r.y, ui.gl.tl, border, all);
            ui.g.put(r.right() - 1, r.y, ui.gl.tr, border, all);
            ui.g.put(r.x, r.bottom() - 1, ui.gl.bl, border, all);
            ui.g.put(r.right() - 1, r.bottom() - 1, ui.gl.br, border, all);
            for (int x = r.x + 1; x < r.right() - 1; ++x)
            {
                ui.g.put(x, r.y, ui.gl.h, border, all);
                ui.g.put(x, r.bottom() - 1, ui.gl.h, border, all);
            }
            for (int y = r.y + 1; y < r.bottom() - 1; ++y)
            {
                ui.g.put(r.x, y, ui.gl.v, border, all);
                ui.g.put(r.right() - 1, y, ui.gl.v, border, all);
            }
            std::string note_label;
            if (!note.empty() && r.w >= 16)
                note_label = std::format(" {} ", fit(ui, note, std::max(1, r.w / 2 - 4)));
            if (!title.empty() && r.w >= 8)
            {
                int const room = r.w - 6 - (note_label.empty() ? 0 : width_of(note_label) + 1);
                auto const label = std::format(" {} ", fit(ui, title, std::max(1, room)));
                Style ts = accent ? Style{*accent, true, true, false} : (focused ? Style{Role::Focus, true, true, false} : plain(Role::Text, true));
                ui.g.put(r.x + 2, r.y, label, ts, all);
            }
            if (!note_label.empty())
                ui.g.put(r.right() - 2 - width_of(note_label), r.y, note_label, plain(Role::Muted), all);
            return content_of(r);
        }

        [[nodiscard]] Line status_glyph(Ui const& ui, progress::Status s)
        {
            switch (s)
            {
                case progress::Status::Completed:
                    return {{std::string(ui.gl.done), plain(Role::Ok, true)}};
                case progress::Status::InProgress:
                    return {{std::string(ui.gl.active), plain(Role::Warn, true)}};
                case progress::Status::NotStarted:
                    return {{std::string(ui.gl.todo), plain(Role::Muted)}};
            }
            return {};
        }

        [[nodiscard]] Role status_role(progress::Status s)
        {
            switch (s)
            {
                case progress::Status::Completed:
                    return Role::Ok;
                case progress::Status::InProgress:
                    return Role::Warn;
                case progress::Status::NotStarted:
                    return Role::Muted;
            }
            return Role::Muted;
        }

        [[nodiscard]] std::string kebab_words(std::string_view s)
        {
            return text::replace_all(s, "-", " ");
        }

        [[nodiscard]] std::string difficulty_word(catalog::Difficulty d)
        {
            return std::string(catalog::to_string(d));
        }

        [[nodiscard]] Line bar_spans(Ui const& ui, int width, int done, int total)
        {
            if (width <= 0)
                return {};
            int filled = total > 0 ? done * width / total : 0;
            if (done > 0 && filled == 0)
                filled = 1;
            filled = std::min(filled, width);
            Role const role = total > 0 && done >= total ? Role::Ok : Role::Accent;
            Line out;
            std::string a;
            for (int i = 0; i < filled; ++i)
                a += ui.gl.full;
            std::string b;
            for (int i = filled; i < width; ++i)
                b += ui.gl.empty;
            if (!a.empty())
                out.push_back({a, plain(role)});
            if (!b.empty())
                out.push_back({b, plain(Role::Muted)});
            return out;
        }

        void append(Line& dst, Line const& src)
        {
            dst.insert(dst.end(), src.begin(), src.end());
        }

        [[nodiscard]] Line text_line(std::string_view s, Style style = {})
        {
            return Line{{std::string(s), style}};
        }

        [[nodiscard]] std::vector<std::string> hanging(std::string_view body, int width, std::string_view first, std::string_view rest)
        {
            int const avail = std::max(4, width - width_of(first));
            std::vector<std::string> out;
            bool head = true;
            for (auto const& piece : text::wrap(body, to_size(avail)))
            {
                out.push_back(std::string(head ? first : rest) + piece);
                head = false;
            }
            if (out.empty())
                out.push_back(std::string(first));
            return out;
        }

        void add_wrapped(std::vector<Line>& lines, std::string_view body, int width, Style style, std::string_view indent = {})
        {
            for (auto const& piece : text::wrap(clean_line(body), to_size(std::max(4, width - width_of(indent)))))
                lines.push_back(text_line(std::string(indent) + piece, style));
        }

        [[nodiscard]] Style md_style(markdown::Kind kind)
        {
            using K = markdown::Kind;
            switch (kind)
            {
                case K::Text:
                    return plain(Role::Text);
                case K::Strong:
                    return plain(Role::Text, true);
                case K::Emphasis: {
                    Style st = plain(Role::Text);
                    st.italic = true;
                    return st;
                }
                case K::StrongEmphasis: {
                    Style st = plain(Role::Text, true);
                    st.italic = true;
                    return st;
                }
                case K::Code:
                    return plain(Role::Inline);
                case K::Heading:
                    return plain(Role::Accent, true);
                case K::CodeBlock:
                    return plain(Role::Code);
                case K::Border:
                    return plain(Role::Border);
                case K::TableHead:
                    return plain(Role::Accent, true);
                case K::Marker:
                    return plain(Role::Accent);
                case K::Link: {
                    Style st = plain(Role::Accent);
                    st.underline = true;
                    return st;
                }
                case K::Url:
                    return plain(Role::Muted);
            }
            return plain(Role::Text);
        }

        [[nodiscard]] Line from_markdown(markdown::Line const& line)
        {
            Line out;
            for (auto const& sp : line)
                out.push_back({sp.text, md_style(sp.kind)});
            return out;
        }

        [[nodiscard]] std::vector<Line> markdown_block(Ui const& ui, std::string_view source, int width, bool skip_title = false)
        {
            markdown::Options options;
            options.width = std::max(4, width);
            options.ascii = ui.ascii;
            options.skip_title = skip_title;
            std::vector<Line> out;
            for (auto const& l : markdown::render(source, options))
                out.push_back(from_markdown(l));
            return out;
        }

        void markdown_lines(Ui const& ui, std::vector<Line>& out, std::string_view source, int width, bool skip_title)
        {
            for (auto& l : markdown_block(ui, source, width, skip_title))
                out.push_back(std::move(l));
        }

        [[nodiscard]] std::string lesson_word(catalog::Exercise const& ex);

        [[nodiscard]] std::string markdown_inline(std::string_view source)
        {
            markdown::Options options;
            options.width = 4000;
            std::string out;
            for (auto const& l : markdown::render(source, options))
            {
                if (!out.empty())
                    out += ' ';
                out += markdown::line_text(l);
            }
            return out;
        }

        [[nodiscard]] std::vector<Line> instruction_lines(Ui& ui, int width)
        {
            auto const& m = ui.m;
            auto const& es = m.exercise;
            auto const& ex = *es.ex;
            std::vector<Line> lines;
            if (ex.is_boss())
            {
                Line banner;
                banner.push_back({std::format(" {} BOSS FIGHT ", ui.gl.skull), Style{Role::Boss, true, true, false}});
                banner.push_back({" ", plain(Role::Text)});
                banner.push_back({fit(ui, clean_line(ex.title), std::max(1, width - 17)), plain(Role::Boss, true)});
                lines.push_back(std::move(banner));
                std::string rating;
                for (int i = 1; i <= 5; ++i)
                    rating += i <= ex.boss->rating ? ui.gl.full : ui.gl.empty;
                Line facts;
                facts.push_back({"Difficulty ", plain(Role::Muted)});
                facts.push_back({rating, plain(Role::Boss)});
                facts.push_back({std::format(" {} of 5", ex.boss->rating), plain(Role::Muted)});
                lines.push_back(std::move(facts));
                add_wrapped(lines, std::format("about {} min{}{} stages", ex.estimated_minutes, ui.sep(), ex.stages.size()), width, plain(Role::Muted));
                lines.push_back({});
                markdown_lines(ui, lines, ex.boss->story, width, false);
            }
            else if (model::layout_for(ui.size) == LayoutMode::Compact)
                lines.push_back(text_line(fit(ui, text::sanitize(ex.title), width), plain(Role::Accent, true)));
            if (!ex.is_boss())
                add_wrapped(lines, std::format("{}{}{} min", difficulty_word(ex.difficulty), ui.sep(), ex.estimated_minutes), width, plain(Role::Muted));
            auto const st = m.status_of(ex.id);
            Line status;
            status.push_back({lesson_word(ex) + ui.sep(), plain(Role::Muted)});
            status.push_back({std::string(model::status_word(st)), plain(status_role(st), true)});
            if (m.bookmarked(ex.id))
                status.push_back({std::format("{}bookmarked", ui.sep()), plain(Role::Muted)});
            lines.push_back(status);
            lines.push_back({});
            markdown_lines(ui, lines, ex.summary, width, false);
            if (ex.staged())
            {
                lines.push_back({});
                auto const views = m.stage_views();
                int cleared = 0;
                int required = 0;
                for (auto const& v : views)
                    if (!v.stage->optional)
                    {
                        ++required;
                        if (v.state == app::StageState::Verified)
                            ++cleared;
                    }
                Line head;
                head.push_back({"Objectives", plain(Role::Accent, true)});
                head.push_back({std::format("  {} of {} required stages passing", cleared, required), plain(Role::Muted)});
                lines.push_back(std::move(head));
                for (auto const& v : views)
                {
                    std::string glyph;
                    Role role = Role::Muted;
                    switch (v.state)
                    {
                        case app::StageState::Verified:
                            glyph = std::string(ui.gl.pass);
                            role = Role::Ok;
                            break;
                        case app::StageState::Regressed:
                            glyph = std::string(ui.gl.warn);
                            role = Role::Fail;
                            break;
                        case app::StageState::Unverified:
                        case app::StageState::Outdated:
                            glyph = std::string(ui.gl.active);
                            role = Role::Warn;
                            break;
                        case app::StageState::Current:
                            glyph = std::string(ui.gl.sel);
                            role = Role::Accent;
                            break;
                        case app::StageState::Todo:
                            glyph = std::string(ui.gl.todo);
                            break;
                    }
                    bool const current = v.state == app::StageState::Current || v.state == app::StageState::Regressed;
                    Line row;
                    row.push_back({pad_right(glyph, ui.gl.status_width) + " ", plain(role, true)});
                    row.push_back({std::format("{}. ", v.number), plain(Role::Muted)});
                    row.push_back({clean_line(v.stage->title), plain(current ? Role::Accent : Role::Text, current)});
                    if (v.stage->optional)
                        row.push_back({" (bonus)", plain(Role::Muted)});
                    if (v.state != app::StageState::Todo)
                        row.push_back({std::format("  {}", app::to_string(v.state)), plain(role)});
                    lines.push_back(std::move(row));
                    std::string const indent(to_size(ui.gl.status_width + 4), ' ');
                    if (current || v.state == app::StageState::Outdated)
                        add_wrapped(lines, clean_line(markdown_inline(v.stage->summary)), width, plain(Role::Text), indent);
                    for (int h = 0; h < v.hint_level && h < to_int(v.stage->hints.size()); ++h)
                        add_wrapped(lines,
                                    std::format("Hint {} of {}: {}", h + 1, v.stage->hints.size(), clean_line(markdown_inline(v.stage->hints[to_size(h)]))),
                                    width, plain(Role::Warn), indent);
                }
            }
            else if (!ex.objectives.empty())
            {
                lines.push_back({});
                lines.push_back(text_line("Goals", plain(Role::Accent, true)));
                std::string list;
                for (auto const& o : ex.objectives)
                    list += "- " + text::replace_all(o, "\n", " ") + "\n";
                markdown_lines(ui, lines, list, width, false);
            }
            if (!ex.readme.empty())
            {
                lines.push_back({});
                markdown_lines(ui, lines, markdown::drop_section(ex.readme, "Acceptance criteria"), width, true);
            }
            if (es.hints_shown > 0)
            {
                lines.push_back({});
                lines.push_back(text_line("Hints", plain(Role::Accent, true)));
                for (int i = 0; i < es.hints_shown && i < 3; ++i)
                {
                    lines.push_back(text_line(std::format("Hint {} of 3", i + 1), plain(Role::Warn, true)));
                    markdown_lines(ui, lines, ex.hints[to_size(i)], width, false);
                }
            }
            return lines;
        }

        [[nodiscard]] std::string seconds_text(std::int64_t ms)
        {
            return std::format("{:.2f}s", static_cast<double>(ms) / 1000.0);
        }

        [[nodiscard]] Line output_line(Ui const& ui, diff::OutputLine const& l, int width, bool marked, bool observed_numbers = false)
        {
            Role const role = l.mark == diff::Mark::Same ? Role::Muted : l.mark == diff::Mark::Expected ? Role::Del : Role::Add;
            Line line;
            std::string const marker = !marked ? "  " : l.mark == diff::Mark::Same ? "    " : l.mark == diff::Mark::Expected ? "  - " : "  + ";
            std::string const number =
                std::to_string(l.mark == diff::Mark::Observed || (observed_numbers && l.mark == diff::Mark::Same) ? l.observed_line : l.expected_line);
            line.push_back({marker, plain(role, true)});
            line.push_back({pad_left(number, 4) + " ", plain(Role::Muted)});
            int const avail = std::max(8, width - width_of(marker) - 5);
            std::string_view text = l.text;
            std::size_t begin = l.has_change ? l.changed_begin : 0;
            std::size_t end = l.has_change ? l.changed_end : 0;
            std::string lead;
            if (l.has_change && width_of(diff::visible_line(text.substr(0, begin), ui.ascii)) > avail - 12 && begin > 12)
            {
                std::size_t cut = begin - 12;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                    --cut;
                lead = std::string(ui.gl.ell);
                text = text.substr(cut);
                begin -= cut;
                end -= cut;
            }
            if (!lead.empty())
                line.push_back({lead, plain(Role::Muted)});
            if (l.has_change && end > begin)
            {
                auto changed = text::replace_all(std::string(text.substr(begin, end - begin)), " ", ui.ascii ? "~" : "\xC2\xB7");
                line.push_back({diff::visible_line(text.substr(0, begin), ui.ascii, false), plain(role)});
                line.push_back({diff::visible_line(changed, ui.ascii), Style{role, true, true, false}});
                line.push_back({diff::visible_line(text.substr(end), ui.ascii), plain(role)});
            }
            else
                line.push_back({diff::visible_line(text, ui.ascii), plain(role, l.mark != diff::Mark::Same)});
            if (!l.newline)
                line.push_back({ui.ascii ? "  (no newline at end)" : " \xE2\x8F\x8E missing", plain(Role::Warn)});
            int used = 0;
            Line clipped;
            for (auto& span : line)
            {
                int const w = width_of(span.text);
                if (used + w <= width)
                {
                    used += w;
                    clipped.push_back(std::move(span));
                    continue;
                }
                if (width - used > 1)
                    clipped.push_back({fit(ui, span.text, width - used), span.style});
                break;
            }
            return clipped;
        }

        void diff_lines_for(Ui const& ui, std::vector<Line>& out, diff::OutputDiff const& d, int width, bool full)
        {
            auto const view = ui.m.exercise.result_view;
            if (view != model::ResultView::Diff)
            {
                bool const want_expected = view == model::ResultView::Expected;
                out.push_back(text_line(want_expected ? "      Expected output:" : "      Your output:", plain(want_expected ? Role::Ok : Role::Fail, true)));
                std::size_t shown = 0;
                for (auto const& l : d.lines)
                {
                    if (l.mark == (want_expected ? diff::Mark::Observed : diff::Mark::Expected))
                        continue;
                    if (l.mark == diff::Mark::Gap)
                    {
                        out.push_back(text_line("      ...", plain(Role::Muted)));
                        continue;
                    }
                    if (!full && shown >= 12)
                    {
                        out.push_back(text_line("      (l shows every line)", plain(Role::Muted)));
                        break;
                    }
                    Line row;
                    row.push_back({"    ", plain(Role::Text)});
                    append(row, output_line(ui, l, width - 4, false, !want_expected));
                    out.push_back(std::move(row));
                    ++shown;
                }
                return;
            }
            Line head;
            head.push_back({"      ", plain(Role::Text)});
            head.push_back({"- expected", plain(Role::Del, true)});
            head.push_back({"   ", plain(Role::Text)});
            head.push_back({"+ your output", plain(Role::Add, true)});
            if (d.expected_lines != d.observed_lines)
                head.push_back({std::format("   {} vs {} lines", d.expected_lines, d.observed_lines), plain(Role::Muted)});
            out.push_back(std::move(head));
            auto const parts = diff::hunks(d, full ? 3 : 2, full);
            for (std::size_t h = 0; h < parts.size(); ++h)
            {
                if (h > 0)
                    out.push_back(text_line("      ...", plain(Role::Muted)));
                for (auto i = parts[h].first; i < parts[h].second; ++i)
                {
                    Line row;
                    row.push_back({"  ", plain(Role::Text)});
                    append(row, output_line(ui, d.lines[i], width - 2, true));
                    out.push_back(std::move(row));
                }
            }
            auto const total = diff::hunks(d, 2, true).size();
            if (!full && total > 1)
                out.push_back(
                    text_line(std::format("      {} more place{} differ (l shows the full diff)", total - 1, total == 2 ? "" : "s"), plain(Role::Muted)));
            if (d.truncated)
                out.push_back(text_line("      (the comparison stops here: the outputs are very long)", plain(Role::Muted)));
        }

        void add_block(Ui const& ui, std::vector<Line>& out, int width, std::string_view label, Role label_role, std::string_view body, std::size_t max_lines,
                       bool all)
        {
            auto const clean = text::sanitize(body);
            auto lines = text::split_lines(clean);
            while (!lines.empty() && lines.back().empty())
                lines.pop_back();
            if (lines.empty())
                return;
            if (lines.size() == 1 && width_of(label) + 1 + width_of(lines[0]) <= width - 6)
            {
                Line one;
                one.push_back({std::format("      {} ", label), plain(label_role, true)});
                one.push_back({clean_line(lines[0]), plain(Role::Code)});
                out.push_back(std::move(one));
                return;
            }
            out.push_back(text_line(std::format("      {}", label), plain(label_role, true)));
            std::size_t shown = 0;
            for (auto const& l : lines)
            {
                if (!all && shown >= max_lines)
                {
                    out.push_back(text_line(std::format("        {} {} more lines", ui.gl.ell, lines.size() - shown), plain(Role::Muted)));
                    break;
                }
                out.push_back(text_line("        " + fit(ui, clean_line(l), std::max(1, width - 8)), plain(Role::Code)));
                ++shown;
            }
        }

        void result_lines(Ui const& ui, std::vector<Line>& out, runner::CheckReport const& report, int width, bool stale)
        {
            bool const raw = ui.m.exercise.raw_log;
            Role headline_role = Role::Fail;
            std::string glyph(ui.gl.fail);
            if (report.outcome == runner::Outcome::Passed)
            {
                headline_role = Role::Ok;
                glyph = std::string(ui.gl.pass);
            }
            else if (report.outcome == runner::Outcome::Cancelled)
            {
                headline_role = Role::Warn;
                glyph = std::string(ui.gl.skip);
            }
            else if (report.outcome == runner::Outcome::Error)
                glyph = std::string(ui.gl.warn);
            if (stale)
                headline_role = Role::Muted;
            Line head;
            head.push_back({glyph + " ", plain(headline_role, true)});
            head.push_back(
                {std::format("{} of {} {} passed", report.passed, report.total, report.total == 1 ? "check" : "checks"), plain(headline_role, true)});
            if (report.outcome == runner::Outcome::Cancelled)
                head.push_back({" (cancelled)", plain(Role::Warn)});
            head.push_back({std::format("{}{}", ui.sep(), seconds_text(report.duration_ms)), plain(Role::Muted)});
            if (stale)
                head.push_back({"  out of date: your files changed", plain(Role::Warn)});
            out.push_back(std::move(head));
            struct Entry
            {
                runner::CheckResult const* result = nullptr;
                runner::StageResult const* stage = nullptr;
                int number = 0;
            };
            std::vector<Entry> order;
            for (auto const& r : report.results)
                if (report.stages.empty() || r.stage_id.empty())
                    order.push_back({&r, nullptr, 0});
            int stage_number = 0;
            for (auto const& st : report.stages)
            {
                order.push_back({nullptr, &st, ++stage_number});
                for (auto const& r : report.results)
                    if (r.stage_id == st.id)
                        order.push_back({&r, nullptr, 0});
            }
            for (auto const& entry : order)
            {
                if (entry.stage != nullptr)
                {
                    auto const& st = *entry.stage;
                    Role const stage_role = stale ? Role::Muted : st.passed ? Role::Ok : st.ran ? Role::Fail : Role::Muted;
                    Line h;
                    h.push_back({std::format("Stage {}  ", entry.number), plain(Role::Muted, true)});
                    h.push_back({clean_line(st.title), plain(Role::Accent, true)});
                    if (st.optional)
                        h.push_back({" (bonus)", plain(Role::Muted)});
                    h.push_back({st.passed ? "  passing" : st.ran ? "  failing" : "  not run", plain(stage_role, true)});
                    out.push_back(std::move(h));
                    continue;
                }
                auto const& r = *entry.result;
                std::string g;
                std::string word;
                Role role = Role::Muted;
                switch (r.verdict)
                {
                    case runner::Verdict::Pass:
                        g = std::string(ui.gl.pass);
                        word = "pass";
                        role = Role::Ok;
                        break;
                    case runner::Verdict::Fail:
                        g = std::string(ui.gl.fail);
                        word = "FAIL";
                        role = Role::Fail;
                        break;
                    case runner::Verdict::Error:
                        g = std::string(ui.gl.warn);
                        word = "ERROR";
                        role = Role::Fail;
                        break;
                    case runner::Verdict::Skipped:
                        g = std::string(ui.gl.skip);
                        word = "skipped";
                        break;
                    case runner::Verdict::Cancelled:
                        g = std::string(ui.gl.skip);
                        word = "cancelled";
                        role = Role::Warn;
                        break;
                }
                if (stale)
                    role = Role::Muted;
                Line l;
                l.push_back({"  " + pad_right(g, ui.ascii ? 4 : 1) + " ", plain(role, true)});
                l.push_back({pad_right(word, 10), plain(role, true)});
                l.push_back({clean_line(r.name), plain(Role::Text)});
                if (r.verdict != runner::Verdict::Pass && r.verdict != runner::Verdict::Skipped)
                    l.push_back({std::format("  ({})", runner::to_string(r.stage)), plain(Role::Muted)});
                out.push_back(std::move(l));
                if (r.verdict == runner::Verdict::Pass)
                {
                    if (raw && !r.stdout_preview.empty())
                        add_block(ui, out, width, "Program output:", Role::Muted, r.stdout_preview, 8, true);
                    continue;
                }
                if (r.verdict == runner::Verdict::Skipped)
                    continue;
                if (!r.summary.empty())
                    for (auto const& piece : text::wrap(clean_line(r.summary), to_size(std::max(4, width - 6))))
                        out.push_back(text_line("      " + piece, plain(Role::Text)));
                if (r.output_diff)
                    diff_lines_for(ui, out, *r.output_diff, width, raw);
                else if (!r.expected.empty() || !r.observed.empty())
                {
                    bool const visible = !r.expected_visible.empty() || !r.observed_visible.empty();
                    std::string e = r.expected;
                    std::string o = r.observed;
                    if (ui.ascii)
                    {
                        e = text::visible_whitespace_ascii(text::sanitize(r.expected));
                        o = text::visible_whitespace_ascii(text::sanitize(r.observed));
                    }
                    else if (visible)
                    {
                        e = r.expected_visible;
                        o = r.observed_visible;
                    }
                    add_block(ui, out, width, "Expected:", Role::Ok, e, 8, raw);
                    add_block(ui, out, width, "Observed:", Role::Fail, o, 8, raw);
                }
                else if (!r.stdout_preview.empty() && r.stage != runner::Stage::Build && r.stage != runner::Stage::Link)
                    add_block(ui, out, width, "Program output:", Role::Muted, r.stdout_preview, 6, raw);
                if (!r.stderr_preview.empty() && (raw || r.stage == runner::Stage::Run || r.stage == runner::Stage::Harness))
                    add_block(ui, out, width, "Error output:", Role::Muted, r.stderr_preview, 6, raw);
                for (auto const& loc : r.locations)
                {
                    if (loc.severity == "note")
                        continue;
                    auto const rel = runner::detail::relative_to(report.workspace, loc.file);
                    Line at;
                    at.push_back({"      at ", plain(Role::Muted)});
                    at.push_back({std::format("{}:{}:{}", clean_line(rel), loc.line, loc.column), plain(Role::Accent, true)});
                    at.push_back({std::format(" {}: {}", clean_line(loc.severity), clean_line(loc.message)),
                                  plain(loc.severity == "warning" ? Role::Warn : Role::Fail)});
                    out.push_back(std::move(at));
                }
                bool const located = std::ranges::any_of(r.locations, [](auto const& loc) { return loc.severity != "note"; });
                if (!r.diagnostics.empty() && (raw || !located))
                    add_block(ui, out, width, "Compiler output:", Role::Muted, r.diagnostics, 10, raw);
                if (!r.hint.empty() && r.verdict == runner::Verdict::Fail)
                {
                    bool first = true;
                    for (auto const& piece : markdown_block(ui, r.hint, std::max(4, width - 12)))
                    {
                        Line h;
                        h.push_back({first ? "      Hint: " : "            ", plain(Role::Accent, true)});
                        append(h, piece);
                        out.push_back(std::move(h));
                        first = false;
                    }
                }
            }
            if (raw && !report.log_path.empty())
                out.push_back(text_line("  Full log: " + clean_line(report.log_path), plain(Role::Muted)));
        }

        [[nodiscard]] std::string spinner_frame(Ui const& ui)
        {
            if (ui.m.appearance.reduced_motion)
                return {};
            static constexpr std::array<std::string_view, 10> unicode = {"\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
                                                                         "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F"};
            static constexpr std::array<std::string_view, 4> plain_frames = {"|", "/", "-", "\\"};
            auto const tick = static_cast<std::size_t>(std::max<std::int64_t>(0, ui.m.now_ms / 120));
            if (ui.ascii)
                return std::string(plain_frames[tick % plain_frames.size()]);
            return std::string(unicode[tick % unicode.size()]);
        }

        [[nodiscard]] Line running_line(Ui const& ui)
        {
            auto const& es = ui.m.exercise;
            auto const elapsed = std::max<std::int64_t>(0, (ui.m.now_ms - es.started_ms) / 1000);
            Line l;
            if (es.phase == model::CheckPhase::Cancelling)
            {
                l.push_back({std::format("Cancelling{} {}s", ui.sep(), elapsed), plain(Role::Warn, true)});
                return l;
            }
            auto const spin = spinner_frame(ui);
            l.push_back({spin.empty() ? std::string("Checking") : spin + " Checking", plain(Role::Accent, true)});
            l.push_back({std::format(": {}", clean_line(es.stage_text)), plain(Role::Text)});
            if (spin.empty())
                l.push_back({std::format("{}running... {}s", ui.sep(), elapsed), plain(Role::Muted)});
            else
                l.push_back({std::format("{}{}s", ui.sep(), elapsed), plain(Role::Muted)});
            return l;
        }

        [[nodiscard]] std::vector<Line> results_lines(Ui const& ui, int width)
        {
            auto const& es = ui.m.exercise;
            std::vector<Line> lines;
            if (es.phase != model::CheckPhase::Idle)
            {
                lines.push_back(running_line(ui));
                if (es.report)
                {
                    lines.push_back(text_line("Previous result (out of date):", plain(Role::Muted)));
                    result_lines(ui, lines, *es.report, width, true);
                }
                return lines;
            }
            if (!es.report)
            {
                lines.push_back(text_line("No check yet.", plain(Role::Muted)));
                lines.push_back({});
                lines.push_back(text_line("Checks for this exercise", plain(Role::Accent, true)));
                for (auto const& c : es.ex->checks)
                    lines.push_back(text_line("  " + std::string(ui.gl.todo) + " " + fit(ui, clean_line(c.name), std::max(1, width - 6)), plain(Role::Muted)));
                return lines;
            }
            result_lines(ui, lines, *es.report, width, es.stale);
            return lines;
        }

        [[nodiscard]] std::set<int> error_lines_for(model::ExerciseState const& es, std::string const& rel)
        {
            std::set<int> out;
            if (!es.report)
                return out;
            for (auto const& r : es.report->results)
                for (auto const& loc : r.locations)
                    if (loc.line > 0 && loc.severity != "note" && runner::detail::relative_to(es.report->workspace, loc.file) == rel)
                        out.insert(loc.line);
            return out;
        }

        [[nodiscard]] std::vector<Line> preview_lines(Ui const& ui, int width)
        {
            auto const& es = ui.m.exercise;
            std::vector<Line> lines;
            if (!es.ws)
            {
                lines.push_back(text_line("No file selected.", plain(Role::Muted)));
                return lines;
            }
            if (es.preview_lines.empty())
            {
                lines.push_back(text_line(es.preview_note.empty() ? "This file is empty." : es.preview_note + ".", plain(Role::Muted)));
                return lines;
            }
            auto const errors = error_lines_for(es, ui.m.selected_file());
            int const digits = static_cast<int>(std::to_string(es.preview_lines.size()).size());
            int const code_w = std::max(1, width - digits - 4);
            int n = 0;
            for (auto const& l : es.preview_lines)
            {
                ++n;
                bool const bad = errors.contains(n);
                Line line;
                line.push_back({pad_left(std::to_string(n), digits), plain(bad ? Role::Fail : Role::Muted, bad)});
                line.push_back(
                    {bad ? std::format(" {} ", ui.ascii ? "!" : "\xE2\x96\x8C") : std::format(" {} ", ui.gl.v), plain(bad ? Role::Fail : Role::Border)});
                line.push_back({fit(ui, l, code_w), plain(Role::Code)});
                lines.push_back(std::move(line));
            }
            if (!es.preview_note.empty())
                lines.push_back(text_line(es.preview_note, plain(Role::Muted)));
            return lines;
        }

        [[nodiscard]] std::vector<Line> file_rows(Ui const& ui, int width)
        {
            auto const& es = ui.m.exercise;
            std::vector<Line> rows;
            if (es.files.empty())
            {
                rows.push_back(text_line("No files yet.", plain(Role::Muted)));
                return rows;
            }
            for (std::size_t i = 0; i < es.files.size(); ++i)
            {
                bool const current = i == es.file_sel;
                Line l;
                l.push_back({current ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                l.push_back({fit(ui, clean_line(es.files[i]), std::max(1, width - 12)), plain(current ? Role::Text : Role::Muted, current)});
                if (es.files[i] == es.ex->entry_file)
                    l.push_back({"  entry", plain(Role::Muted)});
                rows.push_back(std::move(l));
            }
            return rows;
        }

        [[nodiscard]] std::string pane_title(Ui const& ui, std::string_view title, bool focused)
        {
            return focused ? std::format("{} {}", ui.gl.sel, title) : std::string(title);
        }

        void draw_scroll_pane(Ui& ui, Rect box, std::string_view title, bool focused, std::vector<Line> const& lines, int scroll, std::string_view note = {})
        {
            auto const inner = draw_box(ui, box, pane_title(ui, title, focused), focused, note);
            if (inner.empty())
                return;
            int const clamped = clamp_scroll(scroll, to_int(lines.size()), inner.h);
            draw_lines(ui, inner, lines, clamped);
            draw_scrollbar(ui, box, clamped, to_int(lines.size()), inner.h);
            if (focused)
            {
                ui.m.scroll_content = to_int(lines.size());
                ui.m.scroll_viewport = inner.h;
            }
        }

        void draw_files_box(Ui& ui, Rect box, bool focused)
        {
            auto const& es = ui.m.exercise;
            auto const inner = draw_box(ui, box, pane_title(ui, "Files", focused), focused);
            if (inner.empty())
                return;
            auto const rows = file_rows(ui, inner.w);
            int const start = first_visible(to_int(es.file_sel), to_int(rows.size()), inner.h);
            for (int i = 0; i < inner.h; ++i)
            {
                auto const idx = start + i;
                if (idx < to_int(rows.size()))
                    draw_line(ui, inner, inner.y + i, rows[to_size(idx)], focused && to_size(idx) == es.file_sel);
            }
        }

        void draw_preview_box(Ui& ui, Rect box, bool focused)
        {
            auto const& es = ui.m.exercise;
            auto title = std::string("Preview");
            if (!ui.m.selected_file().empty())
                title += ": " + clean_line(ui.m.selected_file());
            auto const inner = draw_box(ui, box, title, false);
            if (inner.empty())
                return;
            auto const lines = preview_lines(ui, inner.w);
            int const clamped = clamp_scroll(es.scroll_preview, to_int(lines.size()), inner.h);
            draw_lines(ui, inner, lines, clamped);
            draw_scrollbar(ui, box, clamped, to_int(lines.size()), inner.h);
            if (focused)
            {
                ui.m.scroll_content = to_int(lines.size());
                ui.m.scroll_viewport = inner.h;
            }
        }

        void draw_results_box(Ui& ui, Rect box, bool focused)
        {
            auto const& es = ui.m.exercise;
            std::string note;
            if (es.raw_log)
                note = "raw output";
            auto const inner = content_of(box);
            draw_scroll_pane(ui, box, "Results", focused, results_lines(ui, std::max(1, inner.w)), es.scroll_results, note);
        }

        void draw_instructions_box(Ui& ui, Rect box, bool focused)
        {
            auto const inner = content_of(box);
            draw_scroll_pane(ui, box, "Instructions", focused, instruction_lines(ui, std::max(1, inner.w)), ui.m.exercise.scroll_instructions);
        }

        void draw_files_preview_box(Ui& ui, Rect box, bool focused)
        {
            auto const& es = ui.m.exercise;
            auto const inner = draw_box(ui, box, pane_title(ui, ui.m.selected_file().empty() ? "Files" : "Files: " + clean_line(ui.m.selected_file()), focused), focused);
            if (inner.empty())
                return;
            auto const rows = file_rows(ui, inner.w);
            int const list_h = std::min(to_int(rows.size()), std::max(1, std::min(4, inner.h / 3)));
            int const start = first_visible(to_int(es.file_sel), to_int(rows.size()), list_h);
            for (int i = 0; i < list_h; ++i)
                if (start + i < to_int(rows.size()))
                    draw_line(ui, inner, inner.y + i, rows[to_size(start + i)], focused && to_size(start + i) == es.file_sel);
            int const rule_y = inner.y + list_h;
            if (rule_y < inner.bottom())
                draw_rule(ui, inner, rule_y);
            Rect const code{inner.x, rule_y + 1, inner.w, inner.bottom() - rule_y - 1};
            if (code.empty())
                return;
            auto const lines = preview_lines(ui, code.w);
            int const clamped = clamp_scroll(es.scroll_preview, to_int(lines.size()), code.h);
            draw_lines(ui, code, lines, clamped);
            draw_scrollbar(ui, box, clamped, to_int(lines.size()), code.h);
            if (focused)
            {
                ui.m.scroll_content = to_int(lines.size());
                ui.m.scroll_viewport = code.h;
            }
        }

        [[nodiscard]] Line summary_line(Ui const& ui, int width)
        {
            auto const& es = ui.m.exercise;
            Line l;
            if (es.phase != model::CheckPhase::Idle)
                return running_line(ui);
            if (!es.ws)
                return l;
            if (!es.report)
            {
                l.push_back({"No check yet.", plain(Role::Muted)});
                return l;
            }
            auto const& r = *es.report;
            Role const role = es.stale ? Role::Muted
                                       : (r.outcome == runner::Outcome::Passed      ? Role::Ok
                                          : r.outcome == runner::Outcome::Cancelled ? Role::Warn
                                                                                    : Role::Fail);
            l.push_back({std::format("{} of {} {} passed{}{}", r.passed, r.total, r.total == 1 ? "check" : "checks", ui.sep(), seconds_text(r.duration_ms)),
                         plain(role, true)});
            if (es.stale)
                l.push_back({"  out of date", plain(Role::Warn)});
            (void)width;
            return l;
        }

        [[nodiscard]] std::string age_text(std::int64_t ms)
        {
            if (ms < 2000)
                return "just now";
            if (ms < 60000)
                return std::format("{}s ago", ms / 1000);
            return std::format("{}m ago", ms / 60000);
        }

        [[nodiscard]] int draw_watch_banner(Ui& ui, Rect area)
        {
            auto const status = ui.m.watch_status();
            if (status.phase == model::WatchPhase::Off || area.h < 12)
                return 0;
            bool const compact = model::layout_for(ui.size) == LayoutMode::Compact;
            Rect const box{area.x, area.y, area.w, 4};
            auto const inner = draw_box(ui, box, std::format("{} WATCH ON", ui.gl.watch), true, {}, Role::Watch);
            if (inner.empty())
                return 4;
            Line path;
            path.push_back({text::truncate_middle(clean_line(status.path), to_size(std::max(8, inner.w))), plain(Role::Text, true)});
            draw_line(ui, inner, inner.y, path);
            Line state;
            switch (status.phase)
            {
                case model::WatchPhase::Off:
                    break;
                case model::WatchPhase::Waiting:
                    state.push_back({"Waiting for changes", plain(Role::Watch, true)});
                    break;
                case model::WatchPhase::Checking: {
                    auto const spin = spinner_frame(ui);
                    state.push_back({(compact && !spin.empty() ? spin + " " : std::string()) + (status.triggered ? "Change detected, checking" : "Checking"),
                                     plain(Role::Watch, true)});
                    break;
                }
                case model::WatchPhase::Detected:
                    state.push_back({"Change detected, checking again when this check ends", plain(Role::Watch, true)});
                    break;
                case model::WatchPhase::Result: {
                    if (!status.clock.empty())
                        state.push_back({std::format("Checked at {}", status.clock), plain(Role::Muted)});
                    else
                        state.push_back({std::format("Checked {}", age_text(status.age_ms)), plain(Role::Muted)});
                    if (compact)
                    {
                        state.push_back({ui.sep(), plain(Role::Muted)});
                        append(state, summary_line(ui, inner.w));
                    }
                    break;
                }
            }
            draw_line(ui, inner, inner.y + 1, state);
            return 4;
        }

        [[nodiscard]] std::vector<Line> activity_lines(Ui const& ui, std::size_t qi, int width, int& focus)
        {
            auto const& m = ui.m;
            auto const& ex = *m.exercise.ex;
            auto const& q = ex.questions[qi];
            auto const& ui_state = m.exercise.lecture.items[qi];
            auto const qp = progress::question_of(m.prog, ex.id, q.id);
            std::vector<Line> lines;
            Line head;
            head.push_back({activity::type_word(q.type), plain(Role::Accent, true)});
            head.push_back({std::format("{}question {} of {}", ui.sep(), qi + 1, ex.questions.size()), plain(Role::Muted)});
            if (qp.last == "right")
                head.push_back({std::format("{}{} solved", ui.sep(), ui.gl.done), plain(Role::Ok)});
            else if (qp.missed() && !ui_state.feedback)
                head.push_back({std::format("{}missed last time", ui.sep()), plain(Role::Warn)});
            lines.push_back(std::move(head));
            lines.push_back({});
            markdown_lines(ui, lines, q.prompt, width, false);
            lines.push_back({});
            auto const code = q.code_lines();
            int const digits = code.size() >= 10 ? 2 : 1;
            bool const code_cursor = q.type == catalog::QuestionType::Bug || q.type == catalog::QuestionType::Order;
            if (q.type == catalog::QuestionType::Order)
            {
                for (std::size_t i = 0; i < ui_state.order.size(); ++i)
                {
                    bool const selected = to_int(i) == ui_state.cursor;
                    bool const held = selected && ui_state.holding;
                    Line l;
                    l.push_back({selected ? std::format(" {} ", held ? "=" : std::string(ui.gl.sel)) : std::string("   "), plain(Role::Accent, true)});
                    l.push_back({std::format("{} ", pad_left(std::to_string(i + 1), digits)), plain(Role::Muted)});
                    l.push_back({clean_line(code[to_size(ui_state.order[i])]), Style{held ? Role::Accent : Role::Code, held, held, false}});
                    if (selected)
                        focus = to_int(lines.size());
                    lines.push_back(std::move(l));
                }
                lines.push_back({});
            }
            else if (!code.empty())
            {
                for (std::size_t i = 0; i < code.size(); ++i)
                {
                    bool const selected = code_cursor && to_int(i) == ui_state.cursor;
                    Line l;
                    l.push_back({selected ? std::format(" {} ", ui.gl.sel) : std::string("   "), plain(Role::Accent, true)});
                    l.push_back({std::format("{} {} ", pad_left(std::to_string(i + 1), digits), ui.gl.v), plain(Role::Muted)});
                    l.push_back({fit(ui, clean_line(code[i]), std::max(4, width - 6 - digits)), plain(selected ? Role::Accent : Role::Code, selected)});
                    if (selected)
                        focus = to_int(lines.size());
                    lines.push_back(std::move(l));
                }
                lines.push_back({});
            }
            if (q.type == catalog::QuestionType::Error)
            {
                Line l;
                l.push_back({"error: ", plain(Role::Fail, true)});
                l.push_back({clean_line(q.diagnostic), plain(Role::Text)});
                lines.push_back(std::move(l));
                lines.push_back({});
            }
            if (q.has_choices())
            {
                for (std::size_t c = 0; c < q.choices.size(); ++c)
                {
                    bool const selected = to_int(c) == ui_state.cursor;
                    bool const picked = q.type == catalog::QuestionType::Multi && c < ui_state.picked.size() && ui_state.picked[c];
                    std::string const marker = q.type == catalog::QuestionType::Multi ? (picked ? (ui.ascii ? "[x] " : "\xE2\x98\x91 ") : (ui.ascii ? "[ ] " : "\xE2\x98\x90 "))
                                                                                      : std::format("{} ", selected ? ui.gl.radio_on : ui.gl.radio_off);
                    auto const pieces = text::wrap(clean_line(markdown_inline(q.choices[c])), to_size(std::max(4, width - 4 - width_of(marker))));
                    if (selected)
                        focus = to_int(lines.size());
                    for (std::size_t pi = 0; pi < pieces.size(); ++pi)
                    {
                        Line l;
                        l.push_back({pi == 0 && selected ? std::format(" {} ", ui.gl.sel) : std::string("   "), plain(Role::Accent, true)});
                        l.push_back({pi == 0 ? marker : std::string(to_size(width_of(marker)), ' '), plain(selected || picked ? Role::Accent : Role::Text, picked)});
                        l.push_back({pieces[pi], plain(selected ? Role::Accent : Role::Text, selected)});
                        lines.push_back(std::move(l));
                    }
                }
                lines.push_back({});
            }
            if (model::text_activity(q))
            {
                bool const typing = m.exercise.lecture.typing && m.current_question() == qi;
                lines.push_back(text_line(q.type == catalog::QuestionType::Predict ? "Your prediction:" : "Your answer:", plain(Role::Muted)));
                auto rows = text::split_lines(ui_state.input);
                if (ui_state.input.empty() || ui_state.input.ends_with('\n'))
                    rows.push_back({});
                for (std::size_t r = 0; r < rows.size(); ++r)
                {
                    bool const last = r + 1 == rows.size();
                    Line l;
                    l.push_back({"   ", plain(Role::Text)});
                    l.push_back({clean_line(rows[r]), plain(Role::Accent, true)});
                    if (last && typing)
                        l.push_back({"_", plain(Role::Accent, true)});
                    if (last)
                        focus = to_int(lines.size());
                    lines.push_back(std::move(l));
                }
                if (!typing)
                    lines.push_back(text_line("   (not typing: Enter to type)", plain(Role::Muted)));
                lines.push_back({});
            }
            if (ui_state.feedback)
            {
                auto const& f = *ui_state.feedback;
                Line v;
                v.push_back({std::format("{} {}", f.correct ? ui.gl.pass : ui.gl.fail, f.verdict), plain(f.correct ? Role::Ok : Role::Fail, true)});
                if (!f.correct && ui_state.tries > 0)
                    v.push_back({std::format("{}attempt {}", ui.sep(), ui_state.tries), plain(Role::Muted)});
                lines.push_back(std::move(v));
                for (auto const& note : f.notes)
                    add_wrapped(lines, clean_line(markdown_inline(note)), width, plain(Role::Text), "  ");
                if (!f.explanation.empty())
                    add_wrapped(lines, clean_line(markdown_inline(f.explanation)), width, plain(f.correct ? Role::Ok : Role::Text), "  ");
                if (!f.correct && f.revealed)
                    for (auto const& piece : text::split_lines(activity::reveal_answer(q)))
                        add_wrapped(lines, clean_line(piece), width, plain(Role::Warn), "  ");
                if (!f.correct && !f.revealed)
                    lines.push_back(text_line("  Change your answer and check again.", plain(Role::Muted)));
                if (focus >= 0)
                    focus = to_int(lines.size()) - 1;
            }
            return lines;
        }

        void draw_activity_area(Ui& ui, Rect area, std::size_t qi, Rect scroll_box)
        {
            int focus = -1;
            auto const lines = activity_lines(ui, qi, std::max(8, area.w - 1), focus);
            int const total = to_int(lines.size());
            int start = 0;
            if (focus >= 0)
                start = first_visible(focus, total, area.h);
            start = std::clamp(start + ui.m.exercise.lecture.scroll, 0, std::max(0, total - area.h));
            for (int i = 0; i < area.h; ++i)
            {
                auto const idx = start + i;
                if (idx >= total)
                    break;
                draw_line(ui, area, area.y + i, lines[to_size(idx)]);
            }
            draw_scrollbar(ui, scroll_box, start, total, area.h);
        }

        void draw_lecture(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const& es = m.exercise;
            auto const& ex = *es.ex;
            auto const& lec = es.lecture;
            auto const pages = m.lecture_pages();
            auto const page = std::min(lec.page, pages - 1);
            bool const done = m.status_of(ex.id) == progress::Status::Completed;
            auto const inner = draw_box(ui, main, clean_line(ex.title), true,
                                        std::format("{}{}page {} of {}{}", ex.drill ? "drill" : "lecture", ui.sep(), page + 1, pages,
                                                    done ? std::format("{}completed", ui.sep()) : std::string()));
            if (inner.empty())
                return;
            Rect text_area = inner;
            if (inner.w >= 90)
            {
                int const toc_w = 26;
                Rect const toc{inner.x, inner.y, toc_w, inner.h};
                std::vector<Line> entries;
                entries.push_back(text_line("Contents", plain(Role::Muted, true)));
                for (std::size_t i = 0; i < pages; ++i)
                {
                    std::string title;
                    Role role = Role::Muted;
                    std::string mark = " ";
                    if (i < es.sections.size())
                        title = clean_line(es.sections[i].title);
                    else if (i < es.sections.size() + ex.questions.size())
                    {
                        auto const& q = ex.questions[i - es.sections.size()];
                        title = activity::type_word(q.type);
                        auto const qp = progress::question_of(m.prog, ex.id, q.id);
                        if (qp.last == "right")
                        {
                            mark = std::string(ui.gl.done);
                            role = Role::Ok;
                        }
                        else if (qp.missed())
                        {
                            mark = std::string(ui.gl.fail);
                            role = Role::Warn;
                        }
                    }
                    else
                        title = ex.questions.empty() ? "Finish" : "Summary";
                    bool const current = i == page;
                    Line l;
                    l.push_back({current ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                    l.push_back({pad_right(mark, ui.gl.status_width) + " ", plain(role, true)});
                    l.push_back({fit(ui, title, toc_w - 5 - ui.gl.status_width), plain(current ? Role::Accent : Role::Muted, current)});
                    entries.push_back(std::move(l));
                }
                int const start = first_visible(to_int(page) + 1, to_int(entries.size()), toc.h);
                for (int i = 0; i < toc.h && start + i < to_int(entries.size()); ++i)
                    draw_line(ui, toc, toc.y + i, entries[to_size(start + i)]);
                for (int y = inner.y; y < inner.bottom(); ++y)
                    ui.g.put(inner.x + toc_w, y, ui.gl.v, plain(Role::Border), inner);
                text_area = Rect{inner.x + toc_w + 2, inner.y, inner.w - toc_w - 2, inner.h};
            }
            if (auto const qi = m.current_question())
            {
                draw_activity_area(ui, text_area, *qi, main);
                return;
            }
            int const width = std::max(8, text_area.w - 1);
            std::vector<Line> lines;
            if (page < es.sections.size())
            {
                auto const& sec = es.sections[page];
                lines.push_back(text_line(clean_line(sec.title), plain(Role::Accent, true)));
                lines.push_back({});
                markdown_lines(ui, lines, sec.body, width, false);
            }
            else
            {
                lines.push_back(text_line(ex.questions.empty() ? "Finish" : "Summary", plain(Role::Accent, true)));
                lines.push_back({});
                if (!ex.objectives.empty())
                {
                    lines.push_back(text_line(ex.drill ? "This drill practises:" : "After this lecture you can:", plain(Role::Muted)));
                    std::string list;
                    for (auto const& o : ex.objectives)
                        list += "- " + o + "\n";
                    markdown_lines(ui, lines, list, width, false);
                    lines.push_back({});
                }
                int solved = 0;
                for (std::size_t i = 0; i < ex.questions.size(); ++i)
                {
                    auto const& q = ex.questions[i];
                    auto const qp = progress::question_of(m.prog, ex.id, q.id);
                    bool const right = qp.last == "right";
                    if (right)
                        ++solved;
                    Line l;
                    l.push_back({std::format("  {} ", right ? ui.gl.pass : qp.missed() ? ui.gl.fail : ui.gl.todo),
                                 plain(right ? Role::Ok : qp.missed() ? Role::Fail : Role::Muted, true)});
                    l.push_back({std::format("{}. {}", i + 1, activity::type_word(q.type)), plain(Role::Text)});
                    if (qp.attempts > 0)
                        l.push_back({std::format("{}{} {}", ui.sep(), qp.attempts, qp.attempts == 1 ? "try" : "tries"), plain(Role::Muted)});
                    lines.push_back(std::move(l));
                }
                if (!ex.questions.empty())
                {
                    lines.push_back({});
                    lines.push_back(text_line(std::format("{} of {} solved", solved, ex.questions.size()), plain(solved == to_int(ex.questions.size()) ? Role::Ok : Role::Text, true)));
                    if (solved < to_int(ex.questions.size()))
                        add_wrapped(lines, "Questions you miss come back on the Review screen, so you can practise them again later.", width, plain(Role::Muted));
                }
                else
                {
                    Line l;
                    l.push_back({std::format(" {} ", ui.gl.sel), plain(Role::Accent, true)});
                    l.push_back({"[ Mark as read ]", plain(Role::Key, true)});
                    lines.push_back(std::move(l));
                }
                if (!ex.cards.empty())
                {
                    lines.push_back({});
                    add_wrapped(lines, std::format("{} flashcard{} join your Review once this lesson is complete.", ex.cards.size(), ex.cards.size() == 1 ? "" : "s"),
                                width, plain(Role::Muted));
                }
                if (done)
                {
                    lines.push_back({});
                    lines.push_back(text_line(std::format("{} Completed", ui.gl.done), plain(Role::Ok, true)));
                }
            }
            int const total = to_int(lines.size());
            int const start = std::clamp(lec.scroll, 0, std::max(0, total - text_area.h));
            for (int i = 0; i < text_area.h; ++i)
            {
                auto const idx = start + i;
                if (idx >= total)
                    break;
                draw_line(ui, text_area, text_area.y + i, lines[to_size(idx)]);
            }
            draw_scrollbar(ui, main, start, total, text_area.h);
        }

        void draw_centered(Ui& ui, Rect area, int y, std::vector<Line> const& lines)
        {
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                int const row = y + to_int(i);
                if (row >= area.bottom())
                    break;
                int const w = line_width(lines[i]);
                int const x = area.x + std::max(0, (area.w - w) / 2);
                draw_line(ui, Rect{x, row, area.right() - x, 1}, row, lines[i]);
            }
        }

        [[nodiscard]] std::vector<Line> card_text(Ui const& ui, std::string_view source, int width, Role role, bool bold)
        {
            std::vector<Line> out;
            for (auto const& piece : text::wrap(clean_line(markdown_inline(source)), to_size(std::max(4, width))))
                out.push_back(text_line(piece, plain(role, bold)));
            std::ignore = ui;
            return out;
        }

        void draw_flashcards(Ui& ui, Rect pane, bool focused)
        {
            auto const& m = ui.m;
            auto const& rs = m.review_state;
            auto const queue = m.review_queue();
            std::size_t const left = rs.index < rs.due.size() ? rs.due.size() - rs.index : 0;
            auto const inner =
                draw_box(ui, pane, focused ? std::format("{} Flashcards", ui.gl.sel) : std::string("Flashcards"), focused, std::format("{} due", left));
            if (inner.empty())
                return;
            catalog::Exercise const* ex = nullptr;
            catalog::Card const* card = nullptr;
            if (rs.index < rs.due.size())
            {
                auto const& key = rs.due[rs.index];
                auto const hash = key.find('#');
                ex = m.ctx->catalog.find(key.substr(0, hash));
                if (ex)
                    for (auto const& c : ex->cards)
                        if (c.id == key.substr(hash + 1))
                            card = &c;
            }
            if (!card)
            {
                std::vector<Line> lines;
                if (rs.rated > 0)
                    lines.push_back(
                        text_line(std::format("{} Session done: {} card{} reviewed.", ui.gl.done, rs.rated, rs.rated == 1 ? "" : "s"), plain(Role::Ok, true)));
                else if (queue.cards_unlocked == 0)
                    lines.push_back(text_line("No cards yet. Lessons add theirs once you complete them.", plain(Role::Muted)));
                else if (queue.next_due_day)
                    lines.push_back(
                        text_line(std::format("Nothing is due. The next card comes back in {} day(s).", std::max(1, *queue.next_due_day - app::today_day())),
                                  plain(Role::Muted)));
                else
                    lines.push_back(text_line("Nothing is due right now.", plain(Role::Muted)));
                lines.push_back({});
                for (auto const& piece :
                     text::wrap(std::format("{} unlocked{}{} locked until their lesson is complete", queue.cards_unlocked, ui.sep(), queue.cards_locked),
                                to_size(std::max(8, inner.w - 2))))
                    lines.push_back(text_line(piece, plain(Role::Muted)));
                draw_centered(ui, inner, inner.y + std::max(0, inner.h / 2 - 2), lines);
                return;
            }
            int const card_w = std::clamp(inner.w - 4, 12, 64);
            int const text_w = card_w - 6;
            auto front = card_text(ui, card->front, text_w, Role::Text, true);
            std::vector<Line> body;
            if (rs.flipped)
            {
                for (auto const& l : card_text(ui, card->front, text_w, Role::Muted, false))
                    body.push_back(l);
                std::string rule;
                for (int i = 0; i < std::min(text_w, 24); ++i)
                    rule += ui.gl.h;
                body.push_back(text_line(rule, plain(Role::Border)));
                for (auto const& l : card_text(ui, card->back, text_w, Role::Accent, true))
                    body.push_back(l);
            }
            else
                body = std::move(front);
            int const stack = static_cast<int>(std::min<std::size_t>(2, left > 0 ? left - 1 : 0));
            int const card_h = std::clamp(to_int(body.size()) + 4, 7, std::max(7, inner.h - 4 - stack));
            int const card_x = inner.x + (inner.w - card_w) / 2;
            int const card_y = inner.y + std::max(0, (inner.h - card_h - stack - 2) / 2);
            Rect const card_rect{card_x, card_y, card_w, card_h};
            ui.g.fill(card_rect, plain(Role::Text));
            auto const card_inner = draw_box(ui, card_rect, rs.flipped ? "BACK" : "FRONT", focused, clean_line(ex->title),
                                             rs.flipped ? std::optional<Role>(Role::Accent) : std::nullopt);
            Rect const all{0, 0, ui.g.width(), ui.g.height()};
            for (int k = 1; k <= stack; ++k)
            {
                int const y = card_rect.bottom() - 1 + k;
                int const x0 = card_rect.x + 2 * k;
                int const x1 = card_rect.right() - 1 - 2 * k;
                if (y >= inner.bottom() || x1 <= x0)
                    break;
                ui.g.put(x0, y, ui.gl.bl, plain(Role::Border), all);
                for (int x = x0 + 1; x < x1; ++x)
                    ui.g.put(x, y, ui.gl.h, plain(Role::Border), all);
                ui.g.put(x1, y, ui.gl.br, plain(Role::Border), all);
            }
            if (!card_inner.empty())
            {
                int const start = card_inner.y + std::max(0, (card_inner.h - to_int(body.size())) / 2);
                draw_centered(ui, card_inner, start, body);
            }
            int const below = card_rect.bottom() + stack + 1;
            if (below < inner.bottom())
            {
                Line dots;
                std::size_t const total = rs.due.size();
                std::string marks;
                for (std::size_t i = 0; i < total && i < 20; ++i)
                    marks += i < rs.index    ? std::string(ui.ascii ? "*" : "\xE2\x97\x8F")
                             : i == rs.index ? std::string(ui.ascii ? "o" : "\xE2\x97\x89")
                                             : std::string(ui.ascii ? "." : "\xE2\x97\x8B");
                dots.push_back({marks, plain(Role::Accent)});
                dots.push_back({std::format("  card {} of {}", rs.index + 1, total), plain(Role::Muted)});
                draw_centered(ui, inner, below, {dots});
                if (below + 1 < inner.bottom())
                    draw_centered(
                        ui, inner, below + 1,
                        card_text(ui, rs.flipped ? "How well did you remember it: again, hard, good or easy?" : "Recall the answer, then turn the card over.",
                                  std::max(8, inner.w - 4), rs.flipped ? Role::Accent : Role::Muted, rs.flipped));
            }
        }

        void draw_missed(Ui& ui, Rect pane, bool focused)
        {
            auto const& m = ui.m;
            auto const& rs = m.review_state;
            auto const queue = m.review_queue();
            auto const inner = draw_box(ui, pane, focused ? std::format("{} Try again", ui.gl.sel) : std::string("Try again"), focused,
                                        std::format("{} missed", queue.missed.size()));
            if (inner.empty())
                return;
            std::vector<Line> lines;
            int focus = -1;
            if (queue.missed.empty())
                for (auto const& piece :
                     text::wrap("Every question you answered is solved. Questions you miss come back here.", to_size(std::max(8, inner.w - 1))))
                    lines.push_back(text_line(piece, plain(Role::Muted)));
            for (std::size_t i = 0; i < queue.missed.size(); ++i)
            {
                auto const& mref = queue.missed[i];
                bool const selected = focused && i == std::min(rs.missed_sel, queue.missed.size() - 1);
                Line l;
                l.push_back({selected ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                l.push_back({fit(ui, activity::type_word(mref.question->type), std::max(4, inner.w - 4)), plain(selected ? Role::Accent : Role::Text, true)});
                if (selected)
                    focus = to_int(lines.size());
                lines.push_back(std::move(l));
                lines.push_back(text_line(
                    "  " + fit(ui, std::format("{}{}{} {}", clean_line(mref.lesson->title), ui.sep(), mref.attempts, mref.attempts == 1 ? "try" : "tries"),
                               std::max(4, inner.w - 2)),
                    plain(Role::Muted)));
            }
            int const total = to_int(lines.size());
            int const start = focus >= 0 ? first_visible(focus, total, inner.h) : 0;
            for (int i = 0; i < inner.h; ++i)
            {
                auto const idx = start + i;
                if (idx >= total)
                    break;
                bool const selected = idx == focus;
                draw_line(ui, inner, inner.y + i, lines[to_size(idx)], selected);
            }
            draw_scrollbar(ui, pane, start, total, inner.h);
        }

        void draw_review(Ui& ui, Rect main)
        {
            auto const& rs = ui.m.review_state;
            if (main.w >= 70)
            {
                int const right_w = std::clamp(main.w * 2 / 5, 26, 44);
                draw_flashcards(ui, Rect{main.x, main.y, main.w - right_w, main.h}, !rs.on_missed);
                draw_missed(ui, Rect{main.x + main.w - right_w, main.y, right_w, main.h}, rs.on_missed);
                return;
            }
            int const missed_h = std::clamp(main.h / 3, 4, 8);
            draw_flashcards(ui, Rect{main.x, main.y, main.w, main.h - missed_h}, !rs.on_missed);
            draw_missed(ui, Rect{main.x, main.y + main.h - missed_h, main.w, missed_h}, rs.on_missed);
        }

        void draw_exercise(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const& es = m.exercise;
            if (!es.ex)
                return;
            if (es.ex->is_lecture())
            {
                draw_lecture(ui, main);
                return;
            }
            if (es.activity)
            {
                auto const inner = draw_box(ui, main, std::format("{}: questions", clean_line(es.ex->title)), true, "Esc back to the exercise");
                if (!inner.empty())
                    draw_activity_area(ui, inner, *es.activity, main);
                return;
            }
            int const banner = draw_watch_banner(ui, main);
            main.y += banner;
            main.h -= banner;
            auto const mode = model::layout_for(ui.size);
            if (mode == LayoutMode::Wide)
            {
                bool const has_report = es.report.has_value() || es.phase != model::CheckPhase::Idle;
                int const results_h = std::clamp(has_report ? main.h * 2 / 5 : 7, 6, std::max(6, main.h - 8));
                int const top_h = main.h - results_h;
                int const left_w = std::max(24, main.w * 11 / 20);
                draw_instructions_box(ui, Rect{main.x, main.y, left_w, top_h}, es.pane == Pane::Instructions);
                Rect const right{main.x + left_w, main.y, main.w - left_w, top_h};
                int const files_h = std::clamp(to_int(es.files.size()) + 2, 3, std::max(3, std::min(6, right.h / 3)));
                draw_files_box(ui, Rect{right.x, right.y, right.w, files_h}, es.pane == Pane::Files);
                draw_preview_box(ui, Rect{right.x, right.y + files_h, right.w, right.h - files_h}, es.pane == Pane::Files);
                draw_results_box(ui, Rect{main.x, main.y + top_h, main.w, results_h}, es.pane == Pane::Results);
                return;
            }
            if (mode == LayoutMode::Normal)
            {
                bool const has_report = es.report.has_value() || es.phase != model::CheckPhase::Idle;
                int const results_h = std::clamp(has_report ? main.h * 2 / 5 : 6, 6, std::max(6, main.h - 6));
                int const top_h = main.h - results_h - 1;
                if (es.pane == Pane::Files)
                    draw_files_preview_box(ui, Rect{main.x, main.y, main.w, top_h + 1}, true);
                else
                    draw_instructions_box(ui, Rect{main.x, main.y, main.w, top_h}, es.pane == Pane::Instructions);
                Line strip;
                if (es.pane != Pane::Files)
                {
                    strip.push_back({"Files ", plain(Role::Muted)});
                    for (std::size_t i = 0; i < es.files.size(); ++i)
                    {
                        bool const current = i == es.file_sel;
                        strip.push_back({current ? std::format("{} {}  ", ui.gl.sel, clean_line(es.files[i])) : std::format("  {}  ", clean_line(es.files[i])),
                                         plain(current ? Role::Accent : Role::Muted, current)});
                    }
                    if (es.files.empty())
                        strip.push_back({"none", plain(Role::Muted)});
                }
                if (es.pane != Pane::Files)
                    draw_line(ui, Rect{main.x + 1, main.y + top_h, main.w - 2, 1}, main.y + top_h, strip);
                draw_results_box(ui, Rect{main.x, main.y + top_h + 1, main.w, results_h}, es.pane == Pane::Results);
                return;
            }
            Line tabs;
            static constexpr std::array<std::pair<Pane, std::string_view>, 3> names = {std::pair{Pane::Instructions, "Instructions"},
                                                                                       std::pair{Pane::Files, "Files"}, std::pair{Pane::Results, "Results"}};
            for (auto const& [pane, name] : names)
            {
                bool const current = es.pane == pane;
                tabs.push_back({std::format(" {} ", name), current ? Style{Role::Accent, true, true, false} : plain(Role::Muted)});
                tabs.push_back({" ", plain(Role::Text)});
            }
            draw_line(ui, main, main.y, tabs);
            bool const show_summary = banner == 0 && es.pane != Pane::Results && es.ws.has_value();
            int const head_rows = show_summary ? 2 : 1;
            if (show_summary)
                draw_line(ui, main, main.y + 1, summary_line(ui, main.w));
            Rect const pane_rect{main.x, main.y + head_rows, main.w, main.h - head_rows};
            switch (es.pane)
            {
                case Pane::Instructions:
                    draw_instructions_box(ui, pane_rect, true);
                    break;
                case Pane::Files:
                    draw_files_preview_box(ui, pane_rect, true);
                    break;
                case Pane::Results:
                    draw_results_box(ui, pane_rect, true);
                    break;
            }
        }

        [[nodiscard]] std::vector<std::string> chunks(std::string_view s, int width)
        {
            std::vector<std::string> out;
            std::string cur;
            int used = 0;
            for (std::size_t i = 0; i < s.size();)
            {
                auto const d = text::decode_utf8(s, i);
                int const cw = d.valid ? text::char_width(d.cp) : 1;
                if (used + cw > width && used > 0)
                {
                    out.push_back(std::move(cur));
                    cur.clear();
                    used = 0;
                }
                cur += s.substr(i, d.length);
                used += cw;
                i += d.length;
            }
            out.push_back(std::move(cur));
            return out;
        }

        void draw_solution(Ui& ui, Rect area)
        {
            auto const& sol = *ui.m.exercise.solution;
            auto const inner = draw_box(ui, area, std::format("Solution: {}", clean_line(ui.m.exercise.ex->title)), true);
            if (inner.empty())
                return;
            std::vector<Line> lines;
            bool const side = inner.w >= 90;
            int const num_w = 4;
            int const cell_w = std::max(8, (inner.w - 2 * (num_w + 1) - 3) / 2);
            for (auto const& f : sol.files)
            {
                lines.push_back(text_line(clean_line(f.path), plain(Role::Accent, true)));
                if (f.identical && sol.has_workspace)
                {
                    lines.push_back(text_line("Your file already matches the solution.", plain(Role::Ok)));
                    lines.push_back({});
                    continue;
                }
                if (side)
                {
                    Line head;
                    head.push_back({pad_right(sol.has_workspace ? "     yours" : "     (empty)", num_w + 1 + cell_w + 3), plain(Role::Muted, true)});
                    head.push_back({"     solution", plain(Role::Muted, true)});
                    lines.push_back(std::move(head));
                    for (auto const& row : f.rows)
                    {
                        Role lr = Role::Text;
                        Role rr = Role::Text;
                        std::string mark = " ";
                        Role mark_role = Role::Muted;
                        if (row.kind == diff::RowKind::Change)
                        {
                            lr = Role::Del;
                            rr = Role::Add;
                            mark = "~";
                            mark_role = Role::Warn;
                        }
                        else if (row.kind == diff::RowKind::Delete)
                        {
                            lr = Role::Del;
                            mark = "-";
                            mark_role = Role::Del;
                        }
                        else if (row.kind == diff::RowKind::Insert)
                        {
                            rr = Role::Add;
                            mark = "+";
                            mark_role = Role::Add;
                        }
                        auto const lefts = chunks(clean_line(row.left), cell_w);
                        auto const rights = chunks(clean_line(row.right), cell_w);
                        for (std::size_t k = 0; k < std::max(lefts.size(), rights.size()); ++k)
                        {
                            Line l;
                            bool const first = k == 0;
                            auto const ln = first && row.left_number ? pad_left(std::to_string(*row.left_number), num_w) : std::string(to_size(num_w), ' ');
                            auto const rn = first && row.right_number ? pad_left(std::to_string(*row.right_number), num_w) : std::string(to_size(num_w), ' ');
                            l.push_back({ln + " ", plain(Role::Muted)});
                            l.push_back({pad_right(k < lefts.size() ? lefts[k] : std::string(), cell_w), plain(lr)});
                            l.push_back({" " + (first ? mark : std::string(" ")) + " ", plain(mark_role, true)});
                            l.push_back({rn + " ", plain(Role::Muted)});
                            l.push_back({k < rights.size() ? rights[k] : std::string(), plain(rr)});
                            lines.push_back(std::move(l));
                        }
                    }
                }
                else
                {
                    for (auto const& raw : text::split_lines(f.unified))
                    {
                        Role r = Role::Text;
                        if (raw.starts_with("+++") || raw.starts_with("---"))
                            r = Role::Muted;
                        else if (raw.starts_with("@@"))
                            r = Role::Accent;
                        else if (raw.starts_with("+"))
                            r = Role::Add;
                        else if (raw.starts_with("-"))
                            r = Role::Del;
                        for (auto const& piece : chunks(clean_line(raw), inner.w))
                            lines.push_back(text_line(piece, plain(r)));
                    }
                }
                lines.push_back({});
            }
            if (!sol.explanation.empty())
            {
                markdown_lines(ui, lines, sol.explanation, inner.w, false);
            }
            int const clamped = clamp_scroll(sol.scroll, to_int(lines.size()), inner.h);
            draw_lines(ui, inner, lines, clamped);
            draw_scrollbar(ui, area, clamped, to_int(lines.size()), inner.h);
            ui.m.scroll_content = to_int(lines.size());
            ui.m.scroll_viewport = inner.h;
        }

        [[nodiscard]] std::string lesson_word(catalog::Exercise const& ex)
        {
            if (ex.is_boss())
                return "boss fight";
            if (ex.is_lecture())
                return "lecture";
            if (ex.staged())
                return std::format("{} stages", ex.stages.size());
            return kebab_words(catalog::to_string(ex.kind));
        }

        [[nodiscard]] Line boss_marker(Ui const& ui, model::Model const& m, app::TrackSummary const& t)
        {
            Line l;
            if (t.boss_id.empty())
                return l;
            if (t.boss_defeated)
            {
                l.push_back({std::format("  {} ", ui.gl.skull), plain(Role::Muted)});
                l.push_back({"boss defeated", plain(Role::Ok)});
            }
            else
            {
                l.push_back({std::format("  {} ", ui.gl.skull), plain(Role::Boss, true)});
                l.push_back({m.status_of(t.boss_id) == progress::Status::InProgress ? "boss in progress" : "boss ahead", plain(Role::Boss)});
            }
            return l;
        }

        [[nodiscard]] Line exercise_row(Ui const& ui, catalog::Exercise const& ex, int width, bool with_difficulty)
        {
            auto const st = ui.m.status_of(ex.id);
            Line l;
            append(l, status_glyph(ui, st));
            l.push_back({" ", plain(Role::Text)});
            std::string const word(model::status_word(st));
            std::string const level = with_difficulty ? difficulty_word(ex.difficulty) : "";
            int const right_w = (with_difficulty ? 13 : 0) + 12;
            int const title_w = std::max(4, width - ui.gl.status_width - 1 - right_w - 2);
            std::string prefix;
            std::string suffix;
            if (ex.is_boss())
                prefix = std::format("{} BOSS ", ui.gl.skull);
            if (ex.is_lecture())
                suffix = " (lecture)";
            else if (ex.optional)
                suffix = " (bonus)";
            int const name_w = std::max(1, title_w - width_of(prefix) - width_of(suffix));
            auto const name = fit(ui, clean_line(ex.title), name_w);
            if (!prefix.empty())
                l.push_back({prefix, plain(Role::Boss, true)});
            l.push_back({name, plain(Role::Text, st == progress::Status::InProgress || ex.is_boss())});
            if (!suffix.empty())
                l.push_back({suffix, plain(Role::Muted)});
            l.push_back({std::string(to_size(std::max(0, title_w - width_of(prefix) - width_of(name) - width_of(suffix))), ' '), plain(Role::Text)});
            l.push_back({"  ", plain(Role::Text)});
            if (with_difficulty)
                l.push_back({pad_right(level, 13), plain(Role::Muted)});
            l.push_back({pad_right(word, 12), plain(status_role(st))});
            return l;
        }

        void draw_home(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const inner = draw_box(ui, main, "", false);
            if (inner.empty())
                return;
            auto const items = m.home_items();
            std::vector<Line> lines;
            std::vector<int> item_line(items.size(), -1);
            std::vector<bool> is_selected(items.size(), false);
            auto section = [&](model::HomeKind kind, std::string_view title) {
                bool any = false;
                for (std::size_t i = 0; i < items.size(); ++i)
                {
                    if (items[i].kind != kind)
                        continue;
                    if (!any)
                    {
                        if (!lines.empty())
                            lines.push_back({});
                        lines.push_back(text_line(std::string(title), plain(Role::Accent, true)));
                        any = true;
                    }
                    auto const* ex = m.ctx->catalog.find(items[i].id);
                    if (!ex)
                        continue;
                    bool const selected = i == std::min(m.home_sel, items.size() - 1);
                    item_line[i] = to_int(lines.size());
                    is_selected[i] = selected;
                    Line row;
                    row.push_back({selected ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                    append(row, exercise_row(ui, *ex, inner.w - 2, inner.w >= 56));
                    lines.push_back(std::move(row));
                    if (kind != model::HomeKind::Recent)
                    {
                        std::string detail = std::format("{}{}{}", clean_line(ex->id), ui.sep(), kebab_words(catalog::to_string(ex->kind)));
                        if (kind == model::HomeKind::Continue)
                        {
                            auto it = m.prog.exercises.find(ex->id);
                            if (it != m.prog.exercises.end() && it->second.attempts > 0)
                                detail += std::format("{}last check {} of {} passed", ui.sep(), it->second.last_passed, it->second.last_total);
                        }
                        else
                            detail += std::format("{}about {} min", ui.sep(), ex->estimated_minutes);
                        lines.push_back(text_line("    " + fit(ui, detail, std::max(1, inner.w - 4)), plain(Role::Muted)));
                    }
                }
            };
            section(model::HomeKind::Continue, "Continue where you left off");
            section(model::HomeKind::Next, "Recommended next");
            if (items.empty())
            {
                lines.push_back(text_line("You have not started an exercise yet.", plain(Role::Text)));
            }
            if (!lines.empty())
                lines.push_back({});
            lines.push_back(text_line("Chapters", plain(Role::Accent, true)));
            int name_w = 8;
            for (auto const& t : m.summary.tracks)
                name_w = std::max(name_w, width_of(clean_line(t.title)) + 4);
            name_w = std::min(name_w, std::max(8, inner.w / 3));
            bool const roomy = inner.w >= 64;
            int const bar_w = std::clamp(inner.w - name_w - (roomy ? 36 : 16), 6, 20);
            for (auto const& t : m.summary.tracks)
            {
                Line l;
                l.push_back({"  " + pad_right(fit(ui, std::format("{:>2} {}", t.number, clean_line(t.title)), name_w), name_w) + " ", plain(Role::Text, t.done)});
                append(l, bar_spans(ui, bar_w, t.completed, t.total));
                l.push_back({std::format("  {} of {}", t.completed, t.total), plain(Role::Muted)});
                if (roomy)
                    append(l, boss_marker(ui, m, t));
                lines.push_back(std::move(l));
            }
            section(model::HomeKind::Recent, "Recent");
            int focus = 0;
            for (std::size_t i = 0; i < items.size(); ++i)
                if (is_selected[i] && item_line[i] >= 0)
                    focus = item_line[i];
            int const start = first_visible(focus, to_int(lines.size()), inner.h);
            for (int i = 0; i < inner.h; ++i)
            {
                auto const idx = start + i;
                if (idx >= to_int(lines.size()))
                    break;
                bool selected = false;
                for (std::size_t k = 0; k < items.size(); ++k)
                    if (item_line[k] == idx && is_selected[k])
                        selected = true;
                draw_line(ui, inner, inner.y + i, lines[to_size(idx)], selected);
            }
            draw_scrollbar(ui, main, start, to_int(lines.size()), inner.h);
        }

        void draw_learn(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const list = m.learn_list();
            auto const inner = draw_box(ui, main, "", false, std::format("{} {}", list.size(), list.size() == 1 ? "lesson" : "lessons"));
            if (inner.empty())
                return;
            auto const& tracks = m.ctx->catalog.tracks();
            auto const filter_text = [&](std::string_view label, std::string_view value, bool active) {
                Line l;
                l.push_back({std::format("{} ", label), plain(Role::Muted)});
                l.push_back({std::string(value), plain(active ? Role::Accent : Role::Text, active)});
                return l;
            };
            Line f;
            append(f, filter_text("status", model::to_string(m.filters.status), m.filters.status != model::StatusFilter::All));
            f.push_back({"  ", plain(Role::Text)});
            append(f, filter_text("level", m.filters.difficulty ? difficulty_word(model::all_difficulties[m.filters.difficulty - 1]) : "any",
                                  m.filters.difficulty != 0));
            f.push_back({"  ", plain(Role::Text)});
            append(f, filter_text("chapter", m.filters.track && m.filters.track <= tracks.size() ? clean_line(tracks[m.filters.track - 1].title) : "all",
                                  m.filters.track != 0));
            f.push_back({"  ", plain(Role::Text)});
            append(f, filter_text("kind", m.filters.kind ? kebab_words(catalog::to_string(model::all_kinds[m.filters.kind - 1])) : "any", m.filters.kind != 0));
            int y = inner.y;
            draw_line(ui, inner, y++, f);
            Line search;
            search.push_back({"search ", plain(Role::Muted)});
            if (m.searching || !m.query.empty())
            {
                search.push_back({"/ ", plain(Role::Accent, true)});
                search.push_back({clean_line(m.query), plain(Role::Text, true)});
                if (m.searching)
                    search.push_back({"_", plain(Role::Accent, true)});
            }
            else
                search.push_back({"none", plain(Role::Text)});
            draw_line(ui, inner, y++, search);
            bool const show_detail = inner.h >= 12;
            int const detail_h = show_detail ? 3 : 0;
            Rect const list_area{inner.x, y + 1, inner.w, inner.bottom() - y - 1 - detail_h};
            draw_rule(ui, inner, y);
            std::vector<Line> rows;
            std::vector<bool> selectable;
            int focus = 0;
            std::string current_track;
            auto const idx = m.learn_index(list);
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                auto const& ex = *list[i];
                if (m.query.empty() && ex.track != current_track)
                {
                    current_track = ex.track;
                    auto const* t = m.ctx->catalog.find_track(ex.track);
                    int done = 0;
                    int total = 0;
                    if (t)
                        for (auto const& id : t->exercise_ids)
                        {
                            ++total;
                            if (m.status_of(id) == progress::Status::Completed)
                                ++done;
                        }
                    if (!rows.empty())
                    {
                        rows.push_back({});
                        selectable.push_back(false);
                    }
                    Line head;
                    if (t)
                        head.push_back({std::format("Chapter {}  ", t->number), plain(Role::Muted, true)});
                    head.push_back({t ? clean_line(t->title) : clean_line(ex.track), plain(Role::Accent, true)});
                    head.push_back({std::format("  {} of {} completed", done, total), plain(Role::Muted)});
                    rows.push_back(std::move(head));
                    selectable.push_back(false);
                }
                bool const selected = i == idx;
                if (selected)
                    focus = to_int(rows.size());
                Line row;
                row.push_back({selected ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                append(row, exercise_row(ui, ex, list_area.w - 2, list_area.w >= 56));
                if (m.bookmarked(ex.id) && list_area.w >= 20)
                {
                    row.push_back({" *", plain(Role::Warn)});
                }
                rows.push_back(std::move(row));
                selectable.push_back(true);
            }
            if (list.empty())
            {
                rows.push_back(text_line(m.query.empty() ? "No exercises match the filters." : "No exercise matches the search.", plain(Role::Muted)));
                selectable.push_back(false);
            }
            int const start = first_visible(focus, to_int(rows.size()), list_area.h);
            for (int i = 0; i < list_area.h; ++i)
            {
                auto const ri = start + i;
                if (ri >= to_int(rows.size()))
                    break;
                draw_line(ui, list_area, list_area.y + i, rows[to_size(ri)], ri == focus && selectable[to_size(ri)]);
            }
            draw_scrollbar(ui, main, start, to_int(rows.size()), list_area.h);
            if (show_detail && !list.empty())
            {
                auto const& ex = *list[idx];
                int const dy = inner.bottom() - detail_h;
                draw_rule(ui, inner, dy);
                auto const wrapped = text::wrap(clean_line(markdown_inline(ex.summary)), to_size(std::max(4, inner.w)));
                if (!wrapped.empty())
                    draw_line(ui, inner, dy + 1, text_line(fit(ui, wrapped[0], inner.w), plain(Role::Text)));
                std::string concepts;
                for (auto const& c : ex.concepts)
                    concepts += (concepts.empty() ? "" : ", ") + clean_line(c);
                std::vector<std::string> missing;
                for (auto const& id : ex.prerequisites)
                    if (m.status_of(id) != progress::Status::Completed)
                        if (auto const* pre = m.ctx->catalog.find(id))
                            missing.push_back(clean_line(pre->title));
                if (!missing.empty() && m.status_of(ex.id) == progress::Status::NotStarted)
                    draw_line(ui, inner, dy + 2, text_line(fit(ui, std::format("Comes after: {}", text::join(missing, ", ")), inner.w), plain(Role::Warn)));
                else
                    draw_line(ui, inner, dy + 2,
                              text_line(fit(ui, std::format("{}{}{} min{}{}", lesson_word(ex), ui.sep(), ex.estimated_minutes, ui.sep(), concepts), inner.w),
                                        plain(Role::Muted)));
            }
        }

        void draw_progress(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const inner = draw_box(ui, main, "", false);
            if (inner.empty())
                return;
            auto const& s = m.summary;
            std::vector<Line> lines;
            Line overall;
            overall.push_back({"Overall  ", plain(Role::Accent, true)});
            append(overall, bar_spans(ui, std::clamp(inner.w - 30, 6, 30), s.completed, s.total));
            lines.push_back(std::move(overall));
            Line counts;
            counts.push_back({std::format("{} completed", s.completed), plain(Role::Ok)});
            counts.push_back({ui.sep(), plain(Role::Muted)});
            counts.push_back({std::format("{} in progress", s.in_progress), plain(Role::Warn)});
            counts.push_back({ui.sep(), plain(Role::Muted)});
            counts.push_back({std::format("{} not started", s.not_started), plain(Role::Muted)});
            if (s.needs_revisit > 0)
            {
                counts.push_back({ui.sep(), plain(Role::Muted)});
                counts.push_back({std::format("{} updated", s.needs_revisit), plain(Role::Accent)});
            }
            lines.push_back(std::move(counts));
            lines.push_back({});
            Line journey;
            journey.push_back({"Journey", plain(Role::Accent, true)});
            journey.push_back({std::format("  {} of {} chapters complete", s.chapters_done, s.tracks.size()), plain(Role::Muted)});
            if (s.bosses_total > 0)
                journey.push_back({std::format("{}{} of {} bosses defeated", ui.sep(), s.bosses_defeated, s.bosses_total), plain(Role::Muted)});
            lines.push_back(std::move(journey));
            int name_w = 8;
            for (auto const& t : s.tracks)
                name_w = std::max(name_w, width_of(clean_line(t.title)) + 4);
            name_w = std::min(name_w, std::max(8, inner.w / 3));
            bool const roomy = inner.w >= 64;
            int const bar_w = std::clamp(inner.w - name_w - (roomy ? 36 : 16), 6, 20);
            for (auto const& t : s.tracks)
            {
                Line l;
                l.push_back({"  " + pad_right(fit(ui, std::format("{:>2} {}", t.number, clean_line(t.title)), name_w), name_w) + " ", plain(Role::Text, t.done)});
                append(l, bar_spans(ui, bar_w, t.completed, t.total));
                l.push_back({std::format("  {} of {}", t.completed, t.total), plain(Role::Muted)});
                if (roomy)
                    append(l, boss_marker(ui, m, t));
                lines.push_back(std::move(l));
            }
            auto const earned = app::achievements(*m.ctx, m.prog);
            lines.push_back({});
            lines.push_back(text_line("Achievements", plain(Role::Accent, true)));
            if (earned.empty())
                lines.push_back(text_line("  Complete a chapter or defeat a boss to earn the first one.", plain(Role::Muted)));
            for (auto const& a : earned)
            {
                Line l;
                l.push_back({"  ", plain(Role::Text)});
                l.push_back({a.boss ? std::format("{} ", ui.gl.skull) : std::format("{} ", ui.gl.done), plain(a.boss ? Role::Boss : Role::Ok, true)});
                l.push_back({clean_line(a.title), plain(Role::Text)});
                if (!a.detail.empty())
                    l.push_back({std::format("  ({})", clean_line(a.detail)), plain(Role::Muted)});
                lines.push_back(std::move(l));
            }
            lines.push_back({});
            lines.push_back(text_line("Worth another look", plain(Role::Accent, true)));
            int focus = 0;
            if (m.review.empty())
                lines.push_back(text_line("  Nothing to review yet.", plain(Role::Muted)));
            std::vector<int> review_lines;
            for (std::size_t i = 0; i < m.review.size(); ++i)
            {
                auto const& item = m.review[i];
                bool const selected = i == std::min(m.review_sel, m.review.size() - 1);
                Line row;
                row.push_back({selected ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                row.push_back({fit(ui, clean_line(item.exercise->title), std::max(8, inner.w / 3)) + "  ", plain(Role::Text)});
                row.push_back({clean_line(item.reason), plain(Role::Muted)});
                if (selected)
                    focus = to_int(lines.size());
                review_lines.push_back(to_int(lines.size()));
                lines.push_back(std::move(row));
            }
            lines.push_back({});
            lines.push_back(text_line(std::format("Bookmarks ({})", m.prog.bookmarks.size()), plain(Role::Accent, true)));
            auto const marks = m.bookmark_list();
            if (marks.empty())
                lines.push_back(text_line("  No bookmarks yet.", plain(Role::Muted)));
            for (auto const* ex : marks)
                lines.push_back(text_line("  " + fit(ui, clean_line(ex->title), std::max(1, inner.w - 2)), plain(Role::Text)));
            int const start = first_visible(focus, to_int(lines.size()), inner.h);
            for (int i = 0; i < inner.h; ++i)
            {
                auto const idx = start + i;
                if (idx >= to_int(lines.size()))
                    break;
                bool selected = false;
                for (std::size_t k = 0; k < review_lines.size(); ++k)
                    if (review_lines[k] == idx && k == std::min(m.review_sel, m.review.size() - 1))
                        selected = true;
                draw_line(ui, inner, inner.y + i, lines[to_size(idx)], selected);
            }
            draw_scrollbar(ui, main, start, to_int(lines.size()), inner.h);
        }

        void draw_bookmarks(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const list = m.bookmark_list();
            auto const inner = draw_box(ui, main, "", false, std::format("{} saved", list.size()));
            if (inner.empty())
                return;
            if (list.empty())
            {
                draw_line(ui, inner, inner.y, text_line("No bookmarks yet.", plain(Role::Muted)));
                return;
            }
            auto const sel = std::min(m.bookmark_sel, list.size() - 1);
            int const start = first_visible(to_int(sel), to_int(list.size()), inner.h);
            for (int i = 0; i < inner.h; ++i)
            {
                auto const idx = to_size(start + i);
                if (idx >= list.size())
                    break;
                Line row;
                row.push_back({idx == sel ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                append(row, exercise_row(ui, *list[idx], inner.w - 2, inner.w >= 56));
                draw_line(ui, inner, inner.y + i, row, idx == sel);
            }
            draw_scrollbar(ui, main, start, to_int(list.size()), inner.h);
        }

        void draw_doctor(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const inner = draw_box(ui, main, "", false);
            if (inner.empty())
                return;
            std::vector<Line> lines;
            if (!m.doctor_report)
            {
                lines.push_back(text_line(m.doctor_running ? "Checking your setup..." : "Setup not checked yet.", plain(Role::Muted)));
            }
            else
            {
                auto const rep = m.doctor_view();
                Line sum;
                sum.push_back({std::format("{} ok", rep.count(doctor::Status::Ok)), plain(Role::Ok, true)});
                sum.push_back({ui.sep(), plain(Role::Muted)});
                sum.push_back({std::format("{} {}", rep.count(doctor::Status::Warn), rep.count(doctor::Status::Warn) == 1 ? "warning" : "warnings"), plain(Role::Warn, true)});
                sum.push_back({ui.sep(), plain(Role::Muted)});
                sum.push_back({std::format("{} {}", rep.count(doctor::Status::Error), rep.count(doctor::Status::Error) == 1 ? "error" : "errors"), plain(Role::Fail, true)});
                if (m.doctor_running)
                    sum.push_back({"   rechecking...", plain(Role::Muted)});
                lines.push_back(std::move(sum));
                lines.push_back({});
                for (auto const& item : rep.items)
                {
                    Role role = Role::Muted;
                    std::string word = "info";
                    switch (item.status)
                    {
                        case doctor::Status::Ok:
                            role = Role::Ok;
                            word = "ok";
                            break;
                        case doctor::Status::Warn:
                            role = Role::Warn;
                            word = "warn";
                            break;
                        case doctor::Status::Error:
                            role = Role::Fail;
                            word = "ERROR";
                            break;
                        case doctor::Status::Info:
                            break;
                    }
                    Line head;
                    head.push_back({pad_right(std::format("[{}]", word), 8), plain(role, true)});
                    head.push_back({clean_line(item.label), plain(Role::Text, true)});
                    lines.push_back(std::move(head));
                    if (!item.detail.empty())
                        for (auto const& piece : text::wrap(clean_line(item.detail), to_size(std::max(4, inner.w - 10))))
                            lines.push_back(text_line("        " + piece, plain(Role::Text)));
                    if (!item.fix.empty())
                    {
                        bool first = true;
                        for (auto const& piece : text::wrap(clean_line(item.fix), to_size(std::max(4, inner.w - 15))))
                        {
                            Line fix;
                            fix.push_back({first ? "        Fix: " : "             ", plain(Role::Accent, true)});
                            fix.push_back({piece, plain(Role::Text)});
                            lines.push_back(std::move(fix));
                            first = false;
                        }
                    }
                }
            }
            int const clamped = clamp_scroll(m.doctor_scroll, to_int(lines.size()), inner.h);
            draw_lines(ui, inner, lines, clamped);
            draw_scrollbar(ui, main, clamped, to_int(lines.size()), inner.h);
            m.scroll_content = to_int(lines.size());
            m.scroll_viewport = inner.h;
        }

        void draw_settings(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const rows = m.settings_rows();
            auto const inner = draw_box(ui, main, "", false);
            if (inner.empty())
                return;
            auto const sel = std::min(m.settings_sel, rows.size() - 1);
            int key_w = 10;
            for (auto const& r : rows)
                key_w = std::max(key_w, width_of(r.key));
            key_w = std::min(key_w, std::max(12, inner.w / 2));
            int const list_h = std::max(1, inner.h - 3);
            int const start = first_visible(to_int(sel), to_int(rows.size()), list_h);
            for (int i = 0; i < list_h; ++i)
            {
                auto const idx = to_size(start + i);
                if (idx >= rows.size())
                    break;
                auto const& r = rows[idx];
                Line l;
                l.push_back({idx == sel ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                l.push_back({pad_right(fit(ui, r.key, key_w), key_w) + "  ", plain(Role::Text)});
                l.push_back({fit(ui, clean_line(r.value), std::max(1, inner.w - key_w - 6)), plain(r.editable ? Role::Accent : Role::Muted, r.editable)});
                draw_line(ui, inner, inner.y + i, l, idx == sel);
            }
            draw_scrollbar(ui, main, start, to_int(rows.size()), list_h);
            int const dy = inner.bottom() - 2;
            draw_rule(ui, inner, dy - 1);
            auto const& cur = rows[sel];
            auto const wrapped = text::wrap(cur.description + (cur.editable ? "" : ". Change it with: learndc config set " + cur.key + " <value>"),
                                            to_size(std::max(4, inner.w)));
            for (std::size_t i = 0; i < wrapped.size() && i < 2; ++i)
                draw_line(ui, inner, dy + to_int(i), text_line(fit(ui, wrapped[i], inner.w), plain(Role::Muted)));
        }

        void draw_help(Ui& ui, Rect main)
        {
            auto const& m = ui.m;
            auto const inner = draw_box(ui, main, "", false);
            if (inner.empty())
                return;
            std::vector<Line> lines;
            for (auto const& section : m.help())
            {
                if (!lines.empty())
                    lines.push_back({});
                lines.push_back(text_line(section.title, plain(Role::Accent, true)));
                for (auto const& l : section.lines)
                {
                    auto const gap = l.find("  ");
                    bool const keyed = gap != std::string::npos && gap < 16 && section.title != "Safety";
                    if (keyed)
                    {
                        auto const key = l.substr(0, gap);
                        std::size_t body_start = gap;
                        while (body_start < l.size() && l[body_start] == ' ')
                            ++body_start;
                        auto const body = l.substr(body_start);
                        int const key_w = 14;
                        auto const pieces = text::wrap(body, to_size(std::max(4, inner.w - key_w - 2)));
                        for (std::size_t i = 0; i < pieces.size(); ++i)
                        {
                            Line row;
                            row.push_back({"  " + pad_right(i == 0 ? key : "", key_w), plain(Role::Key, true)});
                            row.push_back({pieces[i], plain(Role::Text)});
                            lines.push_back(std::move(row));
                        }
                    }
                    else
                        for (auto const& piece : hanging(l, inner.w - 2, "  ", "  "))
                            lines.push_back(text_line(piece, plain(Role::Text)));
                }
            }
            int const clamped = clamp_scroll(m.help_scroll, to_int(lines.size()), inner.h);
            draw_lines(ui, inner, lines, clamped);
            draw_scrollbar(ui, main, clamped, to_int(lines.size()), inner.h);
            m.scroll_content = to_int(lines.size());
            m.scroll_viewport = inner.h;
        }

        void draw_onboarding(Ui& ui, Rect area)
        {
            auto const& m = ui.m;
            int const w = std::min(area.w, 64);
            int const h = std::min(area.h, 13);
            Rect const box{area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h};
            auto const inner = draw_box(ui, box, "Welcome to learndc", true, std::format("step {} of 3", m.onboarding.step + 1));
            if (inner.empty())
                return;
            std::string question;
            std::string explain;
            std::vector<std::string> labels;
            switch (m.onboarding.step)
            {
                case 0:
                    question = "How would you describe yourself?";
                    explain = "This only changes the tone of a few tips.";
                    labels = {"new: I am new to programming", "experienced: I know another language"};
                    break;
                case 1:
                    question = "Which editor should open your files?";
                    explain = "learndc starts it for you. Custom commands: learndc config set editor.command.";
                    labels = m.onboarding.editor_labels;
                    break;
                default:
                    question = "Pick a color theme";
                    explain = "You can change it later in Settings.";
                    labels = {"dark", "light", "high-contrast", "no-color: no colors at all"};
                    break;
            }
            int y = inner.y;
            draw_line(ui, inner, y++, text_line(question, plain(Role::Text, true)));
            for (auto const& piece : text::wrap(explain, to_size(std::max(4, inner.w))))
                draw_line(ui, inner, y++, text_line(piece, plain(Role::Muted)));
            ++y;
            for (std::size_t i = 0; i < labels.size() && y < inner.bottom() - 2; ++i)
            {
                bool const selected = i == m.onboarding.selected;
                Line l;
                l.push_back({selected ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                l.push_back({fit(ui, labels[i], inner.w - 2), plain(Role::Text)});
                draw_line(ui, inner, y++, l, selected);
            }
        }

        void draw_dialog(Ui& ui, Rect area)
        {
            auto const& d = ui.m.dialog;
            if (!d.open())
                return;
            int const w = std::min(area.w - 2, 62);
            std::vector<Line> lines;
            bool const listed = d.kind == model::DialogKind::Choice || d.kind == model::DialogKind::Complete;
            if (d.boss)
            {
                if (ui.ascii)
                    lines.push_back(text_line("  [X]  BOSS DEFEATED  [X]", plain(Role::Boss, true)));
                else
                {
                    std::string skulls;
                    for (int i = 0; i < 3; ++i)
                        skulls += std::format("{}  ", ui.gl.skull);
                    lines.push_back(text_line(std::format("  {}BOSS DEFEATED  {}", skulls, skulls), plain(Role::Boss, true)));
                }
                lines.push_back({});
            }
            for (std::size_t bi = 0; bi < d.body.size(); ++bi)
                for (auto const& piece : text::wrap(clean_line(d.body[bi]), to_size(std::max(4, w - 4))))
                    lines.push_back(text_line(piece, d.kind == model::DialogKind::Complete && bi == 0 ? plain(Role::Accent, true) : plain(Role::Text)));
            if (listed)
            {
                lines.push_back({});
                for (std::size_t i = 0; i < d.labels.size(); ++i)
                {
                    bool const selected = i == d.selected;
                    Line l;
                    l.push_back({selected ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                    l.push_back({clean_line(d.labels[i]), plain(Role::Text)});
                    lines.push_back(std::move(l));
                }
            }
            else if (d.kind == model::DialogKind::Reset)
            {
                lines.push_back({});
                Line l;
                l.push_back({"Confirm: ", plain(Role::Muted)});
                l.push_back({d.input + "_", plain(Role::Accent, true)});
                lines.push_back(std::move(l));
                lines.push_back({});
                Line buttons;
                buttons.push_back({"[Enter]", plain(Role::Key, true)});
                buttons.push_back({std::format(" {}   ", d.yes_label.empty() ? std::string("Yes") : d.yes_label), plain(Role::Text)});
                buttons.push_back({"[Esc]", plain(Role::Key, true)});
                buttons.push_back({std::format(" {}", d.no_label.empty() ? std::string("No") : d.no_label), plain(Role::Text)});
                lines.push_back(std::move(buttons));
            }
            else
            {
                lines.push_back({});
                Line buttons;
                buttons.push_back({"[y]", plain(Role::Key, true)});
                buttons.push_back({std::format(" {}   ", d.yes_label.empty() ? std::string("Yes") : d.yes_label), plain(Role::Text)});
                buttons.push_back({"[n]", plain(Role::Key, true)});
                buttons.push_back({std::format(" {}", d.no_label.empty() ? std::string("No") : d.no_label), plain(Role::Text)});
                lines.push_back(std::move(buttons));
            }
            int const h = std::min(area.h, to_int(lines.size()) + 2);
            Rect const box{area.x + (area.w - w) / 2, area.y + std::max(0, (area.h - h) / 2), w, h};
            ui.g.fill(box, plain(Role::Text));
            auto const inner = draw_box(ui, box, d.title, true);
            int const start = listed
                                  ? first_visible(to_int(d.selected) + to_int(lines.size()) - to_int(d.labels.size()), to_int(lines.size()), inner.h)
                                  : 0;
            for (int i = 0; i < inner.h; ++i)
            {
                auto const idx = start + i;
                if (idx >= to_int(lines.size()))
                    break;
                bool const selected =
                    listed && to_size(idx) == to_size(to_int(lines.size()) - to_int(d.labels.size())) + d.selected;
                draw_line(ui, inner, inner.y + i, lines[to_size(idx)], selected);
            }
        }

        void draw_too_small(Ui& ui)
        {
            Rect const all{0, 0, ui.size.width, ui.size.height};
            int const avail = std::max(1, ui.size.width - 2);
            std::vector<std::pair<std::string, Style>> msgs;
            auto push = [&](std::string const& t, Style st) {
                for (auto const& piece : text::wrap(t, to_size(avail)))
                    msgs.push_back({piece, st});
            };
            push("learndc needs a larger window", plain(Role::Accent, true));
            push(std::format("This one is {}x{}; at least {}x{} is needed.", ui.size.width, ui.size.height, model::compact_threshold.width,
                             model::compact_threshold.height),
                 plain(Role::Text));
            push("Enlarge it, or run:", plain(Role::Muted));
            push("learndc list", plain(Role::Key, true));
            int y = std::max(0, (ui.size.height - to_int(msgs.size())) / 2);
            for (auto const& [line, st] : msgs)
            {
                int const x = std::max(0, (ui.size.width - width_of(line)) / 2);
                ui.g.put(x, y++, fit(ui, line, ui.size.width), st, all);
            }
        }

        void draw_header(Ui& ui, Rect row)
        {
            auto const& m = ui.m;
            Line l;
            l.push_back({" " + clean_line(m.ctx->catalog.title().empty() ? std::string("learndc") : m.ctx->catalog.title()), plain(Role::Accent, true)});
            std::string where(model::to_string(m.screen));
            if (m.screen == model::Screen::Exercise && m.exercise.ex)
                where = clean_line(m.exercise.ex->title);
            l.push_back({std::format("{}{}", ui.sep(), where), plain(Role::Text, true)});
            auto const watching = m.exercise.watching && m.exercise.ws.has_value() && m.screen != model::Screen::Exercise;
            if (watching)
            {
                l.push_back({"  ", plain(Role::Text)});
                l.push_back({std::format(" {} WATCHING ", ui.gl.watch), Style{Role::Watch, true, true, false}});
            }
            draw_line(ui, row, row.y, l);
            std::string const done = std::format("{} of {} completed ", m.summary.completed, m.summary.total);
            int const bar_w = row.w >= 70 ? 12 : 0;
            int const total_w = width_of(done) + (bar_w > 0 ? bar_w + 1 : 0);
            if (row.w > total_w + line_width(l) + 2)
            {
                Line r;
                r.push_back({done, plain(Role::Muted)});
                if (bar_w > 0)
                {
                    append(r, bar_spans(ui, bar_w, m.summary.completed, m.summary.total));
                    r.push_back({" ", plain(Role::Text)});
                }
                draw_line(ui, Rect{row.right() - total_w, row.y, total_w, 1}, row.y, r);
            }
        }

        void draw_tab_bar(Ui& ui, Rect row)
        {
            auto const& m = ui.m;
            static constexpr std::array<std::pair<model::Screen, std::string_view>, 8> tabs = {
                std::pair{model::Screen::Home, "Home"},           std::pair{model::Screen::Learn, "Learn"},
                std::pair{model::Screen::Exercise, "Lesson"},     std::pair{model::Screen::Progress, "Progress"},
                std::pair{model::Screen::Bookmarks, "Bookmarks"}, std::pair{model::Screen::Doctor, "Doctor"},
                std::pair{model::Screen::Settings, "Settings"},   std::pair{model::Screen::Review, "Review"}};
            auto build = [&](bool full) {
                Line l;
                l.push_back({" ", plain(Role::Text)});
                int n = 0;
                for (auto const& [screen, name] : tabs)
                {
                    ++n;
                    bool const current = m.screen == screen;
                    if (current)
                        l.push_back({std::format(" {} {} ", n, name), Style{Role::Accent, true, true, false}});
                    else if (full)
                        l.push_back({std::format(" {} {} ", n, name), plain(Role::Muted)});
                    else
                        l.push_back({std::format(" {} ", n), plain(Role::Muted)});
                }
                return l;
            };
            bool const chip = m.exercise.watching && m.exercise.ws.has_value() && m.screen != model::Screen::Exercise;
            auto with_chip = [&](Line line) {
                if (chip)
                {
                    line.push_back({" ", plain(Role::Text)});
                    line.push_back({std::format(" {} WATCHING ", ui.gl.watch), Style{Role::Watch, true, true, false}});
                }
                return line;
            };
            auto line = with_chip(build(true));
            if (line_width(line) > row.w)
                line = with_chip(build(false));
            if (m.screen == model::Screen::Help)
            {
                line.clear();
                line.push_back({" Help ", Style{Role::Accent, true, true, false}});
                line = with_chip(std::move(line));
            }
            draw_line(ui, row, row.y, line);
        }

        void draw_sidebar(Ui& ui, Rect area, bool narrow)
        {
            auto const& m = ui.m;
            auto const inner = draw_box(ui, area, narrow ? "" : "Menu", false);
            if (inner.empty())
                return;
            static constexpr std::array<std::pair<model::Screen, std::string_view>, 8> entries = {
                std::pair{model::Screen::Home, "Home"},           std::pair{model::Screen::Learn, "Learn"},
                std::pair{model::Screen::Exercise, "Lesson"},     std::pair{model::Screen::Progress, "Progress"},
                std::pair{model::Screen::Bookmarks, "Bookmarks"}, std::pair{model::Screen::Doctor, "Doctor"},
                std::pair{model::Screen::Settings, "Settings"},   std::pair{model::Screen::Review, "Review"}};
            int y = inner.y;
            int n = 0;
            for (auto const& [screen, name] : entries)
            {
                ++n;
                bool const current = m.screen == screen || (m.screen == model::Screen::Help && m.help_from == screen);
                bool const dim = screen == model::Screen::Exercise && !m.exercise.ex;
                Line l;
                if (narrow)
                {
                    l.push_back({std::format("{}", n), plain(current ? Role::Text : (dim ? Role::Muted : Role::Text), current)});
                    draw_line(ui, inner, y++, l, current);
                    continue;
                }
                l.push_back({current ? std::format("{} ", ui.gl.sel) : std::string("  "), plain(Role::Accent, true)});
                l.push_back({std::format("{} ", n), plain(current ? Role::Text : Role::Muted, current)});
                l.push_back({std::string(name), plain(dim ? Role::Muted : Role::Text, current)});
                draw_line(ui, inner, y++, l, current);
            }
            ++y;
            Line help;
            if (narrow)
                help.push_back({"?", plain(Role::Text)});
            else
            {
                help.push_back({"  ? ", plain(Role::Muted)});
                help.push_back({"Help", plain(Role::Text)});
            }
            draw_line(ui, inner, y++, help, m.screen == model::Screen::Help);
        }

        void draw_status(Ui& ui, Rect row)
        {
            auto const& m = ui.m;
            if (m.dialog.open())
                return;
            if (!m.toasts.empty())
            {
                auto const& t = m.toasts.front();
                Line l;
                l.push_back({t.error ? std::format(" {} ", ui.gl.warn) : std::string(" "), plain(t.error ? Role::Fail : Role::Accent, true)});
                l.push_back({fit(ui, clean_line(t.text), row.w - 3), plain(t.error ? Role::Fail : Role::Accent, !t.error)});
                draw_line(ui, row, row.y, l);
                return;
            }
            bool const shown_in_pane = m.screen == model::Screen::Exercise;
            if (m.exercise.ex && m.check_running() && !shown_in_pane)
            {
                Line l = running_line(ui);
                l.insert(l.begin(), {" ", plain(Role::Text)});
                draw_line(ui, row, row.y, l);
                return;
            }
        }

        void draw_keys(Ui& ui, Rect row)
        {
            auto const& m = ui.m;
            auto hints = m.key_hints();
            auto const need = [](model::KeyHint const& h) { return width_of(h.key) + 1 + width_of(h.label) + 2; };
            auto const essential = [&](std::size_t i) {
                auto const& k = hints[i].key;
                return i == 0 || k == "?" || k == "q" || k == "Tab" || k == "Esc";
            };
            auto total = [&] {
                int t = 1;
                for (auto const& h : hints)
                    t += need(h);
                return t;
            };
            while (total() > row.w)
            {
                std::optional<std::size_t> victim;
                for (std::size_t i = hints.size(); i-- > 0;)
                    if (!essential(i))
                    {
                        victim = i;
                        break;
                    }
                if (!victim)
                    break;
                hints.erase(hints.begin() + static_cast<std::ptrdiff_t>(*victim));
            }
            Line l;
            l.push_back({" ", plain(Role::Text)});
            int used = 1;
            for (auto const& h : hints)
            {
                if (used + need(h) > row.w)
                    break;
                l.push_back({h.key, plain(Role::Key, true)});
                l.push_back({" " + h.label + "  ", plain(Role::Muted)});
                used += need(h);
            }
            draw_line(ui, row, row.y, l);
        }

        void draw_all(Ui& ui)
        {
            auto const& m = ui.m;
            int const total_w = ui.size.width;
            int const total_h = ui.size.height;
            auto const mode = model::layout_for(ui.size);
            if (mode == LayoutMode::TooSmall)
            {
                draw_too_small(ui);
                return;
            }
            if (m.screen == model::Screen::Onboarding)
            {
                draw_onboarding(ui, Rect{0, 0, total_w, total_h - 1});
                draw_keys(ui, Rect{0, total_h - 1, total_w, 1});
                draw_dialog(ui, Rect{0, 0, total_w, total_h - 1});
                return;
            }
            int const footer = mode == LayoutMode::Compact ? 1 : 2;
            Rect const body{0, 1, total_w, total_h - 1 - footer};
            bool const narrow_side = m.screen == model::Screen::Exercise;
            int const side_w = mode == LayoutMode::Compact ? 0 : (narrow_side ? 5 : (mode == LayoutMode::Wide ? 24 : 18));
            bool const solution = m.screen == model::Screen::Exercise && m.exercise.solution.has_value();
            if (mode == LayoutMode::Compact)
                draw_tab_bar(ui, Rect{0, 0, total_w, 1});
            else
                draw_header(ui, Rect{0, 0, total_w, 1});
            Rect main{side_w, body.y, total_w - side_w, body.h};
            if (solution)
                main = body;
            else if (side_w > 0)
                draw_sidebar(ui, Rect{0, body.y, side_w, body.h}, narrow_side);
            switch (m.screen)
            {
                case model::Screen::Home:
                    draw_home(ui, main);
                    break;
                case model::Screen::Learn:
                    draw_learn(ui, main);
                    break;
                case model::Screen::Exercise:
                    if (solution)
                        draw_solution(ui, main);
                    else
                        draw_exercise(ui, main);
                    break;
                case model::Screen::Progress:
                    draw_progress(ui, main);
                    break;
                case model::Screen::Bookmarks:
                    draw_bookmarks(ui, main);
                    break;
                case model::Screen::Doctor:
                    draw_doctor(ui, main);
                    break;
                case model::Screen::Settings:
                    draw_settings(ui, main);
                    break;
                case model::Screen::Help:
                    draw_help(ui, main);
                    break;
                case model::Screen::Review:
                    draw_review(ui, main);
                    break;
                case model::Screen::Onboarding:
                    break;
            }
            if (footer == 2)
            {
                draw_status(ui, Rect{0, total_h - 2, total_w, 1});
                draw_keys(ui, Rect{0, total_h - 1, total_w, 1});
            }
            else if (!m.dialog.open() && (!m.toasts.empty() || (m.exercise.ex && m.check_running() && m.screen != model::Screen::Exercise)))
                draw_status(ui, Rect{0, total_h - 1, total_w, 1});
            else
                draw_keys(ui, Rect{0, total_h - 1, total_w, 1});
            if (m.dialog.open())
            {
                ui.g.fill(body, plain(Role::Text));
                draw_dialog(ui, body);
            }
        }

        [[nodiscard]] std::optional<ftxui::Color> role_color(ThemeId theme, Role role)
        {
            using C = ftxui::Color;
            switch (theme)
            {
                case ThemeId::NoColor:
                    return std::nullopt;
                case ThemeId::Dark:
                    switch (role)
                    {
                        case Role::Inline:
                            return C::RGB(125, 211, 252);
                        case Role::Watch:
                            return C::RGB(251, 146, 60);
                        case Role::Boss:
                            return C::RGB(239, 68, 68);
                        case Role::Accent:
                        case Role::Focus:
                        case Role::Key:
                            return C::RGB(45, 212, 191);
                        case Role::Ok:
                        case Role::Add:
                            return C::RGB(74, 222, 128);
                        case Role::Fail:
                        case Role::Del:
                            return C::RGB(248, 113, 113);
                        case Role::Warn:
                            return C::RGB(250, 204, 21);
                        case Role::Muted:
                            return C::RGB(148, 163, 184);
                        case Role::Border:
                            return C::RGB(100, 116, 139);
                        case Role::Text:
                        case Role::Code:
                            return std::nullopt;
                    }
                    break;
                case ThemeId::Light:
                    switch (role)
                    {
                        case Role::Inline:
                            return C::RGB(3, 105, 161);
                        case Role::Watch:
                            return C::RGB(194, 65, 12);
                        case Role::Boss:
                            return C::RGB(185, 28, 28);
                        case Role::Accent:
                        case Role::Focus:
                        case Role::Key:
                            return C::RGB(0, 121, 107);
                        case Role::Ok:
                        case Role::Add:
                            return C::RGB(22, 128, 60);
                        case Role::Fail:
                        case Role::Del:
                            return C::RGB(185, 28, 28);
                        case Role::Warn:
                            return C::RGB(146, 84, 0);
                        case Role::Muted:
                            return C::RGB(90, 100, 112);
                        case Role::Border:
                            return C::RGB(120, 130, 142);
                        case Role::Text:
                        case Role::Code:
                            return std::nullopt;
                    }
                    break;
                case ThemeId::HighContrast:
                    switch (role)
                    {
                        case Role::Inline:
                            return std::nullopt;
                        case Role::Watch:
                            return C::RGB(255, 165, 0);
                        case Role::Boss:
                            return C::RGB(255, 80, 80);
                        case Role::Accent:
                        case Role::Focus:
                        case Role::Key:
                            return C::RGB(0, 255, 255);
                        case Role::Ok:
                        case Role::Add:
                            return C::RGB(0, 255, 0);
                        case Role::Fail:
                        case Role::Del:
                            return C::RGB(255, 100, 100);
                        case Role::Warn:
                            return C::RGB(255, 255, 0);
                        case Role::Border:
                            return C::RGB(255, 255, 255);
                        case Role::Muted:
                        case Role::Text:
                        case Role::Code:
                            return std::nullopt;
                    }
                    break;
            }
            return std::nullopt;
        }

        [[nodiscard]] ftxui::Element styled(std::string const& run, Style s, ThemeId theme)
        {
            ftxui::Element e = ftxui::text(run);
            if (auto c = role_color(theme, s.role))
                e = ftxui::color(*c, e);
            bool bold = s.bold || theme == ThemeId::HighContrast || s.role == Role::Boss;
            if (bold)
                e = ftxui::bold(e);
            if (s.role == Role::Muted && theme == ThemeId::NoColor)
                e = ftxui::dim(e);
            if (s.italic)
                e = ftxui::italic(e);
            if (s.role == Role::Inline && (theme == ThemeId::NoColor || theme == ThemeId::HighContrast))
                e = ftxui::inverted(e);
            if (s.inverse)
                e = ftxui::inverted(e);
            if (s.underline)
                e = ftxui::underlined(e);
            return e;
        }

        [[nodiscard]] ftxui::Element to_element(Grid const& g, ThemeId theme)
        {
            ftxui::Elements rows;
            for (int y = 0; y < g.height(); ++y)
            {
                ftxui::Elements runs;
                std::string current;
                Style current_style;
                bool have = false;
                auto flush = [&] {
                    if (have && !current.empty())
                        runs.push_back(styled(current, current_style, theme));
                    current.clear();
                };
                for (int x = 0; x < g.width(); ++x)
                {
                    auto const& cell = g.at(x, y);
                    if (cell.cont)
                        continue;
                    if (!have || !(cell.style == current_style))
                    {
                        flush();
                        current_style = cell.style;
                        have = true;
                    }
                    current += cell.glyph;
                }
                flush();
                rows.push_back(ftxui::hbox(std::move(runs)));
            }
            return ftxui::vbox(std::move(rows));
        }

        [[nodiscard]] ftxui::Element build(Model const& m, model::Size size)
        {
            Grid grid(size.width, size.height);
            Ui ui(grid, m, size);
            draw_all(ui);
            return to_element(grid, m.appearance.theme);
        }
    } // namespace

    std::shared_ptr<void> render_element(model::Model const& m)
    {
        return std::shared_ptr<void>(build(m, m.size));
    }

    std::string render_snapshot(model::Model const& m, int width, int height, bool ansi)
    {
        auto element = build(m, model::Size{width, height});
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
        ftxui::Render(screen, element);
        auto raw = screen.ToString();
        std::string out;
        for (auto const& line : text::split_lines(text::replace_all(raw, "\r\n", "\n")))
        {
            auto piece = ansi ? line : text::strip_ansi(line);
            if (!ansi)
                while (!piece.empty() && piece.back() == ' ')
                    piece.pop_back();
            out += piece;
            out += '\n';
        }
        return out;
    }
} // namespace learndc::tui
