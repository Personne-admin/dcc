export module dcc.diag;

import std;
import dcc.utf8;
import dcc.sm;

export namespace dcc::diag
{
    enum class Severity : std::uint8_t
    {
        Error,
        Warning,
        Note,
        Help,
    };

    [[nodiscard]] constexpr std::string_view to_string(Severity s) noexcept
    {
        switch (s)
        {
            case Severity::Error:
                return "error";
            case Severity::Warning:
                return "warning";
            case Severity::Note:
                return "note";
            case Severity::Help:
                return "help";
        }
        return "unknown";
    }

    enum class LabelStyle : std::uint8_t
    {
        Primary,
        Secondary,
    };

    enum class DiagnosticOrigin : std::uint8_t
    {
        Semantic,
        Parser,
    };

    struct Label
    {
        sm::SourceRange range;
        std::string message;
        LabelStyle style{LabelStyle::Primary};
    };

    struct FixIt
    {
        sm::SourceRange range;
        std::string replacement;
        std::string message;
    };

    class Diagnostic
    {
    public:
        constexpr explicit Diagnostic(Severity severity, std::string message) : m_severity{severity}, m_message{std::move(message)} {}

        Diagnostic&& primary(sm::SourceRange range, std::string msg = {}) &&
        {
            m_labels.push_back({range, std::move(msg), LabelStyle::Primary});
            return std::move(*this);
        }

        Diagnostic&& secondary(sm::SourceRange range, std::string msg = {}) &&
        {
            m_labels.push_back({range, std::move(msg), LabelStyle::Secondary});
            return std::move(*this);
        }

        Diagnostic&& note(std::string n) &&
        {
            m_notes.push_back(std::move(n));
            return std::move(*this);
        }

        Diagnostic&& help(std::string h) &&
        {
            m_helps.push_back(std::move(h));
            return std::move(*this);
        }

        Diagnostic&& fix(FixIt f) &&
        {
            m_fixes.push_back(std::move(f));
            return std::move(*this);
        }

        [[nodiscard]] Severity severity() const noexcept { return m_severity; }
        [[nodiscard]] std::string const& message() const noexcept { return m_message; }
        [[nodiscard]] std::span<Label const> labels() const noexcept { return m_labels; }
        [[nodiscard]] std::span<std::string const> notes() const noexcept { return m_notes; }
        [[nodiscard]] std::span<std::string const> helps() const noexcept { return m_helps; }
        [[nodiscard]] std::span<FixIt const> fixes() const noexcept { return m_fixes; }
        [[nodiscard]] DiagnosticOrigin origin() const noexcept { return m_origin; }
        void set_origin(DiagnosticOrigin origin) noexcept { m_origin = origin; }

    private:
        Severity m_severity;
        std::string m_message;
        std::vector<Label> m_labels;
        std::vector<std::string> m_notes;
        std::vector<std::string> m_helps;
        std::vector<FixIt> m_fixes;
        DiagnosticOrigin m_origin{DiagnosticOrigin::Semantic};
    };

    class DiagnosticEngine
    {
    public:
        explicit DiagnosticEngine(sm::SourceManager const& sm, std::ostream& os) : m_sm{sm}, m_os{os} {}

        void emit(Diagnostic diag)
        {
            if (m_parser_recovery_suppression_enabled && diag.origin() != DiagnosticOrigin::Parser && depends_on_parser_recovery(diag))
            {
                ++m_suppressed_parser_cascades;
                return;
            }

            switch (diag.severity())
            {
                case Severity::Error:
                    ++m_errors;
                    break;
                case Severity::Warning:
                    ++m_warnings;
                    break;
                default:
                    break;
            }

            if (m_max_errors > 0 && m_errors > m_max_errors)
                return;

            if (!m_silent)
                print(diag);

            m_emitted.push_back(std::move(diag));

            if (m_max_errors > 0 && m_errors == m_max_errors)
            {
                auto sentinel = Diagnostic{Severity::Error, std::format("too many errors emitted, stopping (limit: {})", m_max_errors)};
                if (!m_silent)
                    print(sentinel);

                m_emitted.push_back(std::move(sentinel));
            }
        }

        template <typename... Args> void error(sm::SourceRange range, std::format_string<Args...> fmt, Args&&... args)
        {
            emit(Diagnostic{Severity::Error, std::format(fmt, std::forward<Args>(args)...)}.primary(range));
        }

