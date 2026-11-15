export module learndc.tui.model;

import std;
import learndc.text;
import learndc.platform;
import learndc.catalog;
import learndc.settings;
import learndc.progress;
import learndc.workspace;
import learndc.runner;
import learndc.doctor;
import learndc.editor;
import learndc.watch;
import learndc.diff;
import learndc.app;

export namespace learndc::tui::model
{
    enum class Screen
    {
        Onboarding,
        Home,
        Learn,
        Exercise,
        Progress,
        Bookmarks,
        Doctor,
        Settings,
        Help,
    };

    [[nodiscard]] inline std::string_view to_string(Screen s)
    {
        switch (s)
        {
            case Screen::Onboarding:
                return "Welcome";
            case Screen::Home:
                return "Home";
            case Screen::Learn:
                return "Learn";
            case Screen::Exercise:
                return "Exercise";
            case Screen::Progress:
                return "Progress";
            case Screen::Bookmarks:
                return "Bookmarks";
            case Screen::Doctor:
                return "Doctor";
            case Screen::Settings:
                return "Settings";
            case Screen::Help:
                return "Help";
        }
        return "Home";
    }

    enum class LayoutMode
    {
        Wide,
        Normal,
        Compact,
        TooSmall,
    };

    struct Size
    {
        int width = 100;
        int height = 30;

        friend bool operator==(Size const&, Size const&) = default;
    };

    inline constexpr Size wide_threshold{100, 30};
    inline constexpr Size normal_threshold{80, 24};
    inline constexpr Size compact_threshold{40, 12};

    [[nodiscard]] inline LayoutMode layout_for(Size size)
    {
        if (size.width >= wide_threshold.width && size.height >= wide_threshold.height)
            return LayoutMode::Wide;
        if (size.width >= normal_threshold.width && size.height >= normal_threshold.height)
            return LayoutMode::Normal;
        if (size.width >= compact_threshold.width && size.height >= compact_threshold.height)
            return LayoutMode::Compact;
        return LayoutMode::TooSmall;
    }

    enum class Pane
    {
        Instructions,
        Files,
        Results,
    };

    enum class ThemeId
    {
        Dark,
        Light,
        HighContrast,
        NoColor,
    };

    struct Appearance
    {
        ThemeId theme = ThemeId::Dark;
        bool ascii = false;
        bool reduced_motion = false;

        friend bool operator==(Appearance const&, Appearance const&) = default;
    };

    [[nodiscard]] inline Appearance appearance_of(settings::Settings const& s, platform::Env const& env)
    {
        Appearance a;
        if (s.ui_theme == "light")
            a.theme = ThemeId::Light;
        else if (s.ui_theme == "high-contrast")
            a.theme = ThemeId::HighContrast;
        else if (s.ui_theme == "no-color")
            a.theme = ThemeId::NoColor;
        if (auto it = env.find("NO_COLOR"); it != env.end() && !it->second.empty())
            a.theme = ThemeId::NoColor;
        if (s.ui_ascii == "on")
            a.ascii = true;
        else if (s.ui_ascii == "auto")
            a.ascii = !platform::utf8_locale(env);
        a.reduced_motion = s.ui_reduced_motion;
        return a;
    }

    enum class StatusFilter
    {
        All,
        NotStarted,
        InProgress,
        Completed,
        Unfinished,
    };

    [[nodiscard]] inline std::string_view to_string(StatusFilter f)
    {
        switch (f)
        {
            case StatusFilter::All:
                return "any";
            case StatusFilter::NotStarted:
                return "not started";
            case StatusFilter::InProgress:
                return "in progress";
            case StatusFilter::Completed:
                return "completed";
            case StatusFilter::Unfinished:
                return "unfinished";
        }
        return "any";
    }

    [[nodiscard]] inline std::string_view status_word(progress::Status s)
    {
        switch (s)
        {
            case progress::Status::NotStarted:
                return "not started";
            case progress::Status::InProgress:
                return "in progress";
            case progress::Status::Completed:
                return "completed";
        }
        return "not started";
    }

    inline constexpr std::array<catalog::Difficulty, 3> all_difficulties = {catalog::Difficulty::Beginner, catalog::Difficulty::Intermediate,
                                                                            catalog::Difficulty::Advanced};
    inline constexpr std::array<catalog::ExerciseKind, 9> all_kinds = {catalog::ExerciseKind::RepairCompilation,
                                                                       catalog::ExerciseKind::FixBehavior,
                                                                       catalog::ExerciseKind::ImplementFunction,
                                                                       catalog::ExerciseKind::CompleteProgram,
                                                                       catalog::ExerciseKind::UnderstandDiagnostics,
                                                                       catalog::ExerciseKind::Refactor,
                                                                       catalog::ExerciseKind::MultiModule,
                                                                       catalog::ExerciseKind::Libdcext,
                                                                       catalog::ExerciseKind::Challenge};

    struct Filters
    {
        std::size_t track = 0;
        StatusFilter status = StatusFilter::All;
        std::size_t difficulty = 0;
        std::size_t kind = 0;

        [[nodiscard]] bool any() const { return track != 0 || status != StatusFilter::All || difficulty != 0 || kind != 0; }
    };

    struct Key
    {
        std::string name;
    };

    struct Resize
    {
        int width = 0;
        int height = 0;
    };

    struct CheckStage
    {
        std::string text;
    };

    struct CheckDone
    {
        runner::CheckReport report;
    };

    struct EditorDone
    {
        bool ok = true;
        std::string message;
    };

    struct ProgramDone
    {
        bool ok = true;
        std::string message;
    };

    struct DoctorDone
    {
        doctor::DoctorReport report;
    };

    struct FilesChanged
    {
    };

    struct Tick
    {
        std::int64_t now_ms = 0;
    };

    using Event = std::variant<Key, Resize, CheckStage, CheckDone, EditorDone, ProgramDone, DoctorDone, FilesChanged, Tick>;

    struct LaunchEditor
    {
        editor::EditorChoice choice;
        std::string file;
        std::string workspace;
        int line = 1;
        int column = 1;
        bool folder = false;
    };

    struct StartCheck
    {
        std::string id;
        std::filesystem::path root;
    };

    struct CancelCheck
    {
    };

    struct RunProgram
    {
        std::string id;
        std::filesystem::path root;
    };

    struct StartWatch
    {
        std::string id;
        std::filesystem::path root;
    };

    struct StopWatch
    {
    };

    struct RunDoctor
    {
        bool build = false;
    };

    struct Quit
    {
    };

    using Effect = std::variant<LaunchEditor, StartCheck, CancelCheck, RunProgram, StartWatch, StopWatch, RunDoctor, Quit>;

    struct Toast
    {
        std::string text;
        bool error = false;
        std::int64_t duration_ms = 4000;
        std::int64_t shown_ms = -1;
    };

    enum class DialogKind
    {
        None,
        QuitRunning,
        Solution,
        Reset,
        Choice,
    };

    struct Dialog
    {
        DialogKind kind = DialogKind::None;
        std::string title;
        std::vector<std::string> body;
        std::string input;
        std::vector<std::string> labels;
        std::vector<std::string> values;
        std::size_t selected = 0;
        std::string target;

        [[nodiscard]] bool open() const { return kind != DialogKind::None; }
    };

    enum class CheckPhase
    {
        Idle,
        Running,
        Cancelling,
    };

    struct SolutionFile
    {
        std::string path;
        std::vector<diff::SideRow> rows;
        std::string unified;
        bool identical = false;
    };

