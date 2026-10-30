module;

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

export module dpm.edit;

import std;
import dcc.lex;
import dcc.session;
import dpm.manifest;

export namespace dpm::edit
{
    enum class List : std::uint8_t
    {
        Dependencies,
        DevDependencies,
    };

    struct EditResult
    {
        bool ok{false};
        std::string text;
        std::vector<manifest::Diagnostic> diagnostics;
        std::string error;
    };

    [[nodiscard]] constexpr std::string_view field_name(List list) noexcept
    {
        return list == List::Dependencies ? "dependencies" : "dev_dependencies";
    }

    namespace detail
    {
        using TokenKind = dcc::lex::TokenKind;
        using manifest::detail::Tok;

        struct Splice
        {
            std::size_t pos{};
            std::size_t len{};
            std::string text;
        };

        struct Elem
        {
            std::size_t first{};
            std::size_t last{};
            std::optional<std::size_t> comma;
        };

        struct Layout
        {
            std::size_t open{};
            std::size_t close{};
            std::vector<Elem> elems;
        };

        struct Doc
        {
            std::string text;
            std::vector<Tok> tokens;
            std::string eol{"\n"};
        };

        inline bool is_open(TokenKind k)
        {
            return k == TokenKind::LBrace || k == TokenKind::LParen || k == TokenKind::LBracket;
        }

        inline bool is_close(TokenKind k)
        {
            return k == TokenKind::RBrace || k == TokenKind::RParen || k == TokenKind::RBracket;
        }

        inline std::optional<Layout> layout_of(Doc const& doc, std::size_t open)
        {
            auto const& t = doc.tokens;
            if (open >= t.size() || t[open].kind != TokenKind::LBrace)
                return std::nullopt;
            Layout l;
            l.open = open;
            int depth = 0;
            std::optional<std::size_t> start;
            std::size_t i = open;
            for (; i < t.size(); ++i)
            {
                auto k = t[i].kind;
                if (is_open(k))
                {
                    ++depth;
                    if (depth == 2 && !start)
                        start = i;
                    continue;
                }
                if (is_close(k))
                {
                    --depth;
                    if (depth == 0)
                        break;
                    continue;
                }
                if (depth == 1)
                {
                    if (k == TokenKind::Comma)
                    {
                        if (start)
                        {
                            l.elems.push_back({*start, i - 1, i});
                            start.reset();
                        }
                        continue;
                    }
                    if (!start)
                        start = i;
                }
                if (depth >= 2 && !start)
                    start = i;
            }
            if (i >= t.size())
                return std::nullopt;
            if (start)
                l.elems.push_back({*start, i - 1, std::nullopt});
            l.close = i;
            return l;
        }

        inline std::size_t line_start(Doc const& d, std::size_t pos)
        {
            while (pos > 0 && d.text[pos - 1] != '\n')
                --pos;
            return pos;
        }

        inline std::size_t line_end(Doc const& d, std::size_t pos)
        {
            while (pos < d.text.size() && d.text[pos] != '\n' && d.text[pos] != '\r')
                ++pos;
            return pos;
        }

        inline std::size_t eol_length(Doc const& d, std::size_t pos)
        {
            if (pos < d.text.size() && d.text[pos] == '\r' && pos + 1 < d.text.size() && d.text[pos + 1] == '\n')
                return 2;
            return pos < d.text.size() && (d.text[pos] == '\n' || d.text[pos] == '\r') ? 1 : 0;
        }

        inline bool own_line(Doc const& d, std::size_t pos)
        {
            for (auto i = line_start(d, pos); i < pos; ++i)
                if (d.text[i] != ' ' && d.text[i] != '\t')
                    return false;
            return true;
        }

        inline std::string indent_at(Doc const& d, std::size_t pos)
        {
            auto s = line_start(d, pos);
            auto e = s;
            while (e < d.text.size() && (d.text[e] == ' ' || d.text[e] == '\t'))
                ++e;
            return d.text.substr(s, e - s);
        }

