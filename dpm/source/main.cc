import std;
import dcc.config;
import dcc.help;
import dpm.build;
import dpm.cache;
import dpm.cli;
import dpm.edit;
import dpm.fetch;
import dpm.lock;
import dpm.manifest;
import dpm.process;
import dpm.publish;
import dpm.registry;
import dpm.resolve;
import dpm.semver;
import dpm.settings;
import dpm.stamp;

namespace
{
    namespace m = dpm::manifest;
    namespace fs = std::filesystem;

    constexpr int k_exit_failure = 1;
    constexpr int k_exit_usage = 2;
    constexpr int k_exit_manifest = 3;
    constexpr int k_exit_resolution = 4;
    constexpr int k_exit_network = 5;

    constexpr std::string_view k_project_file = "project.dcon";

    char** g_argv = nullptr;

    bool env_flag(char const* name)
    {
        char const* value = std::getenv(name);
        return value && std::string_view{value} == "1";
    }

    fs::path schema_root()
    {
        if (char const* env = std::getenv("DPM_SCHEMA_ROOT"))
            return env;
        return dcc::config::current_prefix(g_argv).path / "include";
    }

    fs::path dcc_path()
    {
        if (char const* env = std::getenv("DCC"))
            return env;
        return dcc::config::current_prefix(g_argv).path / "bin" / "dcc";
    }

