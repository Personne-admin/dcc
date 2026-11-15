export module learndc.app;

import std;
import learndc.text;
import learndc.markdown;
import learndc.json;
import learndc.platform;
import learndc.catalog;
import learndc.bundle;
import learndc.settings;
import learndc.progress;
import learndc.workspace;
import learndc.toolchain;
import learndc.runner;

export namespace learndc::app
{
    inline constexpr int exit_ok = 0;
    inline constexpr int exit_failed = 1;
    inline constexpr int exit_usage = 2;
    inline constexpr int exit_data = 3;
    inline constexpr int exit_toolchain = 4;
    inline constexpr int exit_not_found = 5;
    inline constexpr int exit_refused = 6;
    inline constexpr int exit_interrupted = 130;

    struct Failure
    {
        int code = exit_failed;
        std::string message;
    };

    template <typename T>
    using Result = std::expected<T, Failure>;

    [[nodiscard]] inline std::unexpected<Failure> failure(int code, std::string message)
    {
        return std::unexpected(Failure{code, std::move(message)});
    }

    [[nodiscard]] inline int exit_code_for(workspace::ErrorKind kind)
    {
        switch (kind)
        {
            case workspace::ErrorKind::InvalidPath:
                return exit_usage;
            case workspace::ErrorKind::Unsafe:
            case workspace::ErrorKind::Refused:
                return exit_refused;
            case workspace::ErrorKind::NotFound:
                return exit_not_found;
            case workspace::ErrorKind::Corrupt:
                return exit_data;
            case workspace::ErrorKind::Io:
                return exit_failed;
        }
        return exit_failed;
    }

    [[nodiscard]] inline Failure failure_of(workspace::WorkspaceError const& e)
    {
        return Failure{exit_code_for(e.kind), e.message};
    }

    struct OpenOptions
    {
        std::optional<std::filesystem::path> home;
        std::optional<std::filesystem::path> content_dir;
        platform::Env env;
        std::optional<std::filesystem::path> self_exe;
        platform::Os os = platform::host_os();
    };

    struct Context
    {
        platform::Dirs dirs;
        platform::Env env;
        platform::Os os = platform::host_os();
        settings::Settings settings;
        toolchain::Toolchain tools;
        catalog::ContentSource source;
        catalog::Catalog catalog;
        std::string content_error;
        progress::ProgressStore store;
        std::vector<std::string> warnings;

        explicit Context(platform::Dirs d) : dirs(std::move(d)), store(dirs.progress_file, dirs.progress_lock) {}

        [[nodiscard]] bool has_content() const { return content_error.empty(); }
    };

    [[nodiscard]] inline Result<Context> open(OpenOptions const& options)
    {
        platform::Overrides overrides;
        overrides.home = options.home;
        auto dirs = platform::resolve_dirs(overrides, options.env, options.os);
        if (!dirs)
            return failure(exit_data, dirs.error().message);
        Context ctx(std::move(*dirs));
        ctx.env = options.env;
        ctx.os = options.os;
        auto loaded_settings = settings::load(ctx.dirs.config_file);
        if (!loaded_settings)
        {
            if (!platform::read_file(ctx.dirs.config_file))
                return failure(exit_data, loaded_settings.error().message);
            auto stamp = platform::now_utc_iso8601();
            std::erase_if(stamp, [](char c) { return c == '-' || c == ':'; });
            auto quarantine = ctx.dirs.config_file;
            quarantine += std::format(".corrupt-{}", stamp);
            for (int n = 1; std::filesystem::exists(quarantine); ++n)
            {
                quarantine = ctx.dirs.config_file;
                quarantine += std::format(".corrupt-{}-{}", stamp, n);
            }
            std::error_code ec;
            std::filesystem::rename(ctx.dirs.config_file, quarantine, ec);
            if (ec)
                return failure(exit_data, std::format("{} and could not be moved aside: {}", loaded_settings.error().message, ec.message()));
            ctx.warnings.push_back(std::format("the settings file was damaged ({}); it was moved to '{}' and the defaults are in use",
                                               loaded_settings.error().message, platform::path_to_utf8(quarantine)));
            loaded_settings = settings::Settings{};
        }
        ctx.settings = std::move(*loaded_settings);
        ctx.tools = toolchain::resolve(toolchain::Inputs{ctx.settings, ctx.env, options.self_exe, options.os});

        std::optional<std::filesystem::path> content_dir = options.content_dir;
        if (!content_dir)
            if (auto it = options.env.find("LEARNDC_CONTENT_DIR"); it != options.env.end() && !it->second.empty())
                content_dir = platform::path_from_utf8(it->second);
        if (content_dir)
        {
            auto src = catalog::load_directory(*content_dir);
            if (!src)
                ctx.content_error = src.error().message;
            else
                ctx.source = std::move(*src);
        }
        else
        {
            auto src = bundle::embedded();
            if (!src)
                ctx.content_error = src.error().message;
            else
                ctx.source = std::move(*src);
        }
        if (ctx.content_error.empty())
        {
            auto cat = catalog::Catalog::load(ctx.source);
            if (!cat)
                ctx.content_error = catalog::format_errors(cat.error());
            else
                ctx.catalog = std::move(*cat);
        }
        return Result<Context>(std::move(ctx));
    }