        inline std::size_t elem_end(Doc const& d, Elem const& e)
        {
            return e.comma ? d.tokens[*e.comma].end : d.tokens[e.last].end;
        }

        inline bool trailing_clear(Doc const& d, Elem const& e)
        {
            auto next = (e.comma ? *e.comma : e.last) + 1;
            if (next >= d.tokens.size())
                return true;
            return d.tokens[next].begin >= line_end(d, elem_end(d, e));
        }

        inline bool elem_own_line(Doc const& d, Elem const& e)
        {
            return own_line(d, d.tokens[e.first].begin) && trailing_clear(d, e);
        }

        inline std::string apply(std::string text, std::vector<Splice> splices)
        {
            std::ranges::stable_sort(splices, [](Splice const& a, Splice const& b) { return a.pos > b.pos; });
            for (auto const& s : splices)
                text.replace(s.pos, s.len, s.text);
            return text;
        }

        inline std::string quote(std::string_view s)
        {
            std::string r = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                    r += '\\';
                r += c;
            }
            r += '"';
            return r;
        }

        inline std::string print_dependency(manifest::Dependency const& d)
        {
            std::string out = "{ location = " + quote(d.location);
            if (!d.version.empty() && !d.location.starts_with("path:"))
                out += ", version = " + quote(d.version);
            out += " }";
            return out;
        }

        struct Prepared
        {
            Doc doc;
            manifest::LoadResult loaded;
            std::size_t root_open{};
            Layout root;
        };

        inline std::variant<Prepared, EditResult> prepare(std::string_view text, manifest::LoadOptions const& options)
        {
            Prepared p;
            p.loaded = manifest::load(text, options);
            if (!p.loaded.ok || !p.loaded.project)
            {
                EditResult r;
                r.diagnostics = p.loaded.diagnostics;
                r.error = "the manifest has errors; fix them before editing";
                return r;
            }
            p.doc.text = std::string{text};
            if (text.find("\r\n") != std::string_view::npos)
                p.doc.eol = "\r\n";
            manifest::detail::Source src;
            src.text = p.doc.text;
            auto id = src.session.open_in_memory("file:///dpm-virtual/edit.dcon", src.text);
            auto const* file = src.session.source_manager().get(id);
            p.doc.tokens = manifest::detail::lex_tokens(src.session, *file);
            int depth = 0;
            std::size_t stmt_start = 0;
            for (std::size_t i = 0; i < p.doc.tokens.size(); ++i)
            {
                auto k = p.doc.tokens[i].kind;
                if (is_open(k))
                    ++depth;
                else if (is_close(k))
                    --depth;
                else if (k == TokenKind::Semicolon && depth == 0)
                    stmt_start = i + 1;
            }
            std::size_t open = stmt_start;
            while (open < p.doc.tokens.size() && p.doc.tokens[open].kind != TokenKind::LBrace)
                ++open;
            auto layout = layout_of(p.doc, open);
            if (!layout)
            {
                EditResult r;
                r.error = "cannot locate the Project literal";
                return r;
            }
            p.root_open = open;
            p.root = std::move(*layout);
            return p;
        }

        inline std::optional<std::size_t> find_field(Prepared const& p, Layout const& l, std::string_view name)
        {
            auto const& t = p.doc.tokens;
            for (std::size_t i = 0; i < l.elems.size(); ++i)
            {
                auto const& e = l.elems[i];
                if (e.last < e.first + 2 || t[e.first].kind != TokenKind::Identifier || t[e.first + 1].kind != TokenKind::Eq)
                    continue;
                if (std::string_view{p.doc.text}.substr(t[e.first].begin, t[e.first].end - t[e.first].begin) == name)
                    return i;
            }
            return std::nullopt;
        }