        template <typename... Args> void warning(sm::SourceRange range, std::format_string<Args...> fmt, Args&&... args)
        {
            emit(Diagnostic{Severity::Warning, std::format(fmt, std::forward<Args>(args)...)}.primary(range));
        }

        template <typename... Args> void note(sm::SourceRange range, std::format_string<Args...> fmt, Args&&... args)
        {
            emit(Diagnostic{Severity::Note, std::format(fmt, std::forward<Args>(args)...)}.primary(range));
        }

        template <typename... Args> void error(std::format_string<Args...> fmt, Args&&... args)
        {
            emit(Diagnostic{Severity::Error, std::format(fmt, std::forward<Args>(args)...)});
        }

        template <typename... Args> void warning(std::format_string<Args...> fmt, Args&&... args)
        {
            emit(Diagnostic{Severity::Warning, std::format(fmt, std::forward<Args>(args)...)});
        }

        [[nodiscard]] std::uint32_t error_count() const noexcept { return m_errors; }
        [[nodiscard]] std::uint32_t warning_count() const noexcept { return m_warnings; }
        [[nodiscard]] bool has_errors() const noexcept { return m_errors > 0; }

        [[nodiscard]] std::span<Diagnostic const> diagnostics() const noexcept { return m_emitted; }

        void clear_diagnostics() noexcept
        {
            m_emitted.clear();
            m_errors = 0;
            m_warnings = 0;
            m_parser_recovery_ranges.clear();
            m_suppressed_parser_cascades = 0;
            m_parser_recovery_suppression_enabled = false;
        }

        void set_parser_recovery_ranges(std::span<sm::SourceRange const> ranges) { m_parser_recovery_ranges.assign(ranges.begin(), ranges.end()); }

        [[nodiscard]] std::uint32_t suppressed_parser_cascade_count() const noexcept { return m_suppressed_parser_cascades; }
        void set_parser_recovery_suppression(bool enabled) noexcept { m_parser_recovery_suppression_enabled = enabled; }

        void set_silent(bool s) noexcept { m_silent = s; }
        [[nodiscard]] bool silent() const noexcept { return m_silent; }

        [[nodiscard]] sm::SourceManager const& source_manager() const noexcept { return m_sm; }

        [[nodiscard]] std::size_t diagnostic_count() const noexcept { return m_emitted.size(); }

        [[nodiscard]] std::span<Diagnostic const> diagnostics_since(std::size_t index) const noexcept
        {
            if (index >= m_emitted.size())
                return {};

            return std::span<Diagnostic const>{m_emitted}.subspan(index);
        }

        void set_max_errors(std::uint32_t n) noexcept { m_max_errors = n; }
        void set_color(bool enable) noexcept { m_color = enable; }
        void set_context_lines(std::uint32_t n) noexcept { m_context_lines = n; }
        void set_tab_width(std::uint32_t n) noexcept { m_tab_width = n; }

    private:
        sm::SourceManager const& m_sm;
        std::ostream& m_os;

        std::uint32_t m_errors{};
        std::uint32_t m_warnings{};
        std::uint32_t m_max_errors{};
        std::uint32_t m_suppressed_parser_cascades{};

        bool m_color{true};
        bool m_silent{false};
        bool m_parser_recovery_suppression_enabled{};
        std::uint32_t m_context_lines{1};
        std::uint32_t m_tab_width{4};

        std::vector<Diagnostic> m_emitted;
        std::vector<sm::SourceRange> m_parser_recovery_ranges;

        [[nodiscard]] bool depends_on_parser_recovery(Diagnostic const& diag) const noexcept
        {
            auto primary = std::ranges::find_if(diag.labels(), [](Label const& label) { return label.style == LabelStyle::Primary; });
            if (primary == diag.labels().end() || !primary->range.valid())
                return false;

            auto const location = primary->range.begin;
            return std::ranges::any_of(m_parser_recovery_ranges, [location](sm::SourceRange const& recovery) {
                return recovery.valid() && recovery.begin.fileId == location.fileId && location.offset >= recovery.begin.offset &&
                       location.offset < recovery.end.offset;
            });
        }

