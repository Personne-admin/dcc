export module dpm.scaffold;

import std;
import dpm.extract;

export namespace dpm::scaffold
{
    using Variables = std::map<std::string, std::string, std::less<>>;

    struct File
    {
        std::string path;
        std::string content;
        bool executable{};
        bool substitute{true};
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
            if (auto bad = extract::member_name_problem(path.text))
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
} // namespace dpm::scaffold
