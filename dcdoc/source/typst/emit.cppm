module;

#include <cstdio>

export module dcdoc.typst.emit;

import std;
import dcc.sm;
import dcc.si;
import dcc.lex.tokens;
import dcc.lex;
import dcdoc.model;

export namespace dcdoc::typst
{
    [[nodiscard]] std::string esc(std::string_view s)
    {
        constexpr char kBackslash = 92;
        std::string out;
        for (char c : s)
        {
            switch (c)
            {
                case kBackslash:
                case 35:
                case 36:
                case 64:
                case 60:
                case 42:
                case 95:
                case 96:
                case 91:
                case 93:
                    out += kBackslash;
                    out += c;
                    break;
                default:
                    out += c;
            }
        }
        return out;
    }

    class LabelMaker
    {
    public:
        std::string make(std::string_view stable_id)
        {
            std::string base;
            for (char c : stable_id)
            {
                bool keep = (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57) || c == 95 || c == 45;
                base += keep ? c : 45;
            }
            while (!base.empty() && base.back() == 45)
                base.pop_back();
            if (base.empty())
                base = "item";
            std::string label = base;
            int n = 2;
            while (!m_used.insert(label).second)
                label = base + "-" + std::to_string(n++);
            return label;
        }

    private:
        std::unordered_set<std::string> m_used;
    };

    struct TextBlock
    {
        bool code{};
        std::size_t start{};
        std::string text;
    };

    [[nodiscard]] bool blank_line(std::string_view l) noexcept
    {
        for (char c : l)
            if (c != 32 && c != 9 && c != 13)
                return false;
        return true;
    }

    [[nodiscard]] std::string trim(std::string_view l)
    {
        std::size_t b = 0;
        while (b < l.size() && (l[b] == 32 || l[b] == 9 || l[b] == 13))
            ++b;
        std::size_t e = l.size();
        while (e > b && (l[e - 1] == 32 || l[e - 1] == 9 || l[e - 1] == 13))
            --e;
        return std::string{l.substr(b, e - b)};
    }

    [[nodiscard]] bool fence_line(std::string_view l)
    {
        std::string t = trim(l);
        return t.size() >= 3 && t[0] == 96 && t[1] == 96 && t[2] == 96;
    }

    [[nodiscard]] std::vector<TextBlock> split_blocks(std::string_view doc)
    {
        std::vector<TextBlock> out;
        std::string cur;
        std::size_t cur_start = 0;
        bool cur_code = false;
        bool in_code = false;
        bool has_start = false;
        auto push = [&] {
            if (cur.empty())
                return;
            if (!cur_code)
            {
                while (!cur.empty() && cur.back() == 10)
                    cur.pop_back();
                if (cur.empty())
                    return;
            }
            out.push_back({.code = cur_code, .start = cur_start, .text = cur});
            cur.clear();
        };
        std::size_t i = 0;
        while (i <= doc.size())
        {
            std::size_t j = doc.find(10, i);
            if (j == std::string_view::npos)
                j = doc.size();
            std::string_view line = doc.substr(i, j - i);
            if (fence_line(line))
            {
                push();
                in_code = !in_code;
                cur_code = in_code;
                has_start = false;
            }
            else if (in_code)
            {
                if (!has_start)
                {
                    cur_start = i;
                    has_start = true;
                }
                cur += std::string{line};
                cur += 10;
            }
            else if (blank_line(line))
            {
                push();
                cur_code = false;
                has_start = false;
            }
            else
            {
                if (!has_start)
                {
                    cur_start = i;
                    has_start = true;
                }
                cur_code = false;
                cur += std::string{line};
                cur += 10;
            }
            if (j == doc.size())
                break;
            i = j + 1;
        }
        push();
        return out;
    }

    [[nodiscard]] std::string render_raw_inline(std::string_view code)
    {
        if (code.find(96) == std::string_view::npos)
            return std::string{"`"}.append(code).append("`");
        std::string s = "#raw(\"";
        for (char c : code)
        {
            if (c == 34)
                s += "\\\"";
            else if (c == 92)
                s += "\\\\";
            else
                s += c;
        }
        return s + "\")";
    }

    [[nodiscard]] std::string render_plain_inline(std::string_view s)
    {
        std::string out;
        std::size_t i = 0;
        while (i < s.size())
        {
            std::size_t j = s.find(96, i);
            if (j == std::string_view::npos)
            {
                out += esc(s.substr(i));
                break;
            }
            std::size_t k = s.find(96, j + 1);
            if (k == std::string_view::npos)
            {
                out += esc(s.substr(i));
                break;
            }
            out += esc(s.substr(i, j - i));
            out += render_raw_inline(s.substr(j + 1, k - j - 1));
            i = k + 1;
        }
        return out;
    }

