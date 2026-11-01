export module dpm.publish;

import std;
import dpm.cache;
import dpm.edit;
import dpm.fetch;
import dpm.git;
import dpm.hash;
import dpm.lock;
import dpm.manifest;
import dpm.process;
import dpm.registry;
import dpm.semver;
import dpm.stage;

export namespace dpm::publish
{
    struct Problem
    {
        std::string path;
        std::string message;

        [[nodiscard]] std::string format() const { return path.empty() ? message : path + ": " + message; }
    };

    struct Context
    {
        std::filesystem::path schema_root;
        bool allow_file{};
    };

    struct Written
    {
        bool changed{};
        std::string alias_rel;
        std::optional<std::string> pointer_rel;
        std::vector<std::string> files;
    };

    inline constexpr std::size_t k_max_registry_files = 200000;

    namespace detail
    {
        inline manifest::LoadOptions options_for(Context const& ctx, manifest::FileKind kind, std::string name)
        {
            manifest::LoadOptions options;
            options.file_name = std::move(name);
            options.schema_root = ctx.schema_root;
            options.kind = kind;
            options.allow_file_urls = ctx.allow_file;
            return options;
        }

        inline std::string first_diagnostic(manifest::LoadResult const& loaded)
        {
            return loaded.diagnostics.empty() ? "invalid file" : loaded.diagnostics.front().format();
        }

        inline std::vector<std::string> sorted_names(std::filesystem::path const& dir)
        {
            std::vector<std::string> names;
            std::error_code ec;
            for (auto const& item : std::filesystem::directory_iterator{dir, ec})
                names.push_back(item.path().filename().string());
            std::ranges::sort(names);
            return names;
        }