        inline std::vector<Splice> remove_element(Doc const& d, Layout const& l, std::size_t idx)
        {
            auto const& t = d.tokens;
            auto const& e = l.elems[idx];
            auto begin = t[e.first].begin;
            auto end = elem_end(d, e);
            bool const is_last = idx + 1 == l.elems.size();
            std::vector<Splice> out;
            if (elem_own_line(d, e))
            {
                auto s = line_start(d, begin);
                auto stop = line_end(d, end);
                stop += eol_length(d, stop);
                while (s > 0)
                {
                    auto prev_end = s - 1;
                    if (prev_end > 0 && d.text[prev_end - 1] == '\r')
                        --prev_end;
                    auto ps = line_start(d, s - 1);
                    auto line = std::string_view{d.text}.substr(ps, prev_end - ps);
                    auto first = line.find_first_not_of(" \t");
                    if (first == std::string_view::npos || !line.substr(first).starts_with("//"))
                        break;
                    s = ps;
                }
                out.push_back({s, stop - s, {}});
                if (!e.comma && idx > 0 && l.elems[idx - 1].comma)
                    out.push_back({t[*l.elems[idx - 1].comma].begin, 1, {}});
            }
            else if (!is_last)
                out.push_back({begin, t[l.elems[idx + 1].first].begin - begin, {}});
            else if (idx > 0)
            {
                auto const& prev = l.elems[idx - 1];
                auto from = e.comma && prev.comma ? t[*prev.comma].end : t[prev.last].end;
                out.push_back({from, end - from, {}});
            }
            else
                out.push_back({begin, end - begin, {}});
            return out;
        }

        inline std::vector<Splice> append_element(Prepared const& p, Layout const& l, std::string const& text, std::string const& indent_fallback)
        {
            auto const& d = p.doc;
            auto const& t = d.tokens;
            auto const& last = l.elems.back();
            std::vector<Splice> out;
            if (elem_own_line(d, last) && t[l.close].begin >= line_end(d, elem_end(d, last)))
            {
                auto indent = indent_at(d, t[last.first].begin);
                auto at = line_end(d, elem_end(d, last));
                out.push_back({at, 0, d.eol + indent + text + (last.comma ? "," : "")});
                if (!last.comma)
                    out.push_back({t[last.last].end, 0, ","});
                (void)indent_fallback;
                return out;
            }
            if (last.comma)
                out.push_back({t[*last.comma].end, 0, " " + text + ","});
            else
                out.push_back({t[last.last].end, 0, ", " + text});
            return out;
        }

        inline manifest::Node* mutable_field(manifest::Node& root, std::string_view name)
        {
            for (auto& [field, node] : root.fields)
                if (field == name)
                    return &node;
            return nullptr;
        }

        inline manifest::Node dependency_node(manifest::Dependency const& d)
        {
            manifest::Node n;
            n.kind = manifest::Node::Kind::Struct;
            manifest::Node location;
            location.kind = manifest::Node::Kind::String;
            location.text = d.location;
            manifest::Node version;
            version.kind = manifest::Node::Kind::String;
            version.text = d.version;
            n.fields.emplace_back("location", std::move(location));
            n.fields.emplace_back("version", std::move(version));
            return n;
        }

        inline EditResult finish(Prepared const& p, std::vector<Splice> splices, manifest::Node const& expected, manifest::LoadOptions const& options)
        {
            EditResult r;
            r.text = apply(p.doc.text, std::move(splices));
            auto reloaded = manifest::load(r.text, options);
            if (!reloaded.ok)
            {
                r.diagnostics = reloaded.diagnostics;
                r.error = "the edited manifest does not load; the file is unchanged";
                r.text.clear();
                return r;
            }
            if (reloaded.tree.dump(true) != expected.dump(true))
            {
                r.error = "the edited manifest does not match the intended change; the file is unchanged";
                r.text.clear();
                return r;
            }
            r.ok = true;
            return r;
        }

        inline std::vector<manifest::Dependency> const& list_of(manifest::Project const& project, List list)
        {
            return list == List::Dependencies ? project.dependencies : project.dev_dependencies;
        }

