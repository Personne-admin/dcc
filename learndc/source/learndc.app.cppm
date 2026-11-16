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
import learndc.activity;

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

    struct MigrationReport
    {
        int records = 0;
        int workspaces = 0;
        std::vector<std::string> problems;
    };

    inline MigrationReport migrate_renamed_lessons(Context& ctx)
    {
        MigrationReport out;
        auto const& renamed = ctx.catalog.renamed();
        auto loaded = ctx.store.load();
        if (!loaded)
            return out;
        auto const& p = loaded->progress;
        std::map<std::string, std::string> adopt;
        for (auto const& [from, to] : renamed)
        {
            auto const* ex = ctx.catalog.find(to);
            if (ex == nullptr)
                continue;
            std::filesystem::path root;
            if (auto it = p.workspaces.find(from); it != p.workspaces.end())
                root = platform::path_from_utf8(it->second);
            else if (!p.workspaces.contains(to))
            {
                auto const slash = from.find('/');
                root = ctx.dirs.workspaces_dir / platform::path_from_utf8(from.substr(0, slash)) / platform::path_from_utf8(from.substr(slash + 1));
            }
            if (root.empty())
                continue;
            auto ws = workspace::load(root);
            if (!ws || ws->meta.exercise != from)
                continue;
            if (auto r = workspace::retarget(*ws, to); !r)
            {
                out.problems.push_back(r.error().message);
                continue;
            }
            if (root != workspace::default_root(ctx.dirs, *ex))
                adopt[to] = platform::path_to_utf8(ws->root);
            ++out.workspaces;
        }
        bool const needs_progress = std::ranges::any_of(renamed, [&](auto const& kv) {
            return p.exercises.contains(kv.first) || p.workspaces.contains(kv.first) || progress::is_bookmarked(p, kv.first) ||
                   std::ranges::find(p.recent, kv.first) != p.recent.end() || p.active_exercise == kv.first;
        });
        if (!needs_progress && adopt.empty())
            return out;
        for (auto const& [from, to] : renamed)
            if (p.exercises.contains(from))
                ++out.records;
        auto updated = ctx.store.update([&](progress::Progress& pr) {
            progress::migrate_renamed(pr, renamed);
            for (auto const& [id, path] : adopt)
                pr.workspaces[id] = path;
        });
        if (!updated)
            out.problems.push_back(updated.error().message);
        else
            for (auto& w : updated->warnings)
                ctx.warnings.push_back(std::move(w));
        for (auto const& problem : out.problems)
            ctx.warnings.push_back("could not move older progress to the new lesson layout: " + problem);
        return out;
    }

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
        if (ctx.content_error.empty() && !ctx.catalog.renamed().empty())
            migrate_renamed_lessons(ctx);
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
            if (std::ranges::find(ex.former_ids, loaded->meta.exercise) != ex.former_ids.end())
                if (auto r = workspace::retarget(*loaded, ex.id); !r)
                    return std::unexpected(failure_of(r.error()));
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
            if (enclosing && ctx.catalog.find(enclosing->meta.exercise) == t.exercise)
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
        if (t.workspace && t.workspace->meta.exercise != t.exercise->id &&
            std::ranges::find(t.exercise->former_ids, t.workspace->meta.exercise) != t.exercise->former_ids.end())
            if (auto r = workspace::retarget(*t.workspace, t.exercise->id); !r)
                return std::unexpected(failure_of(r.error()));
        if (t.exercise->is_lecture() && need_workspace)
            return failure(exit_usage, std::format("'{}' is a lecture; read it with 'learndc show {}' and answer it with 'learndc answer {}'", t.exercise->id,
                                                   t.exercise->id, t.exercise->id));
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
        if (ex.is_lecture())
            return failure(exit_usage, std::format("'{}' is a lecture and has no workspace; read it with 'learndc show {}'", ex.id, ex.id));
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
        bool boss_defeated = false;
        bool chapter_completed = false;
        std::string chapter_title;
        int chapter_number = 0;
        std::vector<std::string> stages_achieved;
        std::string next_id;
        std::string next_title;
    };

    [[nodiscard]] inline bool chapter_done(Context const& ctx, progress::Progress const& p, std::string_view track_id)
    {
        auto const* track = ctx.catalog.find_track(track_id);
        if (track == nullptr)
            return false;
        for (auto const& id : track->exercise_ids)
            if (auto const* e = ctx.catalog.find(id); e != nullptr && !e->optional && !progress::is_completed(p, id))
                return false;
        return true;
    }

    inline void fill_completion(Context const& ctx, catalog::Exercise const& ex, progress::Progress const& before, progress::Progress const& after,
                                CheckRecord& rec)
    {
        rec.completed_now = progress::is_completed(after, ex.id) && !progress::is_completed(before, ex.id);
        rec.boss_defeated = rec.completed_now && ex.is_boss();
        if (rec.completed_now && !chapter_done(ctx, before, ex.track) && chapter_done(ctx, after, ex.track))
        {
            rec.chapter_completed = true;
            if (auto const* t = ctx.catalog.find_track(ex.track))
            {
                rec.chapter_title = t->title;
                rec.chapter_number = t->number;
            }
        }
        if (progress::is_completed(after, ex.id))
            if (auto const* next = ctx.catalog.next_recommended(completed_predicate(after), ex.id))
            {
                rec.next_id = next->id;
                rec.next_title = next->title;
            }
    }

    [[nodiscard]] inline bool questions_solved(progress::Progress const& p, catalog::Exercise const& ex, catalog::Stage const& st)
    {
        return std::ranges::all_of(st.questions, [&](std::string const& qid) { return progress::question_of(p, ex.id, qid).last == "right"; });
    }

    [[nodiscard]] inline Result<CheckRecord> record_report(Context& ctx, catalog::Exercise const& ex, runner::CheckReport const& report)
    {
        CheckRecord rec;
        if (report.outcome != runner::Outcome::Passed && report.outcome != runner::Outcome::Failed)
            return rec;
        progress::Progress before;
        auto const now = platform::now_utc_iso8601();
        bool complete = !ex.staged() && report.outcome == runner::Outcome::Passed;
        auto updated = update_progress(ctx, [&](progress::Progress& pr) {
            before = pr;
            std::vector<progress::StageOutcome> outcomes;
            bool build_ok = false;
            for (auto const& r : report.results)
                if (r.kind == catalog::CheckKind::Build)
                    build_ok = r.verdict == runner::Verdict::Pass;
            bool required_ok = true;
            for (auto const& stage : ex.stages)
            {
                auto const* st = report.find_stage(stage.id);
                bool const solved = questions_solved(pr, ex, stage);
                bool const has_checks = !ex.checks_of(stage.id).empty();
                bool const ran = has_checks ? st != nullptr && st->ran : solved;
                bool const passed = has_checks ? st != nullptr && st->passed : solved;
                outcomes.push_back({stage.id, passed, ran, stage.revision, solved});
                if (!stage.optional && !(passed && solved))
                    required_ok = false;
            }
            if (ex.staged())
                complete = (report.outcome == runner::Outcome::Passed || report.outcome == runner::Outcome::Failed) && build_ok && required_ok;
            auto const it = pr.exercises.find(ex.id);
            std::set<std::string> had;
            if (it != pr.exercises.end())
                for (auto const& [name, st] : it->second.stages)
                    if (st.achieved())
                        had.insert(name);
            progress::record_check(pr, ex.id, report.passed, report.total, ex.revision, now, complete);
            if (ex.staged())
                progress::record_stages(pr, ex.id, outcomes, report.fingerprint, now);
            else
                pr.exercises[ex.id].last_fingerprint = report.fingerprint;
            progress::set_active(pr, ex.id);
            rec.stages_achieved.clear();
            for (auto const& st : ex.stages)
                if (!had.contains(st.id) && pr.exercises[ex.id].stages[st.id].achieved())
                    rec.stages_achieved.push_back(st.title);
        });
        if (!updated)
            return std::unexpected(updated.error());
        rec.recorded = true;
        rec.all_passed = complete;
        fill_completion(ctx, ex, before, *updated, rec);
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
            std::string explanation = ex.explanation;
            if (!diff)
            {
                std::vector<std::string> references;
                for (auto const& [path, bytes] : ex.solution)
                    references.push_back(bytes);
                explanation = markdown::drop_quoted_code(explanation, references);
            }
            out += markdown::render_plain(explanation);
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

    struct LectureOutcome
    {
        int correct = 0;
        int total = 0;
        std::vector<bool> right;
        CheckRecord record;
    };

    struct QuestionOutcome
    {
        activity::Feedback feedback;
        int attempt = 0;
        CheckRecord record;
        std::vector<std::string> stages_achieved;
    };

    [[nodiscard]] inline Result<QuestionOutcome> answer_question(Context& ctx, catalog::Exercise const& ex, std::string_view question_id,
                                                                 activity::Answer const& answer)
    {
        auto const* q = ex.find_question(question_id);
        if (q == nullptr)
        {
            std::vector<std::string> ids;
            for (auto const& each : ex.questions)
                ids.push_back(each.id);
            return failure(exit_not_found, ids.empty() ? std::format("'{}' has no questions", ex.id)
                                                       : std::format("'{}' has no question '{}' (questions: {})", ex.id, text::sanitize(question_id),
                                                                     text::join(ids, ", ")));
        }
        if (!activity::answered(*q, answer))
            return failure(exit_usage, "the answer is empty");
        auto p = load_progress(ctx);
        if (!p)
            return std::unexpected(p.error());
        QuestionOutcome out;
        out.attempt = progress::question_of(*p, ex.id, q->id).attempts + 1;
        out.feedback = activity::grade(*q, answer, out.attempt);
        auto const now = platform::now_utc_iso8601();
        progress::Progress before;
        auto updated = update_progress(ctx, [&](progress::Progress& pr) {
            before = pr;
            progress::record_answer(pr, ex.id, q->id, out.feedback.correct, now);
            progress::set_active(pr, ex.id);
            if (ex.is_lecture())
            {
                bool const all = std::ranges::all_of(ex.questions, [&](catalog::Question const& each) {
                    return progress::question_of(pr, ex.id, each.id).last == "right";
                });
                if (all)
                    progress::record_lecture(pr, ex.id, true, ex.revision, now);
                else
                    progress::mark_started(pr, ex.id, now);
                return;
            }
            progress::mark_started(pr, ex.id, now);
            bool all_required = !ex.stages.empty();
            for (auto const& st : ex.stages)
            {
                bool const question_only = ex.checks_of(st.id).empty();
                bool const solved = questions_solved(pr, ex, st);
                auto& sp = pr.exercises[ex.id].stages[st.id];
                bool const checks_ok = question_only || sp.last == "pass";
                if (solved && checks_ok && (!sp.achieved() || sp.achieved_revision < st.revision))
                {
                    if (!sp.achieved())
                    {
                        sp.achieved_at = now;
                        out.stages_achieved.push_back(st.title);
                    }
                    sp.achieved_revision = st.revision;
                }
                if (question_only && solved)
                {
                    sp.last = "pass";
                    sp.last_checked_at = now;
                }
                if (!st.optional && !(solved && checks_ok))
                    all_required = false;
            }
            if (all_required)
                progress::mark_completed(pr, ex.id, ex.revision, now);
        });
        if (!updated)
            return std::unexpected(updated.error());
        out.record.recorded = true;
        out.record.all_passed = progress::is_completed(*updated, ex.id);
        out.record.stages_achieved = out.stages_achieved;
        fill_completion(ctx, ex, before, *updated, out.record);
        return out;
    }

    [[nodiscard]] inline Result<LectureOutcome> answer_lecture(Context& ctx, catalog::Exercise const& ex, std::vector<int> const& answers)
    {
        if (!ex.is_lecture())
            return failure(exit_usage, std::format("'{}' is not a lecture", ex.id));
        if (answers.size() != ex.questions.size())
            return failure(exit_usage, ex.questions.empty() ? std::format("'{}' has no questions; answer it with no choices to mark it as read", ex.id)
                                                            : std::format("'{}' has {} question{}; give one choice for each", ex.id, ex.questions.size(),
                                                                          ex.questions.size() == 1 ? "" : "s"));
        for (auto const& q : ex.questions)
            if (q.type != catalog::QuestionType::Choice)
                return failure(exit_usage, std::format("question '{}' is not multiple choice; answer it with --question {}", q.id, q.id));
        for (std::size_t i = 0; i < answers.size(); ++i)
            if (answers[i] < 1 || answers[i] > static_cast<int>(ex.questions[i].choices.size()))
                return failure(exit_usage, std::format("question {} has choices 1 to {}", i + 1, ex.questions[i].choices.size()));
        LectureOutcome out;
        out.total = static_cast<int>(ex.questions.size());
        if (ex.questions.empty())
        {
            auto const now = platform::now_utc_iso8601();
            progress::Progress before;
            auto updated = update_progress(ctx, [&](progress::Progress& pr) {
                before = pr;
                progress::record_lecture(pr, ex.id, true, ex.revision, now);
                progress::set_active(pr, ex.id);
            });
            if (!updated)
                return std::unexpected(updated.error());
            out.record.recorded = true;
            out.record.all_passed = true;
            fill_completion(ctx, ex, before, *updated, out.record);
            return out;
        }
        for (std::size_t i = 0; i < answers.size(); ++i)
        {
            activity::Answer a;
            a.choice = answers[i];
            auto r = answer_question(ctx, ex, ex.questions[i].id, a);
            if (!r)
                return std::unexpected(r.error());
            out.right.push_back(r->feedback.correct);
            if (r->feedback.correct)
                ++out.correct;
            if (r->record.completed_now)
                out.record = r->record;
            else if (!out.record.completed_now)
                out.record = r->record;
        }
        return out;
    }

    struct CardRef
    {
        catalog::Exercise const* lesson = nullptr;
        catalog::Card const* card = nullptr;
        progress::CardProgress state;
        std::string key;
    };

    struct MissedRef
    {
        catalog::Exercise const* lesson = nullptr;
        catalog::Question const* question = nullptr;
        std::string key;
        int attempts = 0;
    };

    struct ReviewQueue
    {
        std::vector<CardRef> due;
        int cards_unlocked = 0;
        int cards_locked = 0;
        std::optional<int> next_due_day;
        std::vector<MissedRef> missed;
    };

    [[nodiscard]] inline int today_day()
    {
        return activity::day_number(platform::now_utc_iso8601());
    }

    [[nodiscard]] inline ReviewQueue review_queue(Context const& ctx, progress::Progress const& p, int today)
    {
        ReviewQueue out;
        for (auto const& ex : ctx.catalog.exercises())
        {
            bool const unlocked = progress::is_completed(p, ex.id);
            for (auto const& card : ex.cards)
            {
                if (!unlocked)
                {
                    ++out.cards_locked;
                    continue;
                }
                ++out.cards_unlocked;
                CardRef ref;
                ref.lesson = &ex;
                ref.card = &card;
                ref.key = progress::item_key(ex.id, card.id);
                if (auto it = p.cards.find(ref.key); it != p.cards.end())
                    ref.state = it->second;
                if (activity::card_due(ref.state, today))
                    out.due.push_back(std::move(ref));
                else if (!out.next_due_day || ref.state.due_day < *out.next_due_day)
                    out.next_due_day = ref.state.due_day;
            }
            for (auto const& q : ex.questions)
            {
                auto const qp = progress::question_of(p, ex.id, q.id);
                if (qp.missed())
                    out.missed.push_back({&ex, &q, progress::item_key(ex.id, q.id), qp.attempts});
            }
        }
        std::ranges::stable_sort(out.due, [](CardRef const& a, CardRef const& b) {
            bool const a_new = a.state.reviews == 0;
            bool const b_new = b.state.reviews == 0;
            if (a_new != b_new)
                return !a_new;
            return a.state.due_day < b.state.due_day;
        });
        return out;
    }

    [[nodiscard]] inline Result<progress::CardProgress> rate_card(Context& ctx, catalog::Exercise const& ex, std::string_view card_id, activity::Rating rating)
    {
        auto it = std::ranges::find(ex.cards, card_id, &catalog::Card::id);
        if (it == ex.cards.end())
            return failure(exit_not_found, std::format("'{}' has no card '{}'", ex.id, text::sanitize(card_id)));
        auto const now = platform::now_utc_iso8601();
        int const today = activity::day_number(now);
        progress::CardProgress result;
        auto updated = update_progress(ctx, [&](progress::Progress& pr) {
            auto& c = pr.cards[progress::item_key(ex.id, it->id)];
            c = activity::schedule(c, rating, today, now);
            result = c;
        });
        if (!updated)
            return std::unexpected(updated.error());
        return result;
    }

    enum class StageState
    {
        Verified,
        Regressed,
        Unverified,
        Outdated,
        Current,
        Todo,
    };

    [[nodiscard]] inline std::string_view to_string(StageState s)
    {
        switch (s)
        {
            case StageState::Verified:
                return "passing";
            case StageState::Regressed:
                return "passed before, failing now";
            case StageState::Unverified:
                return "passed before, not checked since your last change";
            case StageState::Outdated:
                return "passed in an older version of this stage";
            case StageState::Current:
                return "current objective";
            case StageState::Todo:
                return "not reached yet";
        }
        return "";
    }

    struct StageView
    {
        catalog::Stage const* stage = nullptr;
        int number = 0;
        StageState state = StageState::Todo;
        bool achieved = false;
        bool failing_now = false;
        int hint_level = 0;
        std::string achieved_at;
        int questions_left = 0;
        bool question_only = false;
    };

    [[nodiscard]] inline std::vector<StageView> stage_views(catalog::Exercise const& ex, progress::Progress const& p, std::string_view current_fingerprint)
    {
        std::vector<StageView> out;
        progress::ExerciseProgress const empty;
        auto it = p.exercises.find(ex.id);
        auto const& e = it == p.exercises.end() ? empty : it->second;
        bool const fresh = !current_fingerprint.empty() && current_fingerprint == e.last_fingerprint;
        bool current_assigned = false;
        int number = 0;
        for (auto const& st : ex.stages)
        {
            StageView v;
            v.stage = &st;
            v.number = ++number;
            progress::StageProgress sp;
            if (auto s_it = e.stages.find(st.id); s_it != e.stages.end())
                sp = s_it->second;
            v.achieved = sp.achieved();
            v.achieved_at = sp.achieved_at;
            v.hint_level = sp.hint_level;
            v.failing_now = fresh && sp.last == "fail";
            for (auto const& qid : st.questions)
                if (progress::question_of(p, ex.id, qid).last != "right")
                    ++v.questions_left;
            v.question_only = std::ranges::none_of(ex.checks, [&](catalog::Check const& c) { return c.stage == st.id; });
            if (v.question_only)
            {
                v.achieved = v.achieved || v.questions_left == 0;
                v.state = v.questions_left == 0 ? StageState::Verified : StageState::Todo;
            }
            else if (v.achieved && sp.achieved_revision < st.revision)
                v.state = StageState::Outdated;
            else if (fresh && sp.last == "pass" && v.questions_left == 0)
                v.state = StageState::Verified;
            else if (v.achieved && v.failing_now)
                v.state = StageState::Regressed;
            else if (v.achieved)
                v.state = StageState::Unverified;
            else
                v.state = StageState::Todo;
            out.push_back(v);
        }
        for (auto& v : out)
            if (!current_assigned && !v.stage->optional && (v.state == StageState::Todo || v.state == StageState::Outdated || v.state == StageState::Regressed))
            {
                if (v.state == StageState::Todo)
                    v.state = StageState::Current;
                current_assigned = true;
            }
        if (!current_assigned)
            for (auto& v : out)
                if (v.state == StageState::Todo)
                {
                    v.state = StageState::Current;
                    break;
                }
        return out;
    }

    [[nodiscard]] inline StageView const* focus_stage(std::vector<StageView> const& views)
    {
        for (auto const& v : views)
            if (v.state == StageState::Current || v.state == StageState::Regressed || v.state == StageState::Outdated)
                if (!v.stage->optional)
                    return &v;
        for (auto const& v : views)
            if (v.state == StageState::Current)
                return &v;
        return nullptr;
    }

    struct StageHintOutcome
    {
        std::string stage_title;
        int level = 0;
        int count = 0;
        std::string text;
    };

    [[nodiscard]] inline Result<StageHintOutcome> reveal_stage_hint(Context& ctx, catalog::Exercise const& ex, std::string_view stage_id,
                                                                    std::optional<int> level)
    {
        auto const* st = ex.find_stage(stage_id);
        if (st == nullptr)
            return failure(exit_not_found, std::format("'{}' has no stage '{}'", ex.id, text::sanitize(stage_id)));
        if (st->hints.empty())
            return failure(exit_not_found, std::format("stage '{}' has no hints of its own; try 'learndc hint {}'", st->id, ex.id));
        int const count = static_cast<int>(st->hints.size());
        if (level && (*level < 1 || *level > count))
            return failure(exit_usage, std::format("stage '{}' has hints 1 to {}", st->id, count));
        auto p = load_progress(ctx);
        if (!p)
            return std::unexpected(p.error());
        int current = 0;
        if (auto it = p->exercises.find(ex.id); it != p->exercises.end())
            if (auto s_it = it->second.stages.find(st->id); s_it != it->second.stages.end())
                current = s_it->second.hint_level;
        StageHintOutcome out;
        out.stage_title = st->title;
        out.count = count;
        out.level = level ? *level : std::min(current + 1, count);
        out.text = st->hints[static_cast<std::size_t>(out.level - 1)];
        auto updated = update_progress(ctx, [&](progress::Progress& pr) { progress::reveal_stage_hint(pr, ex.id, st->id, out.level); });
        if (!updated)
            return std::unexpected(updated.error());
        return out;
    }

    struct TrackSummary
    {
        std::string id;
        std::string path;
        std::string title;
        int total = 0;
        int completed = 0;
        int in_progress = 0;
        int number = 0;
        std::string goal;
        int bonus_total = 0;
        int bonus_completed = 0;
        std::string boss_id;
        std::string boss_title;
        bool boss_defeated = false;
        bool done = false;
    };

    struct PathSummary
    {
        std::string id;
        std::string title;
        std::string summary;
        catalog::PathKind kind = catalog::PathKind::Core;
        int chapters = 0;
        int chapters_done = 0;
        int total = 0;
        int completed = 0;
        int in_progress = 0;
        int bonus_total = 0;
        int bonus_completed = 0;
        int bosses_total = 0;
        int bosses_defeated = 0;
        bool started = false;
    };

    struct Summary
    {
        int total = 0;
        int completed = 0;
        int in_progress = 0;
        int not_started = 0;
        int needs_revisit = 0;
        std::vector<TrackSummary> tracks;
        std::vector<PathSummary> paths;
        std::string active;
        std::vector<std::string> recent;
        std::vector<std::string> bookmarks;
        int bosses_total = 0;
        int bosses_defeated = 0;
        int chapters_done = 0;
    };

    [[nodiscard]] inline Summary summarize(Context const& ctx, progress::Progress const& p)
    {
        Summary s;
        for (auto const& track : ctx.catalog.tracks())
        {
            TrackSummary t;
            t.id = track.id;
            t.path = track.path;
            t.title = track.title;
            t.number = track.number;
            t.goal = track.goal;
            for (auto const& id : track.exercise_ids)
            {
                auto const* ex = ctx.catalog.find(id);
                auto const status = progress::status_of(p, id);
                if (ex != nullptr && ex->is_boss() && t.boss_id.empty())
                {
                    t.boss_id = id;
                    t.boss_title = ex->title;
                    t.boss_defeated = status == progress::Status::Completed;
                }
                if (ex != nullptr && ex->optional)
                {
                    ++t.bonus_total;
                    if (status == progress::Status::Completed)
                        ++t.bonus_completed;
                    continue;
                }
                ++t.total;
                if (status == progress::Status::Completed)
                    ++t.completed;
                else if (status == progress::Status::InProgress)
                    ++t.in_progress;
            }
            if (t.total == 0 && t.bonus_total > 0)
            {
                t.total = t.bonus_total;
                t.completed = t.bonus_completed;
                t.bonus_total = 0;
                t.bonus_completed = 0;
                for (auto const& id : track.exercise_ids)
                    if (progress::status_of(p, id) == progress::Status::InProgress)
                        ++t.in_progress;
            }
            t.done = t.total > 0 && t.completed == t.total;
            s.total += t.total;
            s.completed += t.completed;
            s.in_progress += t.in_progress;
            s.tracks.push_back(std::move(t));
        }
        s.not_started = s.total - s.completed - s.in_progress;
        for (auto const& t : s.tracks)
        {
            if (!t.boss_id.empty())
                ++s.bosses_total;
            if (t.boss_defeated)
                ++s.bosses_defeated;
            if (t.done)
                ++s.chapters_done;
        }
        for (auto const& path : ctx.catalog.paths())
        {
            PathSummary ps;
            ps.id = path.id;
            ps.title = path.title;
            ps.summary = path.summary;
            ps.kind = path.kind;
            for (auto const& t : s.tracks)
            {
                if (t.path != path.id)
                    continue;
                ++ps.chapters;
                if (t.done)
                    ++ps.chapters_done;
                ps.total += t.total;
                ps.completed += t.completed;
                ps.in_progress += t.in_progress;
                ps.bonus_total += t.bonus_total;
                ps.bonus_completed += t.bonus_completed;
                if (!t.boss_id.empty())
                    ++ps.bosses_total;
                if (t.boss_defeated)
                    ++ps.bosses_defeated;
            }
            ps.started = ps.completed + ps.in_progress + ps.bonus_completed > 0;
            s.paths.push_back(std::move(ps));
        }
        for (auto const& ex : ctx.catalog.exercises())
            if (auto it = p.exercises.find(ex.id); it != p.exercises.end() && progress::needs_revisit(it->second, ex.revision))
                ++s.needs_revisit;
        s.active = p.active_exercise;
        s.recent = p.recent;
        s.bookmarks = p.bookmarks;
        return s;
    }

    struct Achievement
    {
        std::string title;
        std::string detail;
        bool boss = false;
    };

    [[nodiscard]] inline std::vector<Achievement> achievements(Context const& ctx, progress::Progress const& p)
    {
        std::vector<Achievement> out;
        auto const summary = summarize(ctx, p);
        for (auto const& t : summary.tracks)
        {
            if (!t.boss_id.empty() && t.boss_defeated)
            {
                Achievement a;
                a.boss = true;
                a.title = std::format("Boss defeated: {}", t.boss_title);
                auto const* boss = ctx.catalog.find(t.boss_id);
                auto it = p.exercises.find(t.boss_id);
                if (boss != nullptr && it != p.exercises.end())
                {
                    auto const& e = it->second;
                    bool clean = e.hint_level == 0 && !e.solution_viewed;
                    int bonus_total = 0;
                    int bonus_done = 0;
                    for (auto const& st : boss->stages)
                    {
                        auto s_it = e.stages.find(st.id);
                        if (s_it != e.stages.end() && s_it->second.hint_level > 0)
                            clean = false;
                        if (st.optional)
                        {
                            ++bonus_total;
                            if (s_it != e.stages.end() && s_it->second.achieved())
                                ++bonus_done;
                        }
                    }
                    std::vector<std::string> notes;
                    if (clean)
                        notes.push_back("no hints, no solution");
                    if (bonus_total > 0)
                        notes.push_back(std::format("bonus {} of {}", bonus_done, bonus_total));
                    a.detail = text::join(notes, ", ");
                }
                out.push_back(std::move(a));
            }
            auto const* path = ctx.catalog.find_path(t.path);
            bool const core = !path || path->kind == catalog::PathKind::Core || ctx.catalog.paths().size() < 2;
            if (t.done)
                out.push_back({core ? std::format("Chapter {} complete: {}", t.number, t.title) : std::format("{} complete: {}", path->title, t.title),
                               t.bonus_total > 0 ? std::format("bonus lessons {} of {}", t.bonus_completed, t.bonus_total) : std::string(), false});
        }
        return out;
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
