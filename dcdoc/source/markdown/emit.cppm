export module dcdoc.markdown.emit;

import std;
import dcdoc.model;
import dcdoc.prose;

export namespace dcdoc::markdown
{
    [[nodiscard]] std::string esc_inline(std::string_view s)
    {
        std::string out;
        for (char c : s)
        {
            switch (c)
            {
                case 92:
                case 96:
                case 42:
                case 91:
                case 93:
                case 95:
                    out += static_cast<char>(92);
                    out += c;
                    break;
                default:
                    out += c;
            }
        }
        return out;
    }

    [[nodiscard]] std::string esc_line_start(std::string_view line)
    {
        if (line.empty())
            return {};
        char c = line.front();
        bool marker = c == 35 || c == 45 || c == 43 || c == 62;
        bool space_after = line.size() > 1 && (line[1] == 32 || line[1] == 9);
        if (marker && (space_after || line.size() == 1))
            return std::string{"\\"} + esc_inline(line);
        std::size_t i = 0;
        while (i < line.size() && line[i] >= 48 && line[i] <= 57)
            ++i;
        if (i > 0 && i < line.size() && (line[i] == 46 || line[i] == 41))
            return esc_inline(line.substr(0, i)) + std::string{"\\"} + esc_inline(line.substr(i));
        return esc_inline(line);
    }

    [[nodiscard]] std::string esc_prose(std::string_view s)
    {
        std::string out;
        std::size_t i = 0;
        bool first = true;
        while (i <= s.size())
        {
            std::size_t j = s.find(10, i);
            if (j == std::string_view::npos)
                j = s.size();
            std::string_view line = s.substr(i, j - i);
            std::size_t k = 0;
            std::size_t spaces = 0;
            bool tab = false;
            while (k < line.size() && (line[k] == 32 || line[k] == 9))
            {
                spaces += line[k] == 32 ? 1 : 0;
                tab = tab || line[k] == 9;
                ++k;
            }
            if (!first)
                out += static_cast<char>(10);
            first = false;
            if (tab || spaces >= 4)
            {
                for (std::size_t n = 0; n < k; ++n)
                    out += "&#32;";
                out += esc_inline(line.substr(k));
            }
            else
            {
                out += std::string{line.substr(0, k)};
                out += esc_line_start(line.substr(k));
            }
            if (j == s.size())
                break;
            i = j + 1;
        }
        return out;
    }

