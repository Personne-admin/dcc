module;

export module dccd.project_config;

import std;
import dcc.host.process;
import dccd.compilation_database;
#ifndef DCCD_NO_DCON
import dpm.compdb;
#endif

export namespace dccd::project
{
    struct Settings
    {
        std::filesystem::path schema_root;
        std::filesystem::path dcc;
        std::filesystem::path dpm;
    };

    struct FileStamp
    {
        std::filesystem::path path;
        bool exists{false};
        std::uintmax_t size{0};
        std::filesystem::file_time_type mtime{};

        friend bool operator==(FileStamp const&, FileStamp const&) = default;
    };

    [[nodiscard]] inline FileStamp stamp_of(std::filesystem::path const& path)
    {
        FileStamp s;
        s.path = path;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec)
            return s;
        s.exists = true;
        s.size = std::filesystem::file_size(path, ec);
        s.mtime = std::filesystem::last_write_time(path, ec);
        return s;
    }

    [[nodiscard]] inline std::optional<std::filesystem::path> find_root(std::filesystem::path const& file)
    {
        std::error_code ec;
        for (auto directory = file.parent_path(); !directory.empty(); directory = directory.parent_path())
        {
            if (std::filesystem::is_regular_file(directory / "project.dcon", ec))
                return directory;
            if (directory == directory.root_path())
                break;
        }
        return std::nullopt;
    }

    struct Plan
    {
        bool ok{false};
        std::filesystem::path root;
        std::filesystem::path cache_root;
        std::vector<std::string> problems;
        std::vector<FileStamp> inputs;
        std::string via;
        CompilationDatabase database;
    };

    [[nodiscard]] inline std::filesystem::path fallback_cache_root()
    {
        if (char const* home = std::getenv("DPM_HOME"); home && *home)
            return home;
#ifdef _WIN32
        if (char const* local = std::getenv("LOCALAPPDATA"); local && *local)
            return std::filesystem::path{local} / "dpm";
#endif
        if (char const* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
            return std::filesystem::path{xdg} / "dpm";
        if (char const* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path{home} / ".cache" / "dpm";
        return {};
    }

    namespace detail
    {
        [[nodiscard]] inline bool allow_file_urls()
        {
            char const* v = std::getenv("DPM_ALLOW_FILE_URLS");
            return v && *v && std::string_view{v} != "0";
        }

#ifndef DCCD_NO_DCON
        inline void plan_in_process(Plan& plan, Settings const& settings, std::ostream& log)
        {
            dpm::compdb::Options options;
            options.schema_root = settings.schema_root;
            options.dcc = settings.dcc;
            options.allow_file_urls = allow_file_urls();
            auto result = dpm::compdb::plan(plan.root, options);
            plan.via = "dpm project planner";
            plan.ok = result.ok;
            plan.cache_root = result.cache_root;
            plan.problems = std::move(result.problems);
            for (auto const& in : result.inputs)
                plan.inputs.push_back({in.path, in.exists, in.size, in.mtime});
            std::vector<CompileCommand> commands;
            for (auto& e : result.entries)
                commands.push_back({std::move(e.directory), std::move(e.file), std::move(e.arguments), std::nullopt});
            plan.database.assign(plan.root / "project.dcon", std::move(commands));
            std::ignore = log;
        }
#endif

        inline void plan_with_dpm(Plan& plan, Settings const& settings, std::ostream& log)
        {
            plan.via = "dpm compile-commands";
            plan.cache_root = fallback_cache_root();
            plan.inputs.push_back(stamp_of(plan.root / "project.dcon"));
            plan.inputs.push_back(stamp_of(plan.root / "project.lock.dcon"));
            dcc::host::process::Command cmd;
            std::error_code ec;
            cmd.argv = {std::filesystem::is_regular_file(settings.dpm, ec) ? settings.dpm.string() : std::string{"dpm"}, "compile-commands"};
            cmd.cwd = plan.root;
            cmd.inherit_env = true;
            cmd.timeout = std::chrono::seconds(20);
            auto r = dcc::host::process::run(cmd);
            for (auto const& line : std::views::split(std::string_view{r.err}, '\n'))
            {
                std::string_view text{line.begin(), line.end()};
                if (text.starts_with("warning: "))
                    plan.problems.emplace_back(text.substr(9));
                else if (!text.empty() && r.exit_code != 0)
                    plan.problems.emplace_back(text);
            }
            if (!r.started)
            {
                plan.problems.push_back(std::format("cannot run dpm to read the project settings: {}", r.error));
                return;
            }
            if (r.exit_code != 0)
                return;
            plan.ok = plan.database.load_text(r.out, plan.root / "project.dcon", log);
        }
    } // namespace detail

    class Cache
    {
    public:
        [[nodiscard]] Plan const* get(std::filesystem::path const& root, Settings const& settings, std::ostream& log, bool* replanned = nullptr)
        {
            auto const key = root.string();
            auto const now = std::chrono::steady_clock::now();
            auto it = m_items.find(key);
            if (it != m_items.end())
            {
                auto& item = it->second;
                auto const recheck = item.plan.problems.empty() ? std::chrono::milliseconds(500) : std::chrono::milliseconds(2000);
                if (now - item.checked < recheck)
                    return &item.plan;
                item.checked = now;
                bool const changed = std::ranges::any_of(item.plan.inputs, [](FileStamp const& s) { return stamp_of(s.path) != s; });
                if (!changed)
                    return &item.plan;
                std::println(log, "[dccd] project: the configuration of \"{}\" changed; reading it again", key);
            }
            Item fresh;
            fresh.checked = now;
            fresh.plan.root = root;
#ifndef DCCD_NO_DCON
            detail::plan_in_process(fresh.plan, settings, log);
#else
            detail::plan_with_dpm(fresh.plan, settings, log);
#endif
            std::println(log, "[dccd] project: \"{}\" configured from project.dcon by the {} ({} product command(s), {} problem(s))", key, fresh.plan.via,
                         fresh.plan.database.size(), fresh.plan.problems.size());
            for (auto const& problem : fresh.plan.problems)
                std::println(log, "[dccd] project: {}", problem);
            if (replanned)
                *replanned = true;
            auto& slot = m_items[key];
            slot = std::move(fresh);
            return &slot.plan;
        }

        void clear() { m_items.clear(); }

        [[nodiscard]] std::vector<CompilationDatabase const*> databases() const
        {
            std::vector<CompilationDatabase const*> out;
            for (auto const& [key, item] : m_items)
                if (item.plan.ok && !item.plan.database.empty())
                    out.push_back(&item.plan.database);
            return out;
        }

        [[nodiscard]] std::size_t size() const { return m_items.size(); }

    private:
        struct Item
        {
            Plan plan;
            std::chrono::steady_clock::time_point checked{};
        };

        std::map<std::string, Item> m_items;
    };
} // namespace dccd::project