    std::optional<std::string> read_file(fs::path const& path)
    {
        std::ifstream in{path, std::ios::binary};
        if (!in)
            return std::nullopt;
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    void print_diagnostics(std::vector<m::Diagnostic> const& diagnostics)
    {
        for (auto const& d : diagnostics)
            std::println(std::cerr, "{}", d.format());
    }

    struct Common
    {
        std::string profile{"debug"};
        std::optional<m::Target> target;
        std::optional<m::Optimize> optimize;
        bool offline{};
        bool refresh{};
        bool locked{};
        bool verbose{};
        bool dev{};
        bool dry_run{};
        bool check{};
        std::size_t jobs{1};
        std::chrono::seconds timeout{60};
        std::string runner;
        std::string name;
        bool lib{};
        std::string registry_dir;
        std::string location;
        std::string base;
        bool no_build{};
        bool verify_sources{};
        bool undo{};
        std::vector<std::string> positional;
        std::vector<std::string> passthrough;
    };

    std::optional<int> common_from(dpm::cli::Parsed const& parsed, Common& c, std::string& error)
    {
        using dpm::cli::Opt;
        if (auto v = parsed.value(Opt::Profile))
            c.profile = *v;
        if (auto v = parsed.value(Opt::Target))
        {
            c.target = dpm::build::parse_target(*v);
            if (!c.target)
            {
                error = std::format("unknown target '{}'", *v);
                return k_exit_usage;
            }
        }
        if (auto v = parsed.value(Opt::Optimize))
        {
            static constexpr std::array<std::pair<std::string_view, m::Optimize>, 4> levels{
                {{"0", m::Optimize::O0}, {"1", m::Optimize::O1}, {"2", m::Optimize::O2}, {"s", m::Optimize::Size}}};
            auto it = std::ranges::find(levels, *v, &std::pair<std::string_view, m::Optimize>::first);
            if (it == levels.end())
            {
                error = std::format("unknown optimization level '{}'", *v);
                return k_exit_usage;
            }
            c.optimize = it->second;
        }
        if (auto v = parsed.value(Opt::ManifestPath))
        {
            fs::path file{*v};
            if (file.filename() != k_project_file)
            {
                error = "--manifest-path must name a project.dcon";
                return k_exit_usage;
            }
            std::error_code ec;
            auto dir = file.has_parent_path() ? file.parent_path() : fs::path{"."};
            fs::current_path(dir, ec);
            if (ec)
            {
                error = std::format("cannot enter {}", dir.string());
                return k_exit_usage;
            }
        }
        c.offline = parsed.has(Opt::Offline);
        c.refresh = parsed.has(Opt::Refresh);
        c.locked = parsed.has(Opt::Locked);
        c.verbose = parsed.has(Opt::Verbose);
        c.dev = parsed.has(Opt::Dev);
        c.dry_run = parsed.has(Opt::DryRun);
        c.check = parsed.has(Opt::Check);
        c.lib = parsed.has(Opt::Lib);
        c.no_build = parsed.has(Opt::NoBuild);
        c.verify_sources = parsed.has(Opt::VerifySources);
        c.undo = parsed.has(Opt::Undo);
        c.name = parsed.value(Opt::Name).value_or("");
        c.runner = parsed.value(Opt::Runner).value_or("");
        c.registry_dir = parsed.value(Opt::RegistryDir).value_or("");
        c.location = parsed.value(Opt::Location).value_or("");
        c.base = parsed.value(Opt::Base).value_or("");
        if (auto v = parsed.value(Opt::Timeout))
            c.timeout = std::chrono::seconds{std::stoi(*v)};
        if (auto v = parsed.value(Opt::Jobs))
            c.jobs = static_cast<std::size_t>(std::stoi(*v));
        c.positional = parsed.positional;
        c.passthrough = parsed.passthrough;
        return std::nullopt;
    }

    dpm::fetch::Policy fetch_policy(Common const& c)
    {
        dpm::fetch::Policy policy;
        policy.allow_file = env_flag("DPM_ALLOW_FILE_URLS");
        policy.offline = c.offline || env_flag("DPM_OFFLINE");
        return policy;
    }

    int resolution_exit(std::string const& message)
    {
        return message.find("not in cache") != std::string::npos || message.find("offline") != std::string::npos || message.find("cannot fetch ") != std::string::npos
                   ? k_exit_network
                   : k_exit_resolution;
    }

    struct Session
    {
        Common common;
        dpm::fetch::Policy policy;
        m::LoadOptions options;
        std::string project_text;
        m::Project project;
        std::optional<m::Lock> previous;
        dpm::cache::Cache cache{dpm::cache::default_root()};
        std::optional<dpm::cache::FileLock> cache_lock;
        m::UserConfig user;
        std::unique_ptr<dpm::registry::Client> registry;
    };

    fs::path user_config_path()
    {
        if (char const* env = std::getenv("DPM_CONFIG"); env && *env)
            return env;
        if (char const* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return fs::path{xdg} / "dpm" / "config.dcon";
        if (char const* home = std::getenv("HOME"); home && *home)
            return fs::path{home} / ".config" / "dpm" / "config.dcon";
        return {};
    }

    std::optional<int> load_user_config(Session& s)
    {
        auto path = user_config_path();
        std::error_code ec;
        if (path.empty() || !fs::is_regular_file(path, ec))
            return std::nullopt;
        auto text = read_file(path);
        if (!text)
        {
            std::println(std::cerr, "dpm: cannot read {}", path.string());
            return k_exit_manifest;
        }
        auto options = s.options;
        options.kind = m::FileKind::UserConfig;
        options.file_name = path.string();
        options.project_root.clear();
        auto loaded = m::load(*text, options);
        print_diagnostics(loaded.diagnostics);
        if (!loaded.ok || !loaded.user_config)
            return k_exit_manifest;
        s.user = std::move(*loaded.user_config);
        return std::nullopt;
    }

    dpm::registry::Client* registry_client(Session& s, std::string& error)
    {
        if (s.registry)
            return s.registry.get();
        dpm::registry::Config config;
        config.schema_root = s.options.schema_root;
        config.allow_file = s.policy.allow_file;
        config.allow_loopback = env_flag("DPM_ALLOW_INSECURE_LOOPBACK");
        config.offline = s.policy.offline;
        config.refresh = s.common.refresh;
        std::optional<std::string> env;
        if (char const* value = std::getenv("DPM_REGISTRY"))
            env = std::string{value};
        auto choice = dpm::registry::choose_registries(env, s.project.registry, s.user.registries, config.allow_file, config.allow_loopback);
        if (!choice.error.empty())
        {
            error = choice.error;
            return nullptr;
        }
        config.urls = std::move(choice.urls);
        s.registry = std::make_unique<dpm::registry::Client>(s.cache, std::move(config));
        return s.registry.get();
    }

    void print_warnings(std::vector<std::string> const& warnings)
    {
        for (auto const& w : warnings)
            std::println(std::cerr, "warning: {}", w);
    }

    std::optional<m::Project> load_project_text(Session& s, std::string const& text, bool quiet = false)
    {
        auto loaded = m::load(text, s.options);
        if (!quiet)
            print_diagnostics(loaded.diagnostics);
        if (!loaded.ok || !loaded.project)
            return std::nullopt;
        return std::move(loaded.project);
    }

    std::optional<int> open_session(Session& s, Common common)
    {
        s.common = std::move(common);
        s.policy = fetch_policy(s.common);
        s.options.file_name = std::string{k_project_file};
        s.options.schema_root = schema_root();
        s.options.project_root = ".";
        s.options.allow_file_urls = s.policy.allow_file;
        if (auto code = load_user_config(s))
            return code;
        if (s.user.offline)
            s.policy.offline = true;
        auto text = read_file(k_project_file);
        if (!text)
        {
            std::println(std::cerr, "dpm: cannot read {}", k_project_file);
            return k_exit_manifest;
        }
        s.project_text = std::move(*text);
        auto project = load_project_text(s, s.project_text);
        if (!project)
            return k_exit_manifest;
        s.project = std::move(*project);
        if (auto lock_text = read_file(dpm::lock::k_lock_file))
        {
            auto lock_options = s.options;
            lock_options.kind = m::FileKind::Lock;
            lock_options.file_name = std::string{dpm::lock::k_lock_file};
            auto lock_loaded = m::load(*lock_text, lock_options);
            if (lock_loaded.ok)
                s.previous = std::move(lock_loaded.lock);
        }
        auto cache_root = dpm::cache::default_root();
        if (!s.user.cache_dir.empty() && !(std::getenv("DPM_HOME") && *std::getenv("DPM_HOME")))
            cache_root = s.user.cache_dir;
        s.cache = dpm::cache::Cache{fs::absolute(cache_root)};
        s.cache_lock = s.cache.lock();
        return std::nullopt;
    }

    struct Resolved
    {
        m::Lock lock;
        dpm::resolve::Resolution resolution;
        bool resolved{};
    };

    bool report_fetch_errors(dpm::fetch::FetchAllResult const& fetched, int& code)
    {
        if (fetched.ok)
            return true;
        code = k_exit_resolution;
        for (auto const& e : fetched.errors)
        {
            std::println(std::cerr, "error: {}", e);
            code = std::max(code, resolution_exit(e));
        }
        return false;
    }

    bool check_namespaces(Session const& s, dpm::fetch::FetchAllResult const& fetched, int& code)
    {
        std::map<std::string, std::string> owners;
        owners[s.project.name] = s.project.name;
        bool ok = true;
        for (auto const& [location, name] : fetched.names)
        {
            auto [it, inserted] = owners.emplace(name, location);
            if (!inserted)
            {
                std::println(std::cerr, "error: {} and {} both provide the namespace '{}'", it->second, location, name);
                ok = false;
            }
        }
        if (!ok)
            code = k_exit_resolution;
        return ok;
    }

    std::optional<Resolved> resolve_project(Session& s, m::Project const& project, int& code, bool fetch_sources = true)
    {
        dpm::resolve::Options options;
        options.root_name = project.name;
        if (s.previous)
            options.previous = &*s.previous;
        dpm::fetch::FetchSource fetch_source{s.cache, s.policy, ".", s.options.schema_root, s.previous ? &*s.previous : nullptr};
        std::string registry_error;
        auto* client = registry_client(s, registry_error);
        if (!client)
        {
            std::println(std::cerr, "error: {}", registry_error);
            code = k_exit_manifest;
            return std::nullopt;
        }
        dpm::registry::RegistrySource registry_source{*client, fetch_source, s.previous ? &*s.previous : nullptr};
        dpm::resolve::RequirementSource& source = client->enabled() ? static_cast<dpm::resolve::RequirementSource&>(registry_source) : fetch_source;
        Resolved r;
        r.resolution = dpm::resolve::resolve(project, source, options);
        r.resolved = true;
        print_warnings(fetch_source.warnings);
        print_warnings(registry_source.warnings);
        print_warnings(client->take_warnings());
        print_warnings(r.resolution.warnings);
        if (!r.resolution.ok)
        {
            code = k_exit_resolution;
            for (auto const& e : r.resolution.errors)
            {
                std::println(std::cerr, "error: {}", e);
                code = std::max(code, resolution_exit(e));
            }
            return std::nullopt;
        }
        r.lock = r.resolution.lock;
        if (fetch_sources)
        {
            auto fetched = dpm::fetch::fetch_locked(s.cache, r.lock, s.policy, s.options.schema_root, s.previous ? &*s.previous : nullptr);
            if (!report_fetch_errors(fetched, code) || !check_namespaces(s, fetched, code))
                return std::nullopt;
        }
        return r;
    }

    bool write_lock(m::Lock const& lock)
    {
        std::string error;
        if (!dpm::edit::write_atomic(std::string{dpm::lock::k_lock_file}, dpm::lock::print(lock), error))
        {
            std::println(std::cerr, "dpm: {}", error);
            return false;
        }
        return true;
    }

    struct Prepared
    {
        m::Lock lock;
        std::vector<dpm::build::PackageRoot> roots;
    };

    std::optional<Prepared> prepare(Session& s, int& code)
    {
        Prepared p;
        if (s.previous && dpm::lock::is_current(s.project, *s.previous))
            p.lock = *s.previous;
        else
        {
            if (s.common.locked)
            {
                std::println(std::cerr, "error: {} is out of date; run dpm lock", dpm::lock::k_lock_file);
                code = k_exit_resolution;
                return std::nullopt;
            }
            auto r = resolve_project(s, s.project, code);
            if (!r)
                return std::nullopt;
            if (!write_lock(r->lock))
            {
                code = k_exit_failure;
                return std::nullopt;
            }
            p.lock = std::move(r->lock);
        }
        auto fetched = dpm::fetch::fetch_locked(s.cache, p.lock, s.policy, s.options.schema_root, s.previous ? &*s.previous : nullptr);
        if (!report_fetch_errors(fetched, code) || !check_namespaces(s, fetched, code))
            return std::nullopt;
        std::string error;
        auto roots = dpm::build::package_roots(p.lock, s.cache, ".", s.options.schema_root, s.policy.allow_file, error);
        if (!roots)
        {
            std::println(std::cerr, "error: {}", error);
            code = k_exit_resolution;
            return std::nullopt;
        }
        p.roots = std::move(*roots);
        return p;
    }

    dpm::build::Request make_request(Session const& s, Prepared const& p, bool include_dev)
    {
        dpm::build::Request r;
        r.project = &s.project;
        r.project_root = ".";
        r.packages = p.roots;
        r.profile = s.common.profile;
        r.overrides.target = s.common.target;
        r.overrides.optimize = s.common.optimize;
        r.dcc = dcc_path();
        r.include_dev = include_dev;
        return r;
    }

    bool profile_known(m::Project const& project, std::string const& profile)
    {
        if (profile == "debug" || profile == "release")
            return true;
        return std::ranges::any_of(project.profiles, [&](m::Profile const& p) { return p.name == profile; });
    }

    std::string join_command(std::vector<std::string> const& argv)
    {
        std::string out;
        for (auto const& a : argv)
            out += (out.empty() ? "" : " ") + a;
        return out;
    }

    int run_dcc(dpm::build::Plan const& plan, bool verbose, bool capture, std::string* output = nullptr)
    {
        std::error_code ec;
        fs::create_directories(plan.output.parent_path(), ec);
        if (!fs::exists(dcc_path()))
        {
            std::println(std::cerr, "dpm: cannot find dcc at {} (set DCC)", dcc_path().string());
            return k_exit_failure;
        }
        if (verbose)
            std::println(std::cerr, "{}", join_command(plan.argv));
        dpm::process::Command cmd;
        cmd.argv = plan.argv;
        cmd.inherit_env = true;
        cmd.timeout = std::chrono::minutes(30);
        cmd.passthrough = !capture;
        auto r = dpm::process::run(cmd);
        if (output)
            *output = r.out + r.err;
        if (!r.started)
        {
            std::println(std::cerr, "dpm: {}", r.error);
            return k_exit_failure;
        }
        return r.ok() ? 0 : k_exit_failure;
    }

    void merge_compdb(std::vector<fs::path> const& entries)
    {
        if (entries.empty())
            return;
        dpm::process::Command cmd;
        cmd.argv = {dcc_path().string(), "--merge-compdb", "compile_commands.json"};
        for (auto const& e : entries)
            cmd.argv.push_back(e.generic_string());
        cmd.inherit_env = true;
        auto r = dpm::process::run(cmd);
        if (!r.ok())
            std::println(std::cerr, "warning: cannot update compile_commands.json");
    }

    m::Product const* select_product(m::Project const& project, std::vector<std::string> const& names, bool need_executable, int& code)
    {
        if (project.products.empty())
        {
            std::println(std::cerr, "error: the package has no products (it is a source library)");
            code = k_exit_usage;
            return nullptr;
        }
        if (!names.empty())
        {
            for (auto const& p : project.products)
                if (p.name == names[0])
                    return &p;
            std::println(std::cerr, "error: no product named '{}'", names[0]);
            code = k_exit_usage;
            return nullptr;
        }
        m::Product const* found = nullptr;
        for (auto const& p : project.products)
        {
            if (need_executable && p.kind != m::Kind::Executable)
                continue;
            if (found)
            {
                std::println(std::cerr, "error: several products; name one of them");
                code = k_exit_usage;
                return nullptr;
            }
            found = &p;
        }
        if (!found)
        {
            std::println(std::cerr, "error: no executable product");
            code = k_exit_usage;
        }
        return found;
    }

    std::string const& compiler_identity()
    {
        static std::string identity = []
        {
            std::string text;
            dpm::process::Command cmd;
            cmd.argv = {dcc_path().string(), "--version"};
            cmd.timeout = std::chrono::seconds(10);
            auto r = dpm::process::run(cmd);
            if (r.ok())
                text = r.out;
            std::error_code ec;
            auto size = fs::file_size(dcc_path(), ec);
            auto mtime = ec ? fs::file_time_type{} : fs::last_write_time(dcc_path(), ec);
            return std::format("{}|{}|{}", text, ec ? 0 : size, dpm::stamp::to_ns(mtime));
        }();
        return identity;
    }

    bool up_to_date(dpm::build::Plan const& plan, fs::path const& stamp_path, std::string const& key, std::string& reason)
    {
        auto stamp = dpm::stamp::read(stamp_path);
        if (!stamp)
        {
            reason = "no valid stamp";
            return false;
        }
        std::error_code ec;
        if (!fs::is_regular_file(plan.compdb_entry, ec))
        {
            reason = "the compilation database entry is missing";
            return false;
        }
        auto verdict = dpm::stamp::verify(*stamp, key, plan.output, fs::file_time_type::clock::now());
        if (verdict.state != dpm::stamp::State::Fresh)
        {
            reason = verdict.detail;
            return false;
        }
        if (verdict.refreshed)
            (void)dpm::stamp::write(stamp_path, *stamp);
        return true;
    }

    void record_stamp(dpm::build::Plan const& plan, fs::path const& stamp_path, std::string key, fs::file_time_type built_at)
    {
        auto depfile = read_file(plan.depfile);
        if (!depfile)
            return;
        auto inputs = dpm::stamp::parse_depfile(*depfile);
        if (inputs.empty())
            return;
        if (auto stamp = dpm::stamp::record(std::move(key), plan.output, inputs, built_at))
            (void)dpm::stamp::write(stamp_path, *stamp);
    }

    std::optional<std::vector<dpm::build::Plan>> build_products(Session& s, Prepared const& p, std::vector<m::Product const*> const& products, int& code)
    {
        std::vector<dpm::build::Plan> plans;
        std::vector<fs::path> entries;
        for (auto const* product : products)
        {
            auto plan = dpm::build::plan_product(make_request(s, p, false), *product);
            auto stamp_path = plan.output;
            stamp_path += ".stamp";
            auto key = dpm::stamp::make_key(plan.argv, compiler_identity(), dpm::lock::fingerprint(s.project));
            std::string reason;
            if (up_to_date(plan, stamp_path, key, reason))
            {
                std::println("    Fresh {} ({}, {})", product->name, s.common.profile, dpm::build::target_dir(plan.settings.target));
                entries.push_back(plan.compdb_entry);
                plans.push_back(std::move(plan));
                continue;
            }
            std::println("    Building {} ({}, {})", product->name, s.common.profile, dpm::build::target_dir(plan.settings.target));
            if (s.common.verbose)
                std::println(std::cerr, "dpm: rebuilding {}: {}", product->name, reason);
            std::fflush(nullptr);
            dpm::stamp::remove(stamp_path);
            auto built_at = fs::file_time_type::clock::now();
            if (run_dcc(plan, s.common.verbose, false) != 0)
            {
                code = k_exit_failure;
                return std::nullopt;
            }
            record_stamp(plan, stamp_path, std::move(key), built_at);
            entries.push_back(plan.compdb_entry);
            plans.push_back(std::move(plan));
        }
        merge_compdb(entries);
        return plans;
    }

    int cmd_build(Common common)
    {
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        if (!profile_known(s.project, s.common.profile))
        {
            std::println(std::cerr, "error: unknown profile '{}'", s.common.profile);
            return k_exit_usage;
        }
        int code = 0;
        std::vector<m::Product const*> products;
        if (s.common.positional.empty())
        {
            if (s.project.products.empty())
            {
                std::println(std::cerr, "error: the package has no products (it is a source library)");
                return k_exit_usage;
            }
            for (auto const& p : s.project.products)
                products.push_back(&p);
        }
        else
        {
            auto* product = select_product(s.project, s.common.positional, false, code);
            if (!product)
                return code;
            products.push_back(product);
        }
        auto prepared = prepare(s, code);
        if (!prepared)
            return code;
        if (!build_products(s, *prepared, products, code))
            return code;
        return 0;
    }

    int cmd_run(Common common)
    {
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        if (!profile_known(s.project, s.common.profile))
        {
            std::println(std::cerr, "error: unknown profile '{}'", s.common.profile);
            return k_exit_usage;
        }
        int code = 0;
        auto* product = select_product(s.project, s.common.positional, true, code);
        if (!product)
            return code;
        if (product->kind != m::Kind::Executable)
        {
            std::println(std::cerr, "error: '{}' is not an executable product", product->name);
            return k_exit_usage;
        }
        auto effective = dpm::settings::effective(s.project, s.common.profile, product, {s.common.target, s.common.optimize});
        if (effective.target != m::Target::Host && s.common.runner.empty())
        {
            std::println(std::cerr, "error: '{}' targets {} and cannot run on this machine; pass --runner CMD", product->name, dpm::build::target_dir(effective.target));
            return k_exit_usage;
        }
        auto prepared = prepare(s, code);
        if (!prepared)
            return code;
        auto plans = build_products(s, *prepared, {product}, code);
        if (!plans)
            return code;
        dpm::process::Command cmd;
        if (!s.common.runner.empty())
        {
            std::istringstream words{s.common.runner};
            for (std::string w; words >> w;)
                cmd.argv.push_back(w);
        }
        cmd.argv.push_back(fs::absolute((*plans)[0].output).string());
        for (auto const& a : s.common.passthrough)
            cmd.argv.push_back(a);
        cmd.inherit_env = true;
        cmd.passthrough = true;
        cmd.timeout = std::chrono::hours(24);
        auto r = dpm::process::run(cmd);
        if (!r.started)
        {
            std::println(std::cerr, "dpm: {}", r.error);
            return k_exit_failure;
        }
        if (r.signal != 0)
            return 128 + r.signal;
        return r.exit_code;
    }

    int cmd_test(Common common)
    {
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        if (!profile_known(s.project, s.common.profile))
        {
            std::println(std::cerr, "error: unknown profile '{}'", s.common.profile);
            return k_exit_usage;
        }
        std::vector<fs::path> tests;
        std::error_code ec;
        if (fs::is_directory(s.project.test_dir, ec))
            for (auto const& entry : fs::directory_iterator{s.project.test_dir, ec})
                if (entry.is_regular_file() && entry.path().extension() == ".dc")
                    tests.push_back(entry.path());
        std::ranges::sort(tests);
        if (!s.common.positional.empty())
        {
            std::vector<fs::path> chosen;
            for (auto const& name : s.common.positional)
            {
                auto it = std::ranges::find_if(tests, [&](fs::path const& t) { return t.stem() == name; });
                if (it == tests.end())
                {
                    std::println(std::cerr, "error: no test named '{}'", name);
                    return k_exit_usage;
                }
                chosen.push_back(*it);
            }
            tests = std::move(chosen);
        }
        if (tests.empty())
        {
            std::println("no tests found in {}", s.project.test_dir);
            return 0;
        }
        int code = 0;
        auto prepared = prepare(s, code);
        if (!prepared)
            return code;
        auto request = make_request(s, *prepared, true);
        struct Outcome
        {
            bool built{};
            bool passed{};
            std::string detail;
        };
        std::vector<Outcome> outcomes(tests.size());
        std::vector<dpm::build::Plan> plans;
        for (auto const& t : tests)
            plans.push_back(dpm::build::plan_test(request, t));
        std::atomic<std::size_t> next{0};
        std::mutex print_mutex;
        auto effective_target = plans[0].settings.target;
        auto worker = [&]
        {
            for (;;)
            {
                auto i = next.fetch_add(1);
                if (i >= tests.size())
                    return;
                std::string output;
                auto& plan = plans[i];
                Outcome o;
                if (run_dcc(plan, false, true, &output) != 0)
                {
                    o.detail = output;
                }
                else
                {
                    o.built = true;
                    dpm::process::Command cmd;
                    if (!s.common.runner.empty())
                    {
                        std::istringstream words{s.common.runner};
                        for (std::string w; words >> w;)
                            cmd.argv.push_back(w);
                    }
                    cmd.argv.push_back(fs::absolute(plan.output).string());
                    cmd.inherit_env = true;
                    cmd.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(s.common.timeout);
                    auto r = dpm::process::run(cmd);
                    o.passed = r.ok();
                    o.detail = r.out + r.err;
                    if (r.timed_out)
                        o.detail += "timed out\n";
                    else if (r.signal != 0)
                        o.detail += std::format("killed by signal {}\n", r.signal);
                    else if (r.exit_code != 0)
                        o.detail += std::format("exit code {}\n", r.exit_code);
                }
                std::lock_guard lock{print_mutex};
                std::println("{} {}", o.passed ? "ok  " : "FAIL", tests[i].stem().string());
                if (!o.passed && !o.detail.empty())
                    std::println("{}", o.detail);
                outcomes[i] = std::move(o);
            }
        };
        if (effective_target != m::Target::Host && s.common.runner.empty())
        {
            std::println(std::cerr, "error: tests target {} and cannot run on this machine; pass --runner CMD", dpm::build::target_dir(effective_target));
            return k_exit_usage;
        }
        std::vector<std::thread> threads;
        for (std::size_t i = 1; i < std::min(s.common.jobs, tests.size()); ++i)
            threads.emplace_back(worker);
        worker();
        for (auto& t : threads)
            t.join();
        std::size_t passed = static_cast<std::size_t>(std::ranges::count_if(outcomes, [](Outcome const& o) { return o.passed; }));
        std::println("{} passed, {} failed", passed, tests.size() - passed);
        return passed == tests.size() ? 0 : k_exit_failure;
    }

    int cmd_clean(Common common)
    {
        (void)common;
        std::error_code ec;
        fs::remove_all(fs::path{".dpm"} / "out", ec);
        if (ec)
        {
            std::println(std::cerr, "dpm: cannot remove .dpm/out");
            return k_exit_failure;
        }
        return 0;
    }

    std::string identifier_from(std::string_view text)
    {
        std::string out;
        for (char c : text)
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            else if (!out.empty() && out.back() != '_')
                out += '_';
        }
        while (!out.empty() && out.back() == '_')
            out.pop_back();
        if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front())))
            out.insert(0, "app_");
        return out;
    }

    int cmd_init(Common common)
    {
        fs::path dir = common.positional.empty() ? fs::path{"."} : fs::path{common.positional[0]};
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
        {
            std::println(std::cerr, "dpm: cannot create {}", dir.string());
            return k_exit_failure;
        }
        if (fs::exists(dir / k_project_file))
        {
            std::println(std::cerr, "error: {} already exists", (dir / k_project_file).string());
            return k_exit_usage;
        }
        auto name = common.name.empty() ? identifier_from(fs::absolute(dir).lexically_normal().filename().empty() ? fs::absolute(dir).parent_path().filename().string()
                                                                                                                    : fs::absolute(dir).lexically_normal().filename().string())
                                        : common.name;
        std::string manifest = std::format("dpm::v1::Project {{\n    name = \"{}\",\n    version = \"0.1.0\",\n", name);
        if (!common.lib)
            manifest += std::format("    products = {{\n        {{ name = \"{}\", entry = \"src/main.dc\" }},\n    }},\n", name);
        manifest += "}\n";
        Session probe;
        probe.options.schema_root = schema_root();
        probe.options.project_root = dir;
        probe.options.kind = m::FileKind::Project;
        std::string entry_text = common.lib ? std::format("module {}::lib;\n\npublic i32 answer()\n{{\n    return 42;\n}}\n", name)
                                            : std::string{"module main;\n\npublic i32 main()\n{\n    return 0;\n}\n"};
        fs::path entry_path = common.lib ? fs::path{"src"} / name / "lib.dc" : fs::path{"src"} / "main.dc";
        fs::create_directories(dir / entry_path.parent_path(), ec);
        {
            std::ofstream f{dir / entry_path, std::ios::binary};
            f << entry_text;
        }
        auto check = m::load(manifest, probe.options);
        if (!check.ok)
        {
            print_diagnostics(check.diagnostics);
            std::println(std::cerr, "error: '{}' is not usable as a package name; pass --name", name);
            fs::remove(dir / entry_path, ec);
            return k_exit_usage;
        }
        std::string error;
        if (!dpm::edit::write_atomic(dir / k_project_file, manifest, error))
        {
            std::println(std::cerr, "dpm: {}", error);
            return k_exit_failure;
        }
        auto ignore = read_file(dir / ".gitignore").value_or("");
        if (ignore.find(".dpm/") == std::string::npos)
        {
            std::ofstream f{dir / ".gitignore", std::ios::binary | std::ios::app};
            if (!ignore.empty() && ignore.back() != '\n')
                f << '\n';
            f << ".dpm/\n";
        }
        std::println("created {} {}", common.lib ? "library" : "executable", name);
        return 0;
    }

    int cmd_lock(Common common, bool check)
    {
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        int code = 0;
        auto r = resolve_project(s, s.project, code);
        if (!r)
            return code;
        auto text = dpm::lock::print(r->lock);
        auto existing = read_file(dpm::lock::k_lock_file);
        if (existing && *existing == text)
        {
            std::println("{} is up to date", dpm::lock::k_lock_file);
            return 0;
        }
        if (check)
        {
            std::println(std::cerr, "dpm: {} {}", dpm::lock::k_lock_file, existing ? "would change" : "is missing");
            return k_exit_resolution;
        }
        if (!write_lock(r->lock))
            return k_exit_failure;
        std::println("wrote {} ({} packages)", dpm::lock::k_lock_file, r->lock.packages.size());
        return 0;
    }

    int cmd_tree(Common common)
    {
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        int code = 0;
        auto r = resolve_project(s, s.project, code, false);
        if (!r)
            return code;
        std::print("{}", dpm::resolve::render_tree(r->resolution, s.project.version));
        return 0;
    }

    int cmd_why(Common common)
    {
        if (common.positional.size() != 1)
        {
            std::println(std::cerr, "usage: dpm why LOCATION");
            return k_exit_usage;
        }
        Session s;
        auto location = common.positional[0];
        if (auto code = open_session(s, std::move(common)))
            return *code;
        int code = 0;
        auto r = resolve_project(s, s.project, code, false);
        if (!r)
            return code;
        auto text = dpm::resolve::why(r->resolution, location);
        if (!text)
        {
            std::println(std::cerr, "dpm: {} is not in the build list", location);
            return k_exit_resolution;
        }
        std::print("{}", *text);
        return 0;
    }

    int cmd_fetch(Common common)
    {
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        if (!s.previous)
        {
            std::println(std::cerr, "dpm: cannot read {} (run dpm lock)", dpm::lock::k_lock_file);
            return k_exit_manifest;
        }
        auto result = dpm::fetch::fetch_locked(s.cache, *s.previous, s.policy, s.options.schema_root, &*s.previous);
        int code = 0;
        report_fetch_errors(result, code);
        if (result.ok)
            check_namespaces(s, result, code);
        std::println("{} fetched, {} already in the cache", result.fetched, result.present);
        return code;
    }

    struct Edited
    {
        std::string text;
        m::Project project;
    };

    int commit_edit(Session& s, std::string const& new_text)
    {
        auto project = load_project_text(s, new_text);
        if (!project)
            return k_exit_manifest;
        int code = 0;
        auto r = resolve_project(s, *project, code);
        if (!r)
            return code;
        std::string error;
        if (!dpm::edit::write_atomic(std::string{k_project_file}, new_text, error))
        {
            std::println(std::cerr, "dpm: {}", error);
            return k_exit_failure;
        }
        if (!write_lock(r->lock))
            return k_exit_failure;
        return 0;
    }

    std::optional<std::string> alias_latest(Session& s, std::string const& alias, std::optional<dpm::semver::Series> within, std::string& error)
    {
        auto* client = registry_client(s, error);
        if (!client)
            return std::nullopt;
        if (!client->enabled())
        {
            error = "no registry is configured, so the alias cannot be resolved";
            return std::nullopt;
        }
        auto hit = client->alias(alias, error);
        print_warnings(client->take_warnings());
        if (!hit)
            return std::nullopt;
        auto latest = dpm::registry::latest_version(hit->record, within);
        if (!latest)
            error = std::format("alias '{}' has no release to install", alias);
        return latest;
    }

    int cmd_add(Common common)
    {
        if (common.positional.size() != 1)
        {
            std::println(std::cerr, "usage: dpm add LOCATION[@VERSION] [--dev]");
            return k_exit_usage;
        }
        auto spec = common.positional[0];
        bool dev = common.dev;
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        std::string location = spec;
        std::string version;
        if (auto at = spec.rfind('@'); at != std::string::npos && at + 1 < spec.size() && std::isdigit(static_cast<unsigned char>(spec[at + 1])))
        {
            location = spec.substr(0, at);
            version = spec.substr(at + 1);
        }
        bool const alias = location.find('/') == std::string::npos && location.find('.') == std::string::npos && location.find(':') == std::string::npos;
        if (alias)
        {
            std::string registry_error;
            if (!registry_client(s, registry_error))
            {
                std::println(std::cerr, "error: {}", registry_error);
                return k_exit_manifest;
            }
        }
        if (!location.starts_with("path:") && version.empty())
        {
            std::string error;
            std::optional<std::string> latest;
            if (alias)
                latest = alias_latest(s, location, std::nullopt, error);
            else
                latest = dpm::fetch::latest_version(s.cache, location, s.policy, std::nullopt, error);
            if (!latest)
            {
                std::println(std::cerr, "error: cannot find a version of {}: {}", location, error);
                return resolution_exit(error);
            }
            version = *latest;
        }
        m::Dependency dep;
        dep.location = location;
        dep.version = location.starts_with("path:") ? std::string{} : version;
        auto edit = dpm::edit::add_dependency(s.project_text, s.options, dev ? dpm::edit::List::DevDependencies : dpm::edit::List::Dependencies, dep);
        if (!edit.ok)
        {
            print_diagnostics(edit.diagnostics);
            std::println(std::cerr, "error: {}", edit.error);
            return k_exit_manifest;
        }
        if (auto code = commit_edit(s, edit.text); code != 0)
            return code;
        std::println("added {}{}{}", location, dep.version.empty() ? "" : "@", dep.version);
        return 0;
    }

    int cmd_remove(Common common)
    {
        if (common.positional.size() != 1)
        {
            std::println(std::cerr, "usage: dpm remove LOCATION [--dev]");
            return k_exit_usage;
        }
        auto location = common.positional[0];
        bool dev = common.dev;
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        auto edit = dpm::edit::remove_dependency(s.project_text, s.options, dev ? dpm::edit::List::DevDependencies : dpm::edit::List::Dependencies, location);
        if (!edit.ok)
        {
            print_diagnostics(edit.diagnostics);
            std::println(std::cerr, "error: {}", edit.error);
            return k_exit_manifest;
        }
        if (auto code = commit_edit(s, edit.text); code != 0)
            return code;
        std::println("removed {}", location);
        return 0;
    }

    int cmd_update(Common common)
    {
        auto wanted = common.positional;
        bool dry_run = common.dry_run;
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        std::string text = s.project_text;
        std::size_t changes = 0;
        for (auto list : {dpm::edit::List::Dependencies, dpm::edit::List::DevDependencies})
        {
            auto const& deps = list == dpm::edit::List::Dependencies ? s.project.dependencies : s.project.dev_dependencies;
            for (auto const& d : deps)
            {
                if (d.location.starts_with("path:"))
                    continue;
                if (!wanted.empty() && std::ranges::find(wanted, d.location) == wanted.end())
                    continue;
                auto current = dpm::semver::parse(d.version);
                if (!current)
                    continue;
                std::string error;
                bool const alias = d.location.find('/') == std::string::npos && d.location.find('.') == std::string::npos && d.location.find(':') == std::string::npos;
                auto latest = alias ? alias_latest(s, d.location, dpm::semver::series_of(*current), error)
                                    : dpm::fetch::latest_version(s.cache, d.location, s.policy, dpm::semver::series_of(*current), error);
                if (!latest)
                {
                    std::println(std::cerr, "warning: {}: {}", d.location, error);
                    continue;
                }
                auto next = dpm::semver::parse(*latest);
                if (!next || dpm::semver::compare(*next, *current) <= 0)
                    continue;
                std::println("{} {} -> {}", d.location, d.version, *latest);
                ++changes;
                if (dry_run)
                    continue;
                auto edit = dpm::edit::set_dependency_version(text, s.options, list, d.location, *latest);
                if (!edit.ok)
                {
                    std::println(std::cerr, "error: {}", edit.error);
                    return k_exit_manifest;
                }
                text = std::move(edit.text);
            }
        }
        if (changes == 0)
        {
            std::println("everything is up to date");
            return 0;
        }
        if (dry_run)
            return 0;
        return commit_edit(s, text);
    }

    int cmd_check(dpm::cli::Parsed const& parsed)
    {
        bool tree = parsed.has(dpm::cli::Opt::Tree);
        fs::path file = parsed.positional.empty() ? fs::path{k_project_file} : fs::path{parsed.positional[0]};
        auto text = read_file(file);
        if (!text)
        {
            std::println(std::cerr, "dpm: cannot read {}", file.string());
            return k_exit_manifest;
        }
        m::LoadOptions options;
        options.file_name = file.string();
        options.schema_root = schema_root();
        options.project_root = file.has_parent_path() ? file.parent_path() : fs::path{"."};
        options.kind = m::detect_kind(*text, file.string());
        options.allow_file_urls = env_flag("DPM_ALLOW_FILE_URLS");
        auto result = m::load(*text, options);
        print_diagnostics(result.diagnostics);
        if (!result.ok)
            return k_exit_manifest;
        if (tree)
            std::print("{}", result.tree.dump(true));
        return 0;
    }

    int cmd_cache(dpm::cli::Parsed const& parsed)
    {
        std::string_view const sub = std::string_view{parsed.command}.substr(6);
        dpm::cache::Cache cache{dpm::cache::default_root()};
        if (sub == "path")
        {
            std::println("{}", cache.root().string());
            return 0;
        }
        if (sub == "verify")
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
            return bad == 0 ? 0 : k_exit_resolution;
        }
        if (sub == "clean")
        {
            auto lock = cache.lock();
            auto result = cache.clean(parsed.has(dpm::cli::Opt::All));
            for (auto const& name : result.removed)
                std::println("removed {}", name);
            return 0;
        }
        return k_exit_usage;
    }

    std::string dcc_version()
    {
        dpm::process::Command cmd;
        cmd.argv = {dcc_path().string(), "--version"};
        cmd.timeout = std::chrono::seconds(10);
        auto r = dpm::process::run(cmd);
        if (!r.ok())
            return {};
        std::string_view text{r.out};
        if (!text.starts_with("dcc "))
            return {};
        text.remove_prefix(4);
        auto end = text.find_first_of(" \n");
        auto version = std::string{text.substr(0, end)};
        return dpm::semver::parse(version) ? version : std::string{};
    }

    dpm::publish::Context publish_context()
    {
        dpm::publish::Context ctx;
        ctx.schema_root = schema_root();
        ctx.allow_file = env_flag("DPM_ALLOW_FILE_URLS");
        return ctx;
    }

    std::optional<fs::path> pick_registry_dir(Common const& c)
    {
        if (!c.registry_dir.empty())
            return fs::path{c.registry_dir};
        std::error_code ec;
        if (fs::is_directory("dc-registry", ec))
            return fs::path{"dc-registry"};
        return std::nullopt;
    }

    void print_written(fs::path const& dir, dpm::publish::Written const& written, bool dry_run, std::string const& branch, std::string const& message)
    {
        if (!written.changed)
        {
            std::println("nothing to change in {}", dir.string());
            return;
        }
        for (auto const& f : written.files)
            std::println("{} {}", dry_run ? "would write" : "wrote", (dir / f).string());
        if (dry_run)
            return;
        std::println("\ndpm did not run git. To propose the change, run:\n");
        for (auto const& line : dpm::publish::git_commands(dir, written, branch, message))
            std::println("    {}", line);
    }

    int cmd_publish(Common common)
    {
        auto registry_dir = pick_registry_dir(common);
        if (!registry_dir)
        {
            std::println(std::cerr, "error: pass --registry-dir DIR (no ./dc-registry here)");
            return k_exit_usage;
        }
        bool dry_run = common.dry_run;
        bool no_build = common.no_build;
        auto location = common.location;
        Session s;
        if (auto code = open_session(s, std::move(common)))
            return *code;
        dpm::publish::PublishInput input;
        input.project = &s.project;
        input.project_dir = ".";
        input.registry_dir = *registry_dir;
        input.location = location;
        input.dcc_version = dcc_version();
        input.cache = s.cache;
        input.policy = s.policy;
        input.ctx = publish_context();
        std::string error;
        auto prepared = dpm::publish::prepare(input, error);
        if (!prepared)
        {
            std::println(std::cerr, "error: {}", error);
            return resolution_exit(error) == k_exit_network ? k_exit_network : k_exit_failure;
        }
        if (!no_build && !s.project.products.empty())
        {
            int code = 0;
            auto built = prepare(s, code);
            if (!built)
                return code;
            std::vector<m::Product const*> products;
            for (auto const& p : s.project.products)
                products.push_back(&p);
            if (!build_products(s, *built, products, code))
                return code;
        }
        auto written = dpm::publish::write_entry(*prepared, input, dry_run, error);
        if (!written)
        {
            std::println(std::cerr, "error: {}", error);
            return k_exit_failure;
        }
        std::println("{} {} -> {}", prepared->alias, s.project.version, prepared->location);
        std::println("  tag {}  commit {}  hash {}", prepared->tag, prepared->entry.commit, prepared->entry.hash);
        if (!written->changed)
        {
            std::println("version {} is already in the registry with these contents", s.project.version);
            return 0;
        }
        print_written(*registry_dir, *written, dry_run, std::format("publish-{}-{}", prepared->alias, s.project.version),
                      std::format("add {} {}", prepared->alias, s.project.version));
        return 0;
    }

    int cmd_registry(Common common, std::string_view sub)
    {
        std::string error;
        auto ctx = publish_context();
        if (sub == "init")
        {
            auto initialized = dpm::publish::init_registry(common.positional[0], common.name, ctx, error);
            if (!initialized)
            {
                std::println(std::cerr, "error: {}", error);
                return k_exit_failure;
            }
            std::println("initialized the registry in {}", common.positional[0]);
            std::println("wrote {}/registry.dcon (aliases/ and locations/ appear when the first alias is added; git does not track empty directories)", common.positional[0]);
            return 0;
        }
        auto dir = pick_registry_dir(common);
        if (sub == "alias" || sub == "yank")
        {
            bool const alias = sub == "alias";
            if (!dir)
            {
                std::println(std::cerr, "error: pass --registry-dir DIR (no ./dc-registry here)");
                return k_exit_usage;
            }
            std::optional<dpm::publish::Written> written;
            std::string branch;
            std::string message;
            if (alias)
            {
                written = dpm::publish::create_alias(*dir, common.positional[0], common.positional[1], {}, ctx, common.dry_run, error);
                branch = "alias-" + common.positional[1];
                message = "add alias " + common.positional[1];
            }
            else
            {
                written = dpm::publish::set_yanked(*dir, common.positional[0], common.positional[1], !common.undo, ctx, common.dry_run, error);
                branch = std::format("{}-{}-{}", common.undo ? "unyank" : "yank", common.positional[0], common.positional[1]);
                message = std::format("{} {} {}", common.undo ? "unyank" : "yank", common.positional[0], common.positional[1]);
            }
            if (!written)
            {
                std::println(std::cerr, "error: {}", error);
                return k_exit_failure;
            }
            print_written(*dir, *written, common.dry_run, branch, message);
            return 0;
        }
        if (sub == "check")
        {
            fs::path root = !common.positional.empty() ? fs::path{common.positional[0]} : dir ? *dir : fs::path{"."};
            auto scan = dpm::publish::scan_registry(root, ctx);
            auto problems = scan.problems;
            if (common.verify_sources)
            {
                dpm::publish::VerifyRequest request;
                request.policy = fetch_policy(common);
                request.ctx = ctx;
                request.base = common.base;
                request.cache = dpm::cache::Cache{fs::absolute(dpm::cache::default_root())};
                auto lock = request.cache.lock();
                request.base = common.base;
                for (auto& p : dpm::publish::verify_sources(root, scan, request))
                    problems.push_back(std::move(p));
            }
            int code = 0;
            for (auto const& p : problems)
            {
                std::println(std::cerr, "{}", p.format());
                code = std::max(code, resolution_exit(p.message));
            }
            std::size_t versions = 0;
            for (auto const& [name, record] : scan.aliases)
                versions += record.versions.size();
            std::println("{} aliases, {} versions, {} location pointers, {} problems", scan.aliases.size(), versions, scan.pointers.size(), problems.size());
            return code;
        }
        return k_exit_usage;
    }

} // namespace