        inline bool is_plain_file(std::filesystem::path const& path)
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec));
        }

        inline bool is_plain_dir(std::filesystem::path const& path)
        {
            std::error_code ec;
            return std::filesystem::is_directory(std::filesystem::symlink_status(path, ec));
        }

        inline std::string lower(std::string s)
        {
            for (auto& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        inline std::string shell_quote(std::string_view s)
        {
            std::string out = "'";
            for (char c : s)
            {
                if (c == '\'')
                    out += "'\\''";
                else
                    out += c;
            }
            return out + "'";
        }

        inline std::vector<std::string> tag_candidates(std::string const& subdir, std::string const& version)
        {
            std::vector<std::string> tags;
            if (!subdir.empty())
            {
                tags.push_back(subdir + "/v" + version);
                tags.push_back(subdir + "/" + version);
            }
            tags.push_back("v" + version);
            tags.push_back(version);
            return tags;
        }

        struct GitOut
        {
            bool ok{};
            std::string out;
            std::string err;
        };

        inline GitOut run_git(std::string const& program, std::filesystem::path const& cwd, std::vector<std::string> args)
        {
            process::Command cmd;
            cmd.argv = {program, "-c", "core.fsmonitor=false", "-c", "core.hooksPath=/dev/null"};
            for (auto& a : args)
                cmd.argv.push_back(std::move(a));
            cmd.cwd = cwd;
            cmd.env = fetch::detail::base_env();
            for (char const* name : {"HOME", "GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM", "XDG_CONFIG_HOME"})
                if (char const* v = std::getenv(name))
                    cmd.env[name] = v;
            cmd.env["GIT_TERMINAL_PROMPT"] = "0";
            cmd.timeout = std::chrono::seconds(60);
            auto r = process::run(cmd);
            return {r.ok(), fetch::detail::trim_newlines(r.out), fetch::detail::last_line(r.err)};
        }

        inline bool same_entry(manifest::RegistryVersion const& a, manifest::RegistryVersion const& b)
        {
            if (a.version != b.version || a.source != b.source || a.commit != b.commit || a.hash != b.hash || a.requires_dcc != b.requires_dcc ||
                a.dependencies.size() != b.dependencies.size())
                return false;
            for (std::size_t i = 0; i < a.dependencies.size(); ++i)
                if (a.dependencies[i].location != b.dependencies[i].location || a.dependencies[i].version != b.dependencies[i].version)
                    return false;
            return true;
        }

        inline bool commit_files(std::filesystem::path const& dir, std::vector<std::pair<std::string, std::string>> const& files, std::string& error)
        {
            for (auto const& [rel, text] : files)
            {
                std::error_code ec;
                std::filesystem::create_directories((dir / rel).parent_path(), ec);
                if (!edit::write_atomic(dir / rel, text, error))
                    return false;
            }
            return true;
        }
    } // namespace detail

    [[nodiscard]] inline std::string print_alias(manifest::RegistryAlias const& a)
    {
        auto q = [](std::string_view s) { return lock::detail::quote(s); };
        std::string out = "dpm::v1::RegistryAlias {\n";
        out += std::format("    alias = {},\n", q(a.alias));
        out += std::format("    location = {},\n", q(a.location));
        if (!a.description.empty())
            out += std::format("    description = {},\n", q(a.description));
        if (a.versions.empty())
            return out + "    versions = {},\n}\n";
        out += "    versions = {\n";
        for (auto const& v : a.versions)
        {
            out += "        {\n";
            out += std::format("            version = {},\n", q(v.version));
            out += std::format("            source = {},\n", q(v.source));
            if (!v.commit.empty())
                out += std::format("            commit = {},\n", q(v.commit));
            out += std::format("            hash = {},\n", q(v.hash));
            if (!v.requires_dcc.empty())
                out += std::format("            requires_dcc = {},\n", q(v.requires_dcc));
            if (!v.dependencies.empty())
            {
                out += "            dependencies = {\n";
                for (auto const& d : v.dependencies)
                    out += std::format("                {{ location = {}, version = {} }},\n", q(d.location), q(d.version));
                out += "            },\n";
            }
            if (v.yanked)
                out += "            yanked = true,\n";
            out += "        },\n";
        }
        return out + "    },\n}\n";
    }

    [[nodiscard]] inline std::string print_pointer(std::string_view alias)
    {
        return std::format("dpm::v1::RegistryLocation {{ alias = {} }}\n", lock::detail::quote(alias));
    }

    [[nodiscard]] inline std::string print_template(manifest::RegistryTemplate const& t)
    {
        manifest::RegistryAlias as_alias;
        as_alias.alias = t.name;
        as_alias.location = t.location;
        as_alias.description = t.description;
        as_alias.versions = t.versions;
        auto text = print_alias(as_alias);
        auto header = std::string{"dpm::v1::RegistryAlias {\n    alias = "};
        text.replace(0, header.size(), "dpm::v1::RegistryTemplate {\n    name = ");
        return text;
    }

    struct Scan
    {
        std::map<std::string, manifest::RegistryTemplate> templates;
        std::map<std::string, std::string> template_paths;
        std::map<std::string, manifest::RegistryAlias> aliases;
        std::map<std::string, std::string> alias_paths;
        std::map<std::string, std::string> pointers;
        std::vector<Problem> problems;
    };

    [[nodiscard]] inline Scan scan_registry(std::filesystem::path const& dir, Context const& ctx)
    {
        namespace fs = std::filesystem;
        Scan scan;
        auto problem = [&](std::string path, std::string message) { scan.problems.push_back({std::move(path), std::move(message)}); };
        std::size_t files = 0;

        bool too_big = false;
        if (auto text = registry::detail::read_limited(dir / "registry.dcon", registry::k_max_file, too_big); text && detail::is_plain_file(dir / "registry.dcon"))
        {
            auto loaded = manifest::load(*text, detail::options_for(ctx, manifest::FileKind::RegistryConfig, "registry.dcon"));
            if (!loaded.ok)
                problem("registry.dcon", detail::first_diagnostic(loaded));
        }
        else
            problem("registry.dcon", too_big ? "file is too large" : "missing registry.dcon");

        auto const alias_root = dir / "aliases";
        if (fs::exists(fs::symlink_status(alias_root)))
        {
            if (!detail::is_plain_dir(alias_root))
                problem("aliases", "not a directory");
            else
                for (auto const& shard : detail::sorted_names(alias_root))
                {
                    auto shard_rel = "aliases/" + shard;
                    if (!detail::is_plain_dir(alias_root / shard))
                    {
                        problem(shard_rel, "unexpected entry; expected a shard directory");
                        continue;
                    }
                    for (auto const& file : detail::sorted_names(alias_root / shard))
                    {
                        auto rel = shard_rel + "/" + file;
                        if (++files > k_max_registry_files)
                        {
                            problem("aliases", "too many files");
                            return scan;
                        }
                        if (!detail::is_plain_file(alias_root / shard / file))
                        {
                            problem(rel, "not a regular file");
                            continue;
                        }
                        if (!file.ends_with(".dcon"))
                        {
                            problem(rel, "alias records must end in .dcon");
                            continue;
                        }
                        auto text = registry::detail::read_limited(alias_root / shard / file, registry::k_max_file, too_big);
                        if (!text)
                        {
                            problem(rel, too_big ? std::format("file is larger than {} bytes", registry::k_max_file) : "cannot read the file");
                            continue;
                        }
                        auto loaded = manifest::load(*text, detail::options_for(ctx, manifest::FileKind::RegistryAlias, rel));
                        if (!loaded.ok || !loaded.registry_alias)
                        {
                            problem(rel, detail::first_diagnostic(loaded));
                            continue;
                        }
                        auto record = *loaded.registry_alias;
                        if (registry::alias_path(record.alias) != rel)
                            problem(rel, std::format("the file does not belong at this path; alias '{}' is stored at {}", record.alias, registry::alias_path(record.alias)));
                        if (auto bad = registry::record_problem(record, ctx.allow_file))
                            problem(rel, *bad);
                        auto key = detail::lower(record.alias);
                        if (auto it = scan.alias_paths.find(key); it != scan.alias_paths.end())
                        {
                            problem(rel, std::format("alias '{}' is already defined in {}", record.alias, it->second));
                            continue;
                        }
                        scan.alias_paths[key] = rel;
                        scan.aliases.emplace(record.alias, std::move(record));
                    }
                }
        }

        auto const template_root = dir / "templates";
        if (fs::exists(fs::symlink_status(template_root)))
        {
            if (!detail::is_plain_dir(template_root))
                problem("templates", "not a directory");
            else
                for (auto const& shard : detail::sorted_names(template_root))
                {
                    auto shard_rel = "templates/" + shard;
                    if (!detail::is_plain_dir(template_root / shard))
                    {
                        problem(shard_rel, "unexpected entry; expected a shard directory");
                        continue;
                    }
                    for (auto const& file : detail::sorted_names(template_root / shard))
                    {
                        auto rel = shard_rel + "/" + file;
                        if (++files > k_max_registry_files)
                        {
                            problem("templates", "too many files");
                            return scan;
                        }
                        if (!detail::is_plain_file(template_root / shard / file))
                        {
                            problem(rel, "not a regular file");
                            continue;
                        }
                        if (!file.ends_with(".dcon"))
                        {
                            problem(rel, "template records must end in .dcon");
                            continue;
                        }
                        auto text = registry::detail::read_limited(template_root / shard / file, registry::k_max_file, too_big);
                        if (!text)
                        {
                            problem(rel, too_big ? std::format("file is larger than {} bytes", registry::k_max_file) : "cannot read the file");
                            continue;
                        }
                        auto loaded = manifest::load(*text, detail::options_for(ctx, manifest::FileKind::RegistryTemplate, rel));
                        if (!loaded.ok || !loaded.registry_template)
                        {
                            problem(rel, detail::first_diagnostic(loaded));
                            continue;
                        }
                        auto record = *loaded.registry_template;
                        if (registry::template_path(record.name) != rel)
                            problem(rel, std::format("the file does not belong at this path; template '{}' is stored at {}", record.name, registry::template_path(record.name)));
                        if (auto bad = registry::versions_problem(record.location, record.versions, ctx.allow_file))
                            problem(rel, *bad);
                        auto key = detail::lower(record.name);
                        if (auto it = scan.template_paths.find(key); it != scan.template_paths.end())
                        {
                            problem(rel, std::format("template '{}' is already defined in {}", record.name, it->second));
                            continue;
                        }
                        scan.template_paths[key] = rel;
                        scan.templates.emplace(record.name, std::move(record));
                    }
                }
        }

        auto const location_root = dir / "locations";
        std::function<void(fs::path const&, std::string const&)> walk = [&](fs::path const& path, std::string const& rel_dir)
        {
            for (auto const& name : detail::sorted_names(path))
            {
                auto rel = rel_dir + "/" + name;
                if (detail::is_plain_dir(path / name))
                {
                    walk(path / name, rel);
                    continue;
                }
                if (++files > k_max_registry_files)
                    return;
                if (!detail::is_plain_file(path / name))
                {
                    problem(rel, "not a regular file");
                    continue;
                }
                if (!name.ends_with(".dcon"))
                {
                    problem(rel, "location pointers must end in .dcon");
                    continue;
                }
                auto location = rel.substr(10, rel.size() - 10 - 5);
                if (registry::location_path(location) != std::optional<std::string>{rel})
                {
                    problem(rel, "not a valid location pointer path");
                    continue;
                }
                auto text = registry::detail::read_limited(path / name, registry::k_max_file, too_big);
                if (!text)
                {
                    problem(rel, too_big ? std::format("file is larger than {} bytes", registry::k_max_file) : "cannot read the file");
                    continue;
                }
                auto loaded = manifest::load(*text, detail::options_for(ctx, manifest::FileKind::RegistryLocation, rel));
                if (!loaded.ok || !loaded.registry_location)
                {
                    problem(rel, detail::first_diagnostic(loaded));
                    continue;
                }
                scan.pointers[location] = loaded.registry_location->alias;
            }
        };
        if (fs::exists(fs::symlink_status(location_root)))
        {
            if (!detail::is_plain_dir(location_root))
                problem("locations", "not a directory");
            else
                walk(location_root, "locations");
        }
        if (files > k_max_registry_files)
        {
            problem("", "too many files");
            return scan;
        }

        std::map<std::string, std::string> owners;
        for (auto const& [name, record] : scan.aliases)
        {
            if (auto it = owners.find(record.location); it != owners.end())
                problem(scan.alias_paths[detail::lower(name)], std::format("location {} is already claimed by alias '{}'", record.location, it->second));
            else
                owners[record.location] = name;
            if (auto rel = registry::location_path(record.location))
            {
                auto it = scan.pointers.find(record.location);
                if (it == scan.pointers.end())
                    problem(scan.alias_paths[detail::lower(name)], std::format("missing location pointer {}", *rel));
                else if (it->second != name)
                    problem(*rel, std::format("names alias '{}' but {} belongs to alias '{}'", it->second, record.location, name));
            }
        }
        for (auto const& [location, alias] : scan.pointers)
        {
            auto rel = registry::location_path(location).value_or(location);
            auto it = scan.aliases.find(alias);
            if (it == scan.aliases.end())
                problem(rel, std::format("names alias '{}', which does not exist", alias));
            else if (it->second.location != location)
                problem(rel, std::format("names alias '{}', which belongs to {}", alias, it->second.location));
        }
        std::map<std::string, std::string> template_owners;
        for (auto const& [name, record] : scan.templates)
        {
            if (auto it = template_owners.find(record.location); it != template_owners.end())
                problem(scan.template_paths[detail::lower(name)], std::format("location {} is already claimed by template '{}'", record.location, it->second));
            else
                template_owners[record.location] = name;
        }
        std::ranges::sort(scan.problems, [](Problem const& a, Problem const& b) { return std::tie(a.path, a.message) < std::tie(b.path, b.message); });
        scan.problems.erase(std::unique(scan.problems.begin(), scan.problems.end(),
                                        [](Problem const& a, Problem const& b) { return a.path == b.path && a.message == b.message; }),
                            scan.problems.end());
        return scan;
    }

    struct VerifyRequest
    {
        cache::Cache cache{std::filesystem::path{"."}};
        fetch::Policy policy;
        Context ctx;
        std::string base;
        std::string git{"git"};
    };

    [[nodiscard]] inline std::vector<Problem> verify_sources(std::filesystem::path const& dir, Scan const& scan, VerifyRequest const& request)
    {
        std::vector<Problem> problems;
        for (auto const& [name, record] : scan.aliases)
        {
            auto rel = scan.alias_paths.at(detail::lower(name));
            std::optional<manifest::RegistryAlias> old;
            if (!request.base.empty())
            {
                auto shown = detail::run_git(request.git, dir, {"show", request.base + ":" + rel});
                if (shown.ok)
                {
                    auto loaded = manifest::load(shown.out, detail::options_for(request.ctx, manifest::FileKind::RegistryAlias, rel));
                    if (loaded.ok)
                        old = loaded.registry_alias;
                }
            }
            for (auto const& entry : record.versions)
            {
                if (old)
                {
                    bool known = false;
                    for (auto const& o : old->versions)
                        if (o.version == entry.version && detail::same_entry(o, entry))
                            known = true;
                    if (known)
                        continue;
                }
                auto label = std::format("{} {}", rel, entry.version);
                fetch::Fetched f;
                if (entry.source.starts_with("git+"))
                {
                    std::string error;
                    auto spec = fetch::parse_git_location(entry.source, request.policy, error);
                    if (!spec)
                    {
                        problems.push_back({label, error});
                        continue;
                    }
                    fetch::GitRequest git_request;
                    git_request.spec = *spec;
                    git_request.tags = detail::tag_candidates(spec->subdir, entry.version);
                    git_request.commit = entry.commit;
                    git_request.hash = entry.hash;
                    f = fetch::fetch_git(request.cache, git_request, request.policy);
                }
                else
                    f = fetch::fetch_tarball(request.cache, entry.source, entry.hash, request.policy);
                if (!f.ok)
                {
                    problems.push_back({label, f.error});
                    continue;
                }
                std::ifstream in{f.path / "project.dcon", std::ios::binary};
                if (!in)
                {
                    problems.push_back({label, "the package has no project.dcon"});
                    continue;
                }
                std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
                auto options = detail::options_for(request.ctx, manifest::FileKind::Project, label + " project.dcon");
                options.project_root = f.path;
                auto loaded = manifest::load(text, options);
                if (!loaded.ok || !loaded.project)
                {
                    problems.push_back({label, detail::first_diagnostic(loaded)});
                    continue;
                }
                auto const& project = *loaded.project;
                if (project.version != entry.version)
                    problems.push_back({label, std::format("the package contains version {}", project.version)});
                if (project.alias != record.alias)
                    problems.push_back({label, project.alias.empty() ? std::format("the package does not claim the alias '{}'", record.alias)
                                                                     : std::format("the package claims the alias '{}', not '{}'", project.alias, record.alias)});
                if (project.requires_dcc != entry.requires_dcc)
                    problems.push_back({label, "requires_dcc differs from the package manifest"});
                std::vector<std::pair<std::string, std::string>> actual;
                std::vector<std::pair<std::string, std::string>> listed;
                for (auto const& d : project.dependencies)
                    actual.emplace_back(d.location, d.version);
                for (auto const& d : entry.dependencies)
                    listed.emplace_back(d.location, d.version);
                std::ranges::sort(actual);
                std::ranges::sort(listed);
                if (actual != listed)
                    problems.push_back({label, "the dependency summary differs from the package manifest"});
            }
        }
        for (auto const& [name, record] : scan.templates)
        {
            auto rel = scan.template_paths.at(detail::lower(name));
            std::optional<manifest::RegistryTemplate> old;
            if (!request.base.empty())
            {
                auto shown = detail::run_git(request.git, dir, {"show", request.base + ":" + rel});
                if (shown.ok)
                {
                    auto loaded = manifest::load(shown.out, detail::options_for(request.ctx, manifest::FileKind::RegistryTemplate, rel));
                    if (loaded.ok)
                        old = loaded.registry_template;
                }
            }
            for (auto const& entry : record.versions)
            {
                if (old)
                {
                    bool known = false;
                    for (auto const& o : old->versions)
                        if (o.version == entry.version && detail::same_entry(o, entry))
                            known = true;
                    if (known)
                        continue;
                }
                auto label = std::format("{} {}", rel, entry.version);
                fetch::Fetched f;
                if (entry.source.starts_with("git+"))
                {
                    std::string error;
                    auto spec = fetch::parse_git_location(entry.source, request.policy, error);
                    if (!spec)
                    {
                        problems.push_back({label, error});
                        continue;
                    }
                    fetch::GitRequest git_request;
                    git_request.spec = *spec;
                    git_request.tags = detail::tag_candidates(spec->subdir, entry.version);
                    git_request.commit = entry.commit;
                    git_request.hash = entry.hash;
                    f = fetch::fetch_git(request.cache, git_request, request.policy);
                }
                else
                    f = fetch::fetch_tarball(request.cache, entry.source, entry.hash, request.policy);
                if (!f.ok)
                {
                    problems.push_back({label, f.error});
                    continue;
                }
                auto loaded = stage::load_template(f.path, detail::options_for(request.ctx, manifest::FileKind::Template, label + " template.dcon"));
                if (!loaded.ok())
                {
                    for (auto const& p : loaded.problems)
                        problems.push_back({label, p});
                    continue;
                }
                if (loaded.tmpl.name != record.name)
                    problems.push_back({label, std::format("the template is named '{}', not '{}'", loaded.tmpl.name, record.name)});
                if (loaded.tmpl.requires_dcc != entry.requires_dcc)
                    problems.push_back({label, "requires_dcc differs from template.dcon"});
                auto checked = stage::check_template(loaded, detail::options_for(request.ctx, manifest::FileKind::Project, label));
                for (auto const& p : checked.problems)
                    problems.push_back({label, p});
            }
        }
        std::ranges::sort(problems, [](Problem const& a, Problem const& b) { return std::tie(a.path, a.message) < std::tie(b.path, b.message); });
        return problems;
    }

    struct GitState
    {
        std::string top;
        std::string prefix;
        std::string subdir;
        std::string head;
    };

    [[nodiscard]] inline std::optional<std::filesystem::path> registry_ready(std::filesystem::path const& registry, Context const& ctx, std::string& error)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto registry_dir = fs::absolute(registry, ec);
        if (!detail::is_plain_dir(registry_dir))
        {
            error = std::format("registry directory {} does not exist", registry.string());
            return std::nullopt;
        }
        bool too_big = false;
        auto descriptor = registry::detail::read_limited(registry_dir / "registry.dcon", registry::k_max_file, too_big);
        if (!descriptor)
        {
            error = std::format("{} is not a registry (no registry.dcon)", registry.string());
            return std::nullopt;
        }
        if (auto loaded = manifest::load(*descriptor, detail::options_for(ctx, manifest::FileKind::RegistryConfig, "registry.dcon")); !loaded.ok)
        {
            error = detail::first_diagnostic(loaded);
            return std::nullopt;
        }
        return registry_dir;
    }

    [[nodiscard]] inline std::optional<GitState> git_state(std::string const& git, std::filesystem::path const& dir, std::string const& what, std::string& error)
    {
        GitState state;
        auto top = detail::run_git(git, dir, {"rev-parse", "--show-toplevel"});
        if (!top.ok)
        {
            error = std::format("the {} is not in a git repository", what);
            return std::nullopt;
        }
        state.top = top.out;
        auto prefix_out = detail::run_git(git, dir, {"rev-parse", "--show-prefix"});
        state.prefix = prefix_out.ok ? prefix_out.out : std::string{};
        state.subdir = state.prefix;
        while (state.subdir.ends_with('/'))
            state.subdir.pop_back();
        auto status = detail::run_git(git, dir, {"status", "--porcelain=v1", "--untracked-files=normal", "--", "."});
        if (!status.ok)
        {
            error = "git status failed: " + status.err;
            return std::nullopt;
        }
        if (!status.out.empty())
        {
            error = "the git tree is not clean; commit or remove these changes first:\n" + status.out;
            return std::nullopt;
        }
        auto head = detail::run_git(git, dir, {"rev-parse", "HEAD"});
        if (!head.ok)
        {
            error = "the repository has no commits";
            return std::nullopt;
        }
        state.head = head.out;
        return state;
    }

    struct Tagged
    {
        std::string commit;
        std::string hash;
    };

    [[nodiscard]] inline std::optional<Tagged> hash_tagged_tree(std::string const& git, cache::Cache const& cache, fetch::Policy const& policy, GitState const& state,
                                                                std::string const& tag, std::function<std::optional<std::string>(std::filesystem::path const&)> const& inspect,
                                                                std::string& error)
    {
        git::Policy git_policy;
        git_policy.allow_file = true;
        git_policy.program = git;
        git::Repository repo{cache.git_dir() / "_publish" / (hash::sha256_hex(state.top).substr(0, 24) + ".git"), git_policy};
        std::string cache_error;
        if (!cache.ensure(cache_error) || !repo.init(cache_error))
        {
            error = cache_error;
            return std::nullopt;
        }
        auto fetched = repo.fetch_tag("file://" + state.top, tag);
        if (!fetched.ok)
        {
            error = "cannot read the tag from the repository: " + fetched.error;
            return std::nullopt;
        }
        if (fetched.commit != state.head)
        {
            error = std::format("the tag {} resolved to {}, not to HEAD", tag, fetched.commit);
            return std::nullopt;
        }
        auto staging = cache.new_staging(cache_error);
        if (!staging)
        {
            error = cache_error;
            return std::nullopt;
        }
        auto exported = repo.export_tree(fetched.commit, state.subdir, *staging, policy.limits);
        if (!exported.ok)
        {
            cache::detail::remove_tree(*staging);
            error = "the tagged tree cannot be published: " + exported.error;
            return std::nullopt;
        }
        if (inspect)
            if (auto problem = inspect(*staging))
            {
                cache::detail::remove_tree(*staging);
                error = *problem;
                return std::nullopt;
            }
        auto tree = hash::tree_hash(*staging);
        cache::detail::remove_tree(*staging);
        if (!tree.ok)
        {
            error = tree.error;
            return std::nullopt;
        }
        return Tagged{fetched.commit, tree.hash};
    }

    struct PublishInput
    {
        manifest::Project const* project{};
        std::filesystem::path project_dir{"."};
        std::filesystem::path registry_dir;
        std::string location;
        std::string dcc_version;
        cache::Cache cache{std::filesystem::path{"."}};
        fetch::Policy policy;
        Context ctx;
        std::string git{"git"};
    };

    struct Prepared
    {
        std::string alias;
        std::string location;
        std::string tag;
        manifest::RegistryVersion entry;
        std::optional<manifest::RegistryAlias> existing;
        std::string description;
    };

    [[nodiscard]] inline std::optional<Prepared> prepare(PublishInput const& in, std::string& error)
    {
        namespace fs = std::filesystem;
        auto const& project = *in.project;
        if (project.alias.empty())
        {
            error = "project.dcon needs an alias to be published";
            return std::nullopt;
        }
        for (auto const* list : {&project.dependencies, &project.dev_dependencies})
            for (auto const& d : *list)
                if (d.location.starts_with("path:"))
                {
                    error = std::format("a published package cannot depend on {}", d.location);
                    return std::nullopt;
                }
        if (!project.requires_dcc.empty() && !in.dcc_version.empty())
        {
            auto need = semver::parse(project.requires_dcc);
            auto have = semver::parse(in.dcc_version);
            if (need && have && semver::compare(*have, *need) < 0)
            {
                error = std::format("requires_dcc {} is newer than this dcc ({}); the package could not be built here", project.requires_dcc, in.dcc_version);
                return std::nullopt;
            }
        }

        auto registry_dir = registry_ready(in.registry_dir, in.ctx, error);
        if (!registry_dir)
            return std::nullopt;
        auto state = git_state(in.git, in.project_dir, "package", error);
        if (!state)
            return std::nullopt;
        auto const& prefix = state->prefix;
        auto const& subdir = state->subdir;
        auto const& head_commit = state->head;
        std::string tag;
        std::string tag_commit;
        for (auto const& candidate : detail::tag_candidates(subdir, project.version))
        {
            auto resolved = detail::run_git(in.git, in.project_dir, {"rev-parse", "--verify", "--quiet", "refs/tags/" + candidate + "^{commit}"});
            if (resolved.ok)
            {
                tag = candidate;
                tag_commit = resolved.out;
                break;
            }
        }
        if (tag.empty())
        {
            error = std::format("the tag {} does not exist; create it on the release commit first", detail::tag_candidates(subdir, project.version).front());
            return std::nullopt;
        }
        if (tag_commit != head_commit)
        {
            error = std::format("the tag {} points to {}, not to HEAD ({})", tag, tag_commit, head_commit);
            return std::nullopt;
        }
        auto shown = detail::run_git(in.git, in.project_dir, {"show", tag + ":" + prefix + "project.dcon"});
        if (!shown.ok)
        {
            error = std::format("cannot read {}project.dcon at {}", prefix, tag);
            return std::nullopt;
        }
        {
            auto options = detail::options_for(in.ctx, manifest::FileKind::Project, tag + ":project.dcon");
            auto loaded = manifest::load(shown.out, options);
            if (!loaded.ok || !loaded.project)
            {
                error = detail::first_diagnostic(loaded);
                return std::nullopt;
            }
            if (loaded.project->version != project.version)
            {
                error = std::format("project.dcon at {} has version {}, not {}", tag, loaded.project->version, project.version);
                return std::nullopt;
            }
        }

        Prepared out;
        out.alias = project.alias;
        out.tag = tag;
        auto scan = scan_registry(*registry_dir, in.ctx);
        if (auto it = scan.aliases.find(project.alias); it != scan.aliases.end())
            out.existing = it->second;
        if (out.existing)
        {
            if (!in.location.empty() && in.location != out.existing->location)
            {
                error = std::format("alias '{}' belongs to {}, not to {}", project.alias, out.existing->location, in.location);
                return std::nullopt;
            }
            out.location = out.existing->location;
            out.description = out.existing->description;
        }
        else
        {
            if (in.location.empty())
            {
                error = std::format("alias '{}' is not in the registry; pass --location or run dpm registry alias first", project.alias);
                return std::nullopt;
            }
            if (scan.alias_paths.contains(detail::lower(project.alias)))
            {
                error = std::format("alias '{}' is already taken", project.alias);
                return std::nullopt;
            }
            out.location = in.location;
            out.description = project.description.find('\n') == std::string::npos ? project.description : std::string{};
        }
        if (auto bad = manifest::detail::location_error(out.location, in.ctx.allow_file))
        {
            error = std::format("location '{}': {}", out.location, *bad);
            return std::nullopt;
        }
        if (out.location.find(':') == std::string::npos && out.location.find('/') == std::string::npos)
        {
            error = "the location of an alias must be a canonical location";
            return std::nullopt;
        }
        for (auto const& [name, record] : scan.aliases)
            if (name != project.alias && record.location == out.location)
            {
                error = std::format("location {} is already claimed by alias '{}'", out.location, name);
                return std::nullopt;
            }
        fetch::Policy policy = in.policy;
        policy.allow_file = in.ctx.allow_file;
        if (fetch::is_tarball_location(out.location, policy))
        {
            error = "publishing is only supported for git locations";
            return std::nullopt;
        }
        auto source = registry::expected_git_source(out.location, policy);
        std::string spec_error;
        auto spec = fetch::parse_git_location(out.location, policy, spec_error);
        if (!source || !spec)
        {
            error = "location: " + spec_error;
            return std::nullopt;
        }
        if (spec->subdir != subdir)
        {
            error = std::format("location {} names the subdirectory '{}', but the package is in '{}'", out.location, spec->subdir, subdir);
            return std::nullopt;
        }

        auto tagged = hash_tagged_tree(in.git, in.cache, in.policy, *state, tag, nullptr, error);
        if (!tagged)
            return std::nullopt;

        out.entry.version = project.version;
        out.entry.source = *source;
        out.entry.commit = tagged->commit;
        out.entry.hash = tagged->hash;
        out.entry.requires_dcc = project.requires_dcc;
        for (auto const& d : project.dependencies)
        {
            manifest::Dependency dep;
            dep.location = d.location;
            dep.version = d.version;
            out.entry.dependencies.push_back(std::move(dep));
        }
        return out;
    }

    [[nodiscard]] inline std::optional<Written> write_entry(Prepared const& prepared, PublishInput const& in, bool dry_run, std::string& error)
    {
        manifest::RegistryAlias record;
        if (prepared.existing)
            record = *prepared.existing;
        else
        {
            record.alias = prepared.alias;
            record.location = prepared.location;
            record.description = prepared.description;
        }
        Written written;
        written.alias_rel = registry::alias_path(record.alias);
        bool replaced = false;
        for (auto const& v : record.versions)
            if (v.version == prepared.entry.version)
            {
                if (!detail::same_entry(v, prepared.entry))
                {
                    error = std::format("version {} is already in the registry with different contents; published versions are immutable", v.version);
                    return std::nullopt;
                }
                replaced = true;
            }
        if (!replaced)
        {
            auto position = record.versions.end();
            auto added = semver::parse(prepared.entry.version);
            for (auto it = record.versions.begin(); it != record.versions.end(); ++it)
            {
                auto existing = semver::parse(it->version);
                if (existing && added && semver::compare(*existing, *added) > 0)
                {
                    position = it;
                    break;
                }
            }
            record.versions.insert(position, prepared.entry);
            written.changed = true;
        }
        std::vector<std::pair<std::string, std::string>> files;
        auto registry_dir = std::filesystem::absolute(in.registry_dir);
        std::string current;
        {
            bool too_big = false;
            current = registry::detail::read_limited(registry_dir / written.alias_rel, registry::k_max_file, too_big).value_or("");
        }
        auto text = print_alias(record);
        if (prepared.existing && !written.changed)
            text = current;
        else
        {
            auto loaded = manifest::load(text, detail::options_for(in.ctx, manifest::FileKind::RegistryAlias, written.alias_rel));
            if (!loaded.ok || !loaded.registry_alias || detail::lower(loaded.registry_alias->alias) != detail::lower(record.alias))
            {
                error = "internal error: the generated record does not load: " + detail::first_diagnostic(loaded);
                return std::nullopt;
            }
            if (auto bad = registry::record_problem(*loaded.registry_alias, in.ctx.allow_file))
            {
                error = *bad;
                return std::nullopt;
            }
            if (text != current)
                files.emplace_back(written.alias_rel, text);
        }
        if (auto pointer = registry::location_path(record.location))
        {
            written.pointer_rel = *pointer;
            bool too_big = false;
            auto existing = registry::detail::read_limited(registry_dir / *pointer, registry::k_max_file, too_big);
            auto wanted = print_pointer(record.alias);
            if (!existing)
            {
                files.emplace_back(*pointer, wanted);
                written.changed = true;
            }
            else
            {
                auto loaded = manifest::load(*existing, detail::options_for(in.ctx, manifest::FileKind::RegistryLocation, *pointer));
                if (!loaded.ok || !loaded.registry_location || loaded.registry_location->alias != record.alias)
                {
                    error = std::format("{} names a different alias; fix the registry by hand", *pointer);
                    return std::nullopt;
                }
            }
        }
        for (auto const& [rel, unused] : files)
            written.files.push_back(rel);
        if (!dry_run && !detail::commit_files(registry_dir, files, error))
            return std::nullopt;
        return written;
    }

    struct TemplatePublishInput
    {
        std::filesystem::path template_dir{"."};
        std::filesystem::path registry_dir;
        std::string location;
        std::string dcc_version;
        cache::Cache cache{std::filesystem::path{"."}};
        fetch::Policy policy;
        Context ctx;
        std::string git{"git"};
    };

    struct PreparedTemplate
    {
        std::string name;
        std::string location;
        std::string tag;
        manifest::RegistryVersion entry;
        std::optional<manifest::RegistryTemplate> existing;
        std::string description;
    };

    [[nodiscard]] inline std::optional<PreparedTemplate> prepare_template(TemplatePublishInput const& in, std::string& error)
    {
        auto options = detail::options_for(in.ctx, manifest::FileKind::Template, "template.dcon");
        auto loaded = stage::load_template(in.template_dir, options);
        if (loaded.ok())
        {
            auto checked = stage::check_template(loaded, detail::options_for(in.ctx, manifest::FileKind::Project, "project.dcon"));
            if (!checked.ok())
                loaded.problems = checked.problems;
        }
        if (!loaded.ok())
        {
            error = "the template does not pass 'dpm template check':\n";
            for (auto const& p : loaded.problems)
                error += "  " + p + "\n";
            error.pop_back();
            return std::nullopt;
        }
        auto const& tmpl = loaded.tmpl;
        if (!tmpl.requires_dcc.empty() && !in.dcc_version.empty())
        {
            auto need = semver::parse(tmpl.requires_dcc);
            auto have = semver::parse(in.dcc_version);
            if (need && have && semver::compare(*have, *need) < 0)
            {
                error = std::format("requires_dcc {} is newer than this dcc ({}); the template could not be used here", tmpl.requires_dcc, in.dcc_version);
                return std::nullopt;
            }
        }
        auto registry_dir = registry_ready(in.registry_dir, in.ctx, error);
        if (!registry_dir)
            return std::nullopt;
        auto state = git_state(in.git, in.template_dir, "template", error);
        if (!state)
            return std::nullopt;
        auto at_head = detail::run_git(in.git, in.template_dir, {"tag", "--points-at", "HEAD"});
        std::map<std::string, std::string> versions;
        if (at_head.ok)
        {
            std::set<std::string> tags;
            std::size_t pos = 0;
            while (pos < at_head.out.size())
            {
                auto nl = at_head.out.find('\n', pos);
                tags.insert(at_head.out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
                pos = nl == std::string::npos ? at_head.out.size() : nl + 1;
            }
            for (auto const& tag : tags)
            {
                std::string_view name{tag};
                if (!state->subdir.empty() && name.starts_with(state->subdir + "/"))
                    name.remove_prefix(state->subdir.size() + 1);
                if (name.starts_with('v'))
                    name.remove_prefix(1);
                if (semver::parse(name))
                    for (auto const& candidate : detail::tag_candidates(state->subdir, std::string{name}))
                        if (tags.contains(candidate))
                        {
                            versions.emplace(std::string{name}, candidate);
                            break;
                        }
            }
        }
        if (versions.empty())
        {
            error = std::format("no version tag points at HEAD; create one such as {} on the release commit", detail::tag_candidates(state->subdir, "1.0.0").front());
            return std::nullopt;
        }
        if (versions.size() > 1)
        {
            std::string listed;
            for (auto const& [version, tag] : versions)
                listed += (listed.empty() ? "" : ", ") + tag;
            error = std::format("more than one version tag points at HEAD ({}); publish one version at a time", listed);
            return std::nullopt;
        }
        auto const version = versions.begin()->first;
        PreparedTemplate out;
        out.name = tmpl.name;
        out.tag = versions.begin()->second;
        auto scan = scan_registry(*registry_dir, in.ctx);
        if (auto it = scan.templates.find(tmpl.name); it != scan.templates.end())
            out.existing = it->second;
        if (out.existing)
        {
            if (!in.location.empty() && in.location != out.existing->location)
            {
                error = std::format("template '{}' belongs to {}, not to {}", tmpl.name, out.existing->location, in.location);
                return std::nullopt;
            }
            out.location = out.existing->location;
            out.description = out.existing->description;
        }
        else
        {
            if (in.location.empty())
            {
                error = std::format("template '{}' is not in the registry; pass --location or run dpm registry template first", tmpl.name);
                return std::nullopt;
            }
            if (scan.template_paths.contains(detail::lower(tmpl.name)))
            {
                error = std::format("template '{}' is already taken", tmpl.name);
                return std::nullopt;
            }
            out.location = in.location;
            out.description = tmpl.description;
        }
        if (auto bad = manifest::detail::location_error(out.location, in.ctx.allow_file))
        {
            error = std::format("location '{}': {}", out.location, *bad);
            return std::nullopt;
        }
        if (out.location.find(':') == std::string::npos && out.location.find('/') == std::string::npos)
        {
            error = "the location of a template must be a canonical location";
            return std::nullopt;
        }
        for (auto const& [name, record] : scan.templates)
            if (name != tmpl.name && record.location == out.location)
            {
                error = std::format("location {} is already claimed by template '{}'", out.location, name);
                return std::nullopt;
            }
        fetch::Policy policy = in.policy;
        policy.allow_file = in.ctx.allow_file;
        if (fetch::is_tarball_location(out.location, policy))
        {
            error = "publishing is only supported for git locations";
            return std::nullopt;
        }
        auto source = registry::expected_git_source(out.location, policy);
        std::string spec_error;
        auto spec = fetch::parse_git_location(out.location, policy, spec_error);
        if (!source || !spec)
        {
            error = "location: " + spec_error;
            return std::nullopt;
        }
        if (spec->subdir != state->subdir)
        {
            error = std::format("location {} names the subdirectory '{}', but the template is in '{}'", out.location, spec->subdir, state->subdir);
            return std::nullopt;
        }
        auto inspect = [&](std::filesystem::path const& staged) -> std::optional<std::string>
        {
            auto tagged_template = stage::load_template(staged, detail::options_for(in.ctx, manifest::FileKind::Template, "template.dcon"));
            if (!tagged_template.ok())
                return "template.dcon at the tag does not load: " + tagged_template.problems.front();
            if (tagged_template.tmpl.name != tmpl.name)
                return std::format("template.dcon at the tag is named '{}', not '{}'", tagged_template.tmpl.name, tmpl.name);
            auto checked = stage::check_template(tagged_template, detail::options_for(in.ctx, manifest::FileKind::Project, "project.dcon"));
            if (!checked.ok())
                return "the template at the tag does not pass 'dpm template check': " + checked.problems.front();
            return std::nullopt;
        };
        auto tagged = hash_tagged_tree(in.git, in.cache, in.policy, *state, out.tag, inspect, error);
        if (!tagged)
            return std::nullopt;
        out.entry.version = version;
        out.entry.source = *source;
        out.entry.commit = tagged->commit;
        out.entry.hash = tagged->hash;
        out.entry.requires_dcc = tmpl.requires_dcc;
        return out;
    }

    [[nodiscard]] inline std::optional<Written> write_template_entry(PreparedTemplate const& prepared, TemplatePublishInput const& in, bool dry_run, std::string& error)
    {
        manifest::RegistryTemplate record;
        if (prepared.existing)
            record = *prepared.existing;
        else
        {
            record.name = prepared.name;
            record.location = prepared.location;
            record.description = prepared.description;
        }
        Written written;
        written.alias_rel = registry::template_path(record.name);
        bool replaced = false;
        for (auto const& v : record.versions)
            if (v.version == prepared.entry.version)
            {
                if (!detail::same_entry(v, prepared.entry))
                {
                    error = std::format("version {} is already in the registry with different contents; published versions are immutable", v.version);
                    return std::nullopt;
                }
                replaced = true;
            }
        if (!replaced)
        {
            auto position = record.versions.end();
            auto added = semver::parse(prepared.entry.version);
            for (auto it = record.versions.begin(); it != record.versions.end(); ++it)
            {
                auto existing = semver::parse(it->version);
                if (existing && added && semver::compare(*existing, *added) > 0)
                {
                    position = it;
                    break;
                }
            }
            record.versions.insert(position, prepared.entry);
            written.changed = true;
        }
        auto registry_dir = std::filesystem::absolute(in.registry_dir);
        bool too_big = false;
        auto current = registry::detail::read_limited(registry_dir / written.alias_rel, registry::k_max_file, too_big).value_or("");
        auto text = print_template(record);
        std::vector<std::pair<std::string, std::string>> files;
        if (prepared.existing && !written.changed)
            text = current;
        else
        {
            auto loaded = manifest::load(text, detail::options_for(in.ctx, manifest::FileKind::RegistryTemplate, written.alias_rel));
            if (!loaded.ok || !loaded.registry_template || detail::lower(loaded.registry_template->name) != detail::lower(record.name))
            {
                error = "internal error: the generated record does not load: " + detail::first_diagnostic(loaded);
                return std::nullopt;
            }
            if (auto bad = registry::versions_problem(loaded.registry_template->location, loaded.registry_template->versions, in.ctx.allow_file))
            {
                error = *bad;
                return std::nullopt;
            }
            if (text != current)
                files.emplace_back(written.alias_rel, text);
        }
        for (auto const& [rel, unused] : files)
            written.files.push_back(rel);
        if (!dry_run && !detail::commit_files(registry_dir, files, error))
            return std::nullopt;
        return written;
    }

    [[nodiscard]] inline std::optional<Written> create_template(std::filesystem::path const& dir, std::string const& location, std::string const& name,
                                                                std::string const& description, Context const& ctx, bool dry_run, std::string& error)
    {
        namespace fs = std::filesystem;
        if (auto bad = manifest::detail::template_name_error(name))
        {
            error = *bad;
            return std::nullopt;
        }
        if (auto bad = manifest::detail::location_error(location, ctx.allow_file))
        {
            error = std::format("location '{}': {}", location, *bad);
            return std::nullopt;
        }
        if (location.find(':') == std::string::npos && location.find('/') == std::string::npos)
        {
            error = "the location of a template must be a canonical location";
            return std::nullopt;
        }
        if (location.starts_with("path:"))
        {
            error = "the location of a template cannot be a path";
            return std::nullopt;
        }
        auto root = fs::absolute(dir);
        auto scan = scan_registry(root, ctx);
        if (!detail::is_plain_file(root / "registry.dcon"))
        {
            error = std::format("{} is not a registry (no registry.dcon)", dir.string());
            return std::nullopt;
        }
        if (scan.template_paths.contains(detail::lower(name)))
        {
            error = std::format("template '{}' is already taken", name);
            return std::nullopt;
        }
        for (auto const& [other, record] : scan.templates)
            if (record.location == location)
            {
                error = std::format("location {} is already claimed by template '{}'", location, other);
                return std::nullopt;
            }
        manifest::RegistryTemplate record;
        record.name = name;
        record.location = location;
        record.description = description;
        if (auto bad = registry::versions_problem(record.location, record.versions, ctx.allow_file))
        {
            error = *bad;
            return std::nullopt;
        }
        Written written;
        written.changed = true;
        written.alias_rel = registry::template_path(name);
        std::vector<std::pair<std::string, std::string>> files{{written.alias_rel, print_template(record)}};
        written.files.push_back(written.alias_rel);
        if (!dry_run && !detail::commit_files(root, files, error))
            return std::nullopt;
        return written;
    }

    [[nodiscard]] inline std::optional<Written> set_template_yanked(std::filesystem::path const& dir, std::string const& name, std::string const& version, bool yanked,
                                                                    Context const& ctx, bool dry_run, std::string& error)
    {
        if (auto bad = manifest::detail::template_name_error(name))
        {
            error = *bad;
            return std::nullopt;
        }
        auto root = std::filesystem::absolute(dir);
        Written written;
        written.alias_rel = registry::template_path(name);
        bool too_big = false;
        auto text = registry::detail::read_limited(root / written.alias_rel, registry::k_max_file, too_big);
        if (!text)
        {
            error = std::format("template '{}' is not in {}", name, dir.string());
            return std::nullopt;
        }
        auto edited = edit::set_registry_yanked(*text, detail::options_for(ctx, manifest::FileKind::RegistryTemplate, written.alias_rel), version, yanked);
        if (!edited.ok)
        {
            error = edited.error.empty() ? "the record could not be edited" : edited.error;
            return std::nullopt;
        }
        written.changed = edited.text != *text;
        if (written.changed)
        {
            written.files.push_back(written.alias_rel);
            if (!dry_run && !edit::write_atomic(root / written.alias_rel, edited.text, error))
                return std::nullopt;
        }
        return written;
    }

    [[nodiscard]] inline std::optional<Written> init_registry(std::filesystem::path const& dir, std::string name, Context const& ctx, std::string& error)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto root = fs::absolute(dir, ec).lexically_normal();
        if (name.empty())
        {
            name = root.filename().string();
            if (name.empty())
                name = root.parent_path().filename().string();
        }
        if (name.empty() || name.size() > 64 || std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f; }))
        {
            error = "the registry name must be 1 to 64 printable characters; pass --name";
            return std::nullopt;
        }
        if (fs::exists(fs::symlink_status(root, ec)))
        {
            if (!detail::is_plain_dir(root))
            {
                error = std::format("{} is not a directory", dir.string());
                return std::nullopt;
            }
            for (auto const& entry : detail::sorted_names(root))
            {
                auto upper = entry;
                for (auto& c : upper)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                bool const allowed = entry == ".git" || entry == ".gitignore" || upper.starts_with("README") || upper.starts_with("LICENSE");
                if (!allowed)
                {
                    error = std::format("{} is not empty ('{}' is in the way); only .git, .gitignore, README* and LICENSE* may exist already", dir.string(), entry);
                    return std::nullopt;
                }
            }
        }
        auto text = std::format("dpm::v1::RegistryConfig {{\n    format = 1,\n    name = {},\n}}\n", lock::detail::quote(name));
        auto loaded = manifest::load(text, detail::options_for(ctx, manifest::FileKind::RegistryConfig, "registry.dcon"));
        if (!loaded.ok || !loaded.registry_config || loaded.registry_config->name != name)
        {
            error = "the registry name cannot be written: " + (loaded.ok ? std::string{"it does not round-trip"} : detail::first_diagnostic(loaded));
            return std::nullopt;
        }
        fs::create_directories(root / "aliases", ec);
        fs::create_directories(root / "locations", ec);
        if (ec)
        {
            error = "cannot create " + root.string();
            return std::nullopt;
        }
        if (!edit::write_atomic(root / "registry.dcon", text, error))
            return std::nullopt;
        Written written;
        written.changed = true;
        written.alias_rel = "registry.dcon";
        written.files.push_back("registry.dcon");
        return written;
    }

    [[nodiscard]] inline std::optional<Written> create_alias(std::filesystem::path const& dir, std::string const& location, std::string const& name,
                                                             std::string const& description, Context const& ctx, bool dry_run, std::string& error)
    {
        namespace fs = std::filesystem;
        if (auto bad = manifest::detail::alias_error(name))
        {
            error = *bad;
            return std::nullopt;
        }
        if (auto bad = manifest::detail::location_error(location, ctx.allow_file))
        {
            error = std::format("location '{}': {}", location, *bad);
            return std::nullopt;
        }
        if (location.find(':') == std::string::npos && location.find('/') == std::string::npos)
        {
            error = "the location of an alias must be a canonical location";
            return std::nullopt;
        }
        if (location.starts_with("path:"))
        {
            error = "the location of an alias cannot be a path";
            return std::nullopt;
        }
        auto root = fs::absolute(dir);
        auto scan = scan_registry(root, ctx);
        if (!detail::is_plain_file(root / "registry.dcon"))
        {
            error = std::format("{} is not a registry (no registry.dcon)", dir.string());
            return std::nullopt;
        }
        if (scan.alias_paths.contains(detail::lower(name)))
        {
            error = std::format("alias '{}' is already taken", name);
            return std::nullopt;
        }
        for (auto const& [other, record] : scan.aliases)
            if (record.location == location)
            {
                error = std::format("location {} is already claimed by alias '{}'", location, other);
                return std::nullopt;
            }
        manifest::RegistryAlias record;
        record.alias = name;
        record.location = location;
        record.description = description;
        if (auto bad = registry::record_problem(record, ctx.allow_file))
        {
            error = *bad;
            return std::nullopt;
        }
        Written written;
        written.changed = true;
        written.alias_rel = registry::alias_path(name);
        std::vector<std::pair<std::string, std::string>> files{{written.alias_rel, print_alias(record)}};
        if (auto pointer = registry::location_path(location))
        {
            written.pointer_rel = *pointer;
            files.emplace_back(*pointer, print_pointer(name));
        }
        for (auto const& [rel, unused] : files)
            written.files.push_back(rel);
        if (!dry_run && !detail::commit_files(root, files, error))
            return std::nullopt;
        return written;
    }

    [[nodiscard]] inline std::optional<Written> set_yanked(std::filesystem::path const& dir, std::string const& alias, std::string const& version, bool yanked,
                                                           Context const& ctx, bool dry_run, std::string& error)
    {
        if (auto bad = manifest::detail::alias_error(alias))
        {
            error = *bad;
            return std::nullopt;
        }
        auto root = std::filesystem::absolute(dir);
        Written written;
        written.alias_rel = registry::alias_path(alias);
        bool too_big = false;
        auto text = registry::detail::read_limited(root / written.alias_rel, registry::k_max_file, too_big);
        if (!text)
        {
            error = std::format("alias '{}' is not in {}", alias, dir.string());
            return std::nullopt;
        }
        auto edited = edit::set_registry_yanked(*text, detail::options_for(ctx, manifest::FileKind::RegistryAlias, written.alias_rel), version, yanked);
        if (!edited.ok)
        {
            error = edited.error.empty() ? "the record could not be edited" : edited.error;
            return std::nullopt;
        }
        written.changed = edited.text != *text;
        if (written.changed)
        {
            written.files.push_back(written.alias_rel);
            if (!dry_run && !edit::write_atomic(root / written.alias_rel, edited.text, error))
                return std::nullopt;
        }
        return written;
    }

    [[nodiscard]] inline std::vector<std::string> git_commands(std::filesystem::path const& dir, Written const& written, std::string const& branch,
                                                               std::string const& message)
    {
        std::vector<std::string> out;
        out.push_back("cd " + detail::shell_quote(std::filesystem::absolute(dir).lexically_normal().string()));
        out.push_back("git checkout -b " + detail::shell_quote(branch));
        std::string add = "git add";
        for (auto const& f : written.files)
            add += " " + detail::shell_quote(f);
        out.push_back(add);
        out.push_back("git commit -m " + detail::shell_quote(message));
        out.emplace_back("gh pr create --fill");
        return out;
    }
} // namespace dpm::publish
