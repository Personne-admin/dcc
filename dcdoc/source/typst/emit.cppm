module;

#include <cstdio>

export module dcdoc.typst.emit;

import std;
import dcc.sm;
import dcc.si;
import dcc.lex.tokens;
import dcc.lex;
import dcdoc.model;
import dcdoc.prose;

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

    [[nodiscard]] std::string render_code_span(std::string_view code)
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

    [[nodiscard]] std::string render_spans(std::vector<dcdoc::prose::Inline> const& spans, std::unordered_map<std::string, std::string> const& labels)
    {
        std::string out;
        for (auto const& sp : spans)
        {
            if (sp.kind == dcdoc::prose::InlineKind::Code)
            {
                out += render_code_span(sp.text);
                continue;
            }
            if (sp.kind != dcdoc::prose::InlineKind::Link)
            {
                out += esc(sp.text);
                continue;
            }
            std::string label;
            if (sp.resolved && !sp.ambiguous)
            {
                auto it = labels.find(sp.target);
                if (it != labels.end())
                    label = it->second;
            }
            std::string shown;
            for (auto const& sub : dcdoc::prose::split_inline(sp.text))
                shown += sub.kind == dcdoc::prose::InlineKind::Code ? render_code_span(sub.text) : esc(sub.text);
            if (!label.empty())
                out += "#link(<" + label + ">)" + "[" + shown + "]";
            else if (sp.ambiguous)
                out += "#text(fill: luma(130))[" + shown + "]";
            else if (sp.resolved)
                out += "#text(style: \"italic\")[" + shown + "]";
            else
                out += shown;
        }
        return out;
    }

    [[nodiscard]] std::string code_escape(std::string_view s)
    {
        std::string out;
        for (char c : s)
        {
            if (c == 34 || c == 92)
                out += "\\";
            if (c == 10)
                out += "n";
            else
                out += c;
        }
        return out;
    }

    [[nodiscard]] std::string render_sig(std::vector<dcdoc::prose::HlToken> const& toks)
    {
        std::string out = "#{";
        bool first = true;
        for (auto const& t : toks)
        {
            if (!first)
                out += " + ";
            first = false;
            if (t.cls == dcdoc::prose::HlClass::Keyword)
                out += "(text(fill: rgb(31, 111, 235), \"" + code_escape(t.text) + "\"))";
            else if (t.cls == dcdoc::prose::HlClass::Literal)
                out += "(text(fill: rgb(149, 56, 0), \"" + code_escape(t.text) + "\"))";
            else
                out += "\"" + code_escape(t.text) + "\"";
        }
        return out + "}";
    }

    [[nodiscard]] std::string render_doc(std::string_view text, std::vector<CrossRef> const& refs, std::unordered_map<std::string, std::string> const& labels)
    {
        std::string out;
        for (auto const& block : dcdoc::prose::parse_doc(text, refs))
        {
            if (block.code)
            {
                out += render_raw_block(block.text);
                out += "\n\n";
                continue;
            }
            out += render_spans(block.spans, labels) + "\n\n";
        }
        return out;
    }

    [[nodiscard]] std::string render_item(Item const& item, std::vector<Item const*> const& children, dcdoc::prose::Highlighter& hl, int level,
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
        out += render_sig(hl.highlight(item.sig.text));
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
        dcdoc::prose::Highlighter hl;
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