    [[nodiscard]] std::string render_raw_block(std::string_view text)
    {
        std::string s = "#raw(\"";
        for (char c : text)
        {
            if (c == 34)
                s += "\\\"";
            else if (c == 92)
                s += "\\\\";
            else
                s += c;
        }
        return s + "\", lang: \"dc\")";
    }

    [[nodiscard]] std::string render_doc(std::string_view text, std::vector<CrossRef> const& refs, std::unordered_map<std::string, std::string> const& labels)
    {
        std::vector<CrossRef const*> ordered;
        for (auto const& r : refs)
            if (r.length > 0 && r.start != std::size_t(-1) && r.start + r.length <= text.size())
                ordered.push_back(&r);
        std::ranges::sort(ordered, {}, [](CrossRef const* r) { return r->start; });
        std::string out;
        for (auto const& block : split_blocks(text))
        {
            if (block.code)
            {
                out += render_raw_block(block.text);
                out += "\n\n";
                continue;
            }
            std::string para;
            std::size_t pos = 0;
            for (auto const* r : ordered)
            {
                if (r->start < block.start || r->start + r->length > block.start + block.text.size())
                    continue;
                std::size_t ls = r->start - block.start;
                if (ls < pos)
                    continue;
                para += render_plain_inline(block.text.substr(pos, ls - pos));
                std::string display = r->display.empty() ? r->raw : r->display;
                std::string label;
                if (r->resolved && !r->ambiguous)
                {
                    auto it = labels.find(r->target);
                    if (it != labels.end())
                        label = it->second;
                }
                if (!label.empty())
                    para += "#link(<" + label + ">)" + "[" + render_plain_inline(display) + "]";
                else if (r->ambiguous)
                    para += "#text(fill: luma(130))[" + render_plain_inline(display) + "]";
                else
                    para += render_plain_inline(display);
                pos = ls + r->length;
            }
            para += render_plain_inline(block.text.substr(pos));
            out += para + "\n\n";
        }
        return out;
    }

    class Highlighter
    {
    public:
        [[nodiscard]] std::string highlight(std::string_view sig)
        {
            std::string uri = "dcdoc-sig://" + std::to_string(m_next++);
            dcc::sm::FileId fid = m_sm.open_in_memory(std::move(uri), std::string{sig});
            auto const* file = m_sm.get(fid);
            if (!file)
                return esc(sig);
            dcc::lex::Lexer lexer{*file, m_interner, false};
            std::string out;
            std::uint32_t prev = 0;
            while (true)
            {
                auto tok = lexer.next();
                if (tok.kind == dcc::lex::TokenKind::Eof)
                    break;
                std::string_view text = file->text();
                std::uint32_t begin = tok.range.begin.offset;
                std::uint32_t end = tok.range.end.offset;
                if (begin > text.size())
                    break;
                if (end > static_cast<std::uint32_t>(text.size()))
                    end = static_cast<std::uint32_t>(text.size());
                if (begin > prev)
                    out += esc(text.substr(prev, begin - prev));
                prev = end;
                std::string_view raw = text.substr(begin, end - begin);
                if (dcc::lex::is_keyword(tok.kind))
                    out += "#text(fill: rgb(31, 111, 235))[" + esc(raw) + "]";
                else if (dcc::lex::is_literal(tok.kind))
                    out += "#text(fill: rgb(149, 56, 0))[" + esc(raw) + "]";
                else
                    out += esc(raw);
                if (begin == end && tok.kind != dcc::lex::TokenKind::Eof)
                    break;
            }
            if (prev < file->text().size())
                out += esc(file->text().substr(prev));
            return out;
        }

    private:
        dcc::sm::SourceManager m_sm;
        dcc::si::string_interner m_interner;
        std::uint64_t m_next{};
    };

