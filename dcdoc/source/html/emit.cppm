export module dcdoc.html.emit;

import std;
import dcdoc.model;
import dcdoc.prose;

export namespace dcdoc::html
{
    [[nodiscard]] std::string esc(std::string_view s)
    {
        std::string out;
        for (char c : s)
        {
            switch (c)
            {
                case 38:
                    out += "&amp;";
                    break;
                case 60:
                    out += "&lt;";
                    break;
                case 62:
                    out += "&gt;";
                    break;
                case 34:
                    out += "&quot;";
                    break;
                case 39:
                    out += "&#39;";
                    break;
                default:
                    out += c;
            }
        }
        return out;
    }

    [[nodiscard]] std::string url_encode(std::string_view s)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string out;
        for (char c : s)
        {
            switch (c)
            {
                case 35:
                case 63:
                case 32:
                case 37:
                case 34:
                case 60:
                case 62:
                    out += static_cast<char>(37);
                    out += digits[(static_cast<unsigned char>(c) >> 4) & 15];
                    out += digits[static_cast<unsigned char>(c) & 15];
                    break;
                default:
                    out += c;
            }
        }
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
        return out + ".html";
    }

    [[nodiscard]] std::string json_escape(std::string_view s)
    {
        std::string out;
        for (char c : s)
        {
            switch (c)
            {
                case 34:
                    out += "\\\"";
                    break;
                case 92:
                    out += "\\\\";
                    break;
                case 10:
                    out += "\\n";
                    break;
                case 13:
                    out += "\\r";
                    break;
                case 9:
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 32)
                    {
                        constexpr char digits[] = "0123456789abcdef";
                        out += "\\u00";
                        out += digits[(static_cast<unsigned char>(c) >> 4) & 15];
                        out += digits[static_cast<unsigned char>(c) & 15];
                    }
                    else
                        out += c;
            }
        }
        return out;
    }

    [[nodiscard]] std::string json_string(std::string_view s)
    {
        return std::string{} + static_cast<char>(34) + json_escape(s) + static_cast<char>(34);
    }

    [[nodiscard]] std::string render_sig(std::vector<dcdoc::prose::HlToken> const& toks)
    {
        std::string out;
        for (auto const& t : toks)
        {
            if (t.cls == dcdoc::prose::HlClass::Keyword)
                out += "<span class=\"tok-kw\">" + esc(t.text) + "</span>";
            else if (t.cls == dcdoc::prose::HlClass::Literal)
                out += "<span class=\"tok-lit\">" + esc(t.text) + "</span>";
            else
                out += esc(t.text);
        }
        return out;
    }

    [[nodiscard]] std::string href_for(std::string const& target, std::unordered_map<std::string, std::string> const& page_of, std::string const& self_page)
    {
        auto it = page_of.find(target);
        if (it == page_of.end())
            return {};
        if (it->second == self_page)
            return std::string{"#"} + url_encode(target);
        return it->second + "#" + url_encode(target);
    }

    [[nodiscard]] std::string render_spans(std::vector<dcdoc::prose::Inline> const& spans, std::unordered_map<std::string, std::string> const& page_of,
                                           std::string const& self_page)
    {
        std::string out;
        for (auto const& sp : spans)
        {
            if (sp.kind == dcdoc::prose::InlineKind::Code)
            {
                out += "<code>" + esc(sp.text) + "</code>";
                continue;
            }
            if (sp.kind != dcdoc::prose::InlineKind::Link)
            {
                out += esc(sp.text);
                continue;
            }
            std::string shown;
            for (auto const& sub : dcdoc::prose::split_inline(sp.text))
                shown += sub.kind == dcdoc::prose::InlineKind::Code ? "<code>" + esc(sub.text) + "</code>" : esc(sub.text);
            std::string href;
            if (sp.resolved && !sp.ambiguous)
                href = href_for(sp.target, page_of, self_page);
            if (!href.empty())
                out += "<a class=\"ref\" href=\"" + esc(href) + "\">" + shown + "</a>";
            else if (sp.ambiguous)
                out += "<span class=\"ref-ambig\">" + shown + "</span>";
            else if (sp.resolved)
                out += "<span class=\"ref-ext\">" + shown + "</span>";
            else
                out += "<span class=\"ref-unres\">" + shown + "</span>";
        }
        return out;
    }

    [[nodiscard]] std::string render_doc(std::string_view text, std::vector<CrossRef> const& refs, std::unordered_map<std::string, std::string> const& page_of,
                                         std::string const& self_page, std::string const& fence_default_lang)
    {
        std::string out;
        for (auto const& block : dcdoc::prose::parse_doc(text, refs))
        {
            if (block.code)
            {
                std::string lang = block.lang.empty() ? fence_default_lang : block.lang;
                out += "<pre><code class=\"language-" + esc(lang) + "\">" + esc(block.text) + "</code></pre>\n";
                continue;
            }
            out += "<p>" + render_spans(block.spans, page_of, self_page) + "</p>\n";
        }
        return out;
    }

    [[nodiscard]] std::string render_item(Item const& item, std::vector<Item const*> const& children, dcdoc::prose::Highlighter& hl, int level,
                                          std::unordered_map<std::string, std::string> const& page_of, std::string const& self_page)
    {
        std::string tag = "h" + std::to_string(level);
        std::string out = "<" + tag + " id=\"" + esc(item.id) + "\">" + esc(item.name) + "</" + tag + ">\n";
        out += "<p class=\"kind\">" + esc(item.kind);
        out += item.is_public ? ", public" : ", private";
        out += "</p>\n";
        out += "<pre class=\"sig\"><code>\n";
        out += render_sig(hl.highlight(item.sig.text));
        out += "</code></pre>\n";
        if (!item.doc.empty())
            out += render_doc(item.doc, item.doc_refs, page_of, self_page, "dc");
        if (!item.file.empty())
            out += "<p class=\"defined\">defined in " + esc(item.file) + ":" + std::to_string(item.line) + "</p>\n";
        for (auto const* child : children)
            out += render_item(*child, {}, hl, level + 1, page_of, self_page);
        return out;
    }

    void build_maps(Project const& project, std::unordered_map<std::string, std::string>& page_of, std::unordered_map<std::string, std::string>& mod_of)
    {
        std::unordered_map<std::string, Item const*> by_id;
        for (auto const& it : project.items)
            by_id[it.id] = &it;
        for (auto const& m : project.modules)
        {
            std::string page = page_filename(m.id);
            for (auto const& s : m.sections)
                for (auto const& sid : s.items)
                {
                    page_of[sid] = page;
                    mod_of[sid] = m.id;
                }
        }
        bool progress = true;
        while (progress)
        {
            progress = false;
            for (auto const& it : project.items)
            {
                if (it.parent.empty() || page_of.contains(it.id))
                    continue;
                auto pit = page_of.find(it.parent);
                auto mit = mod_of.find(it.parent);
                if (pit != page_of.end())
                {
                    page_of[it.id] = pit->second;
                    if (mit != mod_of.end())
                        mod_of[it.id] = mit->second;
                    progress = true;
                }
            }
        }
    }

    [[nodiscard]] std::string search_snippet()
    {
        return "<p><input id=\"search\" type=\"text\" placeholder=\"Search items\" autocomplete=\"off\"/></p>\n<ul id=\"results\"></ul>\n<script "
               "src=\"search.js\"></script>\n";
    }

    [[nodiscard]] std::string page_head(std::string_view title)
    {
        std::string out = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\"/>\n<title>";
        out += esc(title);
        out += "</title>\n<link rel=\"stylesheet\" href=\"style.css\"/>\n</head>\n<body>\n";
        return out;
    }

    [[nodiscard]] std::string nav_html(Project const& project, std::string_view self_page)
    {
        std::string out = "<nav><a href=\"index.html\">index</a>";
        for (auto const& m : project.modules)
        {
            std::string page = page_filename(m.id);
            if (page == self_page)
                out += " | " + esc(m.id);
            else
                out += " | <a href=\"" + esc(page) + "\">" + esc(m.id) + "</a>";
        }
        return out + "</nav>\n";
    }

    [[nodiscard]] std::string render_module_page(Module const& m, Project const& project, std::unordered_map<std::string, Item const*> const& by_id,
                                                 std::unordered_map<std::string, std::vector<Item const*>> const& children,
                                                 std::unordered_map<std::string, std::string> const& page_of, dcdoc::prose::Highlighter& hl,
                                                 std::string const& page)
    {
        std::string out = page_head(m.id);
        out += nav_html(project, page);
        out += search_snippet();
        out += "<h1>" + esc(m.id) + "</h1>\n";
        if (!m.file.empty())
            out += "<p class=\"defined\">" + esc(m.file) + "</p>\n";
        if (!m.overview.empty())
            out += render_doc(m.overview, {}, page_of, page, "dc");
        for (auto const& s : m.sections)
        {
            if (!s.title.empty())
                out += "<h2>" + esc(s.title) + "</h2>\n";
            if (!s.body.empty())
                out += render_doc(s.body, {}, page_of, page, "dc");
            for (auto const& id : s.items)
            {
                auto bit = by_id.find(id);
                if (bit == by_id.end())
                    continue;
                auto cit = children.find(id);
                std::vector<Item const*> kids = cit == children.end() ? std::vector<Item const*>{} : cit->second;
                out += render_item(*bit->second, kids, hl, 3, page_of, page);
            }
        }
        return out + "</body>\n</html>\n";
    }

    [[nodiscard]] std::string render_index(Project const& project)
    {
        std::string name = project.entry_module.empty() ? "Project" : project.entry_module;
        std::string out = page_head(name);
        out += nav_html(project, "index.html");
        out += search_snippet();
        out += "<h1>" + esc(name) + "</h1>\n";
        if (!project.overview.empty())
            out += render_doc(project.overview, {}, {}, "index.html", "dc");
        out += "<h2>Modules</h2>\n<ul>\n";
        for (auto const& m : project.modules)
        {
            std::string page = page_filename(m.id);
            out += "<li><a href=\"" + esc(page) + "\">" + esc(m.id) + "</a></li>\n";
        }
        return out + "</ul>\n</body>\n</html>\n";
    }

    [[nodiscard]] std::string render_search_json(Project const& project, std::unordered_map<std::string, std::string> const& page_of,
                                                 std::unordered_map<std::string, std::string> const& mod_of)
    {
        std::string out = "{\"items\":[";
        bool first = true;
        for (auto const& it : project.items)
        {
            auto pit = page_of.find(it.id);
            if (pit == page_of.end())
                continue;
            auto mit = mod_of.find(it.id);
            std::string mod = mit == mod_of.end() ? std::string{} : mit->second;
            if (!first)
                out += ",";
            first = false;
            out += "{\"name\":" + json_string(it.name) + ",\"id\":" + json_string(it.id) + ",\"kind\":" + json_string(it.kind);
            out += ",\"module\":" + json_string(mod) + ",\"page\":" + json_string(pit->second) + ",\"anchor\":" + json_string(it.id) + "}";
        }
        return out + "]}";
    }

    [[nodiscard]] std::string css_text()
    {
        return "body{font-family:sans-serif;line-height:1.5;max-width:60em;margin:0 auto;padding:0 "
               "1em;color:#111}\npre{background:#f3f4f6;padding:0.75em;border-radius:4px;overflow-x:auto}\npre.sig{border-left:3px solid "
               "#1f6feb}\ncode{font-family:monospace;font-size:0.92em}\n.tok-kw{color:#1f6feb}\n.tok-lit{color:#953800}\na.ref{text-decoration:underline}\n."
               "ref-ambig{font-style:italic;color:#555}\n.ref-ext{border-bottom:1px dotted #777}\nnav{border-bottom:1px solid #ccc;padding:0.5em "
               "0}\np.kind,p.defined{color:#888;font-size:0.85em}\n#search{width:100%;max-width:30em;padding:0.4em}\n#results{list-style:none;padding-left:0}"
               "\n";
    }

    [[nodiscard]] std::string js_text()
    {
        return "async function boot(){var box=document.getElementById(\"search\");var list=document.getElementById(\"results\");if(!box||!list)return;var "
               "items=[];try{var res=await fetch(\"search-index.json\");var data=await "
               "res.json();items=data.items||[];}catch(e){return;}\nbox.addEventListener(\"input\",function(){var "
               "q=box.value.toLowerCase();list.innerHTML=\"\";if(!q)return;var n=0;for(var k=0;k<items.length;k++){var "
               "it=items[k];if(n>=20)break;if(it.name.toLowerCase().indexOf(q)<0)continue;var li=document.createElement(\"li\");var "
               "a=document.createElement(\"a\");a.href=it.page+\"#\"+encodeURIComponent(it.anchor);a.textContent=it.name+\" (\"+it.kind+\", "
               "\"+it.module+\")\";li.appendChild(a);list.appendChild(li);n++;}});}\ndocument.addEventListener(\"DOMContentLoaded\",boot);\n";
    }

    struct SiteFile
    {
        std::string name;
        std::string content;
    };

    [[nodiscard]] std::vector<SiteFile> render_site(Project const& project)
    {
        std::unordered_map<std::string, std::string> page_of;
        std::unordered_map<std::string, std::string> mod_of;
        build_maps(project, page_of, mod_of);
        std::unordered_map<std::string, Item const*> by_id;
        for (auto const& it : project.items)
            by_id[it.id] = &it;
        std::unordered_map<std::string, std::vector<Item const*>> children;
        for (auto const& it : project.items)
            if (!it.parent.empty())
                children[it.parent].push_back(&it);
        dcdoc::prose::Highlighter hl;
        std::vector<SiteFile> out;
        out.push_back({.name = "index.html", .content = render_index(project)});
        std::unordered_set<std::string> used{"index.html", "search-index.json", "style.css", "search.js"};
        for (auto const& m : project.modules)
        {
            std::string stem = page_filename(m.id);
            stem = stem.substr(0, stem.size() - 5);
            std::string page = stem + ".html";
            int n = 2;
            while (!used.insert(page).second)
                page = stem + std::to_string(n++) + ".html";
            for (auto& kv : page_of)
                if (kv.second == page_filename(m.id))
                    kv.second = page;
            out.push_back({.name = page, .content = render_module_page(m, project, by_id, children, page_of, hl, page)});
        }
        out.push_back({.name = "search-index.json", .content = render_search_json(project, page_of, mod_of)});
        out.push_back({.name = "style.css", .content = css_text()});
        out.push_back({.name = "search.js", .content = js_text()});
        return out;
    }

    [[nodiscard]] int write_site(Project const& project, std::filesystem::path const& dir)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            std::println(std::cerr, "dcdoc: cannot create directory {}", dir.string());
            return 1;
        }
        for (auto const& f : render_site(project))
        {
            std::ofstream out{dir / f.name, std::ios::binary};
            if (!out)
            {
                std::println(std::cerr, "dcdoc: cannot write {}", (dir / f.name).string());
                return 1;
            }
            out << f.content;
        }
        return 0;
    }
} // namespace dcdoc::html
