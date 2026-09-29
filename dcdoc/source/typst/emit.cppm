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
    // All document-wide typography lives here; content routines only assign roles.
    constexpr std::string_view kPreamble = R"typ(
#set page(paper: "a4", margin: (x: 24mm, y: 22mm),
  header: context {
    let modules = query(heading.where(level: 1)).filter(h => h.location().page() <= here().page())
    let sections = query(heading.where(level: 2)).filter(h => h.location().page() == here().page())
    if modules.len() > 0 {
      let label = if sections.len() > 0 { sections.last().body } else { modules.last().body }
      align(right, text(size: 8pt, fill: luma(110), modules.last().body + " / " + label))
    }
  },
  footer: context align(center, text(size: 8pt, fill: luma(110), counter(page).display())))
#set text(font: "Libertinus Serif", size: 10.5pt)
#set par(leading: 0.68em, spacing: 0.8em)
#set heading(numbering: "1.1.1")
#show heading.where(level: 1): set text(size: 20pt, weight: "bold")
#show heading.where(level: 2): set text(size: 15pt, weight: "bold")
#show heading.where(level: 3): set text(size: 12pt, weight: "bold")
#show raw: set text(font: "DejaVu Sans Mono", size: 8.5pt)
#let source-frame(body) = block(fill: luma(249), inset: 6pt, width: 100%, breakable: true, body)
#let source-row(number, body) = grid(columns: (23pt, 1fr), column-gutter: 4pt,
  [#text(size: 7pt, fill: luma(140))[#number]],
  [#text(font: "DejaVu Sans Mono", size: 7.3pt, body)])
#let source-keyword(s) = text(fill: rgb(31, 111, 235), s)
#let source-literal(s) = text(fill: rgb(149, 56, 0), s)
#let source-comment(s) = text(fill: rgb(51, 119, 72), s)
)typ";

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
        bool skip_first = false;
        for (std::size_t si = 0; si < spans.size(); ++si)
        {
            auto const& sp = spans[si];
            if (sp.kind == dcdoc::prose::InlineKind::Emph || sp.kind == dcdoc::prose::InlineKind::Strong)
            {
                out += sp.kind == dcdoc::prose::InlineKind::Emph ? "#emph[" : "#strong[";
                out += render_spans(sp.children, labels) + "]";
                continue;
            }
            if (sp.kind == dcdoc::prose::InlineKind::Math)
            {
                out += "$" + sp.text + "$";
                continue;
            }
            if (sp.kind == dcdoc::prose::InlineKind::Code)
            {
                out += render_code_span(sp.text);
                continue;
            }
            if (sp.kind != dcdoc::prose::InlineKind::Link)
            {
                out += esc(skip_first ? std::string_view{sp.text}.substr(1) : std::string_view{sp.text});
                skip_first = false;
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
            {
                bool chain = si + 1 < spans.size() && spans[si + 1].kind == dcdoc::prose::InlineKind::Text &&
                             !spans[si + 1].text.empty() && (spans[si + 1].text.front() == '(' || spans[si + 1].text.front() == '[');
                if (chain)
                {
                    out += "#(link(<" + label + ">, [" + shown + "]) + \"" + spans[si + 1].text.front() + "\")";
                    skip_first = true;
                }
                else out += "#(link(<" + label + ">, [" + shown + "]))";
            }
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

    [[nodiscard]] std::string wrapped_signature(std::string_view sig)
    {
        if (sig.size() <= 72)
            return std::string{sig};
        std::string out;
        int depth = 0;
        for (char c : sig)
        {
            if (c == '(') ++depth;
            if (c == ')') --depth;
            out += c;
            if (c == ',' && depth == 1)
            {
                out += "\n    ";
            }
        }
        return out;
    }

    [[nodiscard]] std::string render_sig(Signature const& sig, dcdoc::prose::Highlighter& hl,
                                         std::unordered_map<std::string, std::string> const& labels)
    {
        std::string source = wrapped_signature(sig.text);
        std::vector<std::pair<std::size_t, std::pair<std::size_t, std::string>>> links;
        for (auto const& ref : sig.refs)
        {
            if (!ref.resolved || ref.ambiguous || ref.raw.empty()) continue;
            auto label = labels.find(ref.target);
            if (label == labels.end()) continue;
            std::size_t pos = 0;
            while ((pos = source.find(ref.raw, pos)) != std::string::npos)
            {
                auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
                bool left = pos == 0 || !ident(source[pos - 1]);
                bool right = pos + ref.raw.size() == source.size() || !ident(source[pos + ref.raw.size()]);
                if (left && right)
                    links.push_back({pos, {ref.raw.size(), label->second}});
                pos += ref.raw.size();
            }
        }
        std::ranges::sort(links, {}, [](auto const& p) { return p.first; });
        std::vector<std::string> parts;
        auto append_plain = [&](std::string_view raw) {
            for (auto const& t : hl.highlight(raw))
            {
                std::size_t start = 0;
                for (std::size_t i = 0; i <= t.text.size(); ++i)
                {
                    if (i != t.text.size() && t.text[i] != '\n') continue;
                    if (i > start)
                    {
                        std::string piece = code_escape(std::string_view{t.text}.substr(start, i - start));
                        if (t.cls == dcdoc::prose::HlClass::Keyword)
                            parts.push_back("text(fill: rgb(31, 111, 235), \"" + piece + "\")");
                        else if (t.cls == dcdoc::prose::HlClass::Literal)
                            parts.push_back("text(fill: rgb(149, 56, 0), \"" + piece + "\")");
                        else
                            parts.push_back("\"" + piece + "\"");
                    }
                    if (i != t.text.size()) parts.push_back("linebreak() + h(2em)");
                    start = i + 1;
                }
            }
        };
        std::size_t pos = 0;
        for (auto const& [at, data] : links)
        {
            if (at < pos) continue;
            append_plain(std::string_view{source}.substr(pos, at - pos));
            parts.push_back("link(<" + data.second + ">, text(fill: rgb(31, 111, 235), \"" + code_escape(std::string_view{source}.substr(at, data.first)) + "\"))");
            pos = at + data.first;
        }
        append_plain(std::string_view{source}.substr(pos));
        std::string out = "#{";
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i) out += " + ";
            out += parts[i];
        }
        return out + "}";
    }

    [[nodiscard]] std::string render_blocks(std::vector<dcdoc::prose::Block> const& blocks,
                                             std::unordered_map<std::string, std::string> const& labels)
    {
        std::string out;
        for (auto const& block : blocks)
        {
            using K = dcdoc::prose::BlockKind;
            if (block.kind == K::Code)
            {
                out += render_raw_block(block.text);
                out += "\n\n";
                continue;
            }
            if (block.kind == K::Heading)
                out += "#heading(level: " + std::to_string(std::min(6, block.level + 2)) + ")[" + render_spans(block.spans, labels) + "]\n";
            else if (block.kind == K::BulletList || block.kind == K::NumberList)
            {
                out += block.kind == K::BulletList ? "#list(\n" : "#enum(start: " + std::to_string(block.start_number) + ",\n";
                for (auto const& child : block.children)
                    out += "[" + render_blocks(child.children, labels) + "],\n";
                out += ")\n\n";
            }
            else if (block.kind == K::Quote)
                out += "#quote(block: true)[" + render_blocks(block.children, labels) + "]\n\n";
            else if (block.kind == K::ListItem)
                out += render_blocks(block.children, labels);
            else
                out += render_spans(block.spans, labels) + "\n\n";
        }
        return out;
    }

    [[nodiscard]] std::string render_doc(std::string_view text, std::vector<CrossRef> const& refs, std::unordered_map<std::string, std::string> const& labels)
    {
        return render_blocks(dcdoc::prose::parse_doc(text, refs), labels);
    }

    [[nodiscard]] std::string relative_file(std::string_view file, std::filesystem::path const& root)
    {
        std::filesystem::path p{file};
        std::filesystem::path rel = p.lexically_relative(root);
        if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
        return p.filename().generic_string();
    }

    [[nodiscard]] std::string line_label(std::string_view module, std::uint32_t line)
    {
        std::string out = "source-";
        for (char raw : module)
        {
            unsigned char c = static_cast<unsigned char>(raw);
            out += std::isalnum(c) ? static_cast<char>(c) : '-';
        }
        return out + "-" + std::to_string(line);
    }

    [[nodiscard]] std::string styled_source(std::string_view raw, dcdoc::prose::HlClass cls)
    {
        std::string s = code_escape(raw);
        if (cls == dcdoc::prose::HlClass::Keyword)
            return "source-keyword(\"" + s + "\")";
        if (cls == dcdoc::prose::HlClass::Literal)
            return "source-literal(\"" + s + "\")";
        if (cls == dcdoc::prose::HlClass::Comment)
            return "source-comment(\"" + s + "\")";
        return "\"" + s + "\"";
    }

    [[nodiscard]] std::string source_content(std::string_view line, std::vector<dcdoc::prose::HlClass> const& classes,
                                              std::vector<std::string> const& links, std::size_t begin, std::size_t end)
    {
        std::string out = "#{";
        bool first = true;
        for (std::size_t i = begin; i < end;)
        {
            std::size_t j = i + 1;
            while (j < end && classes[j] == classes[i] && links[j] == links[i]) ++j;
            std::string piece = styled_source(line.substr(i, j - i), classes[i]);
            if (!links[i].empty()) piece = "link(<" + links[i] + ">, " + piece + ")";
            if (!first) out += " + ";
            first = false;
            out += piece;
            i = j;
        }
        if (first) out += "\" \"";
        return out + "}";
    }

    [[nodiscard]] std::string render_listing(Module const& module, Project const& project, dcdoc::prose::Highlighter& hl,
                                              std::unordered_map<std::string, std::string> const& labels,
                                              std::filesystem::path const& root)
    {
        std::ifstream input{module.file, std::ios::binary};
        if (!input) return {};
        std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        std::vector<dcdoc::prose::HlClass> styles(source.size(), dcdoc::prose::HlClass::Plain);
        std::size_t offset = 0;
        for (auto const& tok : hl.highlight(source, true))
        {
            for (std::size_t i = 0; i < tok.text.size() && offset + i < styles.size(); ++i)
                styles[offset + i] = tok.cls;
            offset += tok.text.size();
        }
        std::unordered_map<std::uint32_t, std::vector<Item const*>> declarations;
        for (auto const& item : project.items)
            if (item.file == module.file && item.line > 0) declarations[item.line].push_back(&item);
        std::string out = "#pagebreak()\n#heading(level: 2)[Source: " + esc(relative_file(module.file, root)) + "]\n";
        out += "#source-frame[\n";
        std::size_t start = 0;
        std::uint32_t number = 1;
        while (start < source.size())
        {
            std::size_t end = source.find('\n', start);
            if (end == std::string::npos) end = source.size();
            if (end > start && source[end - 1] == '\r') --end;
            std::string_view line{source.data() + start, end - start};
            std::vector<dcdoc::prose::HlClass> classes(styles.begin() + static_cast<std::ptrdiff_t>(start),
                                                        styles.begin() + static_cast<std::ptrdiff_t>(end));
            std::vector<std::string> links(line.size());
            auto attach = [&](std::string_view needle, std::string const& label) {
                if (needle.empty() || label.empty()) return;
                std::size_t at = 0;
                while ((at = line.find(needle, at)) != std::string_view::npos)
                {
                    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
                    bool left = at == 0 || !ident(line[at - 1]);
                    bool right = at + needle.size() == line.size() || !ident(line[at + needle.size()]);
                    if (left && right)
                        for (std::size_t i = at; i < at + needle.size(); ++i) links[i] = label;
                    at += needle.size();
                }
            };
            if (auto it = declarations.find(number); it != declarations.end())
            {
                for (auto const* item : it->second)
                    for (auto const& ref : item->sig.refs)
                        if (ref.resolved && labels.contains(ref.target)) attach(ref.raw, labels.at(ref.target));
                for (auto const* item : it->second)
                    if (labels.contains(item->id)) attach(item->name, labels.at(item->id));
            }
            std::size_t chunk = 0;
            bool first = true;
            while (chunk < line.size() || first)
            {
                std::size_t stop = std::min(line.size(), chunk + 76);
                if (stop < line.size())
                {
                    std::size_t space = line.rfind(' ', stop);
                    if (space != std::string_view::npos && space > chunk + 30) stop = space + 1;
                }
                if (stop < line.size())
                    while (stop > chunk && (static_cast<unsigned char>(line[stop]) & 0xc0u) == 0x80u) --stop;
                if (stop == chunk) stop = std::min(line.size(), chunk + 76);
                std::string num = first ? std::to_string(number) : "";
                out += "#source-row(\"" + num + "\", [" + source_content(line, classes, links, chunk, stop) + "])";
                if (first) out += " <" + line_label(module.id, number) + ">";
                out += "\n";
                first = false;
                chunk = stop;
            }
            start = source.find('\n', start) == std::string::npos ? source.size() : source.find('\n', start) + 1;
            ++number;
        }
        out += "]\n";
        return out;
    }

    [[nodiscard]] std::string render_item(Item const& item, std::vector<Item const*> const& children, dcdoc::prose::Highlighter& hl, int level,
                                          std::unordered_map<std::string, std::string> const& link_labels,
                                          std::filesystem::path const& root, std::string_view module_id, bool listings)
    {
        std::string out = "#heading(level: " + std::to_string(level) + ")[" + esc(item.name) + "]";
        auto lit = link_labels.find(item.id);
        if (lit != link_labels.end())
            out += " <" + lit->second + ">";
        out += "\n";
        out += "#text(size: 8pt, fill: luma(140))[" + esc(item.kind) + "  ·  ";
        out += item.is_public ? "public" : "private";
        out += "]\n";
        out += "#block(fill: luma(243), inset: 8pt, radius: 4pt, width: 100%)[\n#set text(font: \"DejaVu Sans Mono\", size: 9pt)\n";
        out += render_sig(item.sig, hl, link_labels);
        out += "\n]\n";
        if (!item.doc.empty())
            out += render_doc(item.doc, item.doc_refs, link_labels);
        if (!item.file.empty())
        {
            std::string location = esc(relative_file(item.file, root)) + ":" + std::to_string(item.line);
            out += "#text(size: 8pt, fill: luma(140))[defined in ";
            out += listings && item.line > 0 ? "#link(<" + line_label(module_id, item.line) + ">)[" + location + "]" : location;
            out += "]\n";
        }
        if (!children.empty())
        {
            out += "#table(columns: (auto, 2fr, 3fr), inset: 5pt, stroke: luma(220),\n";
            out += "[*Name*], [*Type*], [*Description*],\n";
            for (auto const* child : children)
            {
                std::string type = child->sig.text;
                if (child->kind == "fparam" || child->kind == "field")
                {
                    if (type.ends_with(';')) type.pop_back();
                    std::string suffix = " " + child->name;
                    if (type.ends_with(suffix)) type.resize(type.size() - suffix.size());
                }
                out += "[";
                if (listings && child->line > 0)
                    out += "#link(<" + line_label(module_id, child->line) + ">)[" + esc(child->name) + "]";
                else out += esc(child->name);
                if (auto it = link_labels.find(child->id); it != link_labels.end()) out += " <" + it->second + ">";
                out += "], [" + render_code_span(type) + "], [";
                if (!child->doc.empty()) out += render_doc(child->doc, child->doc_refs, link_labels);
                out += "],\n";
            }
            out += ")\n";
        }
        return out;
    }

    [[nodiscard]] std::string render(Project const& project, bool listings = true)
    {
        std::unordered_map<std::string, std::string> link_labels;
        {
            LabelMaker maker;
            for (auto const& it : project.items)
                link_labels[it.id] = maker.make(it.id);
            for (auto const& m : project.modules)
                for (auto const& s : m.sections)
                    link_labels[s.id] = maker.make(s.id);
        }
        std::string name = project.entry_module.empty() ? "Project" : project.entry_module;
        auto now = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
        std::string out{kPreamble};
        out += "#align(center + horizon)[\n#text(26pt)[ ";
        out += esc(name);
        out += " ]\n";
        if (!project.overview.empty())
        {
            out += "#text(12pt)[\n";
            out += render_doc(project.overview, project.overview_refs, link_labels);
            out += "]\n";
        }
        out += "#text(10pt)[generated " + std::format("{:%F}", now) + "]\n]\n#pagebreak()\n#outline(title: \"Contents\")\n#pagebreak()\n";
        std::unordered_map<std::string, std::vector<Item const*>> children;
        for (auto const& it : project.items)
            if (!it.parent.empty())
                children[it.parent].push_back(&it);
        dcdoc::prose::Highlighter hl;
        std::unordered_map<std::string, Item const*> by_id;
        for (auto const& it : project.items)
            by_id[it.id] = &it;
        std::filesystem::path root;
        for (auto const& m : project.modules)
            if (m.id == project.entry_module) { root = std::filesystem::path{m.file}.parent_path(); break; }
        if (root.empty() && !project.modules.empty()) root = std::filesystem::path{project.modules.front().file}.parent_path();
        for (auto const& m : project.modules)
        {
            bool module_listing = listings && !m.id.starts_with("std::") && !m.id.starts_with("core::") &&
                                  !m.file.starts_with("dcc-core:");
            out += "#heading(level: 1)[" + esc(m.id) + "]\n";
            if (!m.file.empty())
                out += "#text(size: 8pt, fill: luma(140))[" + esc(relative_file(m.file, root)) + "]\n";
            if (!m.overview.empty())
                out += render_doc(m.overview, m.overview_refs, link_labels);
            for (auto const& s : m.sections)
            {
                if (!s.title.empty())
                    out += "#heading(level: 2)[" + esc(s.title) + "] <" + link_labels.at(s.id) + ">\n";
                if (!s.body.empty())
                    out += render_doc(s.body, s.body_refs, link_labels);
                for (auto const& id : s.items)
                {
                    auto bit = by_id.find(id);
                    if (bit == by_id.end())
                        continue;
                    auto cit = children.find(id);
                    std::vector<Item const*> kids = cit == children.end() ? std::vector<Item const*>{} : cit->second;
                    out += render_item(*bit->second, kids, hl, 3, link_labels, root, m.id, module_listing);
                }
            }
            if (module_listing) out += render_listing(m, project, hl, link_labels, root);
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
