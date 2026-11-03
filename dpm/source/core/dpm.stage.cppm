module;

#include <unistd.h>

export module dpm.stage;

import std;
import dpm.extract;
import dpm.manifest;
import dpm.scaffold;

export namespace dpm::stage
{
    namespace fs = std::filesystem;

    inline constexpr std::string_view k_template_file = "template.dcon";

    struct Loaded
    {
        manifest::ProjectTemplate tmpl;
        std::vector<scaffold::File> tree;
        std::vector<std::string> problems;
        std::vector<std::string> warnings;

        [[nodiscard]] bool ok() const { return problems.empty(); }
    };

    namespace detail
    {
        [[nodiscard]] inline std::optional<std::string> read_small(fs::path const& path, std::uintmax_t limit)
        {
            std::error_code ec;
            auto size = fs::file_size(path, ec);
            if (ec || size > limit)
                return std::nullopt;
            std::ifstream in{path, std::ios::binary};
            if (!in)
                return std::nullopt;
            std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            return text;
        }

        inline void read_tree(fs::path const& root, Loaded& out)
        {
            extract::Limits limits;
            std::vector<std::pair<fs::path, std::string>> stack{{root, ""}};
            std::uint64_t bytes = 0;
            std::size_t files = 0;
            std::vector<extract::Member> members;
            while (!stack.empty())
            {
                auto [dir, prefix] = std::move(stack.back());
                stack.pop_back();
                std::error_code ec;
                fs::directory_iterator it{dir, ec};
                if (ec)
                {
                    out.problems.push_back(std::format("cannot list {}", prefix.empty() ? std::string{"the template root"} : prefix));
                    continue;
                }
                std::vector<fs::directory_entry> entries(fs::begin(it), fs::end(it));
                std::ranges::sort(entries, [](auto const& a, auto const& b) { return a.path().filename().string() < b.path().filename().string(); });
                for (auto const& entry : entries)
                {
                    auto leaf = entry.path().filename().string();
                    if (prefix.empty() && leaf == ".git")
                        continue;
                    auto rel = prefix.empty() ? leaf : prefix + "/" + leaf;
                    auto status = entry.symlink_status(ec);
                    if (ec)
                    {
                        out.problems.push_back(std::format("{}: cannot stat", rel));
                        continue;
                    }
                    if (fs::is_symlink(status))
                    {
                        out.problems.push_back(std::format("{}: symbolic links are not allowed in a template", rel));
                        continue;
                    }
                    if (fs::is_directory(status))
                    {
                        scaffold::File dirent;
                        dirent.path = rel;
                        dirent.directory = true;
                        dirent.substitute = false;
                        out.tree.push_back(std::move(dirent));
                        members.push_back({rel, extract::MemberType::Directory, 0, false, {}});
                        stack.emplace_back(entry.path(), rel);
                    }
                    else if (fs::is_regular_file(status))
                    {
                        auto size = entry.file_size(ec);
                        if (ec)
                        {
                            out.problems.push_back(std::format("{}: cannot read", rel));
                            continue;
                        }
                        bytes += size;
                        ++files;
                        if (files > limits.max_files || bytes > limits.max_bytes)
                        {
                            out.problems.push_back("the template has more than 20000 files or 256 MiB");
                            return;
                        }
                        auto content = read_small(entry.path(), limits.max_bytes);
                        if (!content)
                        {
                            out.problems.push_back(std::format("{}: cannot read", rel));
                            continue;
                        }
                        scaffold::File file;
                        file.path = rel;
                        file.content = std::move(*content);
                        file.executable = (status.permissions() & fs::perms::owner_exec) != fs::perms::none;
                        file.substitute = false;
                        out.tree.push_back(std::move(file));
                        members.push_back({rel, extract::MemberType::File, size, false, {}});
                    }
                    else
                        out.problems.push_back(std::format("{}: only regular files and directories are allowed in a template", rel));
                }
            }
            for (auto const& problem : extract::check_members(members))
                out.problems.push_back(problem.format());
        }
    } // namespace detail

