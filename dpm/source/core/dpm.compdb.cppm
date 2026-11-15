export module dpm.compdb;

import std;
import dpm.build;
import dpm.cache;
import dpm.lock;
import dpm.manifest;

export namespace dpm::compdb
{
    struct Entry
    {
        std::string product;
        std::filesystem::path directory;
        std::filesystem::path file;
        std::vector<std::string> arguments;
    };

    struct Options
    {
        std::filesystem::path schema_root;
        std::filesystem::path dcc{"dcc"};
        std::string profile{"debug"};
        bool allow_file_urls{false};
        std::optional<std::filesystem::path> cache_root;
        std::optional<std::filesystem::path> user_config;
    };

    struct Input
    {
        std::filesystem::path path;
        bool exists{false};
        std::uintmax_t size{0};
        std::filesystem::file_time_type mtime{};

        friend bool operator==(Input const&, Input const&) = default;
    };

    struct Result
    {
        bool ok{false};
        std::filesystem::path project_root;
        std::filesystem::path cache_root;
        std::vector<Entry> entries;
        std::vector<std::string> problems;
        std::vector<Input> inputs;
    };

    [[nodiscard]] inline std::filesystem::path user_config_path()
    {
        if (char const* env = std::getenv("DPM_CONFIG"); env && *env)
            return env;
#ifdef _WIN32
        if (char const* local = std::getenv("LOCALAPPDATA"); local && *local)
            return std::filesystem::path{local} / "dpm" / "config.dcon";
#endif
        if (char const* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return std::filesystem::path{xdg} / "dpm" / "config.dcon";
        if (char const* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path{home} / ".config" / "dpm" / "config.dcon";
        return {};
    }

    [[nodiscard]] inline Input stat_input(std::filesystem::path const& path)
    {
        Input in;
        in.path = path;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec)
            return in;
        in.exists = true;
        in.size = std::filesystem::file_size(path, ec);
        in.mtime = std::filesystem::last_write_time(path, ec);
        return in;
    }

    [[nodiscard]] inline bool inputs_changed(std::vector<Input> const& inputs)
    {
        return std::ranges::any_of(inputs, [](Input const& in) { return stat_input(in.path) != in; });
    }

    [[nodiscard]] inline std::optional<std::filesystem::path> find_project_root(std::filesystem::path const& file)
    {
        std::error_code ec;
        auto start = file;
        if (!std::filesystem::is_directory(start, ec))
            start = start.parent_path();
        for (auto directory = start; !directory.empty(); directory = directory.parent_path())
        {
            if (std::filesystem::is_regular_file(directory / "project.dcon", ec))
                return directory;
            if (directory == directory.root_path())
                break;
        }
        return std::nullopt;
    }

    namespace detail
    {
        [[nodiscard]] inline std::optional<std::string> read_text(std::filesystem::path const& path)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                return std::nullopt;
            auto const size = std::filesystem::file_size(path, ec);
            if (ec || size > manifest::k_max_file_size)
                return std::nullopt;
            std::ifstream in{path, std::ios::binary};
            if (!in)
                return std::nullopt;
            return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        }

        [[nodiscard]] inline std::string first_diagnostic(manifest::LoadResult const& loaded)
        {
            return loaded.diagnostics.empty() ? std::string{"invalid file"} : loaded.diagnostics.front().format();
        }
    } // namespace detail

    [[nodiscard]] inline std::filesystem::path effective_cache_root(Options const& options, std::vector<Input>* inputs = nullptr)
    {
        if (options.cache_root)
            return *options.cache_root;
        auto root = cache::default_root();
        if (char const* home = std::getenv("DPM_HOME"); home && *home)
            return std::filesystem::absolute(root);
        auto const config = options.user_config ? *options.user_config : user_config_path();
        if (config.empty())
            return std::filesystem::absolute(root);
        if (inputs)
            inputs->push_back(stat_input(config));
        if (auto text = detail::read_text(config))
        {
            manifest::LoadOptions lo;
            lo.kind = manifest::FileKind::UserConfig;
            lo.file_name = config.string();
            lo.schema_root = options.schema_root;
            lo.allow_file_urls = options.allow_file_urls;
            auto loaded = manifest::load(*text, lo);
            if (loaded.ok && loaded.user_config && !loaded.user_config->cache_dir.empty())
                root = loaded.user_config->cache_dir;
        }
        return std::filesystem::absolute(root);
    }

