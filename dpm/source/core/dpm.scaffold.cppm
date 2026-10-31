export module dpm.scaffold;

import std;
import dpm.extract;
import dpm.manifest;

export namespace dpm::scaffold
{
    using Variables = std::map<std::string, std::string, std::less<>>;

    struct File
    {
        std::string path;
        std::string content;
        bool executable{};
        bool substitute{true};
        bool directory{};
    };

    struct Problem
    {
        std::string path;
        std::size_t line{};
        std::string message;
    };

    struct Rendered
    {
        std::vector<File> files;
        std::vector<Problem> problems;

        [[nodiscard]] bool ok() const { return problems.empty(); }
    };

    namespace detail
    {
        [[nodiscard]] inline bool is_name_start(char c) { return c >= 'a' && c <= 'z'; }

        [[nodiscard]] inline bool is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9') || c == '_'; }

        [[nodiscard]] inline std::size_t line_of(std::string_view text, std::size_t offset)
        {
            return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
        }
    } // namespace detail

    [[nodiscard]] inline std::optional<std::string> path_problem(std::string_view path)
    {
        if (auto bad = extract::member_name_problem(path))
            return bad;
        std::size_t pos = 0;
        while (pos <= path.size())
        {
            auto slash = path.find('/', pos);
            auto part = path.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
            if (part.empty())
                return "empty path component";
            if (part == ".")
                return "path contains a '.' component";
            if (part.size() > 255)
                return "path component longer than 255 bytes";
            if (part.find(':') != std::string_view::npos)
                return "path component contains ':'";
            if (slash == std::string_view::npos)
                break;
            pos = slash + 1;
        }
        return std::nullopt;
    }

    struct Expansion
    {
        std::string text;
        std::optional<Problem> problem;
    };