    [[nodiscard]] inline Loaded load_template(fs::path const& dir, manifest::LoadOptions options)
    {
        Loaded out;
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
        {
            out.problems.push_back(std::format("{} is not a directory", dir.string()));
            return out;
        }
        auto file = dir / k_template_file;
        auto status = fs::symlink_status(file, ec);
        if (ec || !fs::is_regular_file(status))
        {
            out.problems.push_back(std::format("{} has no {}", dir.string(), k_template_file));
            return out;
        }
        auto text = detail::read_small(file, manifest::k_max_file_size);
        if (!text)
        {
            out.problems.push_back(std::format("cannot read {}", file.string()));
            return out;
        }
        options.file_name = std::string{k_template_file};
        options.kind = manifest::FileKind::Template;
        auto loaded = manifest::load(*text, options);
        if (!loaded.ok || !loaded.project_template)
        {
            for (auto const& d : loaded.diagnostics)
                out.problems.push_back(d.format());
            if (out.problems.empty())
                out.problems.push_back("template.dcon could not be loaded");
            return out;
        }
        out.tmpl = std::move(*loaded.project_template);
        auto root = dir / out.tmpl.root;
        auto root_status = fs::symlink_status(root, ec);
        if (ec || !fs::is_directory(root_status))
        {
            out.problems.push_back(std::format("the template root '{}' is not a directory", out.tmpl.root));
            return out;
        }
        detail::read_tree(root, out);
        return out;
    }

    struct Checked
    {
        std::vector<std::string> problems;
        std::vector<std::string> warnings;
        std::size_t files{};
        std::vector<manifest::Task> tasks;
        std::vector<manifest::Asset> assets;

        [[nodiscard]] bool ok() const { return problems.empty(); }
    };

    [[nodiscard]] inline std::optional<std::string> find_generated(scaffold::Rendered const& rendered, std::string_view path)
    {
        for (auto const& file : rendered.files)
            if (file.path == path)
                return file.content;
        return std::nullopt;
    }

    [[nodiscard]] inline Checked check_generated(scaffold::Generated const& generated, manifest::LoadOptions options)
    {
        Checked out;
        for (auto const& p : generated.rendered.problems)
        {
            if (p.path.empty())
                out.problems.push_back(p.message);
            else if (p.line != 0)
                out.problems.push_back(std::format("{}:{}: {}", p.path, p.line, p.message));
            else
                out.problems.push_back(std::format("{}: {}", p.path, p.message));
        }
        for (auto const& file : generated.rendered.files)
        {
            if (!file.directory)
                ++out.files;
            for (auto part = std::string_view{file.path}; !part.empty();)
            {
                auto slash = part.find('/');
                if (part.substr(0, slash) == ".git")
                {
                    out.problems.push_back(std::format("{}: a template cannot generate .git", file.path));
                    break;
                }
                if (slash == std::string_view::npos)
                    break;
                part.remove_prefix(slash + 1);
            }
        }
        if (!out.problems.empty())
            return out;
        if (auto text = find_generated(generated.rendered, "project.dcon"))
        {
            options.file_name = "project.dcon";
            options.kind = manifest::FileKind::Project;
            auto loaded = manifest::load(*text, options);
            if (!loaded.ok)
            {
                for (auto const& d : loaded.diagnostics)
                    out.problems.push_back("generated " + d.format());
                if (loaded.diagnostics.empty())
                    out.problems.push_back("the generated project.dcon is not valid");
            }
            else if (loaded.project)
            {
                out.tasks = loaded.project->tasks;
                out.assets = loaded.project->assets;
            }
        }
        return out;
    }

    [[nodiscard]] inline Checked check_template(Loaded const& loaded, manifest::LoadOptions options)
    {
        Checked out;
        if (!loaded.ok())
        {
            out.problems = loaded.problems;
            return out;
        }
        auto values = scaffold::sample_values(loaded.tmpl);
        auto generated = scaffold::generate(loaded.tmpl, loaded.tree, values, "DIR");
        auto checked = check_generated(generated, options);
        checked.warnings = loaded.warnings;
        std::set<std::string> directories;
        for (auto const& file : loaded.tree)
            if (file.directory)
                directories.insert(file.path);
        for (auto const* list : {&loaded.tmpl.substitute, &loaded.tmpl.exclude})
            for (auto const& pattern : *list)
            {
                bool any = std::ranges::any_of(loaded.tree, [&](scaffold::File const& file) { return scaffold::glob_match(pattern, file.path); });
                if (!any)
                    checked.warnings.push_back(std::format("the pattern '{}' matches no file", pattern));
            }
        for (auto const& v : loaded.tmpl.variables)
        {
            bool used = false;
            std::string needle = "@@" + v.name + "@@";
            for (auto const& file : loaded.tree)
                if (file.path.find(needle) != std::string::npos || (!file.directory && file.content.find(needle) != std::string::npos))
                    used = true;
            if (loaded.tmpl.next_steps.find(needle) != std::string::npos)
                used = true;
            if (!used)
                checked.warnings.push_back(std::format("the variable '{}' is not used by any file", v.name));
        }
        return checked;
    }