    [[nodiscard]] inline std::optional<Failure> require_content(Context const& ctx)
    {
        if (ctx.has_content())
            return std::nullopt;
        return Failure{exit_data, "the exercise content could not be loaded:\n" + ctx.content_error};
    }

    [[nodiscard]] inline Result<progress::Progress> load_progress(Context& ctx)
    {
        auto loaded = ctx.store.load();
        if (!loaded)
            return failure(exit_data, loaded.error().message);
        for (auto& w : loaded->warnings)
            ctx.warnings.push_back(w);
        return std::move(loaded->progress);
    }

    [[nodiscard]] inline Result<progress::Progress> update_progress(Context& ctx, std::function<void(progress::Progress&)> const& change)
    {
        auto loaded = ctx.store.update(change);
        if (!loaded)
        {
            auto const& m = loaded.error().message;
            bool const busy = m.find("lock") != std::string::npos;
            return failure(busy ? exit_failed : exit_data, m);
        }
        for (auto& w : loaded->warnings)
            ctx.warnings.push_back(w);
        return std::move(loaded->progress);
    }

    [[nodiscard]] inline std::function<bool(std::string_view)> completed_predicate(progress::Progress const& p)
    {
        return [&p](std::string_view id) { return progress::is_completed(p, id); };
    }

    [[nodiscard]] inline Failure unknown_exercise(Context const& ctx, std::string_view id)
    {
        auto message = std::format("unknown exercise '{}'", text::sanitize(id));
        auto hits = ctx.catalog.search(id);
        if (hits.empty() && id.find('/') != std::string_view::npos)
            hits = ctx.catalog.search(id.substr(id.find('/') + 1));
        if (!hits.empty())
        {
            std::vector<std::string> names;
            for (std::size_t i = 0; i < hits.size() && i < 3; ++i)
                names.push_back(hits[i]->id);
            message += std::format(". Did you mean: {}?", text::join(names, ", "));
        }
        else
            message += ". Run 'learndc list' to see all exercises";
        return Failure{exit_not_found, std::move(message)};
    }

    [[nodiscard]] inline Result<catalog::Exercise const*> find_exercise(Context const& ctx, std::string_view id_in)
    {
        auto const id = text::trim(id_in);
        if (auto const* exact = ctx.catalog.find(id))
            return exact;
        if (id.find('/') == std::string_view::npos && !id.empty())
        {
            catalog::Exercise const* only = nullptr;
            int count = 0;
            for (auto const& ex : ctx.catalog.exercises())
                if (ex.name == id)
                {
                    only = &ex;
                    ++count;
                }
            if (count == 1)
                return only;
            if (count > 1)
                return failure(exit_usage, std::format("'{}' is ambiguous; use the full id (track/name)", text::sanitize(id)));
        }
        return std::unexpected(unknown_exercise(ctx, id));
    }