    [[nodiscard]] std::string slug(std::string_view text)
    {
        std::string out;
        for (char c : text)
        {
            if (c >= 65 && c <= 90)
                out += static_cast<char>(c + 32);
            else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 95 || c == 45)
                out += c;
            else if (c == 32)
                out += static_cast<char>(45);
        }
        return out;
    }

    [[nodiscard]] std::string fence(std::string_view content, std::string_view lang)
    {
        std::size_t run = 0;
        std::size_t best = 0;
        for (char c : content)
        {
            if (c == 96)
            {
                ++run;
                best = run > best ? run : best;
            }
            else
                run = 0;
        }
        std::size_t n = best + 1 < 3 ? 3 : best + 1;
        std::string delim(n, static_cast<char>(96));
        std::string out = delim;
        if (!lang.empty())
            out += lang;
        out += static_cast<char>(10);
        out += content;
        if (content.empty() || content.back() != 10)
            out += static_cast<char>(10);
        out += delim;
        return out + static_cast<char>(10);
    }

    class SlugAssigner
    {
    public:
        std::string add_text(std::string_view base)
        {
            std::string text{base};
            std::string s = slug(text);
            int n = 2;
            while (!m_used.insert(s).second)
            {
                text = std::string{base} + " (" + std::to_string(n++) + ")";
                s = slug(text);
            }
            return text;
        }

        std::string add_item(std::string const& id, std::string_view base)
        {
            std::string text = add_text(base);
            m_item_text[id] = text;
            m_item_slug[id] = slug(text);
            return text;
        }

        [[nodiscard]] std::string text_of(std::string const& id) const
        {
            auto it = m_item_text.find(id);
            return it == m_item_text.end() ? std::string{} : it->second;
        }

        [[nodiscard]] std::string slug_of(std::string const& id) const
        {
            auto it = m_item_slug.find(id);
            return it == m_item_slug.end() ? std::string{} : it->second;
        }

    private:
        std::unordered_set<std::string> m_used;
        std::unordered_map<std::string, std::string> m_item_text;
        std::unordered_map<std::string, std::string> m_item_slug;
    };

    struct PagePlan
    {
        std::string page;
        std::string mod_text;
        std::vector<std::string> sec_texts;
        SlugAssigner slugs;
        std::vector<std::string> item_ids;
    };

    struct DocMaps
    {
        std::unordered_map<std::string, std::string> head_text;
        std::unordered_map<std::string, std::string> slug;
        std::unordered_map<std::string, std::string> page;
    };

    [[nodiscard]] DocMaps build_doc_maps(std::vector<PagePlan> const& plans)
    {
        DocMaps maps;
        for (auto const& plan : plans)
            for (auto const& id : plan.item_ids)
            {
                maps.head_text[id] = plan.slugs.text_of(id);
                maps.slug[id] = plan.slugs.slug_of(id);
                maps.page[id] = plan.page;
            }
        return maps;
    }
    [[nodiscard]] std::string render_code(std::string_view text)
    {
        std::size_t run = 0;
        std::size_t best = 0;
        for (char c : text)
        {
            if (c == 96)
            {
                ++run;
                best = run > best ? run : best;
            }
            else
                run = 0;
        }
        std::size_t n = best + 1 < 1 ? 1 : best + 1;
        std::string delim(n, static_cast<char>(96));
        bool pad = !text.empty() && (text.front() == 96 || text.back() == 96);
        return delim + (pad ? " " : "") + std::string{text} + (pad ? " " : "") + delim;
    }

    [[nodiscard]] std::string render_spans(std::vector<prose::Inline> const& spans, DocMaps const& maps, std::string const& self_page)
    {
        std::string out;
        for (auto const& sp : spans)
        {
            if (sp.kind == prose::InlineKind::Code)
            {
                out += render_code(sp.text);
                continue;
            }
            if (sp.kind != prose::InlineKind::Link)
            {
                out += esc_prose(sp.text);
                continue;
            }
            std::string shown;
            for (auto const& sub : prose::split_inline(sp.text))
            {
                if (sub.kind == prose::InlineKind::Code)
                    shown += render_code(sub.text);
                else
                    shown += esc_prose(sub.text);
            }
            std::string href;
            if (sp.resolved && !sp.ambiguous)
            {
                auto sit = maps.slug.find(sp.target);
                auto pit = maps.page.find(sp.target);
                if (sit != maps.slug.end() && pit != maps.page.end() && !sit->second.empty())
                {
                    if (pit->second.empty() || pit->second == self_page)
                        href = "#" + sit->second;
                    else
                        href = pit->second + "#" + sit->second;
                }
            }
            std::string link_text = esc_prose(sp.text);
            if (!href.empty())
                out += "[" + link_text + "](" + href + ")";
            else if (sp.ambiguous)
                out += "*" + shown + "*";
            else if (sp.resolved)
                out += "[" + link_text + "]";
            else
                out += shown;
        }
        return out;
    }

    [[nodiscard]] std::string render_doc(std::string_view text, std::vector<CrossRef> const& refs, DocMaps const& maps, std::string const& self_page)
    {
        std::string out;
        for (auto const& block : prose::parse_doc(text, refs))
        {
            if (block.code)
            {
                out += fence(block.text, block.lang.empty() ? "dc" : block.lang);
                out += "\n";
                continue;
            }
            out += render_spans(block.spans, maps, self_page) + "\n\n";
        }
        return out;
    }

    [[nodiscard]] std::string render_item(Item const& item, std::vector<Item const*> const& children, DocMaps const& maps, int level,
                                          std::string const& self_page)
    {
        std::string head_text = item.name;
        auto hit = maps.head_text.find(item.id);
        if (hit != maps.head_text.end() && !hit->second.empty())
            head_text = hit->second;
        std::string out = std::string(static_cast<std::size_t>(level), static_cast<char>(35)) + " " + esc_prose(head_text) + "\n\n";
        out += fence(item.sig.text, "dc") + "\n";
        if (!item.doc.empty())
            out += render_doc(item.doc, item.doc_refs, maps, self_page);
        if (!item.file.empty())
            out += "Defined in " + esc_prose(item.file) + ":" + std::to_string(item.line) + "\n\n";
        for (auto const* child : children)
            out += render_item(*child, {}, maps, level + 1, self_page);
        return out;
    }

    [[nodiscard]] std::string page_filename(std::string_view module_id)
    {
        std::string out;
        std::size_t i = 0;
        while (i < module_id.size())
        {
            if (module_id[i] == 58 && i + 1 < module_id.size() && module_id[i + 1] == 58)
            {
                out += static_cast<char>(46);
                i += 2;
                continue;
            }
            char c = module_id[i];
            bool keep = (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57) || c == 95 || c == 45 || c == 46;
            out += keep ? c : static_cast<char>(45);
            ++i;
        }
        if (out.empty())
            out = "module";
        return out + ".md";
    }

    struct SiteFile
    {
        std::string name;
        std::string content;
    };

    [[nodiscard]] std::string hashes(int level)
    {
        return std::string(static_cast<std::size_t>(level), static_cast<char>(35));
    }

    [[nodiscard]] std::vector<PagePlan> plan_pages(Project const& project, std::unordered_map<std::string, std::string>& page_of, bool single)
    {
        for (auto const& m : project.modules)
            for (auto const& s : m.sections)
                for (auto const& sid : s.items)
                    page_of[sid] = single ? std::string{} : page_filename(m.id);
        bool progress = true;
        while (progress)
        {
            progress = false;
            for (auto const& it : project.items)
            {
                if (it.parent.empty() || page_of.contains(it.id))
                    continue;
                auto pit = page_of.find(it.parent);
                if (pit != page_of.end())
                {
                    page_of[it.id] = pit->second;
                    progress = true;
                }
            }
        }
        std::vector<PagePlan> plans;
        std::unordered_set<std::string> used;
        if (!single)
            used.insert("index.md");
        SlugAssigner shared;
        std::string name = project.entry_module.empty() ? "Project" : project.entry_module;
        if (single)
            shared.add_text(name);
        for (auto const& m : project.modules)
        {
            PagePlan plan;
            SlugAssigner& a = single ? shared : plan.slugs;
            if (!single)
            {
                std::string stem = page_filename(m.id);
                stem = stem.substr(0, stem.size() - 3);
                plan.page = stem + ".md";
                int n = 2;
                while (!used.insert(plan.page).second)
                    plan.page = stem + std::to_string(n++) + ".md";
                for (auto const& s : m.sections)
                    for (auto const& sid : s.items)
                        page_of[sid] = plan.page;
                bool inner = true;
                while (inner)
                {
                    inner = false;
                    for (auto const& it : project.items)
                    {
                        if (it.parent.empty() || page_of.contains(it.id))
                            continue;
                        auto pit = page_of.find(it.parent);
                        if (pit != page_of.end())
                        {
                            page_of[it.id] = pit->second;
                            inner = true;
                        }
                    }
                }
            }
            plan.mod_text = a.add_text(m.id);
            for (auto const& s : m.sections)
                plan.sec_texts.push_back(s.title.empty() ? std::string{} : a.add_text(s.title));
            std::unordered_map<std::string, Item const*> by_id;
            for (auto const& it : project.items)
                by_id[it.id] = &it;
            std::unordered_map<std::string, std::vector<Item const*>> children;
            for (auto const& it : project.items)
                if (!it.parent.empty())
                    children[it.parent].push_back(&it);
            for (auto const& s : m.sections)
                for (auto const& sid : s.items)
                {
                    auto bit = by_id.find(sid);
                    if (bit == by_id.end())
                        continue;
                    a.add_item(sid, bit->second->name);
                    plan.item_ids.push_back(sid);
                    auto cit = children.find(sid);
                    if (cit != children.end())
                        for (auto const* c : cit->second)
                        {
                            a.add_item(c->id, c->name);
                            plan.item_ids.push_back(c->id);
                        }
                }
            plans.push_back(std::move(plan));
        }
        if (single)
            for (auto& p : plans)
                p.slugs = shared;
        return plans;
    }

    [[nodiscard]] std::string emit_module(Module const& m, PagePlan const& plan, std::unordered_map<std::string, Item const*> const& by_id,
                                          std::unordered_map<std::string, std::vector<Item const*>> const& children, DocMaps const& maps, int base)
    {
        std::string self_page = plan.page;
        std::string out = hashes(base) + " " + esc_prose(plan.mod_text) + "\n\n";
        if (!m.file.empty())
            out += "Defined in " + esc_prose(m.file) + "\n\n";
        if (!m.overview.empty())
            out += render_doc(m.overview, {}, maps, self_page);
        for (std::size_t i = 0; i < m.sections.size(); ++i)
        {
            auto const& s = m.sections[i];
            std::string title = i < plan.sec_texts.size() ? plan.sec_texts[i] : s.title;
            if (!title.empty())
                out += hashes(base + 1) + " " + esc_prose(title) + "\n\n";
            if (!s.body.empty())
                out += render_doc(s.body, {}, maps, self_page);
            for (auto const& id : s.items)
            {
                auto bit = by_id.find(id);
                if (bit == by_id.end())
                    continue;
                auto cit = children.find(id);
                std::vector<Item const*> kids = cit == children.end() ? std::vector<Item const*>{} : cit->second;
                out += render_item(*bit->second, kids, maps, base + 2, self_page);
            }
        }
        return out;
    }

    [[nodiscard]] std::string rstrip_newlines(std::string s)
    {
        while (s.size() >= 2 && s[s.size() - 1] == 10 && s[s.size() - 2] == 10)
            s.pop_back();
        return s;
    }

    [[nodiscard]] std::string render_single(Project const& project)
    {
        std::unordered_map<std::string, std::string> page_of;
        auto plans = plan_pages(project, page_of, true);
        DocMaps maps = build_doc_maps(plans);
        std::unordered_map<std::string, Item const*> by_id;
        for (auto const& it : project.items)
            by_id[it.id] = &it;
        std::unordered_map<std::string, std::vector<Item const*>> children;
        for (auto const& it : project.items)
            if (!it.parent.empty())
                children[it.parent].push_back(&it);
        std::string name = project.entry_module.empty() ? "Project" : project.entry_module;
        std::string out = "# " + esc_prose(name) + "\n\n";
        if (!project.overview.empty())
        {
            for (auto const& block : prose::parse_doc(project.overview, {}))
            {
                if (block.code)
                    out += fence(block.text, block.lang.empty() ? "dc" : block.lang) + "\n";
                else
                    out += render_spans(block.spans, maps, {}) + "\n\n";
            }
        }
        for (std::size_t i = 0; i < project.modules.size(); ++i)
            out += emit_module(project.modules[i], plans[i], by_id, children, maps, 2);
        return rstrip_newlines(out);
    }

    [[nodiscard]] std::vector<SiteFile> render_multi(Project const& project)
    {
        std::unordered_map<std::string, std::string> page_of;
        auto plans = plan_pages(project, page_of, false);
        DocMaps maps = build_doc_maps(plans);
        std::unordered_map<std::string, Item const*> by_id;
        for (auto const& it : project.items)
            by_id[it.id] = &it;
        std::unordered_map<std::string, std::vector<Item const*>> children;
        for (auto const& it : project.items)
            if (!it.parent.empty())
                children[it.parent].push_back(&it);
        std::string name = project.entry_module.empty() ? "Project" : project.entry_module;
        std::string index = "# " + esc_prose(name) + "\n\n";
        if (!project.overview.empty())
        {
            for (auto const& block : prose::parse_doc(project.overview, {}))
            {
                if (block.code)
                    index += fence(block.text, block.lang.empty() ? "dc" : block.lang) + "\n";
                else
                    index += render_spans(block.spans, maps, {}) + "\n\n";
            }
        }
        index += "## Modules\n\n";
        for (std::size_t i = 0; i < project.modules.size(); ++i)
            index += "- [" + esc_prose(project.modules[i].id) + "](" + plans[i].page + ")\n";
        std::vector<SiteFile> out;
        out.push_back({.name = "index.md", .content = rstrip_newlines(index)});
        for (std::size_t i = 0; i < project.modules.size(); ++i)
            out.push_back({.name = plans[i].page, .content = rstrip_newlines(emit_module(project.modules[i], plans[i], by_id, children, maps, 1))});
        return out;
    }

    [[nodiscard]] int write_markdown(Project const& project, std::filesystem::path const& path, bool is_dir)
    {
        if (is_dir)
        {
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (ec)
            {
                std::println(std::cerr, "dcdoc: cannot create directory {}", path.string());
                return 1;
            }
            for (auto const& f : render_multi(project))
            {
                std::ofstream out{path / f.name, std::ios::binary};
                if (!out)
                {
                    std::println(std::cerr, "dcdoc: cannot write {}", (path / f.name).string());
                    return 1;
                }
                out << f.content;
            }
            return 0;
        }
        std::error_code ec;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            std::println(std::cerr, "dcdoc: cannot create directory {}", path.parent_path().string());
            return 1;
        }
        std::ofstream out{path, std::ios::binary};
        if (!out)
        {
            std::println(std::cerr, "dcdoc: cannot write {}", path.string());
            return 1;
        }
        out << render_single(project);
        return 0;
    }
} // namespace dcdoc::markdown