    struct Written
    {
        bool ok{};
        std::string error;
    };

    [[nodiscard]] inline Written write_new(fs::path const& target, std::span<scaffold::File const> files)
    {
        Written out;
        std::error_code ec;
        auto dir = fs::absolute(target, ec).lexically_normal();
        if (dir.filename().empty())
            dir = dir.parent_path();
        bool const exists = fs::exists(fs::symlink_status(dir, ec));
        if (exists)
        {
            if (!fs::is_directory(fs::symlink_status(dir, ec)))
            {
                out.error = std::format("{} exists and is not a directory", dir.string());
                return out;
            }
            for (auto const& entry : fs::directory_iterator(dir, ec))
                if (entry.path().filename() != ".git")
                {
                    out.error = std::format("{} is not empty", dir.string());
                    return out;
                }
        }
        else
        {
            fs::create_directories(dir.parent_path(), ec);
            if (ec)
            {
                out.error = std::format("cannot create {}: {}", dir.parent_path().string(), ec.message());
                return out;
            }
        }
        static std::atomic<int> counter{0};
        auto stage_name = std::format(".dpm-new-{}-{}", ::getpid(), ++counter);
        auto stage = (exists ? dir : dir.parent_path()) / stage_name;
        fs::create_directory(stage, ec);
        if (ec)
        {
            out.error = std::format("cannot create {}: {}", stage.string(), ec.message());
            return out;
        }
        auto fail = [&](std::string message) {
            std::error_code ignore;
            fs::remove_all(stage, ignore);
            out.error = std::move(message);
            return out;
        };
        for (auto const& file : files)
        {
            auto path = stage / file.path;
            if (file.directory)
            {
                fs::create_directories(path, ec);
                if (ec)
                    return fail(std::format("cannot create {}: {}", file.path, ec.message()));
                continue;
            }
            fs::create_directories(path.parent_path(), ec);
            if (ec)
                return fail(std::format("cannot create {}: {}", path.parent_path().string(), ec.message()));
            {
                std::ofstream f{path, std::ios::binary | std::ios::trunc};
                if (!f)
                    return fail(std::format("cannot write {}", file.path));
                f.write(file.content.data(), static_cast<std::streamsize>(file.content.size()));
                if (!f)
                    return fail(std::format("cannot write {}", file.path));
            }
            fs::permissions(path, file.executable ? fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec
                                                  : fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
                            ec);
            if (ec)
                return fail(std::format("cannot set the mode of {}: {}", file.path, ec.message()));
        }
        if (!exists)
        {
            fs::rename(stage, dir, ec);
            if (ec)
                return fail(std::format("cannot create {}: {}", dir.string(), ec.message()));
            out.ok = true;
            return out;
        }
        std::vector<fs::path> moved;
        std::vector<fs::path> staged;
        for (auto const& entry : fs::directory_iterator(stage, ec))
            staged.push_back(entry.path());
        std::ranges::sort(staged);
        for (auto const& source : staged)
        {
            auto destination = dir / source.filename();
            std::error_code rename_ec;
            fs::rename(source, destination, rename_ec);
            if (rename_ec)
            {
                std::error_code ignore;
                for (auto const& done : moved)
                    fs::remove_all(done, ignore);
                return fail(std::format("cannot create {}: {}", destination.string(), rename_ec.message()));
            }
            moved.push_back(destination);
        }
        std::error_code ignore;
        fs::remove_all(stage, ignore);
        out.ok = true;
        return out;
    }
} // namespace dpm::stage