auto main(int argc, char** argv) -> int
{
    g_argv = argv;
    std::span<char const* const> args{argv + 1, static_cast<std::size_t>(argc - 1)};
    auto parsed = dpm::cli::parse(args);
    if (!parsed)
    {
        auto const& failure = parsed.error();
        if (!failure.message.empty())
            std::println(std::cerr, "{}", failure.message);
        else
            std::print(std::cerr, "{}", dpm::cli::help_text(failure.topic, dcc::help::terminal_width()));
        return k_exit_usage;
    }
    if (parsed->help)
    {
        std::print("{}", dpm::cli::help_text(parsed->help_topic, dcc::help::terminal_width()));
        return 0;
    }
    auto const command = parsed->command;
    if (command == "check")
        return cmd_check(*parsed);
    if (command.starts_with("cache "))
        return cmd_cache(*parsed);
    Common common;
    std::string error;
    if (auto code = common_from(*parsed, common, error))
    {
        std::println(std::cerr, "dpm {}: {}", command, error);
        return *code;
    }
    if (command.starts_with("registry "))
        return cmd_registry(std::move(common), std::string_view{command}.substr(9));
    if (command == "init")
        return cmd_init(std::move(common));
    if (command == "publish")
        return cmd_publish(std::move(common));
    if (command == "add")
        return cmd_add(std::move(common));
    if (command == "remove")
        return cmd_remove(std::move(common));
    if (command == "update")
        return cmd_update(std::move(common));
    if (command == "lock")
    {
        bool check = common.check;
        return cmd_lock(std::move(common), check);
    }
    if (command == "fetch")
        return cmd_fetch(std::move(common));
    if (command == "tree")
        return cmd_tree(std::move(common));
    if (command == "why")
        return cmd_why(std::move(common));
    if (command == "build")
        return cmd_build(std::move(common));
    if (command == "run")
        return cmd_run(std::move(common));
    if (command == "test")
        return cmd_test(std::move(common));
    if (command == "clean")
        return cmd_clean(std::move(common));
    return k_exit_usage;
}