        struct ansi
        {
            static constexpr std::string_view reset = "\033[0m";
            static constexpr std::string_view bold = "\033[1m";
            static constexpr std::string_view bold_red = "\033[1;91m";
            static constexpr std::string_view bold_yel = "\033[1;93m";
            static constexpr std::string_view bold_blue = "\033[1;94m";
            static constexpr std::string_view bold_cyan = "\033[1;96m";
            static constexpr std::string_view bold_green = "\033[1;92m";
        };

        void style_on(std::string_view code)
        {
            if (m_color)
                m_os << code;
        }

        void style_off()
        {
            if (m_color)
                m_os << ansi::reset;
        }

        void bold_on()
        {
            if (m_color)
                m_os << ansi::bold;
        }

        [[nodiscard]] std::string_view severity_color(Severity s) const noexcept
        {
            switch (s)
            {
                case Severity::Error:
                    return ansi::bold_red;
                case Severity::Warning:
                    return ansi::bold_yel;
                case Severity::Note:
                    return ansi::bold_blue;
                case Severity::Help:
                    return ansi::bold_cyan;
            }
            return ansi::bold;
        }

        [[nodiscard]] std::string_view label_color(LabelStyle s, Severity sev) const noexcept
        {
            return (s == LabelStyle::Primary) ? severity_color(sev) : ansi::bold_blue;
        }

        [[nodiscard]] static int digit_count(std::uint32_t n) noexcept
        {
            if (n == 0)
                return 1;

            int c = 0;
            while (n > 0)
            {
                ++c;
                n /= 10;
            }

            return c;
        }

        [[nodiscard]] std::string expand_tabs(std::string_view line) const
        {
            std::string result;
            result.reserve(line.size());

            std::uint32_t col = 0;
            for (char c : line)
                if (c == '\t')
                {
                    auto spaces = m_tab_width - (col % m_tab_width);
                    result.append(spaces, ' ');
                    col += spaces;
                }
                else
                {
                    result.push_back(c);
                    ++col;
                }

            return result;
        }

        [[nodiscard]] std::uint32_t byte_to_display_col(std::string_view line, std::uint32_t byte_off) const
        {
            std::uint32_t col = 0;
            for (auto it = utf8::codepoints(line).begin(), e = utf8::codepoints(line).end(); it != e; ++it)
            {
                if (it.byte_offset() >= byte_off)
                    break;

                auto res = *it;
                if (!res)
                {
                    ++col;
                    continue;
                }

                auto cp = res->codepoint;
                if (cp == U'\t')
                    col += m_tab_width - (col % m_tab_width);
                else
                    col += static_cast<std::uint32_t>(utf8::codepoint_width(cp));
            }
            return col;
        }

        void write_gutter(int gutter_w, std::uint32_t line_no)
        {
            auto num = std::to_string(line_no);

            int pad = gutter_w - static_cast<int>(num.size());
            if (pad < 0)
                pad = 0;

            style_on(ansi::bold_blue);
            m_os << ' ' << std::string(static_cast<std::size_t>(pad), ' ') << num << " | ";
            style_off();
        }

        void write_gutter_empty(int gutter_w)
        {
            style_on(ansi::bold_blue);
            m_os << ' ' << std::string(static_cast<std::size_t>(gutter_w), ' ') << " | ";
            style_off();
        }

        void write_gutter_break(int gutter_w)
        {
            style_on(ansi::bold_blue);
            m_os << ' ';

            int pad = gutter_w + 1 - 3;
            if (pad > 0)
                m_os << std::string(static_cast<std::size_t>(pad), ' ');
            m_os << "...";

            style_off();
            m_os << '\n';
        }

        struct LineAnnotation
        {
            std::uint32_t col_start;
            std::uint32_t col_end;
            LabelStyle style;
            std::string_view message;
        };

        struct DisplayLine
        {
            std::uint32_t line_number;
            std::string expanded_text;
            std::vector<LineAnnotation> annotations;
        };

        void render_header(Diagnostic const& diag)
        {
            style_on(severity_color(diag.severity()));
            m_os << to_string(diag.severity());
            style_off();

            bold_on();
            m_os << ": " << diag.message();
            style_off();
            m_os << '\n';
        }

        void render_location(sm::SourceFile const& file, sm::Offset offset, int gutter_w)
        {
            auto lc = file.line_col(offset);

            style_on(ansi::bold_blue);
            m_os << ' ' << std::string(static_cast<std::size_t>(gutter_w), ' ') << "--> ";
            style_off();

            m_os << file.path().string();
            if (lc)
                m_os << ':' << lc->line << ':' << lc->column;
            m_os << '\n';
        }