    [[nodiscard]] inline Result plan(std::filesystem::path const& project_root_in, Options const& options)
    {
        Result r;
        std::error_code ec;
        auto const project_root = std::filesystem::absolute(project_root_in, ec).lexically_normal();
        r.project_root = project_root;
        auto const manifest_path = project_root / "project.dcon";
        auto const lock_path = project_root / lock::k_lock_file;
        r.inputs.push_back(stat_input(manifest_path));
        r.inputs.push_back(stat_input(lock_path));
        r.cache_root = effective_cache_root(options, &r.inputs);

        auto text = detail::read_text(manifest_path);
        if (!text)
        {
            r.problems.push_back(std::format("cannot read {}", manifest_path.string()));
            return r;
        }
        manifest::LoadOptions lo;
        lo.file_name = manifest_path.string();
        lo.schema_root = options.schema_root;
        lo.project_root = project_root;
        lo.allow_file_urls = options.allow_file_urls;
        auto loaded = manifest::load(*text, lo);
        if (!loaded.ok || !loaded.project)
        {
            r.problems.push_back(std::format("project.dcon: {}", detail::first_diagnostic(loaded)));
            return r;
        }
        auto const& project = *loaded.project;

        manifest::Lock lock;
        bool const needs_packages = !project.dependencies.empty();
        if (auto lock_text = detail::read_text(lock_path))
        {
            auto lock_options = lo;
            lock_options.kind = manifest::FileKind::Lock;
            lock_options.file_name = lock_path.string();
            auto lock_loaded = manifest::load(*lock_text, lock_options);
            if (lock_loaded.ok && lock_loaded.lock)
            {
                lock = std::move(*lock_loaded.lock);
                if (!lock::is_current(project, lock))
                    r.problems.push_back("project.lock.dcon is older than project.dcon; dependencies follow the old lock until 'dpm lock' or 'dpm build' runs");
            }
            else
                r.problems.push_back(std::format("project.lock.dcon: {}", detail::first_diagnostic(lock_loaded)));
        }
        else if (needs_packages)
            r.problems.push_back("project.lock.dcon does not exist yet, so the dependencies are not known; run 'dpm lock' or 'dpm build' once");

        build::Request request;
        request.project = &project;
        request.project_root = project_root;
        request.profile = options.profile;
        request.dcc = options.dcc;
        cache::Cache cache{r.cache_root};
        for (auto const& package : lock.packages)
        {
            auto const package_dir =
                package.location.starts_with("path:") ? (project_root / package.location.substr(5)).lexically_normal() : cache.src_path(package.hash);
            r.inputs.push_back(stat_input(package_dir / "project.dcon"));
            manifest::Lock single;
            single.packages.push_back(package);
            std::string error;
            auto roots = build::package_roots(single, cache, project_root, options.schema_root, options.allow_file_urls, error);
            if (!roots)
            {
                r.problems.push_back(package.location.starts_with("path:") ? error : std::format("{} (run 'dpm fetch' to download it)", error));
                continue;
            }
            for (auto& root : *roots)
                request.packages.push_back(std::move(root));
        }

        for (auto const& product : project.products)
        {
            auto planned = build::plan_product(request, product);
            Entry e;
            e.product = product.name;
            e.directory = project_root;
            e.file = (project_root / product.entry).lexically_normal();
            e.arguments = std::move(planned.argv);
            r.entries.push_back(std::move(e));
        }
        r.ok = true;
        return r;
    }
} // namespace dpm::compdb