    struct SolutionView
    {
        std::vector<SolutionFile> files;
        std::string explanation;
        bool has_workspace = false;
        int scroll = 0;
    };

    struct ExerciseState
    {
        catalog::Exercise const* ex = nullptr;
        std::optional<workspace::Workspace> ws;
        std::vector<std::string> files;
        std::size_t file_sel = 0;
        Pane pane = Pane::Instructions;
        int scroll_instructions = 0;
        int scroll_preview = 0;
        int scroll_results = 0;
        int hints_shown = 0;
        bool raw_log = false;
        CheckPhase phase = CheckPhase::Idle;
        std::string stage_text;
        std::int64_t started_ms = 0;
        std::optional<runner::CheckReport> report;
        bool stale = false;
        watch::Snapshot report_snapshot;
        bool watching = false;
        bool rerun_pending = false;
        std::vector<std::string> preview_lines;
        std::string preview_note;
        std::optional<SolutionView> solution;
        std::string entry_missing;
    };

    enum class HomeKind
    {
        Continue,
        Next,
        Recent,
    };

    struct HomeItem
    {
        HomeKind kind = HomeKind::Recent;
        std::string id;
    };

    struct SettingRow
    {
        std::string key;
        std::string description;
        std::string value;
        bool editable = true;
    };

    struct OnboardingState
    {
        int step = 0;
        std::size_t selected = 0;
        std::vector<std::string> editor_values;
        std::vector<std::string> editor_labels;
    };

    struct HelpSection
    {
        std::string title;
        std::vector<std::string> lines;
    };

    struct KeyHint
    {
        std::string key;
        std::string label;
    };

    namespace detail
    {
        [[nodiscard]] inline bool printable_key(std::string const& name)
        {
            if (name.empty())
                return false;
            auto const first = static_cast<unsigned char>(name[0]);
            if (name.size() == 1)
                return first >= 0x20 && first != 0x7F;
            return first >= 0xC0;
        }

        [[nodiscard]] inline bool is_down(std::string const& k)
        {
            return k == "Down" || k == "j";
        }

        [[nodiscard]] inline bool is_up(std::string const& k)
        {
            return k == "Up" || k == "k";
        }

        [[nodiscard]] inline std::string relative_name(std::string_view workspace_root, std::string_view file)
        {
            return runner::detail::relative_to(workspace_root, file);
        }