        void render_source_line(DisplayLine const& dl, int gutter_w)
        {
            write_gutter(gutter_w, dl.line_number);
            m_os << dl.expanded_text << '\n';
        }

        void render_annotations(DisplayLine const& dl, Severity sev, int gutter_w)
        {
            auto const& annots = dl.annotations;
            if (annots.empty())
                return;

            std::uint32_t max_col = 0;
            for (auto const& a : annots)
                max_col = std::max(max_col, a.col_end);

            struct Cell
            {
                char ch;
                LabelStyle style;
            };
            std::vector<Cell> underline(max_col, {' ', LabelStyle::Primary});

            for (auto const& a : annots)
            {
                char ch = (a.style == LabelStyle::Primary) ? '^' : '~';
                for (std::uint32_t c = a.col_start; c < a.col_end && c < max_col; ++c)
                    underline[c] = {ch, a.style};
            }

            write_gutter_empty(gutter_w);
            for (std::uint32_t c = 0; c < max_col; ++c)
                if (underline[c].ch != ' ')
                {
                    style_on(label_color(underline[c].style, sev));
                    m_os.put(underline[c].ch);
                    style_off();
                }
                else
                    m_os.put(' ');

            std::vector<LineAnnotation const*> with_msg;
            for (auto const& a : annots)
                if (!a.message.empty())
                    with_msg.push_back(&a);

            if (with_msg.size() == 1)
            {
                m_os << ' ';
                style_on(label_color(with_msg[0]->style, sev));
                m_os << with_msg[0]->message;
                style_off();
                m_os << '\n';
                return;
            }

            m_os << '\n';
            if (with_msg.empty())
                return;

            for (auto it = with_msg.rbegin(); it != with_msg.rend(); ++it)
            {
                auto const* current = *it;
                write_gutter_empty(gutter_w);

                std::uint32_t col = 0;

                for (auto jt = with_msg.begin(); *jt != current; ++jt)
                {
                    while (col < (*jt)->col_start)
                    {
                        m_os.put(' ');
                        ++col;
                    }

                    style_on(label_color((*jt)->style, sev));
                    m_os.put('|');
                    style_off();

                    ++col;
                }

                while (col < current->col_start)
                {
                    m_os.put(' ');
                    ++col;
                }

                style_on(label_color(current->style, sev));
                m_os << current->message;
                style_off();

                m_os << '\n';
            }
        }

        void render_snippet(sm::SourceFile const& file, std::span<Label const> labels, Severity sev, int gutter_w)
        {
            if (labels.empty())
                return;

            struct LabelInfo
            {
                std::uint32_t start_line, end_line;
                std::uint32_t start_byte_in_line, end_byte_in_line;
                Label const* label;
            };
            std::vector<LabelInfo> infos;
            infos.reserve(labels.size());

            for (auto const& lab : labels)
            {
                auto sl = file.line_col(lab.range.begin.offset);
                auto el = file.line_col(lab.range.end.offset);
                if (!sl || !el)
                    continue;

                if (!file.line_text(sl->line) || !file.line_text(el->line))
                    continue;

                infos.push_back({sl->line, el->line, sl->byte_col - 1, el->byte_col - 1, &lab});
            }

            if (infos.empty())
                return;

            std::set<std::uint32_t> needed;
            for (auto const& info : infos)
                for (std::uint32_t l = info.start_line; l <= info.end_line; ++l)
                    needed.insert(l);

            std::set<std::uint32_t> display;
            auto total = file.line_count();
            for (auto l : needed)
            {
                auto lo = (l > m_context_lines && l != 0) ? l - m_context_lines : 1;
                auto hi = std::min(l + m_context_lines, total);

                for (auto k = lo; k <= hi; ++k)
                    display.insert(k);
            }

            std::vector<DisplayLine> dlines;
            dlines.reserve(display.size());

            for (auto line_no : display)
            {
                auto text = file.line_text(line_no);
                if (!text)
                    continue;

                auto raw = *text;
                if (!raw.empty() && raw.back() == '\n')
                    raw.remove_suffix(1);
                if (!raw.empty() && raw.back() == '\r')
                    raw.remove_suffix(1);

                DisplayLine dl;
                dl.line_number = line_no;
                dl.expanded_text = expand_tabs(raw);

                for (auto const& info : infos)
                {
                    if (line_no < info.start_line || line_no > info.end_line)
                        continue;

                    std::uint32_t byte_start = 0;
                    std::uint32_t byte_end = static_cast<std::uint32_t>(raw.size());

                    if (line_no == info.start_line)
                        byte_start = info.start_byte_in_line;
                    if (line_no == info.end_line)
                        byte_end = info.end_byte_in_line;

                    auto col_start = byte_to_display_col(raw, byte_start);
                    auto col_end = byte_to_display_col(raw, byte_end);
                    if (col_end <= col_start)
                        col_end = col_start + 1;

                    std::string_view msg;
                    if (line_no == info.end_line)
                        msg = info.label->message;

                    dl.annotations.push_back({col_start, col_end, info.label->style, msg});
                }

                std::ranges::sort(dl.annotations, {}, &LineAnnotation::col_start);
                dlines.push_back(std::move(dl));
            }

            write_gutter_empty(gutter_w);
            m_os << '\n';

            for (std::size_t i = 0; i < dlines.size(); ++i)
            {
                if (i > 0 && dlines[i].line_number > dlines[i - 1].line_number + 1)
                    write_gutter_break(gutter_w);

                render_source_line(dlines[i], gutter_w);
                if (!dlines[i].annotations.empty())
                    render_annotations(dlines[i], sev, gutter_w);
            }
        }