        inline EditResult fail(std::string message)
        {
            EditResult r;
            r.error = std::move(message);
            return r;
        }

        inline std::optional<std::size_t> index_of(manifest::Project const& project, List list, std::string_view location)
        {
            auto const& deps = list_of(project, list);
            for (std::size_t i = 0; i < deps.size(); ++i)
                if (deps[i].location == location)
                    return i;
            return std::nullopt;
        }

        inline std::optional<Layout> list_layout(Prepared const& p, std::size_t field_idx)
        {
            auto const& t = p.doc.tokens;
            auto const& f = p.root.elems[field_idx];
            auto value = f.first + 2;
            if (t[value].kind != TokenKind::LBrace)
                return std::nullopt;
            auto l = layout_of(p.doc, value);
            if (!l || l->close != f.last)
                return std::nullopt;
            return l;
        }
    } // namespace detail

    [[nodiscard]] inline EditResult add_dependency(std::string_view text, manifest::LoadOptions const& options, List list, manifest::Dependency const& dep)
    {
        auto prepared = detail::prepare(text, options);
        if (auto* failed = std::get_if<EditResult>(&prepared))
            return std::move(*failed);
        auto& p = std::get<detail::Prepared>(prepared);
        if (detail::index_of(*p.loaded.project, list, dep.location))
            return detail::fail(std::format("'{}' is already in {}", dep.location, field_name(list)));
        auto const& t = p.doc.tokens;
        auto element = detail::print_dependency(dep);
        auto field = detail::find_field(p, p.root, field_name(list));
        std::vector<detail::Splice> splices;
        if (field)
        {
            auto l = detail::list_layout(p, *field);
            if (!l)
                return detail::fail(std::format("{} is not a plain list literal; edit it by hand", field_name(list)));
            if (l->elems.empty())
            {
                auto const& f = p.root.elems[*field];
                bool const multiline =
                    detail::own_line(p.doc, t[f.first].begin) && t[p.root.close].begin >= detail::line_end(p.doc, detail::elem_end(p.doc, f));
                std::string replacement;
                if (multiline)
                {
                    auto indent = detail::indent_at(p.doc, t[f.first].begin);
                    std::string unit = indent.find('\t') != std::string::npos ? "\t" : "    ";
                    replacement = "{" + p.doc.eol + indent + unit + element + "," + p.doc.eol + indent + "}";
                }
                else
                    replacement = "{ " + element + " }";
                splices.push_back({t[l->open].begin, t[l->close].end - t[l->open].begin, replacement});
            }
            else
                splices = detail::append_element(p, *l, element, {});
        }
        else
        {
            auto field_text = std::string{field_name(list)} + " = { " + element + " }";
            splices = detail::append_element(p, p.root, field_text, {});
        }
        manifest::Node expected = p.loaded.tree;
        if (auto* node = detail::mutable_field(expected, field_name(list)))
            node->items.push_back(detail::dependency_node(dep));
        return detail::finish(p, std::move(splices), expected, options);
    }

    [[nodiscard]] inline EditResult remove_dependency(std::string_view text, manifest::LoadOptions const& options, List list, std::string_view location)
    {
        auto prepared = detail::prepare(text, options);
        if (auto* failed = std::get_if<EditResult>(&prepared))
            return std::move(*failed);
        auto& p = std::get<detail::Prepared>(prepared);
        auto index = detail::index_of(*p.loaded.project, list, location);
        if (!index)
            return detail::fail(std::format("'{}' is not in {}", location, field_name(list)));
        auto field = detail::find_field(p, p.root, field_name(list));
        if (!field)
            return detail::fail(std::format("{} is not written in the file", field_name(list)));
        auto l = detail::list_layout(p, *field);
        if (!l || l->elems.size() != detail::list_of(*p.loaded.project, list).size())
            return detail::fail(std::format("{} is not a plain list literal; edit it by hand", field_name(list)));
        std::vector<detail::Splice> splices;
        if (l->elems.size() == 1)
            splices = detail::remove_element(p.doc, p.root, *field);
        else
            splices = detail::remove_element(p.doc, *l, *index);
        manifest::Node expected = p.loaded.tree;
        if (auto* node = detail::mutable_field(expected, field_name(list)))
            node->items.erase(node->items.begin() + static_cast<std::ptrdiff_t>(*index));
        return detail::finish(p, std::move(splices), expected, options);
    }

    [[nodiscard]] inline EditResult set_dependency_version(std::string_view text, manifest::LoadOptions const& options, List list, std::string_view location,
                                                           std::string_view version)
    {
        auto prepared = detail::prepare(text, options);
        if (auto* failed = std::get_if<EditResult>(&prepared))
            return std::move(*failed);
        auto& p = std::get<detail::Prepared>(prepared);
        auto index = detail::index_of(*p.loaded.project, list, location);
        if (!index)
            return detail::fail(std::format("'{}' is not in {}", location, field_name(list)));
        auto field = detail::find_field(p, p.root, field_name(list));
        if (!field)
            return detail::fail(std::format("{} is not written in the file", field_name(list)));
        auto l = detail::list_layout(p, *field);
        if (!l || l->elems.size() != detail::list_of(*p.loaded.project, list).size())
            return detail::fail(std::format("{} is not a plain list literal; edit it by hand", field_name(list)));
        auto const& t = p.doc.tokens;
        auto const& e = l->elems[*index];
        auto inner = detail::layout_of(p.doc, e.first);
        if (!inner || inner->close != e.last)
            return detail::fail("the dependency is not a literal; edit it by hand");
        auto version_field = detail::find_field(p, *inner, "version");
        if (!version_field)
            return detail::fail("the dependency has no version field");
        auto const& vf = inner->elems[*version_field];
        if (vf.last != vf.first + 2 || t[vf.last].kind != detail::TokenKind::StringLiteral)
            return detail::fail("the version is not a string literal; edit it by hand");
        std::vector<detail::Splice> splices{{t[vf.last].begin, t[vf.last].end - t[vf.last].begin, detail::quote(version)}};
        manifest::Node expected = p.loaded.tree;
        if (auto* node = detail::mutable_field(expected, field_name(list)))
            if (auto* item_version = detail::mutable_field(node->items[*index], "version"))
                item_version->text = std::string{version};
        return detail::finish(p, std::move(splices), expected, options);
    }

    [[nodiscard]] inline bool write_atomic(std::filesystem::path const& path, std::string_view content, std::string& error)
    {
        auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
        auto temp = dir / (path.filename().string() + ".dpm-tmp");
#if !defined(_WIN32)
        struct stat st{};
        bool const have_mode = ::stat(path.c_str(), &st) == 0;
        int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, have_mode ? st.st_mode & 07777 : 0644);
        if (fd < 0)
        {
            error = "cannot create " + temp.string();
            return false;
        }
        std::size_t done = 0;
        while (done < content.size())
        {
            auto n = ::write(fd, content.data() + done, content.size() - done);
            if (n <= 0)
            {
                ::close(fd);
                std::filesystem::remove(temp);
                error = "cannot write " + temp.string();
                return false;
            }
            done += static_cast<std::size_t>(n);
        }
        if (have_mode)
            ::fchmod(fd, st.st_mode & 07777);
        if (::fsync(fd) != 0)
        {
            ::close(fd);
            std::filesystem::remove(temp);
            error = "cannot sync " + temp.string();
            return false;
        }
        ::close(fd);
#else
        {
            std::ofstream out{temp, std::ios::binary | std::ios::trunc};
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!out)
            {
                error = "cannot write " + temp.string();
                return false;
            }
        }
#endif
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            std::filesystem::remove(temp, ec);
            error = "cannot replace " + path.string();
            return false;
        }
        return true;
    }
} // namespace dpm::edit