    [[nodiscard]] inline std::filesystem::path workspace_root_for(Context const& ctx, progress::Progress const& p, catalog::Exercise const& ex)
    {
        if (auto it = p.workspaces.find(ex.id); it != p.workspaces.end() && !it->second.empty())
            return platform::path_from_utf8(it->second);
        return workspace::default_root(ctx.dirs, ex);
    }

    [[nodiscard]] inline Result<std::optional<workspace::Workspace>> find_workspace(Context const& ctx, progress::Progress const& p,
                                                                                    catalog::Exercise const& ex)
    {
        auto loaded = workspace::load(workspace_root_for(ctx, p, ex));
        if (loaded)
        {
            if (loaded->meta.exercise != ex.id)
                return failure(exit_refused, std::format("'{}' is the workspace of '{}', not '{}'", platform::path_to_utf8(loaded->root),
                                                         text::sanitize(loaded->meta.exercise), ex.id));
            return std::optional<workspace::Workspace>(std::move(*loaded));
        }
        if (loaded.error().kind == workspace::ErrorKind::NotFound)
            return std::optional<workspace::Workspace>();
        return std::unexpected(failure_of(loaded.error()));
    }

    enum class Resolved
    {
        Argument,
        WorkingDirectory,
        Active,
    };

    [[nodiscard]] inline std::string_view to_string(Resolved r)
    {
        switch (r)
        {
            case Resolved::Argument:
                return "named on the command line";
            case Resolved::WorkingDirectory:
                return "current directory";
            case Resolved::Active:
                return "active exercise";
        }
        return "";
    }

    struct Target
    {
        catalog::Exercise const* exercise = nullptr;
        std::optional<workspace::Workspace> workspace;
        Resolved how = Resolved::Argument;
    };

    [[nodiscard]] inline Result<Target> resolve_target(Context const& ctx, progress::Progress const& p, std::optional<std::string> const& id,
                                                       std::filesystem::path const& cwd, bool need_workspace)
    {
        Target t;
        auto enclosing = workspace::find_enclosing(cwd);
        if (id && !id->empty())
        {
            auto ex = find_exercise(ctx, *id);
            if (!ex)
                return std::unexpected(ex.error());
            t.exercise = *ex;
            t.how = Resolved::Argument;
            if (enclosing && enclosing->meta.exercise == t.exercise->id)
                t.workspace = std::move(enclosing);
        }
        else if (enclosing)
        {
            auto ex = ctx.catalog.find(enclosing->meta.exercise);
            if (!ex)
                return failure(exit_not_found, std::format("the workspace '{}' belongs to '{}', which is not in the loaded content",
                                                           platform::path_to_utf8(enclosing->root), text::sanitize(enclosing->meta.exercise)));
            t.exercise = ex;
            t.how = Resolved::WorkingDirectory;
            t.workspace = std::move(enclosing);
        }
        else if (!p.active_exercise.empty())
        {
            auto ex = ctx.catalog.find(p.active_exercise);
            if (!ex)
                return failure(exit_not_found, std::format("the active exercise '{}' is not in the loaded content", text::sanitize(p.active_exercise)));
            t.exercise = ex;
            t.how = Resolved::Active;
        }
        else
            return failure(exit_not_found, "no exercise selected: name one, run from inside a workspace, or start one with 'learndc start <id>'");
        if (!t.workspace)
        {
            auto ws = find_workspace(ctx, p, *t.exercise);
            if (!ws)
                return std::unexpected(ws.error());
            t.workspace = std::move(*ws);
        }
        if (need_workspace && !t.workspace)
            return failure(exit_not_found, std::format("there is no workspace for '{}' yet; run 'learndc start {}'", t.exercise->id, t.exercise->id));
        return t;
    }

    struct StartOutcome
    {
        workspace::StartResult result;
        std::filesystem::path root;
        std::vector<std::string> missing_prerequisites;
        bool platform_supported = true;
    };