        [[nodiscard]] inline std::vector<std::string> int_choices(std::string_view key, int current)
        {
            std::vector<int> values;
            if (key == "checks.timeout_seconds")
                values = {5, 10, 20, 30, 60, 120, 300, 600};
            else if (key == "checks.build_timeout_seconds")
                values = {30, 60, 120, 300, 600};
            else if (key == "checks.max_output_kib")
                values = {64, 256, 1024, 4096};
            else if (key == "checks.watch_debounce_ms")
                values = {100, 200, 400, 800, 1500, 3000};
            if (std::ranges::find(values, current) == values.end())
                values.push_back(current);
            std::ranges::sort(values);
            std::vector<std::string> out;
            for (int v : values)
                out.push_back(std::to_string(v));
            return out;
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<HelpSection> help_sections(Screen context)
    {
        std::vector<HelpSection> out;
        HelpSection here;
        here.title = std::format("Keys on the {} screen", to_string(context));
        switch (context)
        {
            case Screen::Home:
                here.lines = {"Up Down       choose Continue, Recommended or a recent exercise", "Enter         open the chosen exercise",
                              "n             open the recommended exercise"};
                break;
            case Screen::Learn:
                here.lines = {"Up Down       move through the exercises",
                              "Enter         open the exercise",
                              "/             search; type to filter live, Enter keeps it, Esc clears it",
                              "f d t y       cycle the status, difficulty, track and kind filters",
                              "0             clear every filter",
                              "b             bookmark the exercise"};
                break;
            case Screen::Exercise:
                here.lines = {"Tab           move between instructions, files and results",
                              "Up Down       scroll, or choose a file when the files pane is focused",
                              "Enter         start the exercise (first time) or edit the chosen file",
                              "e             edit the chosen file in your editor",
                              "o             open the workspace folder (graphical editors)",
                              "c             run the checks",
                              "x             cancel a running check",
                              "r             build and run your program",
                              "w             watch: run the checks whenever a file changes",
                              "h             reveal the next hint",
                              "s             show the reference solution (asks first)",
                              "l             show or hide the raw compiler and program output",
                              "n             go to the next exercise      b  bookmark",
                              "R             reset the exercise to its starter files (backup first)"};
                break;
            case Screen::Progress:
                here.lines = {"Up Down       choose an exercise to review", "Enter         open it"};
                break;
            case Screen::Bookmarks:
                here.lines = {"Up Down       choose a bookmark", "Enter         open it", "b             remove the bookmark"};
                break;
            case Screen::Doctor:
                here.lines = {"Up Down       scroll", "r             check again", "t             check again including a real test build"};
                break;
            case Screen::Settings:
                here.lines = {"Up Down       choose a setting", "Enter         change it from a list of valid values"};
                break;
            case Screen::Onboarding:
                here.lines = {"Up Down       choose", "Enter         confirm and continue", "s             skip this step", "Esc           skip the rest"};
                break;
            case Screen::Help:
                here.lines = {"Up Down       scroll", "Esc           go back"};
                break;
        }
        out.push_back(std::move(here));
        out.push_back({"Everywhere",
                       {"1 .. 7        Home, Learn, Exercise, Progress, Bookmarks, Doctor, Settings", "?             this help",
                        "Esc           go back or close a dialog", "q             quit (asks first while a check is running)",
                        "Ctrl+C       cancel a running check; press again to quit"}});
        out.push_back(
            {"A typical session",
             {"1. Open an exercise from Learn and press Enter to create your files.", "2. Press e to edit, then c to check your work.",
              "3. Read the results below the instructions and fix the first failure.", "4. Press h for a hint if you are stuck; s shows the solution last.",
              "5. When every check passes the exercise is completed; press n for the next one."}});
        out.push_back(
            {"Reading check results",
             {"Each check shows pass or FAIL with the stage where it stopped: build, run, compare, harness or timeout.",
              "Expected and Observed show whitespace: a middle dot is a trailing space, an arrow is a tab, the return sign marks the final newline.",
              "file:line:column pointers point into your files; press e to jump there.", "Press l for the full compiler diagnostics and program output."}});
        out.push_back({"Safety", {std::string(runner::safety_note_text)}});
        return out;
    }

    class Model
    {
    public:
        Model(app::Context& context, Size initial) : ctx(&context), size(initial)
        {
            refresh_appearance();
            refresh_progress();
            route_start();
        }

        app::Context* ctx = nullptr;
        Size size;
        Appearance appearance;
        Screen screen = Screen::Home;
        std::vector<Screen> back;
        Screen help_from = Screen::Home;
        progress::Progress prog;
        app::Summary summary;
        std::vector<app::ReviewItem> review;
        std::int64_t now_ms = 0;
        std::deque<Toast> toasts;
        Dialog dialog;
        bool quit_requested = false;

        std::size_t home_sel = 0;
        Filters filters;
        std::string query;
        bool searching = false;
        std::string learn_id;
        std::size_t review_sel = 0;
        std::size_t bookmark_sel = 0;
        std::size_t settings_sel = 0;
        int help_scroll = 0;
        int doctor_scroll = 0;
        std::optional<doctor::DoctorReport> doctor_report;
        bool doctor_running = false;
        OnboardingState onboarding;
        ExerciseState exercise;

        mutable int scroll_content = 0;
        mutable int scroll_viewport = 0;

        [[nodiscard]] LayoutMode layout() const { return layout_for(size); }

        [[nodiscard]] bool text_input_active() const { return (screen == Screen::Learn && searching) || dialog.kind == DialogKind::Reset; }

        [[nodiscard]] bool check_running() const { return exercise.phase != CheckPhase::Idle; }

        [[nodiscard]] progress::Status status_of(std::string_view id) const { return progress::status_of(prog, id); }

        [[nodiscard]] bool bookmarked(std::string_view id) const { return progress::is_bookmarked(prog, id); }

        [[nodiscard]] std::vector<catalog::Exercise const*> learn_list() const
        {
            catalog::Filter f;
            auto const& tracks = ctx->catalog.tracks();
            if (filters.track > 0 && filters.track <= tracks.size())
                f.track = tracks[filters.track - 1].id;
            if (filters.difficulty > 0 && filters.difficulty <= all_difficulties.size())
                f.difficulty = all_difficulties[filters.difficulty - 1];
            if (filters.kind > 0 && filters.kind <= all_kinds.size())
                f.kind = all_kinds[filters.kind - 1];
            f.query = std::string(text::trim(query));
            return ctx->catalog.filter(f, [this](catalog::Exercise const& e) {
                auto const st = progress::status_of(prog, e.id);
                switch (filters.status)
                {
                    case StatusFilter::All:
                        return true;
                    case StatusFilter::NotStarted:
                        return st == progress::Status::NotStarted;
                    case StatusFilter::InProgress:
                        return st == progress::Status::InProgress;
                    case StatusFilter::Completed:
                        return st == progress::Status::Completed;
                    case StatusFilter::Unfinished:
                        return st != progress::Status::Completed;
                }
                return true;
            });
        }

        [[nodiscard]] std::size_t learn_index(std::vector<catalog::Exercise const*> const& list) const
        {
            for (std::size_t i = 0; i < list.size(); ++i)
                if (list[i]->id == learn_id)
                    return i;
            return 0;
        }

        [[nodiscard]] std::vector<HomeItem> home_items() const
        {
            std::vector<HomeItem> items;
            std::set<std::string> seen;
            if (!prog.active_exercise.empty() && ctx->catalog.find(prog.active_exercise))
            {
                items.push_back({HomeKind::Continue, prog.active_exercise});
                seen.insert(prog.active_exercise);
            }
            if (auto const* next = ctx->catalog.next_recommended(app::completed_predicate(prog), prog.active_exercise))
                if (!seen.contains(next->id))
                {
                    items.push_back({HomeKind::Next, next->id});
                    seen.insert(next->id);
                }
            for (auto const& id : prog.recent)
            {
                if (seen.contains(id) || !ctx->catalog.find(id))
                    continue;
                items.push_back({HomeKind::Recent, id});
                seen.insert(id);
                if (items.size() >= 6)
                    break;
            }
            return items;
        }

        [[nodiscard]] std::vector<catalog::Exercise const*> bookmark_list() const
        {
            std::vector<catalog::Exercise const*> out;
            for (auto const& id : prog.bookmarks)
                if (auto const* ex = ctx->catalog.find(id))
                    out.push_back(ex);
            return out;
        }

        [[nodiscard]] std::vector<SettingRow> settings_rows() const
        {
            std::vector<SettingRow> rows;
            for (auto const& info : settings::keys())
            {
                SettingRow row;
                row.key = info.key;
                row.description = info.description;
                row.value = settings::get(ctx->settings, info.key).value_or("");
                if (info.kind == settings::ValueKind::StringList && row.value.empty())
                    row.value = "(not set)";
                else if (info.kind == settings::ValueKind::String && row.value.empty())
                    row.value = "(search automatically)";
                row.editable = info.kind != settings::ValueKind::String && info.kind != settings::ValueKind::StringList && info.key != "onboarding.done";
                rows.push_back(std::move(row));
            }
            return rows;
        }

        [[nodiscard]] catalog::Exercise const* selected_exercise() const
        {
            switch (screen)
            {
                case Screen::Learn: {
                    auto const list = learn_list();
                    if (list.empty())
                        return nullptr;
                    return list[learn_index(list)];
                }
                case Screen::Home: {
                    auto const items = home_items();
                    if (items.empty())
                        return nullptr;
                    return ctx->catalog.find(items[std::min(home_sel, items.size() - 1)].id);
                }
                case Screen::Bookmarks: {
                    auto const list = bookmark_list();
                    if (list.empty())
                        return nullptr;
                    return list[std::min(bookmark_sel, list.size() - 1)];
                }
                case Screen::Progress:
                    if (review.empty())
                        return nullptr;
                    return review[std::min(review_sel, review.size() - 1)].exercise;
                case Screen::Exercise:
                    return exercise.ex;
                default:
                    return nullptr;
            }
        }

        [[nodiscard]] std::string selected_file() const
        {
            if (exercise.file_sel < exercise.files.size())
                return exercise.files[exercise.file_sel];
            return {};
        }

        [[nodiscard]] std::vector<KeyHint> key_hints() const
        {
            std::vector<KeyHint> k;
            if (dialog.open())
            {
                switch (dialog.kind)
                {
                    case DialogKind::Reset:
                        return {{"type y", "confirm"}, {"Enter", "reset"}, {"Esc", "cancel"}};
                    case DialogKind::Choice:
                        return {{"Up Down", "choose"}, {"Enter", "select"}, {"Esc", "cancel"}};
                    default:
                        return {{"y", "yes"}, {"n Esc", "no"}};
                }
            }
            if (screen == Screen::Learn && searching)
                return {{"type", "to search"}, {"Enter", "keep"}, {"Esc", "clear"}};
            switch (screen)
            {
                case Screen::Onboarding:
                    k = {{"Up Down", "choose"}, {"Enter", "next"}, {"s", "skip step"}, {"Esc", "skip all"}};
                    return k;
                case Screen::Home:
                    k = {{"Enter", "open"}, {"Up Down", "move"}, {"n", "next"}, {"2", "learn"}};
                    break;
                case Screen::Learn:
                    k = {{"Enter", "open"}, {"/", "search"}, {"f d t y", "filter"}, {"b", "bookmark"}};
                    break;
                case Screen::Exercise:
                    if (exercise.solution)
                        k = {{"Up Down", "scroll"}, {"Esc", "close solution"}};
                    else if (!exercise.ws)
                        k = {{"Enter", "start"}, {"h", "hint"}, {"s", "solution"}, {"Tab", "pane"}};
                    else if (check_running())
                        k = {{"x", "cancel"}, {"e", "edit"}, {"Tab", "pane"}, {"w", "watch"}};
                    else if (exercise.pane == Pane::Files)
                        k = {{"Up Down", "file"}, {"Enter", "edit"}, {"PgUp PgDn", "scroll"}, {"c", "check"}, {"Tab", "pane"}};
                    else if (exercise.pane == Pane::Results)
                        k = {{"Up Down", "scroll"}, {"l", "raw output"}, {"c", "check"}, {"e", "edit"}, {"Tab", "pane"}};
                    else
                        k = {{"e", "edit"}, {"c", "check"}, {"r", "run"}, {"h", "hint"}, {"s", "solution"}, {"w", "watch"}, {"Tab", "pane"}};
                    break;
                case Screen::Progress:
                    k = {{"Enter", "open"}, {"Up Down", "move"}};
                    break;
                case Screen::Bookmarks:
                    k = {{"Enter", "open"}, {"b", "remove"}};
                    break;
                case Screen::Doctor:
                    k = {{"r", "recheck"}, {"t", "test build"}, {"Up Down", "scroll"}};
                    break;
                case Screen::Settings:
                    k = {{"Enter", "change"}, {"Up Down", "move"}};
                    break;
                case Screen::Help:
                    k = {{"Up Down", "scroll"}, {"Esc", "back"}};
                    return k;
            }
            k.push_back({"?", "help"});
            k.push_back({"q", "quit"});
            return k;
        }

        std::vector<Effect> handle(Event const& event)
        {
            fx.clear();
            std::visit([this](auto const& e) { on(e); }, event);
            return std::move(fx);
        }

        void push_toast(std::string text, bool error = false, std::int64_t duration_ms = 0)
        {
            Toast t;
            t.text = std::move(text);
            t.error = error;
            t.duration_ms = duration_ms > 0 ? duration_ms : (error ? 6000 : 4000);
            if (toasts.empty())
                t.shown_ms = now_ms;
            toasts.push_back(std::move(t));
        }

        void refresh_appearance() { appearance = appearance_of(ctx->settings, ctx->env); }

        void refresh_progress()
        {
            auto p = app::load_progress(*ctx);
            if (!p)
            {
                push_toast(std::format("Could not read your progress: {}", text::sanitize(p.error().message)), true);
                return;
            }
            prog = std::move(*p);
            summary = app::summarize(*ctx, prog);
            review = app::review_list(*ctx, prog);
            normalize_learn();
            if (home_sel > 0)
                home_sel = std::min(home_sel, std::max<std::size_t>(home_items().size(), 1) - 1);
            auto const marks = bookmark_list().size();
            if (marks > 0)
                bookmark_sel = std::min(bookmark_sel, marks - 1);
            else
                bookmark_sel = 0;
            if (!review.empty())
                review_sel = std::min(review_sel, review.size() - 1);
            else
                review_sel = 0;
        }

        void open_exercise(std::string_view id)
        {
            auto const* ex = ctx->catalog.find(id);
            if (!ex)
            {
                push_toast(std::format("Unknown exercise '{}'", text::sanitize(id)), true);
                return;
            }
            if (exercise.ex && exercise.ex->id != ex->id && check_running())
            {
                push_toast("A check is still running; wait for it or press x to cancel it first", true);
                return;
            }
            if (!exercise.ex || exercise.ex->id != ex->id)
            {
                if (exercise.watching)
                    fx.push_back(StopWatch{});
                exercise = ExerciseState{};
                exercise.ex = ex;
                load_workspace();
                if (auto it = prog.exercises.find(ex->id); it != prog.exercises.end())
                    exercise.hints_shown = it->second.hint_level;
            }
            if (screen != Screen::Exercise)
                enter(Screen::Exercise);
        }

        void route_start()
        {
            if (!ctx->settings.onboarding_done)
            {
                screen = Screen::Onboarding;
                setup_onboarding();
                return;
            }
            screen = Screen::Home;
            if (ctx->settings.ui_open_continue && !prog.active_exercise.empty() && ctx->catalog.find(prog.active_exercise))
                open_exercise(prog.active_exercise);
        }

    private:
        std::vector<Effect> fx;

        void enter(Screen target)
        {
            if (target == screen)
                return;
            if (screen == Screen::Exercise && target != Screen::Help && exercise.watching)
            {
                exercise.watching = false;
                fx.push_back(StopWatch{});
            }
            if (target != Screen::Help && screen != Screen::Help)
                back.push_back(screen);
            else if (target == Screen::Help)
            {
                help_from = screen;
                back.push_back(screen);
            }
            screen = target;
            on_enter();
        }

        void jump(Screen target)
        {
            if (target == screen)
                return;
            if (screen == Screen::Exercise && exercise.watching)
            {
                exercise.watching = false;
                fx.push_back(StopWatch{});
            }
            back.clear();
            screen = target;
            searching = false;
            on_enter();
        }

        void go_back()
        {
            if (screen == Screen::Exercise && exercise.watching && (back.empty() || back.back() != Screen::Help))
            {
                exercise.watching = false;
                fx.push_back(StopWatch{});
            }
            if (!back.empty())
            {
                screen = back.back();
                back.pop_back();
            }
            else if (screen != Screen::Home)
                screen = Screen::Home;
            on_enter();
        }

        void on_enter()
        {
            switch (screen)
            {
                case Screen::Home:
                case Screen::Learn:
                case Screen::Progress:
                case Screen::Bookmarks:
                    refresh_progress();
                    break;
                case Screen::Doctor:
                    if (!doctor_report && !doctor_running)
                        request_doctor(false);
                    break;
                case Screen::Exercise:
                    refresh_progress();
                    refresh_files();
                    break;
                default:
                    break;
            }
        }

        void request_doctor(bool build)
        {
            if (doctor_running)
                return;
            doctor_running = true;
            fx.push_back(RunDoctor{build});
        }

        void normalize_learn()
        {
            auto const list = learn_list();
            if (list.empty())
            {
                learn_id.clear();
                return;
            }
            if (std::ranges::none_of(list, [&](auto const* e) { return e->id == learn_id; }))
                learn_id = list.front()->id;
        }

        void load_workspace()
        {
            auto& es = exercise;
            es.ws.reset();
            auto found = app::find_workspace(*ctx, prog, *es.ex);
            if (!found)
            {
                push_toast(text::sanitize(found.error().message), true);
                return;
            }
            es.ws = std::move(*found);
            es.files.clear();
            es.file_sel = 0;
            refresh_files();
        }

        void refresh_files()
        {
            auto& es = exercise;
            if (!es.ex)
                return;
            if (!es.ws)
            {
                es.files.clear();
                es.preview_lines.clear();
                es.preview_note.clear();
                return;
            }
            auto const previous = selected_file();
            es.files = workspace::files(*es.ws);
            es.file_sel = 0;
            auto const wanted = previous.empty() ? es.ex->entry_file : previous;
            for (std::size_t i = 0; i < es.files.size(); ++i)
                if (es.files[i] == wanted)
                    es.file_sel = i;
            load_preview();
        }

        void load_preview()
        {
            auto& es = exercise;
            es.preview_lines.clear();
            es.preview_note.clear();
            if (!es.ws || es.file_sel >= es.files.size())
                return;
            auto const path = es.ws->root / platform::path_from_utf8(es.files[es.file_sel]);
            auto data = platform::read_file(path);
            if (!data)
            {
                es.preview_note = "this file cannot be read";
                return;
            }
            if (data->find('\0') != std::string::npos)
            {
                es.preview_note = "binary file";
                return;
            }
            constexpr std::size_t max_bytes = 256 * 1024;
            bool const clipped = data->size() > max_bytes;
            auto body = text::sanitize(text::clamp_bytes(*data, max_bytes));
            body = text::normalize_newlines(body);
            auto lines = text::split_lines(body);
            if (!lines.empty() && lines.back().empty() && body.ends_with('\n'))
                lines.pop_back();
            for (auto& line : lines)
                line = text::replace_all(line, "\t", "    ");
            es.preview_lines = std::move(lines);
            if (clipped)
                es.preview_note = "file clipped";
        }

        void mark_stale_if_changed()
        {
            auto& es = exercise;
            if (!es.report || !es.ws || es.stale || es.report_snapshot.empty())
                return;
            auto const now = watch::snapshot(es.ws->root);
            if (!watch::compare(es.report_snapshot, now).empty())
                es.stale = true;
        }

        void scroll_by(int& value, int delta) const
        {
            int upper = std::numeric_limits<int>::max();
            if (scroll_content > 0)
                upper = std::max(0, scroll_content - scroll_viewport);
            value = std::clamp(std::min(value, upper) + delta, 0, upper);
        }

        [[nodiscard]] int page() const { return std::max(3, scroll_viewport > 0 ? scroll_viewport - 1 : 10); }

        void on(Resize const& r)
        {
            size.width = std::max(1, r.width);
            size.height = std::max(1, r.height);
        }

        void on(CheckStage const& s)
        {
            if (exercise.phase == CheckPhase::Running)
                exercise.stage_text = text::sanitize(s.text);
        }

        void on(Tick const& t)
        {
            now_ms = std::max(now_ms, t.now_ms);
            while (!toasts.empty())
            {
                auto& front = toasts.front();
                if (front.shown_ms < 0)
                    front.shown_ms = now_ms;
                if (now_ms - front.shown_ms >= front.duration_ms)
                    toasts.pop_front();
                else
                    break;
            }
        }

        void on(DoctorDone const& d)
        {
            doctor_report = d.report;
            doctor_running = false;
        }

        void on(ProgramDone const& p)
        {
            push_toast(text::sanitize(p.message), !p.ok);
            refresh_files();
            mark_stale_if_changed();
        }

        void on(EditorDone const& e)
        {
            if (!e.message.empty())
                push_toast(text::sanitize(e.message), !e.ok);
            refresh_files();
            mark_stale_if_changed();
        }

        void on(FilesChanged const&)
        {
            auto& es = exercise;
            if (!es.ex || !es.ws)
                return;
            refresh_files();
            if (es.report)
                es.stale = true;
            if (!es.watching)
                return;
            if (es.phase == CheckPhase::Idle)
                begin_check();
            else
                es.rerun_pending = true;
        }

        void on(CheckDone const& done)
        {
            auto& es = exercise;
            if (!es.ex || es.ex->id != done.report.exercise)
                return;
            bool const was_cancelling = es.phase == CheckPhase::Cancelling;
            es.phase = CheckPhase::Idle;
            es.stage_text.clear();
            es.report = done.report;
            es.stale = false;
            es.scroll_results = 0;
            mark_stale_if_changed();
            auto rec = app::record_report(*ctx, *es.ex, done.report);
            refresh_progress();
            if (!rec)
                push_toast(std::format("Could not save your progress: {}", text::sanitize(rec.error().message)), true);
            else if (done.report.outcome == runner::Outcome::Cancelled)
                push_toast("Check cancelled");
            else if (rec->completed_now)
            {
                std::string t = "All checks passed: exercise completed!";
                if (!rec->next_title.empty())
                    t += std::format(" Press n for the next one: {}", text::sanitize(rec->next_title));
                push_toast(std::move(t));
            }
            else if (rec->all_passed)
                push_toast("All checks passed");
            else if (done.report.outcome == runner::Outcome::Error)
                push_toast("The checks could not run; see the results below", true);
            if (was_cancelling && es.watching)
            {
                es.rerun_pending = false;
                return;
            }
            if (es.rerun_pending && es.watching)
            {
                es.rerun_pending = false;
                begin_check();
            }
        }

        void begin_check()
        {
            auto& es = exercise;
            if (!es.ex || !es.ws || es.phase != CheckPhase::Idle)
                return;
            es.phase = CheckPhase::Running;
            es.stage_text = "Starting";
            es.started_ms = now_ms;
            es.report_snapshot = watch::snapshot(es.ws->root);
            es.rerun_pending = false;
            if (es.report)
                es.stale = true;
            fx.push_back(StartCheck{es.ex->id, es.ws->root});
        }

        void on(Key const& key)
        {
            auto const& k = key.name;
            if (k == "CtrlC")
            {
                ctrl_c();
                return;
            }
            if (dialog.open())
            {
                dialog_key(k);
                return;
            }
            if (screen == Screen::Learn && searching)
            {
                search_key(k);
                return;
            }
            if (screen == Screen::Exercise && exercise.solution)
            {
                solution_key(k);
                return;
            }
            if (k == "q")
            {
                request_quit();
                return;
            }
            if (screen != Screen::Onboarding)
            {
                if (k == "?")
                {
                    if (screen != Screen::Help)
                        enter(Screen::Help);
                    return;
                }
                if (k.size() == 1 && k[0] >= '1' && k[0] <= '7')
                {
                    sidebar_jump(k[0] - '0');
                    return;
                }
            }
            switch (screen)
            {
                case Screen::Onboarding:
                    onboarding_key(k);
                    break;
                case Screen::Home:
                    home_key(k);
                    break;
                case Screen::Learn:
                    learn_key(k);
                    break;
                case Screen::Exercise:
                    exercise_key(k);
                    break;
                case Screen::Progress:
                    progress_key(k);
                    break;
                case Screen::Bookmarks:
                    bookmarks_key(k);
                    break;
                case Screen::Doctor:
                    doctor_key(k);
                    break;
                case Screen::Settings:
                    settings_key(k);
                    break;
                case Screen::Help:
                    help_key(k);
                    break;
            }
        }

        void ctrl_c()
        {
            auto& es = exercise;
            if (es.phase == CheckPhase::Running)
            {
                es.phase = CheckPhase::Cancelling;
                dialog = Dialog{};
                fx.push_back(CancelCheck{});
                push_toast("Cancelling the check; press Ctrl+C again to quit");
                return;
            }
            quit_requested = true;
            if (es.phase == CheckPhase::Cancelling)
                fx.push_back(CancelCheck{});
            fx.push_back(Quit{});
        }

        void request_quit()
        {
            if (check_running())
            {
                dialog = Dialog{};
                dialog.kind = DialogKind::QuitRunning;
                dialog.title = "Quit while a check is running?";
                dialog.body = {"The running check will be cancelled before learndc exits."};
                return;
            }
            quit_requested = true;
            fx.push_back(Quit{});
        }

        void sidebar_jump(int n)
        {
            switch (n)
            {
                case 1:
                    jump(Screen::Home);
                    break;
                case 2:
                    jump(Screen::Learn);
                    break;
                case 3:
                    if (exercise.ex)
                        jump(Screen::Exercise);
                    else if (!prog.active_exercise.empty() && ctx->catalog.find(prog.active_exercise))
                    {
                        back.clear();
                        open_exercise(prog.active_exercise);
                    }
                    else
                        push_toast("No exercise is open yet; choose one in Learn");
                    break;
                case 4:
                    jump(Screen::Progress);
                    break;
                case 5:
                    jump(Screen::Bookmarks);
                    break;
                case 6:
                    jump(Screen::Doctor);
                    break;
                case 7:
                    jump(Screen::Settings);
                    break;
                default:
                    break;
            }
        }

        void toggle_bookmark(catalog::Exercise const* ex)
        {
            if (!ex)
                return;
            bool const wanted = !bookmarked(ex->id);
            auto r = app::set_bookmark(*ctx, *ex, wanted);
            if (!r)
            {
                push_toast(text::sanitize(r.error().message), true);
                return;
            }
            refresh_progress();
            push_toast(wanted ? std::format("Bookmarked {}", ex->title) : std::format("Removed the bookmark of {}", ex->title));
        }

        void home_key(std::string const& k)
        {
            auto const items = home_items();
            if (detail::is_down(k) && !items.empty())
                home_sel = std::min(home_sel + 1, items.size() - 1);
            else if (detail::is_up(k) && home_sel > 0)
                --home_sel;
            else if (k == "Enter" && !items.empty())
                open_exercise(items[std::min(home_sel, items.size() - 1)].id);
            else if (k == "n")
            {
                for (auto const& item : items)
                    if (item.kind == HomeKind::Next)
                    {
                        open_exercise(item.id);
                        return;
                    }
                push_toast("Nothing left to recommend right now");
            }
            else if (k == "b")
                toggle_bookmark(selected_exercise());
            else if (k == "Esc")
                go_back();
        }

        void learn_move(int delta)
        {
            auto const list = learn_list();
            if (list.empty())
                return;
            auto const idx = static_cast<long>(learn_index(list));
            auto const next = std::clamp<long>(idx + delta, 0, static_cast<long>(list.size()) - 1);
            learn_id = list[static_cast<std::size_t>(next)]->id;
        }

        void search_key(std::string const& k)
        {
            if (k == "Esc")
            {
                query.clear();
                searching = false;
                normalize_learn();
            }
            else if (k == "Enter")
                searching = false;
            else if (k == "Backspace")
            {
                if (!query.empty())
                {
                    auto cut = query.size() - 1;
                    while (cut > 0 && (static_cast<unsigned char>(query[cut]) & 0xC0) == 0x80)
                        --cut;
                    query.erase(cut);
                }
                normalize_learn();
            }
            else if (k == "Down")
                learn_move(1);
            else if (k == "Up")
                learn_move(-1);
            else if (detail::printable_key(k))
            {
                query += k;
                normalize_learn();
            }
        }

        void learn_key(std::string const& k)
        {
            auto const tracks = ctx->catalog.tracks().size();
            if (detail::is_down(k))
                learn_move(1);
            else if (detail::is_up(k))
                learn_move(-1);
            else if (k == "PageDown")
                learn_move(8);
            else if (k == "PageUp")
                learn_move(-8);
            else if (k == "Home")
                learn_move(-100000);
            else if (k == "End")
                learn_move(100000);
            else if (k == "/")
                searching = true;
            else if (k == "Enter")
            {
                if (auto const* ex = selected_exercise())
                    open_exercise(ex->id);
            }
            else if (k == "b")
                toggle_bookmark(selected_exercise());
            else if (k == "f")
            {
                filters.status = static_cast<StatusFilter>((static_cast<int>(filters.status) + 1) % 5);
                normalize_learn();
            }
            else if (k == "d")
            {
                filters.difficulty = (filters.difficulty + 1) % (all_difficulties.size() + 1);
                normalize_learn();
            }
            else if (k == "t")
            {
                filters.track = (filters.track + 1) % (tracks + 1);
                normalize_learn();
            }
            else if (k == "y")
            {
                filters.kind = (filters.kind + 1) % (all_kinds.size() + 1);
                normalize_learn();
            }
            else if (k == "0")
            {
                filters = Filters{};
                normalize_learn();
            }
            else if (k == "Esc")
            {
                if (!query.empty())
                {
                    query.clear();
                    normalize_learn();
                }
                else
                    go_back();
            }
        }

        void progress_key(std::string const& k)
        {
            if (detail::is_down(k) && !review.empty())
                review_sel = std::min(review_sel + 1, review.size() - 1);
            else if (detail::is_up(k) && review_sel > 0)
                --review_sel;
            else if (k == "Enter" && !review.empty())
                open_exercise(review[std::min(review_sel, review.size() - 1)].exercise->id);
            else if (k == "b")
                toggle_bookmark(selected_exercise());
            else if (k == "Esc")
                go_back();
        }

        void bookmarks_key(std::string const& k)
        {
            auto const list = bookmark_list();
            if (detail::is_down(k) && !list.empty())
                bookmark_sel = std::min(bookmark_sel + 1, list.size() - 1);
            else if (detail::is_up(k) && bookmark_sel > 0)
                --bookmark_sel;
            else if (k == "Enter" && !list.empty())
                open_exercise(list[std::min(bookmark_sel, list.size() - 1)]->id);
            else if (k == "b" && !list.empty())
                toggle_bookmark(selected_exercise());
            else if (k == "Esc")
                go_back();
        }

        void doctor_key(std::string const& k)
        {
            if (detail::is_down(k))
                scroll_by(doctor_scroll, 1);
            else if (detail::is_up(k))
                scroll_by(doctor_scroll, -1);
            else if (k == "PageDown")
                scroll_by(doctor_scroll, page());
            else if (k == "PageUp")
                scroll_by(doctor_scroll, -page());
            else if (k == "r")
            {
                if (doctor_running)
                    push_toast("The checks are still running");
                else
                    request_doctor(false);
            }
            else if (k == "t")
            {
                if (doctor_running)
                    push_toast("The checks are still running");
                else
                    request_doctor(true);
            }
            else if (k == "Esc")
                go_back();
        }

        void help_key(std::string const& k)
        {
            if (detail::is_down(k))
                scroll_by(help_scroll, 1);
            else if (detail::is_up(k))
                scroll_by(help_scroll, -1);
            else if (k == "PageDown")
                scroll_by(help_scroll, page());
            else if (k == "PageUp")
                scroll_by(help_scroll, -page());
            else if (k == "Home")
                help_scroll = 0;
            else if (k == "Esc" || k == "Enter")
                go_back();
        }

        void settings_key(std::string const& k)
        {
            auto const rows = settings_rows();
            if (detail::is_down(k))
                settings_sel = std::min(settings_sel + 1, rows.size() - 1);
            else if (detail::is_up(k) && settings_sel > 0)
                --settings_sel;
            else if (k == "Enter")
                edit_setting(rows[std::min(settings_sel, rows.size() - 1)]);
            else if (k == "Esc")
                go_back();
        }

        void edit_setting(SettingRow const& row)
        {
            auto const* info = settings::find_key(row.key);
            if (!info)
                return;
            if (!row.editable)
            {
                push_toast(std::format("Change it from the command line: learndc config set {} <value>", row.key));
                return;
            }
            Dialog d;
            d.kind = DialogKind::Choice;
            d.title = row.key;
            d.body = {info->description};
            d.target = row.key;
            switch (info->kind)
            {
                case settings::ValueKind::Choice:
                    d.values = info->choices;
                    break;
                case settings::ValueKind::Bool:
                    d.values = {"true", "false"};
                    break;
                case settings::ValueKind::Int:
                    d.values = detail::int_choices(row.key, std::stoi(row.value));
                    break;
                default:
                    return;
            }
            d.labels = d.values;
            for (std::size_t i = 0; i < d.values.size(); ++i)
                if (d.values[i] == row.value)
                    d.selected = i;
            dialog = std::move(d);
        }

        bool save_setting(std::string const& key, std::string const& value)
        {
            auto updated = settings::update(ctx->dirs.config_file, [&](settings::Settings& s) { return settings::set(s, key, value); });
            if (!updated)
            {
                push_toast(text::sanitize(updated.error().message), true);
                return false;
            }
            ctx->settings = *updated;
            refresh_appearance();
            return true;
        }

        void setup_onboarding()
        {
            onboarding = OnboardingState{};
            auto const found = editor::detect(ctx->env, ctx->os);
            for (auto const& d : found)
            {
                onboarding.editor_values.push_back(d.preset);
                onboarding.editor_labels.push_back(std::format("{} (found on this computer)", d.preset));
            }
            onboarding.editor_values.push_back("auto");
            onboarding.editor_labels.push_back("Automatic (use VISUAL or EDITOR, or what is installed)");
            onboarding.editor_values.push_back("manual");
            onboarding.editor_labels.push_back("None: show me the file path and I open it myself");
            onboarding_select_current();
        }

        [[nodiscard]] std::vector<std::string> onboarding_values() const
        {
            switch (onboarding.step)
            {
                case 0:
                    return {"new", "experienced"};
                case 1:
                    return onboarding.editor_values;
                default:
                    return {"dark", "light", "high-contrast", "no-color"};
            }
        }

        void onboarding_select_current()
        {
            auto const values = onboarding_values();
            std::string current;
            if (onboarding.step == 0)
                current = ctx->settings.learner_level;
            else if (onboarding.step == 1)
                current = ctx->settings.editor_preset;
            else
                current = ctx->settings.ui_theme;
            onboarding.selected = 0;
            for (std::size_t i = 0; i < values.size(); ++i)
                if (values[i] == current)
                    onboarding.selected = i;
        }

        void onboarding_next()
        {
            ++onboarding.step;
            if (onboarding.step >= 3)
            {
                finish_onboarding();
                return;
            }
            onboarding_select_current();
        }

        void finish_onboarding()
        {
            if (!save_setting("onboarding.done", "true"))
                return;
            back.clear();
            refresh_progress();
            route_start();
        }

        void onboarding_key(std::string const& k)
        {
            auto const values = onboarding_values();
            if (detail::is_down(k))
                onboarding.selected = std::min(onboarding.selected + 1, values.size() - 1);
            else if (detail::is_up(k) && onboarding.selected > 0)
                --onboarding.selected;
            else if (k == "s")
                onboarding_next();
            else if (k == "Esc")
                finish_onboarding();
            else if (k == "Enter")
            {
                static constexpr std::array<std::string_view, 3> keys = {"learner.level", "editor.preset", "ui.theme"};
                if (onboarding.selected < values.size() &&
                    save_setting(std::string(keys[static_cast<std::size_t>(onboarding.step)]), values[onboarding.selected]))
                    onboarding_next();
            }
        }

        void dialog_key(std::string const& k)
        {
            switch (dialog.kind)
            {
                case DialogKind::Choice:
                    if (k == "Esc")
                        dialog = Dialog{};
                    else if (detail::is_down(k))
                        dialog.selected = std::min(dialog.selected + 1, dialog.values.size() - 1);
                    else if (detail::is_up(k) && dialog.selected > 0)
                        --dialog.selected;
                    else if (k == "Enter")
                    {
                        auto const key = dialog.target;
                        auto const value = dialog.values[dialog.selected];
                        dialog = Dialog{};
                        if (save_setting(key, value))
                            push_toast(std::format("Saved {} = {}", key, value));
                    }
                    break;
                case DialogKind::QuitRunning:
                    if (k == "y" || k == "Y" || k == "Enter")
                    {
                        dialog = Dialog{};
                        quit_requested = true;
                        fx.push_back(CancelCheck{});
                        fx.push_back(Quit{});
                    }
                    else if (k == "n" || k == "N" || k == "Esc")
                        dialog = Dialog{};
                    break;
                case DialogKind::Solution:
                    if (k == "y" || k == "Y" || k == "Enter")
                    {
                        dialog = Dialog{};
                        reveal_solution();
                    }
                    else if (k == "n" || k == "N" || k == "Esc")
                        dialog = Dialog{};
                    break;
                case DialogKind::Reset:
                    if (k == "Esc")
                        dialog = Dialog{};
                    else if (k == "Backspace")
                    {
                        if (!dialog.input.empty())
                            dialog.input.pop_back();
                    }
                    else if (k == "Enter")
                    {
                        if (dialog.input == "y" || dialog.input == "Y")
                        {
                            dialog = Dialog{};
                            reset_exercise();
                        }
                        else
                            push_toast("Type y to confirm the reset, or press Esc to cancel", true);
                    }
                    else if (detail::printable_key(k) && dialog.input.size() < 8)
                        dialog.input += k;
                    break;
                case DialogKind::None:
                    break;
            }
        }

        void reveal_solution()
        {
            auto& es = exercise;
            if (!es.ex)
                return;
            auto shown = app::reveal_solution(*ctx, *es.ex, es.ws, es.ws.has_value());
            if (!shown)
            {
                push_toast(text::sanitize(shown.error().message), true);
                return;
            }
            SolutionView view;
            view.has_workspace = es.ws.has_value();
            for (auto const& [path, bytes] : es.ex->solution)
            {
                SolutionFile f;
                f.path = path;
                std::string mine;
                if (es.ws)
                    if (auto data = platform::read_file(es.ws->root / platform::path_from_utf8(path)))
                        mine = text::normalize_newlines(text::sanitize(*data));
                auto const theirs = text::normalize_newlines(bytes);
                f.identical = diff::identical(mine, theirs);
                f.rows = diff::side_by_side(mine, theirs);
                f.unified = diff::unified(mine, theirs, view.has_workspace ? "your " + path : "empty", "solution " + path);
                view.files.push_back(std::move(f));
            }
            view.explanation = text::sanitize(es.ex->explanation);
            es.solution = std::move(view);
            refresh_progress();
        }

        void reset_exercise()
        {
            auto& es = exercise;
            if (!es.ex || !es.ws)
                return;
            if (check_running())
            {
                push_toast("Wait for the running check to finish (or cancel it) before resetting", true);
                return;
            }
            auto r = app::reset_exercise(*ctx, *es.ex, *es.ws, true);
            if (!r)
            {
                push_toast(text::sanitize(r.error().message), true);
                return;
            }
            es.report.reset();
            es.stale = false;
            es.scroll_preview = 0;
            es.scroll_results = 0;
            refresh_files();
            refresh_progress();
            auto const room = static_cast<std::size_t>(std::max(24, size.width - 40));
            push_toast(std::format("Reset. Your previous files are saved in {}", text::truncate_middle(platform::path_to_utf8(r->backup_path), room)), false, 12000);
        }

        void solution_key(std::string const& k)
        {
            auto& sol = *exercise.solution;
            if (k == "Esc" || k == "q" || k == "Enter")
                exercise.solution.reset();
            else if (detail::is_down(k))
                scroll_by(sol.scroll, 1);
            else if (detail::is_up(k))
                scroll_by(sol.scroll, -1);
            else if (k == "PageDown")
                scroll_by(sol.scroll, page());
            else if (k == "PageUp")
                scroll_by(sol.scroll, -page());
            else if (k == "Home")
                sol.scroll = 0;
            else if (k == "End")
                sol.scroll = 1 << 20;
        }

        void start_workspace()
        {
            auto& es = exercise;
            auto started = app::start_exercise(*ctx, *es.ex, std::nullopt);
            if (!started)
            {
                push_toast(text::sanitize(started.error().message), true);
                return;
            }
            refresh_progress();
            load_workspace();
            push_toast(started->result.created ? "Created your files. Press e to edit them, then c to check." : "Resumed your existing files");
            if (!started->missing_prerequisites.empty())
                push_toast(std::format("Note: finish {} first for the best order", text::join(started->missing_prerequisites, ", ")));
            if (!started->platform_supported)
                push_toast("Note: this exercise is not validated on this platform");
        }

        void edit_file(bool folder)
        {
            auto& es = exercise;
            if (!es.ex)
                return;
            if (!es.ws)
            {
                start_workspace();
                if (!es.ws)
                    return;
            }
            auto choice = editor::resolve(ctx->settings, ctx->env, std::nullopt, ctx->os);
            auto const rel = selected_file().empty() ? es.ex->entry_file : selected_file();
            auto const absolute = platform::path_to_utf8(es.ws->root / platform::path_from_utf8(rel));
            auto const root = platform::path_to_utf8(es.ws->root);
            if (choice.manual())
            {
                std::string t = std::format("Open {} in your editor", folder ? root : absolute);
                if (!choice.problem.empty())
                    t += std::format(" ({})", choice.problem);
                t += ". Change the editor in Settings.";
                push_toast(text::sanitize(t));
                return;
            }
            if (folder && choice.kind == editor::Kind::Terminal && choice.folder_template.empty())
            {
                push_toast("This editor runs in the terminal and cannot open a folder; press e to open a file", true);
                return;
            }
            LaunchEditor le;
            le.choice = std::move(choice);
            le.file = absolute;
            le.workspace = root;
            le.folder = folder;
            if (es.report && !folder)
                if (auto const at = locate(*es.report, rel))
                {
                    le.line = at->first;
                    le.column = at->second;
                }
            fx.push_back(std::move(le));
        }

        [[nodiscard]] static std::optional<std::pair<int, int>> locate(runner::CheckReport const& report, std::string const& rel)
        {
            for (auto const& result : report.results)
                for (auto const& loc : result.locations)
                    if (loc.line > 0 && detail::relative_name(report.workspace, loc.file) == rel)
                        return std::pair<int, int>{loc.line, std::max(1, loc.column)};
            return std::nullopt;
        }

        void exercise_key(std::string const& k)
        {
            auto& es = exercise;
            if (!es.ex)
            {
                go_back();
                return;
            }
            if (k == "Esc")
            {
                go_back();
                return;
            }
            if (k == "Tab" || k == "ShiftTab")
            {
                int p = static_cast<int>(es.pane);
                p = (p + (k == "Tab" ? 1 : 2)) % 3;
                es.pane = static_cast<Pane>(p);
                return;
            }
            if (k == "Enter")
            {
                if (!es.ws)
                    start_workspace();
                else if (es.pane == Pane::Files)
                    edit_file(false);
                return;
            }
            if (k == "e")
            {
                edit_file(false);
                return;
            }
            if (k == "o")
            {
                edit_file(true);
                return;
            }
            if (k == "c")
            {
                if (!es.ws)
                    push_toast("Press Enter to create your files first", true);
                else if (check_running())
                    push_toast("A check is already running");
                else
                    begin_check();
                return;
            }
            if (k == "x")
            {
                if (es.phase == CheckPhase::Running)
                {
                    es.phase = CheckPhase::Cancelling;
                    fx.push_back(CancelCheck{});
                    push_toast("Cancelling the check");
                }
                else if (es.phase == CheckPhase::Cancelling)
                    push_toast("Still cancelling");
                else
                    push_toast("No check is running");
                return;
            }
            if (k == "r")
            {
                if (!es.ws)
                    push_toast("Press Enter to create your files first", true);
                else if (check_running())
                    push_toast("Wait for the check to finish first", true);
                else
                    fx.push_back(RunProgram{es.ex->id, es.ws->root});
                return;
            }
            if (k == "w")
            {
                if (!es.ws)
                {
                    push_toast("Press Enter to create your files first", true);
                    return;
                }
                es.watching = !es.watching;
                if (es.watching)
                {
                    fx.push_back(StartWatch{es.ex->id, es.ws->root});
                    push_toast("Watching your files: the checks run whenever you save");
                }
                else
                {
                    es.rerun_pending = false;
                    fx.push_back(StopWatch{});
                    push_toast("Stopped watching");
                }
                return;
            }
            if (k == "h")
            {
                auto hint = app::reveal_hint(*ctx, *es.ex, std::nullopt);
                if (!hint)
                {
                    push_toast(text::sanitize(hint.error().message), true);
                    return;
                }
                es.hints_shown = hint->level;
                es.pane = Pane::Instructions;
                es.scroll_instructions = 1 << 20;
                refresh_progress();
                if (hint->all_shown)
                    push_toast("All three hints are already shown");
                return;
            }
            if (k == "s")
            {
                dialog = Dialog{};
                dialog.kind = DialogKind::Solution;
                dialog.title = "Show the solution?";
                dialog.body = {"Try the hints first: working it out yourself teaches the most.",
                               "Looking is recorded in your progress; your files are not changed."};
                return;
            }
            if (k == "R")
            {
                if (!es.ws)
                {
                    push_toast("There are no files to reset yet", true);
                    return;
                }
                dialog = Dialog{};
                dialog.kind = DialogKind::Reset;
                dialog.title = "Reset to the starter files?";
                dialog.body = {"Your current files are copied to a backup first, then replaced by the starter files.", "Type y and press Enter to reset."};
                return;
            }
            if (k == "l")
            {
                es.raw_log = !es.raw_log;
                es.scroll_results = 0;
                return;
            }
            if (k == "b")
            {
                toggle_bookmark(es.ex);
                return;
            }
            if (k == "n")
            {
                auto const* next = ctx->catalog.next_recommended(app::completed_predicate(prog), es.ex->id);
                if (!next || next->id == es.ex->id)
                    push_toast("Nothing left to recommend right now");
                else
                    open_exercise(next->id);
                return;
            }
            scroll_key(k);
        }

        void scroll_key(std::string const& k)
        {
            auto& es = exercise;
            int delta = 0;
            if (detail::is_down(k))
                delta = 1;
            else if (detail::is_up(k))
                delta = -1;
            else if (k == "PageDown")
                delta = page();
            else if (k == "PageUp")
                delta = -page();
            else if (k != "Home" && k != "End")
                return;
            switch (es.pane)
            {
                case Pane::Instructions:
                    if (k == "Home")
                        es.scroll_instructions = 0;
                    else if (k == "End")
                        es.scroll_instructions = 1 << 20;
                    else
                        scroll_by(es.scroll_instructions, delta);
                    break;
                case Pane::Files:
                    if (k == "Down" || k == "j" || k == "Up" || k == "k")
                    {
                        if (!es.files.empty())
                        {
                            auto const next = std::clamp<long>(static_cast<long>(es.file_sel) + delta, 0, static_cast<long>(es.files.size()) - 1);
                            if (static_cast<std::size_t>(next) != es.file_sel)
                            {
                                es.file_sel = static_cast<std::size_t>(next);
                                es.scroll_preview = 0;
                                load_preview();
                            }
                        }
                    }
                    else if (k == "Home")
                        es.scroll_preview = 0;
                    else if (k == "End")
                        es.scroll_preview = 1 << 20;
                    else
                        scroll_by(es.scroll_preview, delta);
                    break;
                case Pane::Results:
                    if (k == "Home")
                        es.scroll_results = 0;
                    else if (k == "End")
                        es.scroll_results = 1 << 20;
                    else
                        scroll_by(es.scroll_results, delta);
                    break;
            }
        }
    };

} // namespace learndc::tui::model