    [[nodiscard]] inline Expansion expand(std::string_view text, Variables const& variables)
    {
        Expansion out;
        out.text.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size())
        {
            if (text[i] != '@' || i + 1 >= text.size() || text[i + 1] != '@')
            {
                out.text += text[i++];
                continue;
            }
            if (text.substr(i, 4) == "@@@@")
            {
                out.text += "@@";
                i += 4;
                continue;
            }
            std::size_t j = i + 2;
            if (j < text.size() && detail::is_name_start(text[j]))
            {
                std::size_t k = j + 1;
                while (k < text.size() && detail::is_name_char(text[k]))
                    ++k;
                if (text.substr(k, 2) == "@@")
                {
                    auto name = text.substr(j, k - j);
                    auto it = variables.find(name);
                    if (it == variables.end())
                    {
                        out.problem = Problem{"", detail::line_of(text, i), std::format("unknown variable '{}'", name)};
                        return out;
                    }
                    out.text += it->second;
                    i = k + 2;
                    continue;
                }
            }
            out.text += text[i++];
        }
        return out;
    }

    [[nodiscard]] inline Rendered render(std::span<File const> files, Variables const& variables)
    {
        Rendered out;
        std::set<std::string> seen;
        for (auto const& file : files)
        {
            auto path = expand(file.path, variables);
            if (path.problem)
            {
                out.problems.push_back({file.path, 0, path.problem->message});
                continue;
            }
            if (auto bad = path_problem(path.text))
            {
                out.problems.push_back({path.text, 0, *bad});
                continue;
            }
            if (!seen.insert(path.text).second)
            {
                out.problems.push_back({path.text, 0, "generated twice"});
                continue;
            }
            File rendered;
            rendered.path = std::move(path.text);
            rendered.executable = file.executable;
            rendered.substitute = file.substitute;
            rendered.directory = file.directory;
            if (file.substitute)
            {
                auto body = expand(file.content, variables);
                if (body.problem)
                {
                    out.problems.push_back({rendered.path, body.problem->line, body.problem->message});
                    continue;
                }
                rendered.content = std::move(body.text);
            }
            else
                rendered.content = file.content;
            out.files.push_back(std::move(rendered));
        }
        return out;
    }


    struct Resolved
    {
        Variables files;
        Variables steps;
        std::vector<Problem> problems;

        [[nodiscard]] bool ok() const { return problems.empty(); }
    };

    [[nodiscard]] inline Resolved resolve(manifest::ProjectTemplate const& tmpl, Variables const& given, std::string_view dir)
    {
        Resolved out;
        manifest::TemplateVariable name_variable;
        name_variable.kind = manifest::VariableKind::Identifier;
        name_variable.name = "name";
        std::set<std::string> declared{"name"};
        for (auto const& v : tmpl.variables)
            declared.insert(v.name);
        for (auto const& [key, value] : given)
        {
            if (key == "dir")
                out.problems.push_back({"", 0, "variable 'dir' is built in and cannot be set"});
            else if (!declared.contains(key))
                out.problems.push_back({"", 0, std::format("unknown variable '{}'", key)});
        }
        if (auto it = given.find("name"); it == given.end())
            out.problems.push_back({"", 0, "variable 'name' is required"});
        else if (auto bad = manifest::template_value_error(name_variable, it->second))
            out.problems.push_back({"", 0, std::format("variable 'name': {}", *bad)});
        else
            out.files["name"] = it->second;
        for (auto const& v : tmpl.variables)
        {
            auto it = given.find(v.name);
            if (it == given.end())
            {
                if (v.required)
                    out.problems.push_back({"", 0, v.description.empty() ? std::format("variable '{}' is required", v.name)
                                                                         : std::format("variable '{}' is required: {}", v.name, v.description)});
                else
                    out.files[v.name] = v.default_value;
                continue;
            }
            if (auto bad = manifest::template_value_error(v, it->second))
                out.problems.push_back({"", 0, std::format("variable '{}': {}", v.name, *bad)});
            else
                out.files[v.name] = it->second;
        }
        out.steps = out.files;
        out.steps["dir"] = std::string{dir};
        return out;
    }

    [[nodiscard]] inline Variables sample_values(manifest::ProjectTemplate const& tmpl)
    {
        Variables values{{"name", "sample"}};
        for (auto const& v : tmpl.variables)
        {
            if (!v.required)
                continue;
            switch (v.kind)
            {
                case manifest::VariableKind::String:
                case manifest::VariableKind::Identifier:
                    values[v.name] = "sample";
                    break;
                case manifest::VariableKind::Integer:
                    values[v.name] = std::to_string(v.min);
                    break;
                case manifest::VariableKind::Bool:
                    values[v.name] = "true";
                    break;
                case manifest::VariableKind::Choice:
                    values[v.name] = v.choices.empty() ? "" : v.choices.front();
                    break;
            }
        }
        return values;
    }

    namespace detail
    {
        [[nodiscard]] inline std::size_t char_length(std::string_view text)
        {
            if (text.empty())
                return 0;
            auto c = static_cast<unsigned char>(text[0]);
            std::size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 1;
            return std::min(n, text.size());
        }

        [[nodiscard]] inline bool segment_match(std::string_view pattern, std::string_view text)
        {
            std::size_t p = 0;
            std::size_t t = 0;
            std::size_t star = std::string_view::npos;
            std::size_t mark = 0;
            while (t < text.size())
            {
                if (p < pattern.size() && pattern[p] == '*')
                {
                    star = p++;
                    mark = t;
                }
                else if (p < pattern.size() && pattern[p] == '?')
                {
                    t += char_length(text.substr(t));
                    ++p;
                }
                else if (p < pattern.size() && pattern[p] == text[t])
                {
                    ++p;
                    ++t;
                }
                else if (star != std::string_view::npos)
                {
                    p = star + 1;
                    t = ++mark;
                }
                else
                    return false;
            }
            while (p < pattern.size() && pattern[p] == '*')
                ++p;
            return p == pattern.size();
        }

        [[nodiscard]] inline std::vector<std::string_view> segments(std::string_view path)
        {
            std::vector<std::string_view> out;
            std::size_t pos = 0;
            while (pos <= path.size())
            {
                auto slash = path.find('/', pos);
                out.push_back(path.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos));
                if (slash == std::string_view::npos)
                    break;
                pos = slash + 1;
            }
            return out;
        }

        [[nodiscard]] inline bool match_from(std::span<std::string_view const> pattern, std::span<std::string_view const> path, bool below)
        {
            if (pattern.empty())
                return below ? !path.empty() : path.empty();
            if (pattern.front() == "**")
            {
                for (std::size_t skip = 0; skip <= path.size(); ++skip)
                    if (match_from(pattern.subspan(1), path.subspan(skip), below))
                        return true;
                return false;
            }
            if (path.empty() || !segment_match(pattern.front(), path.front()))
                return false;
            return match_from(pattern.subspan(1), path.subspan(1), below);
        }
    } // namespace detail

    [[nodiscard]] inline bool glob_match(std::string_view pattern, std::string_view path)
    {
        bool const below = pattern.ends_with('/');
        if (below)
            pattern.remove_suffix(1);
        auto p = detail::segments(pattern);
        auto s = detail::segments(path);
        return detail::match_from(p, s, below);
    }

    [[nodiscard]] inline bool glob_match_any(std::span<std::string const> patterns, std::string_view path)
    {
        return std::ranges::any_of(patterns, [&](std::string const& pattern) { return glob_match(pattern, path); });
    }

    inline constexpr std::size_t k_max_substituted_file = 1U << 20;

    struct Generated
    {
        Rendered rendered;
        std::string next_steps;
    };

    [[nodiscard]] inline Generated generate(manifest::ProjectTemplate const& tmpl, std::span<File const> tree, Variables const& given, std::string_view dir)
    {
        Generated out;
        auto resolved = resolve(tmpl, given, dir);
        if (!resolved.ok())
        {
            out.rendered.problems = std::move(resolved.problems);
            return out;
        }
        std::vector<File const*> ordered;
        for (auto const& file : tree)
            ordered.push_back(&file);
        std::ranges::sort(ordered, [](File const* a, File const* b) { return a->path < b->path; });
        std::vector<File> selected;
        std::vector<Problem> early;
        for (auto const* file : ordered)
        {
            if (glob_match_any(tmpl.exclude, file->path))
                continue;
            File copy = *file;
            copy.substitute = !file->directory && glob_match_any(tmpl.substitute, file->path);
            if (copy.substitute)
            {
                if (file->content.size() > k_max_substituted_file)
                {
                    early.push_back({file->path, 0, "a file with substitution cannot be larger than 1 MiB"});
                    continue;
                }
                if (file->content.find('\0') != std::string::npos || !manifest::detail::valid_utf8(file->content))
                {
                    early.push_back({file->path, 0, "a file with substitution must be UTF-8 text without NUL bytes"});
                    continue;
                }
            }
            selected.push_back(std::move(copy));
        }
        out.rendered = render(selected, resolved.files);
        for (auto& problem : early)
            out.rendered.problems.push_back(std::move(problem));
        std::vector<extract::Member> members;
        for (auto const& file : out.rendered.files)
        {
            extract::Member member;
            member.path = file.path;
            member.type = file.directory ? extract::MemberType::Directory : extract::MemberType::File;
            member.size = file.content.size();
            member.exec = file.executable;
            members.push_back(std::move(member));
        }
        for (auto& problem : extract::check_members(members))
            out.rendered.problems.push_back({problem.path, 0, problem.message});
        auto steps = expand(tmpl.next_steps, resolved.steps);
        if (steps.problem)
            out.rendered.problems.push_back({"next_steps", steps.problem->line, steps.problem->message});
        else
            out.next_steps = std::move(steps.text);
        return out;
    }

    [[nodiscard]] inline std::vector<File> builtin_executable()
    {
        return {
            {"project.dcon",
             "dpm::v1::Project {\n    name = \"@@name@@\",\n    version = \"0.1.0\",\n    products = {\n        { name = \"@@name@@\", entry = \"src/main.dc\" },\n    },\n}\n"},
            {"src/main.dc", "module main;\n\npublic i32 main()\n{\n    return 0;\n}\n"},
            {".gitignore", ".dpm/\n"},
        };
    }

    [[nodiscard]] inline std::vector<File> builtin_library()
    {
        return {
            {"project.dcon", "dpm::v1::Project {\n    name = \"@@name@@\",\n    version = \"0.1.0\",\n}\n"},
            {"src/@@name@@/lib.dc", "module @@name@@::lib;\n\npublic i32 answer()\n{\n    return 42;\n}\n"},
            {".gitignore", ".dpm/\n"},
        };
    }

    struct BuiltIn
    {
        manifest::ProjectTemplate tmpl;
        std::vector<File> tree;
    };

    [[nodiscard]] inline std::optional<BuiltIn> builtin_template(std::string_view name)
    {
        BuiltIn out;
        out.tmpl.root = "files";
        out.tmpl.substitute = {"**"};
        if (name == "executable")
        {
            out.tmpl.name = "executable";
            out.tmpl.description = "A program with one product and src/main.dc";
            out.tree = builtin_executable();
            out.tmpl.next_steps = "dpm run";
        }
        else if (name == "library")
        {
            out.tmpl.name = "library";
            out.tmpl.description = "A source library with src/<name>/lib.dc";
            out.tree = builtin_library();
            out.tmpl.next_steps = "dpm build";
        }
        else
            return std::nullopt;
        return out;
    }
} // namespace dpm::scaffold