    [[nodiscard]] inline Result<StartOutcome> start_exercise(Context& ctx, catalog::Exercise const& ex, std::optional<std::filesystem::path> const& dir)
    {
        auto p = load_progress(ctx);
        if (!p)
            return std::unexpected(p.error());
        std::filesystem::path root;
        bool custom = false;
        if (dir)
        {
            std::error_code ec;
            root = std::filesystem::absolute(*dir, ec);
            if (ec)
                root = *dir;
            custom = true;
        }
        else
        {
            root = workspace_root_for(ctx, *p, ex);
            custom = p->workspaces.contains(ex.id);
        }
        auto started = workspace::start(ex, root);
        if (!started)
            return std::unexpected(failure_of(started.error()));
        StartOutcome out;
        out.result = std::move(*started);
        out.root = out.result.workspace.root;
        for (auto const& pre : ex.prerequisites)
            if (!progress::is_completed(*p, pre))
                out.missing_prerequisites.push_back(pre);
        out.platform_supported = ex.supports_platform(ctx.os == platform::Os::Windows ? "windows" : "linux");
        auto const now = platform::now_utc_iso8601();
        auto const root_text = platform::path_to_utf8(out.root);
        auto updated = update_progress(ctx, [&](progress::Progress& pr) {
            progress::mark_started(pr, ex.id, now);
            progress::set_active(pr, ex.id);
            if (custom)
                pr.workspaces[ex.id] = root_text;
        });
        if (!updated)
            return std::unexpected(updated.error());
        return out;
    }

    [[nodiscard]] inline runner::RunnerOptions runner_options(Context const& ctx, runner::Backend backend, std::atomic<bool> const* cancel,
                                                              std::function<void(std::string_view)> on_stage = {}, bool safety_note = false)
    {
        runner::RunnerOptions opt;
        opt.dirs = ctx.dirs;
        opt.settings = ctx.settings;
        opt.tools = ctx.tools;
        opt.backend = backend;
        opt.cancel = cancel;
        opt.on_stage = std::move(on_stage);
        opt.include_safety_note = safety_note;
        return opt;
    }

    [[nodiscard]] inline int exit_code_for(runner::CheckReport const& report)
    {
        switch (report.outcome)
        {
            case runner::Outcome::Passed:
                return exit_ok;
            case runner::Outcome::Failed:
                return exit_failed;
            case runner::Outcome::Cancelled:
                return exit_interrupted;
            case runner::Outcome::Error:
                if (!report.results.empty() && report.results.front().stage == runner::Stage::Toolchain)
                    return exit_toolchain;
                return exit_failed;
        }
        return exit_failed;
    }

    struct CheckRecord
    {
        bool recorded = false;
        bool all_passed = false;
        bool completed_now = false;
        std::string next_id;
        std::string next_title;
    };

    [[nodiscard]] inline Result<CheckRecord> record_report(Context& ctx, catalog::Exercise const& ex, runner::CheckReport const& report)
    {
        CheckRecord rec;
        if (report.outcome != runner::Outcome::Passed && report.outcome != runner::Outcome::Failed)
            return rec;
        bool was_completed = false;
        auto const now = platform::now_utc_iso8601();
        auto updated = update_progress(ctx, [&](progress::Progress& pr) {
            was_completed = progress::is_completed(pr, ex.id);
            progress::record_check(pr, ex.id, report.passed, report.total, ex.revision, now);
            progress::set_active(pr, ex.id);
        });
        if (!updated)
            return std::unexpected(updated.error());
        rec.recorded = true;
        rec.all_passed = report.outcome == runner::Outcome::Passed;
        rec.completed_now = rec.all_passed && !was_completed;
        if (rec.all_passed)
            if (auto const* next = ctx.catalog.next_recommended(completed_predicate(*updated), ex.id))
            {
                rec.next_id = next->id;
                rec.next_title = next->title;
            }
        return rec;
    }

    [[nodiscard]] inline bool any_attempts(progress::Progress const& p)
    {
        return std::ranges::any_of(p.exercises, [](auto const& kv) { return kv.second.attempts > 0; });
    }

    struct HintOutcome
    {
        int level = 0;
        std::string text;
        bool all_shown = false;
    };

