import std;
import dcc.config;
import dpm.cache;
import dpm.edit;
import dpm.lock;
import dpm.manifest;
import dpm.resolve;

namespace
{
    constexpr int k_exit_usage = 2;
    constexpr int k_exit_manifest = 3;
    constexpr int k_exit_cache = 4;

    std::filesystem::path schema_root(char** argv)
    {
        if (char const* env = std::getenv("DPM_SCHEMA_ROOT"))
            return env;
        return dcc::config::current_prefix(argv).path / "include";
    }

    int run_check(char** argv, std::span<char const* const> args)
    {
        bool tree = false;
        std::filesystem::path file{"project.dcon"};
        for (auto arg : args)
        {
            if (std::string_view{arg} == "--tree")
                tree = true;
            else if (std::string_view{arg}.starts_with("-"))
            {
                std::println(std::cerr, "dpm check: unknown option {}", arg);
                return k_exit_usage;
            }
            else
                file = arg;
        }
        std::ifstream in{file, std::ios::binary};
        if (!in)
        {
            std::println(std::cerr, "dpm: cannot read {}", file.string());
            return k_exit_manifest;
        }
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        dpm::manifest::LoadOptions options;
        options.file_name = file.string();
        options.schema_root = schema_root(argv);
        options.project_root = file.has_parent_path() ? file.parent_path() : std::filesystem::path{"."};
        options.kind = dpm::manifest::detect_kind(text, file.string());
        auto result = dpm::manifest::load(text, options);
        for (auto const& diagnostic : result.diagnostics)
            std::println(std::cerr, "{}", diagnostic.format());
        if (!result.ok)
            return k_exit_manifest;
        if (tree)
            std::print("{}", result.tree.dump(true));
        return 0;
    }

    struct Resolved
    {
        dpm::manifest::Project project;
        dpm::resolve::Resolution resolution;
        std::filesystem::path root;
    };

    std::optional<Resolved> resolve_here(char** argv, int& exit_code)
    {
        std::filesystem::path file{"project.dcon"};
        std::ifstream in{file, std::ios::binary};
        if (!in)
        {
            std::println(std::cerr, "dpm: cannot read {}", file.string());
            exit_code = k_exit_manifest;
            return std::nullopt;
        }
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        dpm::manifest::LoadOptions options;
        options.file_name = file.string();
        options.schema_root = schema_root(argv);
        options.project_root = ".";
        auto loaded = dpm::manifest::load(text, options);
        for (auto const& diagnostic : loaded.diagnostics)
            std::println(std::cerr, "{}", diagnostic.format());
        if (!loaded.ok || !loaded.project)
        {
            exit_code = k_exit_manifest;
            return std::nullopt;
        }
        Resolved r;
        r.project = std::move(*loaded.project);
        r.root = ".";
        dpm::resolve::PathSource source{r.root, options.schema_root};
        dpm::resolve::Options resolve_options;
        resolve_options.root_name = r.project.name;
        std::optional<dpm::manifest::Lock> previous;
        {
            std::ifstream lock_in{std::string{dpm::lock::k_lock_file}, std::ios::binary};
            if (lock_in)
            {
                std::string lock_text{std::istreambuf_iterator<char>{lock_in}, std::istreambuf_iterator<char>{}};
                dpm::manifest::LoadOptions lock_options = options;
                lock_options.kind = dpm::manifest::FileKind::Lock;
                lock_options.file_name = std::string{dpm::lock::k_lock_file};
                auto lock_loaded = dpm::manifest::load(lock_text, lock_options);
                if (lock_loaded.ok)
                    previous = std::move(lock_loaded.lock);
            }
        }
        if (previous)
            resolve_options.previous = &*previous;
        r.resolution = dpm::resolve::resolve(r.project, source, resolve_options);
        for (auto const& w : r.resolution.warnings)
            std::println(std::cerr, "warning: {}", w);
        if (!r.resolution.ok)
        {
            for (auto const& e : r.resolution.errors)
                std::println(std::cerr, "error: {}", e);
            exit_code = k_exit_cache;
            return std::nullopt;
        }
        return r;
    }