    [[nodiscard]] std::string render_item(Item const& item, std::vector<Item const*> const& children, Highlighter& hl, int level,
                                          std::unordered_map<std::string, std::string> const& link_labels)
    {
        std::string out = "#heading(level: " + std::to_string(level) + ")[" + esc(item.name) + "]";
        auto lit = link_labels.find(item.id);
        if (lit != link_labels.end())
            out += " <" + lit->second + ">";
        out += "\n";
        out += "#text(size: 8pt, fill: luma(140))[" + esc(item.kind);
        out += item.is_public ? ", public" : ", private";
        out += "]\n";
        out += "#block(fill: luma(243), inset: 8pt, radius: 4pt, width: 100%)[\n#set text(font: \"DejaVu Sans Mono\", size: 9pt)\n";
        out += hl.highlight(item.sig.text);
        out += "\n]\n";
        if (!item.doc.empty())
            out += render_doc(item.doc, item.doc_refs, link_labels);
        if (!item.file.empty())
            out += "#text(size: 8pt, fill: luma(140))[defined in " + esc(item.file) + ":" + std::to_string(item.line) + "]\n";
        for (auto const* child : children)
            out += render_item(*child, {}, hl, level + 1, link_labels);
        return out;
    }

    [[nodiscard]] std::string render(Project const& project)
    {
        std::string name = project.entry_module.empty() ? "Project" : project.entry_module;
        auto now = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
        std::string out = "#align(center + horizon)[\n#text(26pt)[ ";
        out += esc(name);
        out += " ]\n";
        if (!project.overview.empty())
        {
            out += "#text(12pt)[\n";
            out += render_doc(project.overview, {}, {});
            out += "]\n";
        }
        out += "#text(10pt)[generated " + std::format("{:%F}", now) + "]\n]\n#pagebreak()\n#outline(title: \"Contents\")\n#pagebreak()\n";
        std::unordered_map<std::string, std::vector<Item const*>> children;
        for (auto const& it : project.items)
            if (!it.parent.empty())
                children[it.parent].push_back(&it);
        std::unordered_map<std::string, std::string> link_labels;
        {
            LabelMaker maker;
            for (auto const& it : project.items)
                link_labels[it.id] = maker.make(it.id);
        }
        Highlighter hl;
        std::unordered_map<std::string, Item const*> by_id;
        for (auto const& it : project.items)
            by_id[it.id] = &it;
        for (auto const& m : project.modules)
        {
            out += "#heading(level: 1)[" + esc(m.id) + "]\n";
            if (!m.file.empty())
                out += "#text(size: 8pt, fill: luma(140))[" + esc(m.file) + "]\n";
            if (!m.overview.empty())
                out += render_doc(m.overview, {}, {});
            for (auto const& s : m.sections)
            {
                if (!s.title.empty())
                    out += "#heading(level: 2)[" + esc(s.title) + "]\n";
                if (!s.body.empty())
                    out += render_doc(s.body, {}, {});
                for (auto const& id : s.items)
                {
                    auto bit = by_id.find(id);
                    if (bit == by_id.end())
                        continue;
                    auto cit = children.find(id);
                    std::vector<Item const*> kids = cit == children.end() ? std::vector<Item const*>{} : cit->second;
                    out += render_item(*bit->second, kids, hl, 3, link_labels);
                }
            }
        }
        return out;
    }

    [[nodiscard]] std::string shell_quote(std::string_view s)
    {
        char q = static_cast<char>(39);
        char bs = static_cast<char>(92);
        std::string out;
        out += q;
        for (char c : s)
        {
            if (c == q)
            {
                out += q;
                out += bs;
                out += q;
                out += q;
            }
            else
                out += c;
        }
        out += q;
        return out;
    }

    [[nodiscard]] bool typst_present()
    {
        std::string out;
        std::array<char, 256> buf{};
        auto* pipe = ::popen("typst --version 2>&1", "r");
        if (!pipe)
            return false;
        while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
            out += buf.data();
        int rc = ::pclose(pipe);
        return rc == 0 && out.find("typst") != std::string::npos;
    }

    [[nodiscard]] int compile_pdf(std::filesystem::path const& typ_path, std::filesystem::path const& pdf_path)
    {
        if (!typst_present())
        {
            std::println(std::cerr, "dcdoc: typst not found on PATH; install typst to generate PDFs");
            return 2;
        }
        std::string cmd = "typst compile " + shell_quote(typ_path.string()) + " " + shell_quote(pdf_path.string()) + " 2>&1";
        std::string out;
        std::array<char, 256> buf{};
        auto* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe)
        {
            std::println(std::cerr, "dcdoc: failed to run typst");
            return 1;
        }
        while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
            out += buf.data();
        int rc = ::pclose(pipe);
        if (rc != 0)
        {
            std::println(std::cerr, "dcdoc: typst failed for {}; generated source kept at {}", pdf_path.string(), typ_path.string());
            std::println(std::cerr, "{}", out);
            return 1;
        }
        return 0;
    }
} // namespace dcdoc::typst