    [[nodiscard]] inline Result<HintOutcome> reveal_hint(Context& ctx, catalog::Exercise const& ex, std::optional<int> level)
    {
        if (level && (*level < 1 || *level > 3))
            return failure(exit_usage, "--level must be 1, 2 or 3");
        auto p = load_progress(ctx);
        if (!p)
            return std::unexpected(p.error());
        int current = 0;
        if (auto it = p->exercises.find(ex.id); it != p->exercises.end())
            current = it->second.hint_level;
        HintOutcome out;
        out.level = level ? *level : std::min(current + 1, 3);
        out.all_shown = !level && current >= 3;
        out.text = ex.hints[static_cast<std::size_t>(out.level - 1)];
        auto const now = platform::now_utc_iso8601();
        auto updated = update_progress(ctx, [&](progress::Progress& pr) { progress::reveal_hint(pr, ex.id, out.level, now); });
        if (!updated)
            return std::unexpected(updated.error());
        return out;
    }

    [[nodiscard]] inline Result<std::string> reveal_solution(Context& ctx, catalog::Exercise const& ex, std::optional<workspace::Workspace> const& ws, bool diff)
    {
        std::string out;
        if (diff)
        {
            if (!ws)
                return failure(exit_not_found, std::format("there is no workspace for '{}' yet; run 'learndc start {}'", ex.id, ex.id));
            out = workspace::solution_diff(*ws, ex);
            if (out.empty())
                out = "Your files already match the reference solution.\n";
        }
        else
        {
            for (auto const& [path, bytes] : ex.solution)
            {
                out += std::format("=== {} ===\n{}", path, bytes);
                if (!bytes.empty() && bytes.back() != '\n')
                    out += '\n';
                out += '\n';
            }
            if (ex.solution.empty())
                out += "This exercise has no solution files.\n\n";
        }
        if (!ex.explanation.empty())
        {
            out += markdown::render_plain(ex.explanation);
        }
        auto const now = platform::now_utc_iso8601();
        auto updated = update_progress(ctx, [&](progress::Progress& pr) { progress::mark_solution_viewed(pr, ex.id, now); });
        if (!updated)
            return std::unexpected(updated.error());
        return out;
    }

    [[nodiscard]] inline Result<workspace::ResetResult> reset_exercise(Context& ctx, catalog::Exercise const& ex, workspace::Workspace& ws, bool backup)
    {
        auto r = workspace::reset(ws, ex, ctx.dirs.backups_dir, backup);
        if (!r)
            return std::unexpected(failure_of(r.error()));
        return std::move(*r);
    }

    [[nodiscard]] inline Result<bool> set_bookmark(Context& ctx, catalog::Exercise const& ex, bool wanted)
    {
        auto updated = update_progress(ctx, [&](progress::Progress& pr) {
            if (progress::is_bookmarked(pr, ex.id) != wanted)
                progress::toggle_bookmark(pr, ex.id);
        });
        if (!updated)
            return std::unexpected(updated.error());
        return progress::is_bookmarked(*updated, ex.id);
    }

    struct TrackSummary
    {
        std::string id;
        std::string title;
        int total = 0;
        int completed = 0;
        int in_progress = 0;
    };

    struct Summary
    {
        int total = 0;
        int completed = 0;
        int in_progress = 0;
        int not_started = 0;
        int needs_revisit = 0;
        std::vector<TrackSummary> tracks;
        std::string active;
        std::vector<std::string> recent;
        std::vector<std::string> bookmarks;
    };

    [[nodiscard]] inline Summary summarize(Context const& ctx, progress::Progress const& p)
    {
        Summary s;
        for (auto const& track : ctx.catalog.tracks())
        {
            TrackSummary t{track.id, track.title, 0, 0, 0};
            for (auto const& id : track.exercise_ids)
            {
                ++t.total;
                auto const status = progress::status_of(p, id);
                if (status == progress::Status::Completed)
                    ++t.completed;
                else if (status == progress::Status::InProgress)
                    ++t.in_progress;
            }
            s.total += t.total;
            s.completed += t.completed;
            s.in_progress += t.in_progress;
            s.tracks.push_back(std::move(t));
        }
        s.not_started = s.total - s.completed - s.in_progress;
        for (auto const& ex : ctx.catalog.exercises())
            if (auto it = p.exercises.find(ex.id); it != p.exercises.end() && progress::needs_revisit(it->second, ex.revision))
                ++s.needs_revisit;
        s.active = p.active_exercise;
        s.recent = p.recent;
        s.bookmarks = p.bookmarks;
        return s;
    }