    int run_lock(char** argv, std::span<char const* const> args)
    {
        bool check = false;
        for (auto arg : args)
        {
            if (std::string_view{arg} == "--check")
                check = true;
            else
            {
                std::println(std::cerr, "usage: dpm lock [--check]");
                return k_exit_usage;
            }
        }
        int code = 0;
        auto r = resolve_here(argv, code);
        if (!r)
            return code;
        auto text = dpm::lock::print(r->resolution.lock);
        std::string existing;
        bool have = false;
        {
            std::ifstream in{std::string{dpm::lock::k_lock_file}, std::ios::binary};
            if (in)
            {
                have = true;
                existing.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
            }
        }
        if (have && existing == text)
        {
            std::println("{} is up to date", dpm::lock::k_lock_file);
            return 0;
        }
        if (check)
        {
            std::println(std::cerr, "dpm: {} {}", dpm::lock::k_lock_file, have ? "would change" : "is missing");
            return k_exit_cache;
        }
        std::string error;
        if (!dpm::edit::write_atomic(std::string{dpm::lock::k_lock_file}, text, error))
        {
            std::println(std::cerr, "dpm: {}", error);
            return k_exit_cache;
        }
        std::println("wrote {} ({} packages)", dpm::lock::k_lock_file, r->resolution.lock.packages.size());
        return 0;
    }

    int run_tree(char** argv)
    {
        int code = 0;
        auto r = resolve_here(argv, code);
        if (!r)
            return code;
        std::print("{}", dpm::resolve::render_tree(r->resolution, r->project.version));
        return 0;
    }

    int run_why(char** argv, std::span<char const* const> args)
    {
        if (args.size() != 1)
        {
            std::println(std::cerr, "usage: dpm why LOCATION");
            return k_exit_usage;
        }
        int code = 0;
        auto r = resolve_here(argv, code);
        if (!r)
            return code;
        auto text = dpm::resolve::why(r->resolution, args[0]);
        if (!text)
        {
            std::println(std::cerr, "dpm: {} is not in the build list", args[0]);
            return k_exit_cache;
        }
        std::print("{}", *text);
        return 0;
    }

    int run_cache(std::span<char const* const> args)
    {
        constexpr auto usage = "usage: dpm cache {path|verify|clean [--all]}";
        if (args.empty())
        {
            std::println(std::cerr, "{}", usage);
            return k_exit_usage;
        }
        std::string_view const sub{args[0]};
        dpm::cache::Cache cache{dpm::cache::default_root()};
        if (sub == "path" && args.size() == 1)
        {
            std::println("{}", cache.root().string());
            return 0;
        }
        if (sub == "verify" && args.size() == 1)
        {
            auto lock = cache.lock();
            int bad = 0;
            auto entries = cache.verify();
            for (auto const& entry : entries)
            {
                if (!entry.ok)
                {
                    ++bad;
                    std::println(std::cerr, "src/{}: {}", entry.name, entry.message);
                }
            }
            std::println("{} trees checked, {} bad", entries.size(), bad);
            return bad == 0 ? 0 : k_exit_cache;
        }
        if (sub == "clean" && (args.size() == 1 || (args.size() == 2 && std::string_view{args[1]} == "--all")))
        {
            auto lock = cache.lock();
            auto result = cache.clean(args.size() == 2);
            for (auto const& name : result.removed)
                std::println("removed {}", name);
            return 0;
        }
        std::println(std::cerr, "{}", usage);
        return k_exit_usage;
    }
} // namespace

auto main(int argc, char** argv) -> int
{
    if (argc >= 2 && std::string_view{argv[1]} == "check")
        return run_check(argv, std::span<char const* const>{argv + 2, static_cast<std::size_t>(argc - 2)});
    if (argc >= 2 && std::string_view{argv[1]} == "lock")
        return run_lock(argv, std::span<char const* const>{argv + 2, static_cast<std::size_t>(argc - 2)});
    if (argc == 2 && std::string_view{argv[1]} == "tree")
        return run_tree(argv);
    if (argc >= 2 && std::string_view{argv[1]} == "why")
        return run_why(argv, std::span<char const* const>{argv + 2, static_cast<std::size_t>(argc - 2)});
    if (argc >= 2 && std::string_view{argv[1]} == "cache")
        return run_cache(std::span<char const* const>{argv + 2, static_cast<std::size_t>(argc - 2)});
    std::println(std::cerr, "usage: dpm check [--tree] [project.dcon]\n       dpm lock [--check]\n       dpm tree\n       dpm why LOCATION\n       dpm cache {{path|verify|clean [--all]}}");
    return k_exit_usage;
}