        void render_footer(Diagnostic const& diag, int gutter_w)
        {
            auto item = [&](std::string_view prefix, std::string_view color, std::string_view text) {
                style_on(ansi::bold_blue);
                m_os << ' ' << std::string(static_cast<std::size_t>(gutter_w), ' ') << ' ';
                style_off();

                style_on(color);
                bold_on();
                m_os << "= " << prefix;
                style_off();
                m_os << ": " << text << '\n';
            };

            for (auto const& n : diag.notes())
                item("note", ansi::bold_blue, n);

            for (auto const& h : diag.helps())
                item("help", ansi::bold_cyan, h);

            for (auto const& f : diag.fixes())
            {
                std::string text = f.message;
                if (!f.replacement.empty())
                {
                    if (!text.empty())
                        text += ": ";

                    text += '`';
                    text += f.replacement;
                    text += '`';
                }

                item("fix", ansi::bold_green, text);
            }
        }

        void print(Diagnostic const& diag)
        {
            render_header(diag);

            if (diag.labels().empty())
            {
                render_footer(diag, 1);
                m_os << '\n';
                return;
            }

            std::map<sm::FileId, std::vector<Label const*>> groups;
            for (auto const& lab : diag.labels())
                groups[lab.range.begin.fileId].push_back(&lab);

            int gutter_w = 1;
            for (auto const& [fid, labs] : groups)
            {
                auto const* file = m_sm.get(fid);
                if (!file)
                    continue;

                for (auto const* lab : labs)
                {
                    auto lc = file->line_col(lab->range.end.offset);
                    if (lc)
                        gutter_w = std::max(gutter_w, digit_count(lc->line + m_context_lines));
                }
            }

            bool first = true;
            for (auto const& [fid, lab_ptrs] : groups)
            {
                auto const* file = m_sm.get(fid);
                if (!file)
                    continue;

                std::vector<Label> file_labels;
                file_labels.reserve(lab_ptrs.size());
                for (auto const* lp : lab_ptrs)
                    file_labels.push_back(*lp);

                Label const* primary = nullptr;
                for (auto const& l : file_labels)
                    if (l.style == LabelStyle::Primary)
                    {
                        primary = &l;
                        break;
                    }

                if (!primary)
                    primary = &file_labels.front();

                if (first)
                {
                    render_location(*file, primary->range.begin.offset, gutter_w);
                    first = false;
                }
                else
                {
                    style_on(ansi::bold_blue);
                    m_os << ' ' << std::string(static_cast<std::size_t>(gutter_w), ' ') << "::: ";
                    style_off();

                    m_os << file->path().string();

                    auto lc = file->line_col(primary->range.begin.offset);
                    if (lc)
                        m_os << ':' << lc->line << ':' << lc->column;
                    m_os << '\n';
                }

                render_snippet(*file, file_labels, diag.severity(), gutter_w);
            }

            write_gutter_empty(gutter_w);
            m_os << '\n';

            render_footer(diag, gutter_w);
            m_os << '\n';
        }
    };

} // namespace dcc::diag