    struct ReviewItem
    {
        catalog::Exercise const* exercise = nullptr;
        std::string reason;
    };

    [[nodiscard]] inline std::vector<ReviewItem> review_list(Context const& ctx, progress::Progress const& p)
    {
        std::vector<ReviewItem> out;
        for (auto const& ex : ctx.catalog.exercises())
        {
            auto it = p.exercises.find(ex.id);
            if (it == p.exercises.end())
                continue;
            auto const& e = it->second;
            std::string reason;
            if (progress::needs_revisit(e, ex.revision))
                reason = "the exercise was updated after you completed it";
            else if (e.solution_viewed && e.status == progress::Status::Completed)
                reason = "you looked at the solution";
            else if (e.status == progress::Status::InProgress && e.attempts > 0)
                reason = std::format("last check passed {} of {}", e.last_passed, e.last_total);
            else if (e.status == progress::Status::Completed && e.hint_level >= 2)
                reason = std::format("you used {} hints", e.hint_level);
            if (!reason.empty())
                out.push_back({&ex, std::move(reason)});
        }
        return out;
    }

    [[nodiscard]] inline bool is_learndc_state_file(Context const& ctx, std::filesystem::path const& file)
    {
        std::error_code ec;
        auto backup = ctx.dirs.progress_file;
        backup += ".bak";
        for (auto const& state : {ctx.dirs.progress_file, backup, ctx.dirs.progress_lock, ctx.dirs.config_file})
        {
            if (state.empty())
                continue;
            if (std::filesystem::equivalent(file, state, ec) ||
                std::filesystem::weakly_canonical(file, ec) == std::filesystem::weakly_canonical(state, ec))
                return true;
        }
        auto const name = file.filename().string();
        return name == workspace::meta_file && file.parent_path().filename().string() == workspace::meta_dir;
    }

    [[nodiscard]] inline Result<std::string> export_progress(Context& ctx, std::filesystem::path const& file, bool overwrite)
    {
        std::error_code ec;
        if (is_learndc_state_file(ctx, file))
            return failure(exit_refused, std::format("'{}' is one of learndc's own data files; choose another output file", platform::path_to_utf8(file)));
        if (!overwrite && std::filesystem::exists(file, ec))
            return failure(exit_refused, std::format("'{}' already exists; use --yes to overwrite it", platform::path_to_utf8(file)));
        auto p = load_progress(ctx);
        if (!p)
            return std::unexpected(p.error());
        auto written = platform::atomic_write(file, progress::export_text(*p, platform::now_utc_iso8601()));
        if (!written)
            return failure(exit_failed, written.error().message);
        return platform::path_to_utf8(file);
    }

    struct ImportOutcome
    {
        progress::ImportPlan plan;
        bool applied = false;
    };

    [[nodiscard]] inline Result<ImportOutcome> import_progress(Context& ctx, std::filesystem::path const& file, bool apply)
    {
        auto data = platform::read_file(file);
        if (!data)
            return failure(exit_failed, std::format("cannot read '{}'", platform::path_to_utf8(file)));
        auto incoming = progress::parse_export(*data);
        if (!incoming)
            return failure(exit_data, std::format("{}: {}", platform::path_to_utf8(file), incoming.error().message));
        auto current = load_progress(ctx);
        if (!current)
            return std::unexpected(current.error());
        ImportOutcome out;
        out.plan = progress::plan_import(*current, *incoming);
        if (apply)
        {
            auto imported = progress::import_into(ctx.store, *incoming);
            if (!imported)
                return failure(exit_data, imported.error().message);
            out.applied = true;
        }
        return out;
    }
} // namespace learndc::app
