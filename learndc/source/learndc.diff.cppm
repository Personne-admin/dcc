export module learndc.diff;

import std;
import learndc.text;

export namespace learndc::diff
{
    enum class Op
    {
        Equal,
        Delete,
        Insert,
    };

    struct Line
    {
        std::string text;
        bool newline = true;
        friend bool operator==(Line const&, Line const&) = default;
    };

    struct Edit
    {
        Op op = Op::Equal;
        std::size_t a = 0;
        std::size_t b = 0;
    };

    [[nodiscard]] inline std::vector<Line> to_lines(std::string_view s)
    {
        std::vector<Line> lines;
        std::size_t start = 0;
        while (start < s.size())
        {
            auto const end = s.find('\n', start);
            if (end == std::string_view::npos)
            {
                lines.push_back({std::string(s.substr(start)), false});
                break;
            }
            lines.push_back({std::string(s.substr(start, end - start)), true});
            start = end + 1;
        }
        return lines;
    }

    namespace detail
    {
        constexpr std::size_t max_edit_distance = 1500;

        [[nodiscard]] inline std::vector<Edit> myers(std::vector<Line> const& a, std::vector<Line> const& b, std::size_t a_off, std::size_t b_off)
        {
            auto const n = static_cast<std::ptrdiff_t>(a.size());
            auto const m = static_cast<std::ptrdiff_t>(b.size());
            std::vector<Edit> out;
            if (n == 0 || m == 0)
            {
                for (std::ptrdiff_t i = 0; i < n; ++i)
                    out.push_back({Op::Delete, a_off + static_cast<std::size_t>(i), b_off});
                for (std::ptrdiff_t j = 0; j < m; ++j)
                    out.push_back({Op::Insert, a_off + a.size(), b_off + static_cast<std::size_t>(j)});
                return out;
            }
            auto const max = std::min<std::ptrdiff_t>(n + m, static_cast<std::ptrdiff_t>(max_edit_distance));
            auto const offset = max;
            std::vector<std::ptrdiff_t> v(static_cast<std::size_t>(2 * max + 2), 0);
            std::vector<std::vector<std::ptrdiff_t>> trace;
            bool found = false;
            std::ptrdiff_t final_d = 0;
            for (std::ptrdiff_t d = 0; d <= max && !found; ++d)
            {
                trace.push_back(v);
                for (std::ptrdiff_t k = -d; k <= d; k += 2)
                {
                    std::ptrdiff_t x = 0;
                    auto const idx = [&](std::ptrdiff_t kk) { return static_cast<std::size_t>(kk + offset); };
                    if (k == -d || (k != d && v[idx(k - 1)] < v[idx(k + 1)]))
                        x = v[idx(k + 1)];
                    else
                        x = v[idx(k - 1)] + 1;
                    std::ptrdiff_t y = x - k;
                    while (x < n && y < m && a[static_cast<std::size_t>(x)] == b[static_cast<std::size_t>(y)])
                    {
                        ++x;
                        ++y;
                    }
                    v[idx(k)] = x;
                    if (x >= n && y >= m)
                    {
                        found = true;
                        final_d = d;
                        break;
                    }
                }
            }
            if (!found)
            {
                for (std::ptrdiff_t i = 0; i < n; ++i)
                    out.push_back({Op::Delete, a_off + static_cast<std::size_t>(i), b_off});
                for (std::ptrdiff_t j = 0; j < m; ++j)
                    out.push_back({Op::Insert, a_off + a.size(), b_off + static_cast<std::size_t>(j)});
                return out;
            }
            std::vector<Edit> reversed;
            std::ptrdiff_t x = n;
            std::ptrdiff_t y = m;
            for (std::ptrdiff_t d = final_d; d > 0; --d)
            {
                auto const& vd = trace[static_cast<std::size_t>(d)];
                auto const idx = [&](std::ptrdiff_t kk) { return static_cast<std::size_t>(kk + offset); };
                std::ptrdiff_t const k = x - y;
                std::ptrdiff_t prev_k = 0;
                if (k == -d || (k != d && vd[idx(k - 1)] < vd[idx(k + 1)]))
                    prev_k = k + 1;
                else
                    prev_k = k - 1;
                std::ptrdiff_t const prev_x = vd[idx(prev_k)];
                std::ptrdiff_t const prev_y = prev_x - prev_k;
                while (x > prev_x && y > prev_y)
                {
                    --x;
                    --y;
                    reversed.push_back({Op::Equal, a_off + static_cast<std::size_t>(x), b_off + static_cast<std::size_t>(y)});
                }
                if (x == prev_x)
                {
                    --y;
                    reversed.push_back({Op::Insert, a_off + static_cast<std::size_t>(x), b_off + static_cast<std::size_t>(y)});
                }
                else
                {
                    --x;
                    reversed.push_back({Op::Delete, a_off + static_cast<std::size_t>(x), b_off + static_cast<std::size_t>(y)});
                }
            }
            while (x > 0 && y > 0)
            {
                --x;
                --y;
                reversed.push_back({Op::Equal, a_off + static_cast<std::size_t>(x), b_off + static_cast<std::size_t>(y)});
            }
            std::ranges::reverse(reversed);
            return reversed;
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<Edit> diff_lines(std::vector<Line> const& a, std::vector<Line> const& b)
    {
        std::size_t prefix = 0;
        while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
            ++prefix;
        std::size_t suffix = 0;
        while (suffix < a.size() - prefix && suffix < b.size() - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
            ++suffix;
        std::vector<Edit> edits;
        for (std::size_t i = 0; i < prefix; ++i)
            edits.push_back({Op::Equal, i, i});
        std::vector<Line> const mid_a(a.begin() + static_cast<std::ptrdiff_t>(prefix), a.end() - static_cast<std::ptrdiff_t>(suffix));
        std::vector<Line> const mid_b(b.begin() + static_cast<std::ptrdiff_t>(prefix), b.end() - static_cast<std::ptrdiff_t>(suffix));
        auto middle = detail::myers(mid_a, mid_b, prefix, prefix);
        edits.insert(edits.end(), middle.begin(), middle.end());
        for (std::size_t i = 0; i < suffix; ++i)
            edits.push_back({Op::Equal, a.size() - suffix + i, b.size() - suffix + i});
        return edits;
    }

    [[nodiscard]] inline std::vector<Edit> diff_text(std::string_view a, std::string_view b)
    {
        return diff_lines(to_lines(a), to_lines(b));
    }

    [[nodiscard]] inline bool identical(std::string_view a, std::string_view b)
    {
        return a == b;
    }

    namespace detail
    {
        inline void emit_line(std::string& out, char prefix, Line const& line)
        {
            out += prefix;
            out += line.text;
            out += '\n';
            if (!line.newline)
                out += "\\ No newline at end of file\n";
        }

        [[nodiscard]] inline std::string hunk_range(std::size_t first, std::size_t count)
        {
            if (count == 1)
                return std::format("{}", first + 1);
            return std::format("{},{}", count == 0 ? first : first + 1, count);
        }
    } // namespace detail

    [[nodiscard]] inline std::string unified(std::string_view a, std::string_view b, std::string_view label_a = "a", std::string_view label_b = "b",
                                             std::size_t context = 3)
    {
        auto const la = to_lines(a);
        auto const lb = to_lines(b);
        auto const edits = diff_lines(la, lb);
        std::vector<std::size_t> changed;
        for (std::size_t i = 0; i < edits.size(); ++i)
            if (edits[i].op != Op::Equal)
                changed.push_back(i);
        if (changed.empty())
            return {};
        std::string out = std::format("--- {}\n+++ {}\n", label_a, label_b);
        std::size_t ci = 0;
        while (ci < changed.size())
        {
            std::size_t first = changed[ci];
            std::size_t last = first;
            std::size_t cj = ci + 1;
            while (cj < changed.size() && changed[cj] - last <= 2 * context + 1)
            {
                last = changed[cj];
                ++cj;
            }
            std::size_t const begin = first > context ? first - context : 0;
            std::size_t const end = std::min(edits.size(), last + context + 1);
            std::size_t a_count = 0;
            std::size_t b_count = 0;
            std::optional<std::size_t> a_first;
            std::optional<std::size_t> b_first;
            for (std::size_t i = begin; i < end; ++i)
            {
                auto const& e = edits[i];
                if (e.op != Op::Insert)
                {
                    ++a_count;
                    if (!a_first)
                        a_first = e.a;
                }
                if (e.op != Op::Delete)
                {
                    ++b_count;
                    if (!b_first)
                        b_first = e.b;
                }
            }
            std::size_t const a_start = a_first.value_or(edits[begin].a);
            std::size_t const b_start = b_first.value_or(edits[begin].b);
            out += std::format("@@ -{} +{} @@\n", detail::hunk_range(a_start, a_count), detail::hunk_range(b_start, b_count));
            for (std::size_t i = begin; i < end; ++i)
            {
                auto const& e = edits[i];
                if (e.op == Op::Equal)
                    detail::emit_line(out, ' ', la[e.a]);
                else if (e.op == Op::Delete)
                    detail::emit_line(out, '-', la[e.a]);
                else
                    detail::emit_line(out, '+', lb[e.b]);
            }
            ci = cj;
        }
        return out;
    }

    enum class RowKind
    {
        Equal,
        Change,
        Delete,
        Insert,
    };

    struct SideRow
    {
        RowKind kind = RowKind::Equal;
        std::optional<std::size_t> left_number;
        std::optional<std::size_t> right_number;
        std::string left;
        std::string right;
    };

    [[nodiscard]] inline std::vector<SideRow> side_by_side(std::string_view a, std::string_view b)
    {
        auto const la = to_lines(a);
        auto const lb = to_lines(b);
        auto const edits = diff_lines(la, lb);
        std::vector<SideRow> rows;
        std::size_t i = 0;
        while (i < edits.size())
        {
            if (edits[i].op == Op::Equal)
            {
                rows.push_back({RowKind::Equal, edits[i].a + 1, edits[i].b + 1, la[edits[i].a].text, lb[edits[i].b].text});
                ++i;
                continue;
            }
            std::vector<std::size_t> deletes;
            std::vector<std::size_t> inserts;
            while (i < edits.size() && edits[i].op != Op::Equal)
            {
                if (edits[i].op == Op::Delete)
                    deletes.push_back(edits[i].a);
                else
                    inserts.push_back(edits[i].b);
                ++i;
            }
            std::size_t const paired = std::min(deletes.size(), inserts.size());
            for (std::size_t k = 0; k < paired; ++k)
                rows.push_back({RowKind::Change, deletes[k] + 1, inserts[k] + 1, la[deletes[k]].text, lb[inserts[k]].text});
            for (std::size_t k = paired; k < deletes.size(); ++k)
                rows.push_back({RowKind::Delete, deletes[k] + 1, std::nullopt, la[deletes[k]].text, {}});
            for (std::size_t k = paired; k < inserts.size(); ++k)
                rows.push_back({RowKind::Insert, std::nullopt, inserts[k] + 1, {}, lb[inserts[k]].text});
        }
        return rows;
    }

    enum class Mark
    {
        Same,
        Expected,
        Observed,
        Gap,
    };

    struct OutputLine
    {
        Mark mark = Mark::Same;
        std::string text;
        std::size_t expected_line = 0;
        std::size_t observed_line = 0;
        bool newline = true;
        std::size_t changed_begin = 0;
        std::size_t changed_end = 0;
        bool has_change = false;
    };

    struct OutputDiff
    {
        std::vector<OutputLine> lines;
        std::size_t expected_lines = 0;
        std::size_t observed_lines = 0;
        std::size_t first_difference = 0;
        std::size_t differences = 0;
        bool truncated = false;
        bool only_final_newline = false;
        std::string summary;
    };

    namespace detail
    {
        [[nodiscard]] inline std::size_t utf8_back(std::string_view s, std::size_t i)
        {
            while (i > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
                --i;
            return i;
        }

        inline void mark_change(OutputLine& removed, OutputLine& added)
        {
            std::string_view a = removed.text;
            std::string_view b = added.text;
            std::size_t prefix = 0;
            while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
                ++prefix;
            prefix = utf8_back(a, prefix);
            std::size_t suffix = 0;
            while (suffix < a.size() - prefix && suffix < b.size() - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
                ++suffix;
            auto a_end = a.size() - suffix;
            auto b_end = b.size() - suffix;
            while (a_end < a.size() && (static_cast<unsigned char>(a[a_end]) & 0xC0) == 0x80)
                ++a_end;
            while (b_end < b.size() && (static_cast<unsigned char>(b[b_end]) & 0xC0) == 0x80)
                ++b_end;
            removed.changed_begin = prefix;
            removed.changed_end = std::max(prefix, a_end);
            removed.has_change = true;
            added.changed_begin = prefix;
            added.changed_end = std::max(prefix, b_end);
            added.has_change = true;
        }

        [[nodiscard]] inline std::string quote_short(std::string_view s, std::size_t limit = 60)
        {
            std::string out;
            std::size_t i = 0;
            while (i < s.size() && out.size() < limit)
            {
                auto const d = text::decode_utf8(s, i);
                out += s.substr(i, d.length);
                i += d.length;
            }
            if (i < s.size())
                out += "...";
            return out;
        }
        [[nodiscard]] inline std::string summarize(std::vector<OutputLine> const& removed, std::vector<OutputLine> const& added, bool last_block)
        {
            if (!removed.empty() && !added.empty())
            {
                auto const& e = removed.front();
                auto const& o = added.front();
                if (e.text == o.text && e.newline != o.newline && last_block)
                    return e.newline ? "the last line is missing its final newline" : "there is an extra newline after the last line";
                auto const missing = std::string_view(e.text).substr(e.changed_begin, e.changed_end - e.changed_begin);
                auto const extra = std::string_view(o.text).substr(o.changed_begin, o.changed_end - o.changed_begin);
                if (!missing.empty() && extra.empty())
                    return std::format("line {} is missing \"{}\"", e.expected_line, quote_short(missing));
                if (missing.empty() && !extra.empty())
                    return std::format("line {} has extra \"{}\"", e.expected_line, quote_short(extra));
                return std::format("line {} has \"{}\" where \"{}\" was expected", e.expected_line, quote_short(extra), quote_short(missing));
            }
            if (!removed.empty())
                return std::format("expected line {} is missing from the output", removed.front().expected_line);
            return std::format("output line {} was not expected", added.front().observed_line);
        }
    } // namespace detail

    [[nodiscard]] inline OutputDiff compare_output(std::string_view expected, std::string_view observed, std::size_t max_lines = 4000)
    {
        OutputDiff out;
        auto const a = to_lines(expected);
        auto const b = to_lines(observed);
        out.expected_lines = a.size();
        out.observed_lines = b.size();
        auto const edits = diff_lines(a, b);
        std::vector<OutputLine> lines;
        std::size_t i = 0;
        while (i < edits.size())
        {
            if (edits[i].op == Op::Equal)
            {
                auto const& line = a[edits[i].a];
                lines.push_back({Mark::Same, line.text, edits[i].a + 1, edits[i].b + 1, line.newline});
                ++i;
                continue;
            }
            std::vector<OutputLine> removed;
            std::vector<OutputLine> added;
            while (i < edits.size() && edits[i].op != Op::Equal)
            {
                if (edits[i].op == Op::Delete)
                    removed.push_back({Mark::Expected, a[edits[i].a].text, edits[i].a + 1, 0, a[edits[i].a].newline});
                else
                    added.push_back({Mark::Observed, b[edits[i].b].text, 0, edits[i].b + 1, b[edits[i].b].newline});
                ++i;
            }
            for (std::size_t k = 0; k < removed.size() && k < added.size(); ++k)
                detail::mark_change(removed[k], added[k]);
            if (!out.first_difference)
            {
                out.first_difference = lines.size() + 1;
                out.summary = detail::summarize(removed, added, edits.size() == i && removed.size() == 1 && added.size() == 1);
            }
            out.differences += std::max(removed.size(), added.size());
            for (auto& l : removed)
                lines.push_back(std::move(l));
            for (auto& l : added)
                lines.push_back(std::move(l));
        }
        out.only_final_newline = out.summary.find("newline") != std::string::npos;
        if (lines.size() > max_lines)
        {
            constexpr std::size_t keep = 3;
            std::vector<bool> near(lines.size(), false);
            for (std::size_t k = 0; k < lines.size(); ++k)
                if (lines[k].mark != Mark::Same)
                    for (std::size_t j = k >= keep ? k - keep : 0; j <= std::min(lines.size() - 1, k + keep); ++j)
                        near[j] = true;
            std::vector<OutputLine> kept;
            bool gap = false;
            for (std::size_t k = 0; k < lines.size(); ++k)
            {
                if (!near[k])
                {
                    gap = true;
                    continue;
                }
                if (gap && !kept.empty())
                    kept.push_back({Mark::Gap, {}, 0, 0, true});
                gap = false;
                kept.push_back(std::move(lines[k]));
            }
            if (kept.size() > max_lines)
            {
                kept.resize(max_lines);
                out.truncated = true;
            }
            out.first_difference = 0;
            for (std::size_t k = 0; k < kept.size(); ++k)
                if (kept[k].mark == Mark::Expected || kept[k].mark == Mark::Observed)
                {
                    out.first_difference = k + 1;
                    break;
                }
            lines = std::move(kept);
        }
        out.lines = std::move(lines);
        return out;
    }

    [[nodiscard]] inline std::vector<std::pair<std::size_t, std::size_t>> hunks(OutputDiff const& d, std::size_t context, bool all)
    {
        std::vector<std::pair<std::size_t, std::size_t>> out;
        auto const changed = [&](std::size_t k) { return d.lines[k].mark == Mark::Expected || d.lines[k].mark == Mark::Observed; };
        for (std::size_t i = 0; i < d.lines.size(); ++i)
        {
            if (!changed(i))
                continue;
            auto begin = i;
            while (begin > 0 && i - begin < context && d.lines[begin - 1].mark == Mark::Same)
                --begin;
            auto end = i + 1;
            while (end < d.lines.size() && changed(end))
                ++end;
            for (std::size_t extra = 0; extra < context && end < d.lines.size() && d.lines[end].mark == Mark::Same; ++extra)
                ++end;
            if (!out.empty() && begin <= out.back().second)
                out.back().second = end;
            else
                out.emplace_back(begin, end);
            if (!all)
                break;
            i = end - 1;
        }
        return out;
    }

    [[nodiscard]] inline std::string visible_line(std::string_view line, bool ascii, bool line_end = true)
    {
        std::string out;
        auto trailing = line.size();
        while (line_end && trailing > 0 && (line[trailing - 1] == ' ' || line[trailing - 1] == '\t'))
            --trailing;
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            char const c = line[i];
            if (c == '\t')
                out += ascii ? "\\t" : "\xE2\x86\x92";
            else if (c == '\r')
                out += ascii ? "\\r" : "\xE2\x90\x8D";
            else if (c == ' ' && i >= trailing)
                out += ascii ? "~" : "\xC2\xB7";
            else
                out += c;
        }
        return out;
    }

    [[nodiscard]] inline std::string render_output_diff(OutputDiff const& d, bool all, std::size_t context = 2)
    {
        std::string out;
        if (!d.summary.empty())
            out += "first difference: " + d.summary + "\n";
        for (auto const& [begin, end] : hunks(d, context, all))
        {
            auto const& head = d.lines[begin];
            out += std::format("@@ expected line {}, output line {} @@\n", head.expected_line ? head.expected_line : head.observed_line,
                               head.observed_line ? head.observed_line : head.expected_line);
            for (std::size_t i = begin; i < end; ++i)
            {
                auto const& l = d.lines[i];
                char const marker = l.mark == Mark::Same ? ' ' : l.mark == Mark::Expected ? '-' : '+';
                out += std::format("{} {}{}\n", marker, visible_line(l.text, true), l.newline ? "" : "  (no newline at end)");
            }
        }
        if (d.truncated)
            out += "(the comparison is cut off here; the outputs are very long)\n";
        return out;
    }
} // namespace learndc::diff
