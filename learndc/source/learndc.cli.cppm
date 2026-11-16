export module learndc.cli;

import std;
import learndc.text;
import learndc.markdown;
import learndc.json;
import learndc.platform;
import learndc.catalog;
import learndc.settings;
import learndc.progress;
import learndc.workspace;
import learndc.toolchain;
import learndc.runner;
import learndc.watch;
import learndc.editor;
import learndc.doctor;
import learndc.app;
import learndc.activity;

export namespace learndc::cli
{
#ifdef DCC_VERSION
    inline constexpr std::string_view version_text = DCC_VERSION;
#else
    inline constexpr std::string_view version_text = "unknown";
#endif
#ifdef DCC_GIT_HASH
    inline constexpr std::string_view git_hash_text = DCC_GIT_HASH;
#else
    inline constexpr std::string_view git_hash_text = "unknown";
#endif

    struct Io
    {
        std::ostream* out = &std::cout;
        std::ostream* err = &std::cerr;
        std::istream* in = &std::cin;
        platform::Env env = platform::current_env();
        std::filesystem::path cwd;
        bool stdin_tty = platform::stdin_is_tty();
        bool stdout_tty = platform::stdout_is_tty();
        bool stderr_tty = platform::stderr_is_tty();
        std::atomic<bool> const* cancel = nullptr;
        std::optional<std::filesystem::path> self_exe = platform::executable_path();
        std::function<int(app::Context&)> tui;

        Io()
        {
            std::error_code ec;
            cwd = std::filesystem::current_path(ec);
        }
    };

    struct OptionSpec
    {
        std::string name;
        bool takes_value = false;
        std::string value_name;
        std::string help;
    };

    struct CommandSpec
    {
        std::string name;
        std::string usage;
        std::string summary;
        std::vector<OptionSpec> options;
        std::size_t min_positionals = 0;
        std::size_t max_positionals = 0;
        bool allows_rest = false;
        std::string details;
    };

    struct Parsed
    {
        std::string command;
        std::map<std::string, std::string> values;
        std::set<std::string> flags;
        std::vector<std::string> positionals;
        std::vector<std::string> rest;
        bool help = false;
        bool version = false;

        [[nodiscard]] bool flag(std::string const& name) const { return flags.contains(name); }

        [[nodiscard]] std::optional<std::string> value(std::string const& name) const
        {
            if (auto it = values.find(name); it != values.end())
                return it->second;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string> positional(std::size_t index) const
        {
            if (index < positionals.size())
                return positionals[index];
            return std::nullopt;
        }
    };

    namespace detail
    {
        [[nodiscard]] inline OptionSpec flag_option(std::string name, std::string help)
        {
            return OptionSpec{std::move(name), false, {}, std::move(help)};
        }

        [[nodiscard]] inline OptionSpec value_option(std::string name, std::string value_name, std::string help)
        {
            return OptionSpec{std::move(name), true, std::move(value_name), std::move(help)};
        }

        [[nodiscard]] inline std::vector<CommandSpec> build_commands()
        {
            std::vector<CommandSpec> v;
            auto add = [&](std::string name, std::string usage, std::string summary, std::vector<OptionSpec> options, std::size_t lo, std::size_t hi,
                           bool rest = false, std::string details = {}) {
                v.push_back(CommandSpec{std::move(name), std::move(usage), std::move(summary), std::move(options), lo, hi, rest, std::move(details)});
            };
            auto const unlimited = std::numeric_limits<std::size_t>::max();
            add("list", "list [--path ID|all] [--track ID] [--status STATUS] [--difficulty LEVEL] [--kind KIND] [--json]",
                "List the chapters and their lessons",
                {value_option("path", "ID", "only this learning path, or all (default: the core curriculum)"), value_option("track", "ID", "only this chapter"),
                 value_option("status", "STATUS", "not-started, in-progress, completed or unfinished"),
                 value_option("difficulty", "LEVEL", "beginner, intermediate or advanced"), value_option("kind", "KIND", "exercise kind"),
                 flag_option("json", "print JSON")},
                0, 0);
            add("paths", "paths [--json]", "List the learning paths: the core curriculum, specializations and practice", {flag_option("json", "print JSON")}, 0,
                0);
            add("search", "search <words...> [--json]", "Search exercises by words", {flag_option("json", "print JSON")}, 1, unlimited);
            add("show", "show <id> [--json]", "Show an exercise: instructions, checks and your status", {flag_option("json", "print JSON")}, 1, 1);
            add("start", "start <id> [--dir PATH]", "Create (or resume) the workspace of an exercise",
                {value_option("dir", "PATH", "create the workspace here instead of the default location")}, 1, 1);
            add("next", "next [--json]", "Recommend the next exercise", {flag_option("json", "print JSON")}, 0, 0);
            add("edit", "edit [id] [--editor NAME] [--folder]", "Open the exercise in your editor",
                {value_option("editor", "NAME", "editor preset or command for this run"), flag_option("folder", "open the workspace folder")}, 0, 1);
            add("run", "run [id] [-- args...]", "Build and run your program", {}, 0, 1, true);
            add("check", "check [id] [--json] [--verbose] [--backend dpm|llvm|custom]", "Run the checks against your code",
                {flag_option("json", "print JSON"), flag_option("verbose", "show full output of every check"),
                 value_option("backend", "BACKEND", "dpm (default), llvm or custom")},
                0, 1);
            add("watch", "watch [id]", "Re-run the checks whenever a file changes (Ctrl+C ends)", {}, 0, 1);
            add("hint", "hint [id] [N] [--level N] [--stage STAGE]", "Reveal the next hint (or hint N)",
                {value_option("level", "N", "hint level 1..3"), value_option("stage", "STAGE", "a hint for this stage of a multi-stage exercise")}, 0, 2);
            add("solution", "solution [id] --show|--diff", "Show the reference solution or compare it with your files",
                {flag_option("show", "print the solution files"), flag_option("diff", "diff your files against the solution")}, 0, 1);
            add("reset", "reset <id> [--yes] [--no-backup]", "Restore the starter files of an exercise",
                {flag_option("yes", "do not ask for confirmation"), flag_option("no-backup", "skip the backup of your files")}, 1, 1);
            add("answer", "answer <id> [CHOICE...] | answer <id> --question Q ANSWER... [--json]",
                "Answer lesson questions: one choice per question, or one question of any kind with --question",
                {flag_option("json", "print JSON"), value_option("question", "Q", "the id of the question to answer")}, 1, unlimited);
            add("cards", "cards [--json]", "List the flashcards due for review and the questions you missed", {flag_option("json", "print JSON")}, 0, 0);
            add("card", "card <lesson>#<card> [again|hard|good|easy]", "Show a flashcard, or rate how well you remembered it", {}, 1, 2);
            add("progress", "progress [--json]", "Show your progress", {flag_option("json", "print JSON")}, 0, 0);
            add("review", "review [--json]", "List exercises worth revisiting", {flag_option("json", "print JSON")}, 0, 0);
            add("bookmark", "bookmark <id> [--remove]", "Bookmark an exercise", {flag_option("remove", "remove the bookmark")}, 1, 1);
            add("config", "config get KEY | set KEY VALUE | unset KEY | list [--json]", "Read and change settings", {flag_option("json", "print JSON")}, 1, 3);
            add("doctor", "doctor [--json] [--build]", "Check the toolchain, folders, editor and terminal",
                {flag_option("json", "print JSON"), flag_option("build", "also build a small test program")}, 0, 0);
            add("export", "export --output FILE [--yes]", "Write your progress to a file",
                {value_option("output", "FILE", "destination file"), flag_option("yes", "overwrite an existing file")}, 0, 0);
            add("import", "import --input FILE [--apply]", "Merge progress from an export (preview unless --apply)",
                {value_option("input", "FILE", "export file"), flag_option("apply", "apply the changes")}, 0, 0);
            add("dev", "dev validate [--backend dpm|llvm|custom|all] [--id ID]", "Validate a content directory (for exercise authors)",
                {value_option("backend", "BACKEND", "dpm (default), llvm, custom or all"), value_option("id", "ID", "only this exercise")}, 1, 1);
            add("help", "help [command]", "Show help", {}, 0, 1);
            return v;
        }

        [[nodiscard]] inline std::vector<OptionSpec> const& global_options()
        {
            static std::vector<OptionSpec> const g = {
                value_option("home", "DIR", "keep all learndc data under DIR"),
                value_option("content-dir", "DIR", "load exercises from DIR instead of the built-in set"),
                flag_option("no-color", "never use colors"),
                flag_option("ascii", "use plain ASCII output"),
                flag_option("json", "print JSON where the command supports it"),
                flag_option("help", "show help"),
                flag_option("version", "show the version"),
            };
            return g;
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<CommandSpec> const& commands()
    {
        static std::vector<CommandSpec> const c = detail::build_commands();
        return c;
    }

    [[nodiscard]] inline CommandSpec const* find_command(std::string_view name)
    {
        for (auto const& c : commands())
            if (c.name == name)
                return &c;
        return nullptr;
    }

    [[nodiscard]] inline std::expected<Parsed, std::string> parse(std::vector<std::string> const& args)
    {
        Parsed out;
        CommandSpec const* spec = nullptr;
        bool in_rest = false;
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            std::string const& a = args[i];
            if (in_rest)
            {
                out.rest.push_back(a);
                continue;
            }
            if (a == "--")
            {
                if (spec && spec->allows_rest)
                {
                    in_rest = true;
                    continue;
                }
                return std::unexpected(std::string("unexpected '--'"));
            }
            if (a == "-h")
            {
                out.help = true;
                continue;
            }
            if (a == "-V")
            {
                out.version = true;
                continue;
            }
            if (a.size() > 2 && a.starts_with("--"))
            {
                std::string name = a.substr(2);
                std::optional<std::string> inline_value;
                if (auto eq = name.find('='); eq != std::string::npos)
                {
                    inline_value = name.substr(eq + 1);
                    name.resize(eq);
                }
                OptionSpec const* opt = nullptr;
                bool global = false;
                if (spec)
                    for (auto const& o : spec->options)
                        if (o.name == name)
                            opt = &o;
                if (!opt)
                    for (auto const& o : detail::global_options())
                        if (o.name == name)
                        {
                            opt = &o;
                            global = true;
                        }
                if (!opt)
                    return std::unexpected(spec ? std::format("unknown option '--{}' for '{}'", text::sanitize(name), spec->name)
                                                : std::format("unknown option '--{}'", text::sanitize(name)));
                (void)global;
                if (opt->takes_value)
                {
                    if (!inline_value)
                    {
                        if (i + 1 >= args.size())
                            return std::unexpected(std::format("option '--{}' needs a value ({})", name, opt->value_name));
                        inline_value = args[++i];
                    }
                    out.values[name] = *inline_value;
                }
                else
                {
                    if (inline_value)
                        return std::unexpected(std::format("option '--{}' does not take a value", name));
                    if (name == "help")
                        out.help = true;
                    else if (name == "version")
                        out.version = true;
                    else
                        out.flags.insert(name);
                }
                continue;
            }
            if (a.size() > 1 && a[0] == '-')
                return std::unexpected(std::format("unknown option '{}'", text::sanitize(a)));
            if (!spec)
            {
                spec = find_command(a);
                if (!spec)
                {
                    std::string message = std::format("unknown command '{}'", text::sanitize(a));
                    for (auto const& c : commands())
                        if (c.name.starts_with(a) && !a.empty())
                        {
                            message += std::format(" (did you mean '{}'?)", c.name);
                            break;
                        }
                    return std::unexpected(std::move(message));
                }
                out.command = spec->name;
                continue;
            }
            out.positionals.push_back(a);
        }
        if (spec && !out.help && !out.version)
        {
            if (out.positionals.size() < spec->min_positionals)
                return std::unexpected(std::format("'{}' needs more arguments; usage: learndc {}", spec->name, spec->usage));
            if (out.positionals.size() > spec->max_positionals)
                return std::unexpected(std::format("'{}' got too many arguments; usage: learndc {}", spec->name, spec->usage));
        }
        return out;
    }

    [[nodiscard]] inline std::string command_help(CommandSpec const& c)
    {
        std::string out = std::format("learndc {}\n\n{}\n", c.usage, c.summary);
        if (!c.options.empty())
        {
            out += "\nOptions:\n";
            for (auto const& o : c.options)
            {
                std::string left = "--" + o.name;
                if (o.takes_value)
                    left += " " + o.value_name;
                out += std::format("  {:<24} {}\n", left, o.help);
            }
        }
        out += "\nGlobal options: --home DIR, --content-dir DIR, --no-color, --ascii, --help\n";
        return out;
    }

    [[nodiscard]] inline std::string help_text()
    {
        std::string out = "learndc - learn the DC language offline\n\nUsage: learndc [global options] <command> [arguments]\n"
                          "Run without a command in a terminal to open the interactive interface.\n\nCommands:\n";
        for (auto const& c : commands())
            out += std::format("  {:<10} {}\n", c.name, c.summary);
        out += "\nGlobal options:\n";
        for (auto const& o : detail::global_options())
        {
            std::string left = "--" + o.name;
            if (o.takes_value)
                left += " " + o.value_name;
            out += std::format("  {:<16} {}\n", left, o.help);
        }
        out += "\nExit codes: 0 success, 1 checks or operation failed, 2 usage error, 3 content/config/progress error,\n"
               "4 toolchain problem, 5 not found, 6 refused to avoid data loss, 130 interrupted.\n"
               "Use 'learndc <command> --help' for details.\n";
        return out;
    }

    namespace detail
    {
        struct Session
        {
            app::Context& ctx;
            Parsed const& args;
            Io& io;
            runner::TextStyle style;
            std::optional<progress::Progress> loaded;

            [[nodiscard]] std::ostream& out() const { return *io.out; }
            [[nodiscard]] std::ostream& err() const { return *io.err; }
            [[nodiscard]] bool json() const { return args.flag("json"); }
        };

        [[nodiscard]] inline std::string paint(Session const& s, std::string_view code, std::string_view text_in)
        {
            if (!s.style.color)
                return std::string(text_in);
            return std::format("\x1b[{}m{}\x1b[0m", code, text_in);
        }

        inline int report_failure(Session const& s, app::Failure const& f)
        {
            std::print(s.err(), "learndc: {}\n", f.message);
            s.err().flush();
            return f.code;
        }

        inline int usage_error(Session const& s, std::string_view message)
        {
            std::print(s.err(), "learndc: {}\n", message);
            if (auto const* spec = find_command(s.args.command))
                std::print(s.err(), "usage: learndc {}\n", spec->usage);
            return app::exit_usage;
        }

        inline void flush_warnings(Session const& s)
        {
            for (auto const& w : s.ctx.warnings)
                std::print(s.err(), "learndc: warning: {}\n", w);
            s.ctx.warnings.clear();
        }

        [[nodiscard]] inline std::optional<app::Failure> prepare(Session& s)
        {
            if (auto f = app::require_content(s.ctx))
                return f;
            auto p = app::load_progress(s.ctx);
            if (!p)
                return p.error();
            s.loaded = std::move(*p);
            flush_warnings(s);
            return std::nullopt;
        }

        inline void emit_json(Session const& s, json::Json const& j)
        {
            s.out() << j.dump(2) << "\n";
        }

        [[nodiscard]] inline json::Json strings_json(std::vector<std::string> const& items)
        {
            auto a = json::Json::make_array();
            for (auto const& i : items)
                a.push(json::Json(i));
            return a;
        }

        [[nodiscard]] inline std::string pad(std::string_view s, std::size_t width)
        {
            std::string out(s);
            auto const w = text::display_width(s);
            if (w < width)
                out.append(width - w, ' ');
            return out;
        }

        [[nodiscard]] inline std::string status_name(progress::Progress const& p, std::string_view id)
        {
            return std::string(progress::to_string(progress::status_of(p, id)));
        }

        [[nodiscard]] inline json::Json exercise_json(app::Context const& ctx, catalog::Exercise const& ex, progress::Progress const& p)
        {
            auto j = json::Json::make_object();
            j.set("id", json::Json(ex.id));
            j.set("track", json::Json(ex.track));
            if (auto const* path = ctx.catalog.path_of(ex))
                j.set("path", json::Json(path->id));
            j.set("name", json::Json(ex.name));
            j.set("title", json::Json(ex.title));
            j.set("summary", json::Json(ex.summary));
            j.set("kind", json::Json(catalog::to_string(ex.kind)));
            j.set("difficulty", json::Json(catalog::to_string(ex.difficulty)));
            j.set("estimated_minutes", json::Json(ex.estimated_minutes));
            j.set("revision", json::Json(ex.revision));
            j.set("concepts", strings_json(ex.concepts));
            j.set("tags", strings_json(ex.tags));
            j.set("prerequisites", strings_json(ex.prerequisites));
            j.set("prerequisites_met", json::Json(ctx.catalog.prerequisites_met(ex, app::completed_predicate(p))));
            j.set("status", json::Json(status_name(p, ex.id)));
            j.set("bookmarked", json::Json(progress::is_bookmarked(p, ex.id)));
            auto it = p.exercises.find(ex.id);
            j.set("needs_revisit", json::Json(it != p.exercises.end() && progress::needs_revisit(it->second, ex.revision)));
            j.set("chapter_number", json::Json(ex.chapter_number));
            j.set("lecture", json::Json(ex.is_lecture()));
            j.set("boss", json::Json(ex.is_boss()));
            j.set("optional", json::Json(ex.optional));
            if (ex.staged())
            {
                auto stages = json::Json::make_array();
                for (auto const& st : ex.stages)
                {
                    auto sj = json::Json::make_object();
                    sj.set("id", json::Json(st.id));
                    sj.set("title", json::Json(st.title));
                    sj.set("optional", json::Json(st.optional));
                    bool achieved = false;
                    if (it != p.exercises.end())
                        if (auto s_it = it->second.stages.find(st.id); s_it != it->second.stages.end())
                            achieved = s_it->second.achieved();
                    sj.set("achieved", json::Json(achieved));
                    stages.push(std::move(sj));
                }
                j.set("stages", std::move(stages));
            }
            return j;
        }

        [[nodiscard]] inline std::string lesson_type(catalog::Exercise const& e)
        {
            if (e.is_boss())
                return "BOSS";
            if (e.is_lecture())
                return "lecture";
            if (e.staged())
                return "quest";
            return e.kind == catalog::ExerciseKind::Challenge ? "challenge" : "practice";
        }

        inline void print_exercise_table(Session const& s, std::vector<catalog::Exercise const*> const& items, progress::Progress const& p,
                                         bool grouped = false)
        {
            std::size_t id_w = 2;
            for (auto const* e : items)
                id_w = std::max(id_w, text::display_width(e->id));
            std::print(s.out(), "{}  {}  {}  {}  {}  {}\n", pad("ID", id_w), pad("STATUS", 11), pad("TYPE", 9), pad("LEVEL", 12), pad("MIN", 3), "TITLE");
            std::string chapter;
            std::string path;
            bool const several_paths = std::ranges::any_of(items, [&](auto const* e) {
                auto const* a = s.ctx.catalog.path_of(*e);
                auto const* b = s.ctx.catalog.path_of(*items.front());
                return a != b;
            });
            for (auto const* e : items)
            {
                if (grouped && e->track != chapter)
                {
                    chapter = e->track;
                    if (auto const* t = s.ctx.catalog.find_track(e->track))
                    {
                        if (several_paths && t->path != path)
                        {
                            path = t->path;
                            if (auto const* pth = s.ctx.catalog.find_path(path))
                                std::print(s.out(), "{}\n", paint(s, "1;36", pth->title));
                        }
                        std::print(s.out(), "{}\n", paint(s, "1", std::format("Chapter {}: {}", t->number, t->title)));
                    }
                }
                auto const status = status_name(p, e->id);
                std::string_view const code = status == "completed" ? "32" : status == "in-progress" ? "33" : "2";
                auto shown = pad(status, 11);
                auto const type = lesson_type(*e);
                std::print(s.out(), "{}  {}  {}  {}  {:>3}  {}{}{}\n", pad(e->id, id_w), paint(s, code, shown),
                           type == "BOSS" ? paint(s, "1;31", pad(type, 9)) : pad(type, 9), pad(catalog::to_string(e->difficulty), 12), e->estimated_minutes,
                           e->title, e->optional ? " (bonus)" : "", progress::is_bookmarked(p, e->id) ? " *" : "");
            }
        }

        [[nodiscard]] inline std::function<bool(catalog::Exercise const&)> status_filter(std::string const& want, progress::Progress const& p)
        {
            return [want, &p](catalog::Exercise const& e) {
                auto const st = progress::status_of(p, e.id);
                if (want == "unfinished")
                    return st != progress::Status::Completed;
                return progress::to_string(st) == want;
            };
        }

        inline int cmd_list(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto const& p = *s.loaded;
            catalog::Filter filter;
            auto const& paths = s.ctx.catalog.paths();
            if (auto path = s.args.value("path"))
            {
                if (*path != "all")
                {
                    if (!s.ctx.catalog.find_path(*path))
                        return usage_error(s, std::format("unknown path '{}' (see learndc paths)", text::sanitize(*path)));
                    filter.path = *path;
                }
            }
            else if (paths.size() > 1 && !s.args.value("track"))
                for (auto const& pth : paths)
                    if (pth.kind == catalog::PathKind::Core)
                    {
                        filter.path = pth.id;
                        break;
                    }
            if (auto track = s.args.value("track"))
            {
                if (!s.ctx.catalog.find_track(*track))
                    return usage_error(s, std::format("unknown track '{}'", text::sanitize(*track)));
                filter.track = *track;
            }
            if (auto d = s.args.value("difficulty"))
            {
                auto parsed = catalog::parse_difficulty(*d);
                if (!parsed)
                    return usage_error(s, std::format("unknown difficulty '{}' (beginner, intermediate or advanced)", text::sanitize(*d)));
                filter.difficulty = *parsed;
            }
            if (auto k = s.args.value("kind"))
            {
                auto parsed = catalog::parse_exercise_kind(*k);
                if (!parsed)
                    return usage_error(s, std::format("unknown kind '{}'", text::sanitize(*k)));
                filter.kind = *parsed;
            }
            std::function<bool(catalog::Exercise const&)> extra;
            if (auto st = s.args.value("status"))
            {
                if (*st != "not-started" && *st != "in-progress" && *st != "completed" && *st != "unfinished")
                    return usage_error(s, "unknown status (use not-started, in-progress, completed or unfinished)");
                extra = status_filter(*st, p);
            }
            auto items = s.ctx.catalog.filter(filter, extra);
            if (s.json())
            {
                auto root = json::Json::make_object();
                auto arr = json::Json::make_array();
                for (auto const* e : items)
                    arr.push(exercise_json(s.ctx, *e, p));
                root.set("exercises", std::move(arr));
                emit_json(s, root);
                return app::exit_ok;
            }
            if (items.empty())
            {
                s.out() << "No exercises match.\n";
                return app::exit_ok;
            }
            print_exercise_table(s, items, p, true);
            return app::exit_ok;
        }

        inline int cmd_paths(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto const summary = app::summarize(s.ctx, *s.loaded);
            if (s.json())
            {
                auto root = json::Json::make_object();
                auto arr = json::Json::make_array();
                for (auto const& ps : summary.paths)
                {
                    auto j = json::Json::make_object();
                    j.set("id", json::Json(ps.id));
                    j.set("title", json::Json(ps.title));
                    j.set("summary", json::Json(ps.summary));
                    j.set("kind", json::Json(std::string(catalog::to_string(ps.kind))));
                    j.set("chapters", json::Json(ps.chapters));
                    j.set("lessons", json::Json(ps.total));
                    j.set("completed", json::Json(ps.completed));
                    j.set("bonus", json::Json(ps.bonus_total));
                    j.set("bonus_completed", json::Json(ps.bonus_completed));
                    j.set("bosses", json::Json(ps.bosses_total));
                    j.set("bosses_defeated", json::Json(ps.bosses_defeated));
                    auto tracks = json::Json::make_array();
                    for (auto const& t : summary.tracks)
                        if (t.path == ps.id)
                            tracks.push(json::Json(t.id));
                    j.set("tracks", std::move(tracks));
                    arr.push(std::move(j));
                }
                root.set("paths", std::move(arr));
                emit_json(s, root);
                return app::exit_ok;
            }
            std::size_t id_w = 2;
            for (auto const& ps : summary.paths)
                id_w = std::max(id_w, text::display_width(ps.id));
            for (auto const& ps : summary.paths)
            {
                std::string counts = std::format("{} of {} lessons", ps.completed, ps.total);
                if (ps.bonus_total > 0)
                    counts += std::format(", {} of {} bonus", ps.bonus_completed, ps.bonus_total);
                if (ps.bosses_total > 0)
                    counts += std::format(", {} of {} bosses", ps.bosses_defeated, ps.bosses_total);
                std::print(s.out(), "{}  {}  ({} chapter{}; {})\n", pad(ps.id, id_w), paint(s, "1", ps.title), ps.chapters, ps.chapters == 1 ? "" : "s",
                           counts);
                if (!ps.summary.empty())
                    std::print(s.out(), "{}  {}\n", pad("", id_w), ps.summary);
            }
            s.out() << "\nList the lessons of a path with: learndc list --path ID\n";
            return app::exit_ok;
        }

        inline int cmd_search(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto const& p = *s.loaded;
            auto const query = text::join(s.args.positionals, " ");
            auto items = s.ctx.catalog.search(query);
            if (s.json())
            {
                auto root = json::Json::make_object();
                root.set("query", json::Json(query));
                auto arr = json::Json::make_array();
                for (auto const* e : items)
                    arr.push(exercise_json(s.ctx, *e, p));
                root.set("exercises", std::move(arr));
                emit_json(s, root);
                return app::exit_ok;
            }
            if (items.empty())
            {
                std::print(s.out(), "No exercises match '{}'.\n", text::sanitize(query));
                return app::exit_ok;
            }
            print_exercise_table(s, items, p);
            return app::exit_ok;
        }

        [[nodiscard]] inline std::string check_kind_name(catalog::CheckKind kind)
        {
            return std::string(catalog::to_string(kind));
        }

        [[nodiscard]] inline std::string stage_mark(Session const& s, app::StageView const& v)
        {
            switch (v.state)
            {
                case app::StageState::Verified:
                    return paint(s, "32", "[x]");
                case app::StageState::Regressed:
                    return paint(s, "31", "[!]");
                case app::StageState::Unverified:
                case app::StageState::Outdated:
                    return paint(s, "33", "[~]");
                case app::StageState::Current:
                    return paint(s, "1;36", "[>]");
                case app::StageState::Todo:
                    return paint(s, "2", "[ ]");
            }
            return "[ ]";
        }

        [[nodiscard]] inline std::string question_text(catalog::Question const& q)
        {
            std::string out = std::format("[{}] {}\n", activity::type_word(q.type), text::trim(markdown::render_plain(q.prompt, 76, false)));
            auto const lines = q.code_lines();
            if (q.type == catalog::QuestionType::Order)
            {
                auto const shown = activity::shuffled_order(q);
                for (std::size_t i = 0; i < shown.size(); ++i)
                    out += std::format("   {:>2} | {}\n", i + 1, lines[static_cast<std::size_t>(shown[i])]);
            }
            else if (!lines.empty())
                for (std::size_t i = 0; i < lines.size(); ++i)
                    out += std::format("   {:>2} | {}\n", i + 1, lines[i]);
            if (q.type == catalog::QuestionType::Error)
                out += std::format("   compiler: {}\n", q.diagnostic);
            int c = 0;
            for (auto const& choice : q.choices)
                out += std::format("   {}) {}\n", ++c, choice);
            return out;
        }

        [[nodiscard]] inline std::string answer_form(catalog::Question const& q)
        {
            switch (q.type)
            {
                case catalog::QuestionType::Choice:
                case catalog::QuestionType::Error:
                    return "a choice number";
                case catalog::QuestionType::Multi:
                    return "every choice number that holds, such as 1,3";
                case catalog::QuestionType::Predict:
                    return "the exact output, lines separated by \\n";
                case catalog::QuestionType::Trace:
                case catalog::QuestionType::Short:
                    return "a short answer";
                case catalog::QuestionType::Bug:
                    return "the number of the buggy line";
                case catalog::QuestionType::Order:
                    return "the shown line numbers in the right order, such as 3 1 2";
            }
            return "an answer";
        }

        inline int show_lecture(Session& s, catalog::Exercise const& ex, progress::Progress const& p)
        {
            if (s.json())
            {
                auto j = exercise_json(s.ctx, ex, p);
                j.set("objectives", strings_json(ex.objectives));
                j.set("readme", json::Json(ex.readme));
                auto qs = json::Json::make_array();
                for (auto const& q : ex.questions)
                {
                    auto qj = json::Json::make_object();
                    qj.set("prompt", json::Json(q.prompt));
                    qj.set("choices", strings_json(q.choices));
                    qs.push(std::move(qj));
                }
                j.set("questions", std::move(qs));
                emit_json(s, j);
                return app::exit_ok;
            }
            auto& o = s.out();
            std::print(o, "{}\n", paint(s, "1", ex.title));
            std::print(o, "{}  |  lecture  |  {}  |  about {} min  |  {}\n\n", ex.id, catalog::to_string(ex.difficulty), ex.estimated_minutes,
                       status_name(p, ex.id));
            std::print(o, "{}", markdown::render_plain(ex.readme, 80, true));
            if (ex.questions.empty())
            {
                std::print(o, "\nMark it as read with: learndc answer {}\n", ex.id);
                return app::exit_ok;
            }
            std::print(o, "\n{}\n", paint(s, "1", ex.drill ? "Practice" : "Check your understanding"));
            int n = 0;
            bool all_choice = true;
            for (auto const& q : ex.questions)
            {
                auto const qp = progress::question_of(p, ex.id, q.id);
                std::print(o, "\n{}. {}{}", ++n, qp.last == "right" ? "(done) " : "", question_text(q));
                if (q.type != catalog::QuestionType::Choice)
                    all_choice = false;
            }
            if (all_choice)
            {
                std::vector<std::string> sample(ex.questions.size(), "N");
                std::print(o, "\nAnswer with: learndc answer {} {}\n", ex.id, text::join(sample, " "));
            }
            else
            {
                std::print(o, "\nAnswer one question at a time:\n");
                for (auto const& q : ex.questions)
                    std::print(o, "  learndc answer {} --question {} <{}>\n", ex.id, q.id, answer_form(q));
            }
            if (!ex.cards.empty())
                std::print(o, "\nThis lesson adds {} flashcard{} to your review once it is complete ('learndc cards').\n", ex.cards.size(),
                           ex.cards.size() == 1 ? "" : "s");
            return app::exit_ok;
        }

        inline int answer_one(Session& s, catalog::Exercise const& ex, std::string const& qid)
        {
            auto const* q = ex.find_question(qid);
            if (q == nullptr)
                return report_failure(s, app::Failure{app::exit_not_found, std::format("'{}' has no question '{}'", ex.id, text::sanitize(qid))});
            std::vector<std::string> words(s.args.positionals.begin() + 1, s.args.positionals.end());
            auto parsed = activity::parse_answer(*q, text::join(words, " "));
            if (!parsed)
                return usage_error(s, std::format("question '{}' needs {}: {}", q->id, answer_form(*q), parsed.error()));
            auto out = app::answer_question(s.ctx, ex, q->id, *parsed);
            if (!out)
                return report_failure(s, out.error());
            flush_warnings(s);
            auto const& f = out->feedback;
            if (s.json())
            {
                auto j = json::Json::make_object();
                j.set("lesson", json::Json(ex.id));
                j.set("question", json::Json(q->id));
                j.set("correct", json::Json(f.correct));
                j.set("attempt", json::Json(out->attempt));
                j.set("notes", strings_json(f.notes));
                j.set("explanation", json::Json(f.explanation));
                j.set("completed_now", json::Json(out->record.completed_now));
                j.set("stages_achieved", strings_json(out->stages_achieved));
                emit_json(s, j);
                return f.correct ? app::exit_ok : app::exit_failed;
            }
            auto& o = s.out();
            std::print(o, "{} {}\n", paint(s, f.correct ? "1;32" : "1;31", f.verdict), activity::type_word(q->type));
            for (auto const& note : f.notes)
                std::print(o, "  {}\n", text::trim(markdown::render_plain(note, 76, false)));
            if (!f.explanation.empty())
                std::print(o, "  {}\n", text::trim(markdown::render_plain(f.explanation, 76, false)));
            if (!f.correct && f.revealed)
                std::print(o, "  {}\n", activity::reveal_answer(*q));
            if (!f.correct && !f.revealed)
                std::print(o, "  Try again; the explanation appears after the next attempt.\n");
            for (auto const& title : out->stages_achieved)
                std::print(o, "{} {}\n", paint(s, "1;32", "Stage cleared:"), title);
            if (out->record.boss_defeated)
                std::print(o, "{}\n", paint(s, "1;31", std::format("BOSS DEFEATED: {}", ex.title)));
            else if (out->record.completed_now)
                std::print(o, "{}\n", paint(s, "1;32", ex.is_lecture() ? "Lesson completed." : "Exercise completed."));
            if (out->record.chapter_completed)
                std::print(o, "{}\n", paint(s, "1;32", std::format("Chapter {} complete: {}", out->record.chapter_number, out->record.chapter_title)));
            return f.correct ? app::exit_ok : app::exit_failed;
        }

        inline int cmd_cards(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            int const today = app::today_day();
            auto const queue = app::review_queue(s.ctx, *s.loaded, today);
            if (s.json())
            {
                auto j = json::Json::make_object();
                auto due = json::Json::make_array();
                for (auto const& c : queue.due)
                {
                    auto cj = json::Json::make_object();
                    cj.set("key", json::Json(c.key));
                    cj.set("front", json::Json(c.card->front));
                    cj.set("box", json::Json(c.state.box));
                    cj.set("new", json::Json(c.state.reviews == 0));
                    due.push(std::move(cj));
                }
                j.set("due", std::move(due));
                j.set("unlocked", json::Json(queue.cards_unlocked));
                j.set("locked", json::Json(queue.cards_locked));
                auto missed = json::Json::make_array();
                for (auto const& m : queue.missed)
                    missed.push(json::Json(m.key));
                j.set("missed", std::move(missed));
                emit_json(s, j);
                return app::exit_ok;
            }
            auto& o = s.out();
            std::print(o, "{} card{} due, {} unlocked, {} still locked behind lessons you have not completed\n", queue.due.size(),
                       queue.due.size() == 1 ? "" : "s", queue.cards_unlocked, queue.cards_locked);
            for (auto const& c : queue.due)
                std::print(o, "  {}  {}{}\n", c.key, text::trim(markdown::render_plain(c.card->front, 70, false)), c.state.reviews == 0 ? "  (new)" : "");
            if (!queue.due.empty())
                std::print(o, "Review one with: learndc card {}\n", queue.due.front().key);
            else if (queue.next_due_day)
                std::print(o, "The next card is due in {} day(s).\n", *queue.next_due_day - today);
            if (!queue.missed.empty())
            {
                std::print(o, "\nQuestions to try again\n");
                for (auto const& m : queue.missed)
                    std::print(o, "  {}  {}\n", m.key, activity::type_word(m.question->type));
            }
            return app::exit_ok;
        }

        inline int cmd_card(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto const& key = s.args.positionals[0];
            auto const hash = key.find('#');
            if (hash == std::string::npos)
                return usage_error(s, "name the card as <lesson>#<card>, as 'learndc cards' prints it");
            auto found = app::find_exercise(s.ctx, key.substr(0, hash));
            if (!found)
                return report_failure(s, found.error());
            auto const& ex = **found;
            auto const card_id = key.substr(hash + 1);
            auto it = std::ranges::find(ex.cards, card_id, &catalog::Card::id);
            if (it == ex.cards.end())
                return report_failure(s, app::Failure{app::exit_not_found, std::format("'{}' has no card '{}'", ex.id, text::sanitize(card_id))});
            if (s.args.positionals.size() == 1)
            {
                std::print(s.out(), "{}\n\n{}\nRate it: learndc card {} again|hard|good|easy\n", text::trim(markdown::render_plain(it->front, 76, false)),
                           markdown::render_plain(it->back, 76, false), key);
                return app::exit_ok;
            }
            auto rating = activity::parse_rating(s.args.positionals[1]);
            if (!rating)
                return usage_error(s, "rate a card with again, hard, good or easy");
            auto r = app::rate_card(s.ctx, ex, card_id, *rating);
            if (!r)
                return report_failure(s, r.error());
            int const days = r->due_day - app::today_day();
            std::print(s.out(), "Saved. {} is in box {} of 5 and comes back {}.\n", key, r->box,
                       days <= 0 ? std::string("today") : std::format("in {} day{}", days, days == 1 ? "" : "s"));
            return app::exit_ok;
        }

        inline int cmd_answer(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto found = app::find_exercise(s.ctx, s.args.positionals[0]);
            if (!found)
                return report_failure(s, found.error());
            auto const& ex = **found;
            if (auto qid = s.args.value("question"))
                return answer_one(s, ex, *qid);
            std::vector<int> answers;
            for (std::size_t i = 1; i < s.args.positionals.size(); ++i)
            {
                auto const& word = s.args.positionals[i];
                int v = 0;
                auto r = std::from_chars(word.data(), word.data() + word.size(), v);
                if (r.ec != std::errc() || r.ptr != word.data() + word.size())
                    return usage_error(s, std::format("'{}' is not a choice number", text::sanitize(word)));
                answers.push_back(v);
            }
            auto out = app::answer_lecture(s.ctx, ex, answers);
            if (!out)
                return report_failure(s, out.error());
            flush_warnings(s);
            if (s.json())
            {
                auto j = json::Json::make_object();
                j.set("lecture", json::Json(ex.id));
                j.set("correct", json::Json(out->correct));
                j.set("total", json::Json(out->total));
                auto right = json::Json::make_array();
                for (bool b : out->right)
                    right.push(json::Json(b));
                j.set("right", std::move(right));
                j.set("completed_now", json::Json(out->record.completed_now));
                j.set("next", out->record.next_id.empty() ? json::Json() : json::Json(out->record.next_id));
                emit_json(s, j);
                return out->correct == out->total ? app::exit_ok : app::exit_failed;
            }
            auto& o = s.out();
            for (std::size_t i = 0; i < out->right.size(); ++i)
            {
                auto const& q = ex.questions[i];
                if (out->right[i])
                    std::print(o, "{} {}. {}\n", paint(s, "32", s.style.ascii ? "[ok]" : "\xE2\x9C\x93"), i + 1,
                               q.choices[static_cast<std::size_t>(q.answer - 1)]);
                else
                    std::print(o, "{} {}. not quite. {}\n", paint(s, "31", s.style.ascii ? "[x]" : "\xE2\x9C\x97"), i + 1,
                               text::trim(markdown::render_plain(q.explanation, 76, false)));
            }
            if (out->total == 0)
                std::print(o, "{} marked as read.\n", ex.title);
            else
                std::print(o, "{} of {} right.\n", out->correct, out->total);
            if (out->record.completed_now)
                std::print(o, "{}\n", paint(s, "1;32", "Lecture completed."));
            if (out->record.chapter_completed)
                std::print(o, "{}\n", paint(s, "1;32", std::format("Chapter {} complete: {}", out->record.chapter_number, out->record.chapter_title)));
            if (out->correct == out->total && !out->record.next_id.empty())
                std::print(o, "Next recommended: {} - {}\n", out->record.next_id, out->record.next_title);
            return out->correct == out->total ? app::exit_ok : app::exit_failed;
        }

        inline int cmd_show(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto const& p = *s.loaded;
            auto found = app::find_exercise(s.ctx, s.args.positionals[0]);
            if (!found)
                return report_failure(s, found.error());
            auto const& ex = **found;
            if (ex.is_lecture())
                return show_lecture(s, ex, p);
            auto ws = app::find_workspace(s.ctx, p, ex);
            std::string workspace_path;
            if (ws && *ws)
                workspace_path = platform::path_to_utf8((*ws)->root);
            if (s.json())
            {
                auto j = exercise_json(s.ctx, ex, p);
                j.set("chapter", json::Json(ex.chapter));
                j.set("objectives", strings_json(ex.objectives));
                j.set("entry_file", json::Json(ex.entry_file));
                j.set("requires", strings_json(ex.required_tools));
                j.set("platforms", strings_json(ex.platforms));
                j.set("backends", strings_json(ex.backends));
                j.set("readme", json::Json(ex.readme));
                auto checks = json::Json::make_array();
                for (auto const& c : ex.checks)
                {
                    auto cj = json::Json::make_object();
                    cj.set("name", json::Json(c.name));
                    cj.set("kind", json::Json(check_kind_name(c.kind)));
                    checks.push(std::move(cj));
                }
                j.set("checks", std::move(checks));
                if (workspace_path.empty())
                    j.set("workspace", json::Json());
                else
                    j.set("workspace", json::Json(workspace_path));
                auto it = p.exercises.find(ex.id);
                int const hint_level = it == p.exercises.end() ? 0 : it->second.hint_level;
                j.set("hint_level", json::Json(hint_level));
                emit_json(s, j);
                return app::exit_ok;
            }
            auto& o = s.out();
            if (ex.is_boss())
                std::print(o, "{}  difficulty {} of 5\n", paint(s, "1;31", "BOSS FIGHT"), ex.boss->rating);
            std::print(o, "{}\n", paint(s, "1", ex.title));
            std::print(o, "{}  |  {}  |  {}  |  about {} min  |  {}\n", ex.id, lesson_type(ex), catalog::to_string(ex.difficulty), ex.estimated_minutes,
                       status_name(p, ex.id));
            if (!ex.prerequisites.empty())
            {
                std::vector<std::string> pre;
                for (auto const& id : ex.prerequisites)
                    pre.push_back(std::format("{}{}", id, progress::is_completed(p, id) ? " (done)" : ""));
                std::print(o, "Prerequisites: {}\n", text::join(pre, ", "));
            }
            if (!workspace_path.empty())
                std::print(o, "Workspace: {}\n", workspace_path);
            if (ex.is_boss())
                std::print(o, "\n{}", markdown::render_plain(ex.boss->story, 80, false));
            std::print(o, "\n{}\n", ex.summary);
            std::print(o, "\nObjectives\n");
            for (auto const& obj : ex.objectives)
                std::print(o, "  - {}\n", obj);
            if (ex.staged())
            {
                std::string fingerprint;
                if (ws && *ws)
                    fingerprint = runner::workspace_fingerprint((*ws)->root);
                std::print(o, "\nStages\n");
                for (auto const& v : app::stage_views(ex, p, fingerprint))
                    std::print(o, "  {} {}. {}{}  ({})\n", stage_mark(s, v), v.number, v.stage->title, v.stage->optional ? " (bonus)" : "",
                               app::to_string(v.state));
            }
            else
            {
                std::print(o, "\nChecks\n");
                for (auto const& c : ex.checks)
                    std::print(o, "  - {} ({})\n", c.name, check_kind_name(c.kind));
            }
            if (!ex.readme.empty())
                std::print(o, "\n{}", markdown::render_plain(markdown::drop_section(ex.readme, "Acceptance criteria"), 80, true));
            if (workspace_path.empty())
                std::print(o, "\nStart with: learndc start {}\n", ex.id);
            return app::exit_ok;
        }

        inline int cmd_start(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto found = app::find_exercise(s.ctx, s.args.positionals[0]);
            if (!found)
                return report_failure(s, found.error());
            auto const& ex = **found;
            std::optional<std::filesystem::path> dir;
            if (auto d = s.args.value("dir"))
                dir = platform::path_from_utf8(*d);
            auto started = app::start_exercise(s.ctx, ex, dir);
            if (!started)
                return report_failure(s, started.error());
            flush_warnings(s);
            auto const path = platform::path_to_utf8(started->root);
            auto& o = s.out();
            if (started->result.created)
                std::print(o, "Created the workspace for {}: {}\n", ex.id, path);
            else
                std::print(o, "Resumed the workspace for {}: {}\n", ex.id, path);
            for (auto const& r : started->result.restored)
                std::print(o, "  restored missing file {}\n", r);
            if (started->result.outdated)
                std::print(s.err(), "learndc: note: this workspace was created for an older revision of the exercise; 'learndc reset {}' updates it (your files are backed up)\n",
                           ex.id);
            if (!started->missing_prerequisites.empty())
                std::print(s.err(), "learndc: note: you have not completed the prerequisites yet: {}\n", text::join(started->missing_prerequisites, ", "));
            if (!started->platform_supported)
                std::print(s.err(), "learndc: note: this exercise is not validated on this platform\n");
            std::print(o, "Next: learndc edit {0}   then   learndc check {0}\n", ex.id);
            return app::exit_ok;
        }

        inline int cmd_next(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto const& p = *s.loaded;
            std::string_view after;
            if (!p.active_exercise.empty())
                after = p.active_exercise;
            auto const* next = s.ctx.catalog.next_recommended(app::completed_predicate(p), after);
            if (s.json())
            {
                auto root = json::Json::make_object();
                if (next)
                    root.set("next", exercise_json(s.ctx, *next, p));
                else
                    root.set("next", json::Json());
                root.set("all_done", json::Json(next == nullptr));
                emit_json(s, root);
                return app::exit_ok;
            }
            if (!next)
            {
                s.out() << "Nothing left to recommend: every available exercise is completed or waiting for prerequisites.\n";
                return app::exit_ok;
            }
            std::print(s.out(), "Next: {} - {}\n  learndc start {}\n", next->id, next->title, next->id);
            return app::exit_ok;
        }

        inline int cmd_edit(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto target = app::resolve_target(s.ctx, *s.loaded, s.args.positional(0), s.io.cwd, true);
            if (!target)
                return report_failure(s, target.error());
            auto const& ex = *target->exercise;
            auto const& ws = *target->workspace;
            auto choice = editor::resolve(s.ctx.settings, s.io.env, s.args.value("editor"), s.ctx.os);
            for (auto const& w : choice.warnings)
                std::print(s.err(), "learndc: warning: {}\n", w);
            editor::Placeholders ph;
            ph.workspace = platform::path_to_utf8(ws.root);
            ph.file = platform::path_to_utf8(ws.root / platform::path_from_utf8(ex.entry_file));
            if (!choice.manual() && choice.kind == editor::Kind::Terminal && choice.source == editor::Source::Detected && !(s.io.stdin_tty && s.io.stdout_tty))
            {
                std::print(s.err(), "learndc: {} runs in the terminal and needs an interactive session; open {} in your editor, or choose one with --editor or 'learndc config set editor.command'\n",
                           choice.label, ph.file);
                return app::exit_failed;
            }
            editor::LaunchOptions lo;
            lo.folder = s.args.flag("folder");
            lo.cancel = s.io.cancel;
            s.out().flush();
            auto result = editor::launch(choice, ph, lo);
            if (result.cancelled)
                return app::exit_interrupted;
            if (result.mode == editor::Mode::Manual)
            {
                std::print(s.out(), "{}\n", result.message);
                return app::exit_ok;
            }
            if (!result.ok)
            {
                std::print(s.err(), "learndc: {}\n", result.message);
                return app::exit_failed;
            }
            std::print(s.out(), "{}\n", result.message);
            return app::exit_ok;
        }

        inline int cmd_run(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto target = app::resolve_target(s.ctx, *s.loaded, s.args.positional(0), s.io.cwd, true);
            if (!target)
                return report_failure(s, target.error());
            auto const& ex = *target->exercise;
            auto const& ws = *target->workspace;
            auto opt = app::runner_options(s.ctx, runner::Backend::Dpm, s.io.cancel);
            s.out().flush();
            auto outcome = runner::build_and_run_interactive(ex, ws.root, opt, s.args.rest);
            if (outcome.cancelled)
                return app::exit_interrupted;
            if (!outcome.diagnostics.empty() && !outcome.ok)
                std::print(s.err(), "{}\n", outcome.diagnostics);
            if (outcome.ok)
                return app::exit_ok;
            std::print(s.err(), "learndc: {}\n", outcome.message);
            if (outcome.stage == runner::Stage::Toolchain)
                return app::exit_toolchain;
            return app::exit_failed;
        }

        inline int finish_check(Session& s, catalog::Exercise const& ex, runner::CheckReport const& report, bool as_json)
        {
            auto rec = app::record_report(s.ctx, ex, report);
            int code = rec && rec->all_passed ? app::exit_ok : app::exit_code_for(report);
            if (code == app::exit_ok && ex.staged() && !(rec && rec->all_passed))
                code = app::exit_failed;
            if (!rec)
            {
                std::print(s.err(), "learndc: warning: could not save your progress: {}\n", rec.error().message);
                rec = app::CheckRecord{};
            }
            flush_warnings(s);
            if (as_json)
            {
                auto j = runner::to_json(report);
                j.set("completed_now", json::Json(rec->completed_now));
                j.set("boss_defeated", json::Json(rec->boss_defeated));
                j.set("chapter_completed", json::Json(rec->chapter_completed));
                if (rec->next_id.empty())
                    j.set("next", json::Json());
                else
                    j.set("next", json::Json(rec->next_id));
                emit_json(s, j);
                return code;
            }
            s.out() << runner::render_text(report, s.style);
            for (auto const& title : rec->stages_achieved)
                std::print(s.out(), "{} {}\n", paint(s, "1;32", "Stage cleared:"), title);
            if (rec->all_passed)
            {
                if (rec->boss_defeated)
                    std::print(s.out(), "\n{}\n", paint(s, "1;31", std::format("{} BOSS DEFEATED: {}", s.style.ascii ? "[X]" : "\xE2\x98\xA0", ex.title)));
                else if (rec->completed_now)
                    std::print(s.out(), "\n{}\n", paint(s, "1;32", "Exercise completed."));
                if (rec->chapter_completed)
                    std::print(s.out(), "{}\n", paint(s, "1;32", std::format("Chapter {} complete: {}", rec->chapter_number, rec->chapter_title)));
                if (!rec->next_id.empty())
                    std::print(s.out(), "Next recommended: {} - {}\n  learndc start {}\n", rec->next_id, rec->next_title, rec->next_id);
                else if (rec->completed_now)
                    std::print(s.out(), "There is nothing else to recommend right now.\n");
            }
            return code;
        }

        inline int cmd_check(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto backend = runner::Backend::Dpm;
            if (auto b = s.args.value("backend"))
            {
                auto parsed = runner::parse_backend(*b);
                if (!parsed)
                    return usage_error(s, std::format("unknown backend '{}' (dpm, llvm or custom)", text::sanitize(*b)));
                backend = *parsed;
            }
            auto target = app::resolve_target(s.ctx, *s.loaded, s.args.positional(0), s.io.cwd, true);
            if (!target)
                return report_failure(s, target.error());
            auto const& ex = *target->exercise;
            auto const& ws = *target->workspace;
            if (!s.json())
                std::print(s.out(), "Checking {} in {} ({})\n", ex.id, platform::path_to_utf8(ws.root), app::to_string(target->how));
            if (ws.meta.revision < ex.revision)
                std::print(s.err(), "learndc: note: your workspace is for revision {} of this exercise, the current one is {}\n", ws.meta.revision, ex.revision);
            std::function<void(std::string_view)> on_stage;
            if (s.io.stderr_tty && !s.json())
                on_stage = [&s](std::string_view stage) { std::print(s.err(), "  {}...\n", stage); };
            auto opt = app::runner_options(s.ctx, backend, s.io.cancel, on_stage, !app::any_attempts(*s.loaded));
            s.out().flush();
            auto report = runner::run_checks(ex, ws.root, opt);
            auto style = s.style;
            style.verbose = s.args.flag("verbose");
            s.style = style;
            return finish_check(s, ex, report, s.json());
        }

        inline int cmd_watch(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto target = app::resolve_target(s.ctx, *s.loaded, s.args.positional(0), s.io.cwd, true);
            if (!target)
                return report_failure(s, target.error());
            auto const& ex = *target->exercise;
            auto const root = target->workspace->root;
            std::print(s.out(), "Watching {} in {} (Ctrl+C to stop)\n", ex.id, platform::path_to_utf8(root));
            watch::Options wo;
            wo.debounce = std::chrono::milliseconds(s.ctx.settings.checks_watch_debounce_ms);
            wo.cancel = s.io.cancel;
            watch::Watcher watcher(root, wo);
            auto cancelled = [&] { return s.io.cancel && s.io.cancel->load(); };
            int last = app::exit_ok;
            bool first = true;
            while (!cancelled())
            {
                if (!first)
                {
                    auto changes = watcher.wait();
                    if (!changes)
                        break;
                    std::print(s.out(), "\n{} file(s) changed; checking again\n", changes->count());
                }
                first = false;
                auto opt = app::runner_options(s.ctx, runner::Backend::Dpm, s.io.cancel);
                s.out().flush();
                auto report = runner::run_checks(ex, root, opt);
                last = finish_check(s, ex, report, false);
                s.out().flush();
                if (last == app::exit_toolchain)
                    return last;
            }
            std::print(s.out(), "\nWatch stopped.\n");
            (void)last;
            return app::exit_interrupted;
        }

        inline int cmd_hint(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            std::optional<int> level;
            std::optional<std::string> id;
            for (auto const& word : s.args.positionals)
            {
                if (!word.empty() && word.size() <= 3 && std::ranges::all_of(word, [](char c) { return c >= '0' && c <= '9'; }))
                {
                    if (level)
                        return usage_error(s, "name the hint level only once");
                    level = std::stoi(word);
                }
                else if (id)
                    return usage_error(s, "name only one exercise");
                else
                    id = word;
            }
            if (auto l = s.args.value("level"))
            {
                int v = 0;
                auto r = std::from_chars(l->data(), l->data() + l->size(), v);
                if (r.ec != std::errc() || r.ptr != l->data() + l->size())
                    return usage_error(s, "--level must be 1, 2 or 3");
                if (level)
                    return usage_error(s, "name the hint level only once");
                level = v;
            }
            auto target = app::resolve_target(s.ctx, *s.loaded, id, s.io.cwd, false);
            if (!target)
                return report_failure(s, target.error());
            if (target->exercise->is_lecture())
                return report_failure(s, app::Failure{app::exit_usage, std::format("'{}' is a lecture and has no hints", target->exercise->id)});
            if (auto stage = s.args.value("stage"))
            {
                auto sh = app::reveal_stage_hint(s.ctx, *target->exercise, *stage, level);
                if (!sh)
                    return report_failure(s, sh.error());
                std::print(s.out(), "Hint {} of {} for {}\n\n{}", sh->level, sh->count, sh->stage_title, markdown::render_plain(sh->text));
                return app::exit_ok;
            }
            auto hint = app::reveal_hint(s.ctx, *target->exercise, level);
            if (!hint)
                return report_failure(s, hint.error());
            std::print(s.out(), "Hint {} of 3 for {}\n\n{}", hint->level, target->exercise->id, markdown::render_plain(hint->text));
            if (hint->all_shown)
                s.out() << "\nAll hints are shown. 'learndc solution --diff' compares your files with the reference solution.\n";
            return app::exit_ok;
        }

        inline int cmd_solution(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            bool const show = s.args.flag("show");
            bool const diff = s.args.flag("diff");
            if (show == diff)
            {
                s.err() << "learndc: the solution is hidden by default. Looking at it is recorded in your progress.\n"
                           "Use 'learndc solution --show' to print it or 'learndc solution --diff' to compare it with your files.\n";
                return app::exit_usage;
            }
            auto target = app::resolve_target(s.ctx, *s.loaded, s.args.positional(0), s.io.cwd, false);
            if (!target)
                return report_failure(s, target.error());
            auto text_out = app::reveal_solution(s.ctx, *target->exercise, target->workspace, diff);
            if (!text_out)
                return report_failure(s, text_out.error());
            s.out() << *text_out;
            return app::exit_ok;
        }

        [[nodiscard]] inline bool confirm(Session const& s, std::string_view prompt)
        {
            std::print(s.out(), "{} [y/N] ", prompt);
            s.out().flush();
            std::string line;
            if (!std::getline(*s.io.in, line))
                return false;
            auto const answer = text::lower(text::trim(line));
            return answer == "y" || answer == "yes";
        }

        inline int cmd_reset(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto target = app::resolve_target(s.ctx, *s.loaded, s.args.positional(0), s.io.cwd, true);
            if (!target)
                return report_failure(s, target.error());
            auto const& ex = *target->exercise;
            auto& ws = *target->workspace;
            bool const backup = !s.args.flag("no-backup");
            auto const changed = workspace::modified(ws, ex);
            if (!s.args.flag("yes"))
            {
                if (!(s.io.stdin_tty && s.io.stdout_tty))
                {
                    std::print(s.err(),
                               "learndc: refusing to reset '{}' without confirmation ({} file(s) differ from the starter); pass --yes to reset it{}\n", ex.id,
                               changed.size(), backup ? " (your files are backed up first)" : " WITHOUT a backup");
                    return app::exit_refused;
                }
                auto const prompt = std::format("Reset {} to its starter files? {} file(s) differ. {}", ex.id, changed.size(),
                                                backup ? "A backup is saved first." : "No backup will be made.");
                if (!confirm(s, prompt))
                {
                    s.out() << "Nothing was changed.\n";
                    return app::exit_refused;
                }
            }
            auto result = app::reset_exercise(s.ctx, ex, ws, backup);
            if (!result)
                return report_failure(s, result.error());
            std::print(s.out(), "Reset {} in {}\n", ex.id, platform::path_to_utf8(ws.root));
            if (backup && !result->backup_path.empty())
                std::print(s.out(), "Backup of your files: {}\n", platform::path_to_utf8(result->backup_path));
            std::print(s.out(), "Restored {} starter file(s)", result->restored.size());
            if (!result->removed.empty())
                std::print(s.out(), ", removed {} file(s) you created", result->removed.size());
            s.out() << "\n";
            for (auto const& k : result->kept)
                std::print(s.out(), "  kept {}\n", k);
            return app::exit_ok;
        }

        [[nodiscard]] inline std::string bar(Session const& s, int done, int total, int width)
        {
            int const filled = total > 0 ? done * width / total : 0;
            std::string out = "[";
            out.append(static_cast<std::size_t>(filled), '#');
            out.append(static_cast<std::size_t>(width - filled), '.');
            out += "]";
            (void)s;
            return out;
        }

        inline int cmd_progress(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto const& p = *s.loaded;
            auto sum = app::summarize(s.ctx, p);
            if (s.json())
            {
                auto root = json::Json::make_object();
                root.set("total", json::Json(sum.total));
                root.set("completed", json::Json(sum.completed));
                root.set("in_progress", json::Json(sum.in_progress));
                root.set("not_started", json::Json(sum.not_started));
                root.set("needs_revisit", json::Json(sum.needs_revisit));
                root.set("active_exercise", sum.active.empty() ? json::Json() : json::Json(sum.active));
                root.set("recent", strings_json(sum.recent));
                root.set("bookmarks", strings_json(sum.bookmarks));
                auto tracks = json::Json::make_array();
                for (auto const& t : sum.tracks)
                {
                    auto tj = json::Json::make_object();
                    tj.set("id", json::Json(t.id));
                    tj.set("title", json::Json(t.title));
                    tj.set("total", json::Json(t.total));
                    tj.set("completed", json::Json(t.completed));
                    tj.set("in_progress", json::Json(t.in_progress));
                    tj.set("number", json::Json(t.number));
                    tj.set("bonus_total", json::Json(t.bonus_total));
                    tj.set("bonus_completed", json::Json(t.bonus_completed));
                    tj.set("boss", t.boss_id.empty() ? json::Json() : json::Json(t.boss_id));
                    tj.set("boss_defeated", json::Json(t.boss_defeated));
                    tj.set("done", json::Json(t.done));
                    tracks.push(std::move(tj));
                }
                root.set("tracks", std::move(tracks));
                auto exercises = json::Json::make_array();
                for (auto const& ex : s.ctx.catalog.exercises())
                {
                    auto it = p.exercises.find(ex.id);
                    if (it == p.exercises.end())
                        continue;
                    auto const& e = it->second;
                    auto ej = json::Json::make_object();
                    ej.set("id", json::Json(ex.id));
                    ej.set("status", json::Json(std::string(progress::to_string(e.status))));
                    ej.set("attempts", json::Json(e.attempts));
                    ej.set("passes", json::Json(e.passes));
                    ej.set("hint_level", json::Json(e.hint_level));
                    ej.set("solution_viewed", json::Json(e.solution_viewed));
                    ej.set("first_started_at", json::Json(e.first_started_at));
                    ej.set("last_checked_at", json::Json(e.last_checked_at));
                    ej.set("completed_at", json::Json(e.completed_at));
                    ej.set("completed_revision", json::Json(e.completed_revision));
                    ej.set("last_passed", json::Json(e.last_passed));
                    ej.set("last_total", json::Json(e.last_total));
                    ej.set("needs_revisit", json::Json(progress::needs_revisit(e, ex.revision)));
                    if (!e.stages.empty())
                    {
                        auto stages = json::Json::make_object();
                        for (auto const& [name, st] : e.stages)
                        {
                            auto sj = json::Json::make_object();
                            sj.set("achieved", json::Json(st.achieved()));
                            sj.set("achieved_at", json::Json(st.achieved_at));
                            sj.set("last", json::Json(st.last));
                            sj.set("hint_level", json::Json(st.hint_level));
                            stages.set(name, std::move(sj));
                        }
                        ej.set("stages", std::move(stages));
                    }
                    exercises.push(std::move(ej));
                }
                root.set("exercises", std::move(exercises));
                emit_json(s, root);
                return app::exit_ok;
            }
            auto& o = s.out();
            int const pct = sum.total > 0 ? sum.completed * 100 / sum.total : 0;
            std::print(o, "{} of {} lessons completed ({}%), {} in progress\n", sum.completed, sum.total, pct, sum.in_progress);
            if (sum.bosses_total > 0)
                std::print(o, "{} of {} chapters complete, {} of {} bosses defeated\n", sum.chapters_done, sum.tracks.size(), sum.bosses_defeated,
                           sum.bosses_total);
            s.out() << '\n';
            std::size_t w = 4;
            for (auto const& t : sum.tracks)
                w = std::max(w, text::display_width(t.title) + 4);
            for (auto const& t : sum.tracks)
            {
                std::string boss;
                if (!t.boss_id.empty())
                    boss = t.boss_defeated ? std::format("  boss defeated: {}", t.boss_title) : std::format("  boss: {}", t.boss_title);
                std::print(o, "  {}  {:>2}/{:<2}  {}{}\n", pad(std::format("{:>2}. {}", t.number, t.title), w), t.completed, t.total,
                           bar(s, t.completed, t.total, 20), boss);
            }
            auto const earned = app::achievements(s.ctx, p);
            if (!earned.empty())
            {
                std::print(o, "\nAchievements\n");
                for (auto const& a : earned)
                    std::print(o, "  {}{}\n", a.title, a.detail.empty() ? "" : std::format(" ({})", a.detail));
            }
            if (!sum.active.empty())
                std::print(o, "\nActive exercise: {}\n", sum.active);
            if (!sum.recent.empty())
                std::print(o, "Recent: {}\n", text::join(sum.recent, ", "));
            if (!sum.bookmarks.empty())
                std::print(o, "Bookmarks: {}\n", text::join(sum.bookmarks, ", "));
            if (sum.needs_revisit > 0)
                std::print(o, "{} completed exercise(s) were updated since you finished them; see 'learndc review'\n", sum.needs_revisit);
            return app::exit_ok;
        }

        inline int cmd_review(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto items = app::review_list(s.ctx, *s.loaded);
            if (s.json())
            {
                auto root = json::Json::make_object();
                auto arr = json::Json::make_array();
                for (auto const& item : items)
                {
                    auto j = exercise_json(s.ctx, *item.exercise, *s.loaded);
                    j.set("reason", json::Json(item.reason));
                    arr.push(std::move(j));
                }
                root.set("review", std::move(arr));
                emit_json(s, root);
                return app::exit_ok;
            }
            if (items.empty())
            {
                s.out() << "Nothing to review right now.\n";
                return app::exit_ok;
            }
            for (auto const& item : items)
                std::print(s.out(), "{}  {}\n    {}\n", item.exercise->id, item.exercise->title, item.reason);
            return app::exit_ok;
        }

        inline int cmd_bookmark(Session& s)
        {
            if (auto f = prepare(s))
                return report_failure(s, *f);
            auto found = app::find_exercise(s.ctx, s.args.positionals[0]);
            if (!found)
                return report_failure(s, found.error());
            bool const wanted = !s.args.flag("remove");
            auto r = app::set_bookmark(s.ctx, **found, wanted);
            if (!r)
                return report_failure(s, r.error());
            std::print(s.out(), "{} {}\n", wanted ? "Bookmarked" : "Removed the bookmark from", (*found)->id);
            return app::exit_ok;
        }

        inline int cmd_config(Session& s)
        {
            auto const& pos = s.args.positionals;
            auto const& action = pos[0];
            auto const& file = s.ctx.dirs.config_file;
            if (action == "list")
            {
                if (pos.size() != 1)
                    return usage_error(s, "'config list' takes no arguments");
                if (s.json())
                {
                    auto j = json::Json::make_object();
                    for (auto const& info : settings::keys())
                    {
                        auto v = settings::get_json(s.ctx.settings, info.key);
                        if (v)
                            j.set(info.key, std::move(*v));
                    }
                    emit_json(s, j);
                    return app::exit_ok;
                }
                for (auto const& [k, v] : settings::list(s.ctx.settings))
                    std::print(s.out(), "{} = {}\n", k, v);
                return app::exit_ok;
            }
            if (action == "get")
            {
                if (pos.size() != 2)
                    return usage_error(s, "'config get' needs a key");
                if (s.json())
                {
                    auto v = settings::get_json(s.ctx.settings, pos[1]);
                    if (!v)
                        return usage_error(s, v.error().message);
                    auto j = json::Json::make_object();
                    j.set("key", json::Json(pos[1]));
                    j.set("value", std::move(*v));
                    emit_json(s, j);
                    return app::exit_ok;
                }
                auto v = settings::get(s.ctx.settings, pos[1]);
                if (!v)
                    return usage_error(s, v.error().message);
                std::print(s.out(), "{}\n", *v);
                return app::exit_ok;
            }
            if (action == "set" || action == "unset")
            {
                bool const unset = action == "unset";
                if (pos.size() != (unset ? 2u : 3u))
                    return usage_error(s, unset ? "'config unset' needs a key" : "'config set' needs a key and a value");
                if (!settings::find_key(pos[1]))
                    return usage_error(s, std::format("unknown setting '{}'; run 'learndc config list' for the valid keys", text::sanitize(pos[1])));
                std::string const value = unset ? settings::default_text(pos[1]) : pos[2];
                auto trial = s.ctx.settings;
                if (auto r = settings::set(trial, pos[1], value); !r)
                    return usage_error(s, r.error().message);
                auto saved = settings::update(file, [&](settings::Settings& st) { return settings::set(st, pos[1], value); });
                if (!saved)
                    return report_failure(s, app::Failure{app::exit_data, saved.error().message});
                s.ctx.settings = *saved;
                std::print(s.out(), "{} = {}\n", pos[1], settings::get(*saved, pos[1]).value_or(""));
                return app::exit_ok;
            }
            return usage_error(s, std::format("unknown config action '{}' (get, set, unset or list)", text::sanitize(action)));
        }

        inline int cmd_doctor(Session& s)
        {
            doctor::DoctorOptions o;
            o.dirs = s.ctx.dirs;
            o.settings = s.ctx.settings;
            o.env = s.io.env;
            o.tools = s.ctx.tools;
            o.content = &s.ctx.source;
            o.os = s.ctx.os;
            o.build = s.args.flag("build");
            o.cancel = s.io.cancel;
            o.stdin_tty = s.io.stdin_tty;
            o.stdout_tty = s.io.stdout_tty;
            o.terminal = platform::terminal_size();
            auto report = doctor::run_doctor(o);
            if (s.json())
                emit_json(s, doctor::to_json(report));
            else
                s.out() << doctor::render_text(report, s.style);
            if (!report.has_errors())
                return app::exit_ok;
            for (std::string_view id : {"dcc", "dpm"})
                if (auto const* item = report.find(id); item && item->status == doctor::Status::Error)
                    return app::exit_toolchain;
            return app::exit_failed;
        }

        inline int cmd_export(Session& s)
        {
            auto out_path = s.args.value("output");
            if (!out_path)
                return usage_error(s, "export needs --output FILE");
            auto r = app::export_progress(s.ctx, platform::path_from_utf8(*out_path), s.args.flag("yes"));
            if (!r)
                return report_failure(s, r.error());
            flush_warnings(s);
            std::print(s.out(), "Exported your progress to {}\n", *r);
            return app::exit_ok;
        }

        inline int cmd_import(Session& s)
        {
            auto in_path = s.args.value("input");
            if (!in_path)
                return usage_error(s, "import needs --input FILE");
            auto r = app::import_progress(s.ctx, platform::path_from_utf8(*in_path), s.args.flag("apply"));
            if (!r)
                return report_failure(s, r.error());
            flush_warnings(s);
            auto& o = s.out();
            if (r->plan.changes.empty() && r->plan.conflicts.empty())
                o << "Nothing to import: your progress already contains everything in the file.\n";
            for (auto const& c : r->plan.changes)
                std::print(o, "  change: {}\n", c.describe());
            for (auto const& c : r->plan.conflicts)
                std::print(o, "  conflict: {}\n", c.describe());
            if (r->applied)
                std::print(o, "Applied {} change(s); {} conflict(s) resolved by keeping your current value.\n", r->plan.changes.size(),
                           r->plan.conflicts.size());
            else
                std::print(o, "Preview only. Run again with --apply to merge these changes.\n");
            return app::exit_ok;
        }

        struct ValidationRow
        {
            std::string id;
            std::string backend;
            std::string starter;
            std::string solution;
            bool ok = true;
            bool skipped = false;
            std::string detail;
            std::int64_t millis = 0;
        };

        inline void write_overlay(std::filesystem::path const& root, std::map<std::string, std::string> const& files)
        {
            for (auto const& [path, bytes] : files)
            {
                auto file = root;
                for (auto const& part : text::split(path, '/'))
                    file /= platform::path_from_utf8(part);
                std::error_code ec;
                std::filesystem::create_directories(file.parent_path(), ec);
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
        }

        [[nodiscard]] inline runner::CheckReport run_probe(Session& s, catalog::Exercise const& ex, std::string const& name, std::string const& source,
                                                          std::optional<std::string> const& expect_stdout, bool expect_build_failure,
                                                          std::string const& diagnostic, runner::Backend backend, std::filesystem::path const& scratch)
        {
            catalog::Exercise probe;
            probe.id = ex.id;
            probe.track = ex.track;
            probe.name = ex.name + "-" + name;
            probe.title = ex.title;
            probe.kind = catalog::ExerciseKind::CompleteProgram;
            probe.entry_file = "src/main.dc";
            probe.product = "example";
            probe.required_tools = {"dcc", "dpm"};
            probe.starter["project.dcon"] =
                "dpm::v1::Project {\n    name = \"example\",\n    version = \"0.1.0\",\n    products = {\n        { name = \"example\", entry = \"src/main.dc\" },\n    },\n}\n";
            probe.starter["src/main.dc"] = source;
            if (expect_build_failure)
            {
                catalog::Check diag;
                diag.kind = catalog::CheckKind::Diagnostic;
                diag.name = std::format("{} is rejected by the compiler as described", name);
                diag.expect_build_success = false;
                diag.diagnostic_contains = diagnostic;
                probe.checks = {diag};
            }
            else
            {
                catalog::Check build;
                build.kind = catalog::CheckKind::Build;
                build.name = "Compiles";
                catalog::Check run;
                run.kind = catalog::CheckKind::Run;
                run.name = std::format("{} prints the documented output", name);
                run.expect_stdout = expect_stdout;
                probe.checks = {build, run};
            }
            auto root = scratch / std::format("{}-{}-probe-{}", ex.track, ex.name, name);
            if (auto started = workspace::start(probe, root); !started)
            {
                runner::CheckReport failed;
                failed.outcome = runner::Outcome::Error;
                return failed;
            }
            return runner::run_checks(probe, root, app::runner_options(s.ctx, backend, s.io.cancel));
        }

        inline void validate_questions(Session& s, catalog::Exercise const& ex, runner::Backend backend, std::filesystem::path const& scratch, ValidationRow& row,
                                       int& passed, int& total)
        {
            for (auto const& q : ex.questions)
            {
                auto const tag = std::format("q-{}", q.id);
                auto expect_pass = [&](std::string const& label, std::string const& source, std::optional<std::string> const& expect) {
                    ++total;
                    auto report = run_probe(s, ex, tag, source, expect, false, {}, backend, scratch);
                    if (report.outcome == runner::Outcome::Passed)
                        ++passed;
                    else
                    {
                        row.ok = false;
                        row.detail += std::format("question {}: {}\n{}", q.id, label, runner::render_text(report, s.style));
                    }
                };
                switch (q.type)
                {
                    case catalog::QuestionType::Predict:
                        expect_pass("the program does not print the expected answer", q.code, q.expect);
                        break;
                    case catalog::QuestionType::Trace:
                        if (!q.expect.empty())
                            expect_pass("the program does not print the documented trace", q.code, q.expect);
                        break;
                    case catalog::QuestionType::Error: {
                        ++total;
                        auto report = run_probe(s, ex, tag, q.code, std::nullopt, true, q.diagnostic, backend, scratch);
                        if (report.outcome == runner::Outcome::Passed)
                            ++passed;
                        else
                        {
                            row.ok = false;
                            row.detail += std::format("question {}: the code must fail to compile with '{}'\n{}", q.id, q.diagnostic,
                                                      runner::render_text(report, s.style));
                        }
                        break;
                    }
                    case catalog::QuestionType::Bug:
                        if (!q.fixed.empty())
                        {
                            expect_pass("the fixed program does not behave as documented", q.fixed,
                                        q.expect.empty() ? std::nullopt : std::optional<std::string>(q.expect));
                            if (!q.expect.empty())
                            {
                                ++total;
                                auto report = run_probe(s, ex, tag + "-buggy", q.code, q.expect, false, {}, backend, scratch);
                                if (report.outcome == runner::Outcome::Failed)
                                    ++passed;
                                else
                                {
                                    row.ok = false;
                                    row.detail += std::format("question {}: the buggy program already prints the expected output, so there is no bug to find\n", q.id);
                                }
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        [[nodiscard]] inline ValidationRow validate_lecture(Session& s, catalog::Exercise const& ex, runner::Backend backend, std::filesystem::path const& scratch)
        {
            ValidationRow row;
            row.id = ex.id;
            row.backend = std::string(runner::to_string(backend));
            row.starter = ex.drill ? "drill" : "lecture";
            auto const begin = std::chrono::steady_clock::now();
            int passed = 0;
            int total = 0;
            for (auto const& example : ex.examples)
            {
                ++total;
                auto report = run_probe(s, ex, example.name, example.source, example.expect_stdout, false, {}, backend, scratch);
                if (report.outcome == runner::Outcome::Passed)
                    ++passed;
                else
                {
                    row.ok = false;
                    row.detail += std::format("example {} does not behave as the lecture says:\n{}", example.name, runner::render_text(report, s.style));
                }
            }
            validate_questions(s, ex, backend, scratch, row, passed, total);
            row.solution = total == 0 ? "nothing to run" : std::format("programs {}/{}", passed, total);
            row.millis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
            return row;
        }

        [[nodiscard]] inline ValidationRow validate_one(Session& s, catalog::Exercise const& ex, runner::Backend backend, std::filesystem::path const& scratch)
        {
            if (ex.is_lecture())
                return validate_lecture(s, ex, backend, scratch);
            ValidationRow row;
            row.id = ex.id;
            row.backend = std::string(runner::to_string(backend));
            auto const begin = std::chrono::steady_clock::now();
            auto opt = app::runner_options(s.ctx, backend, s.io.cancel);
            auto const slug = std::format("{}-{}", ex.track, ex.name);

            auto starter_root = scratch / (slug + "-starter");
            auto started = workspace::start(ex, starter_root);
            if (!started)
            {
                row.ok = false;
                row.starter = "error";
                row.detail = "cannot create the starter workspace: " + started.error().message;
                return row;
            }
            auto starter_report = runner::run_checks(ex, starter_root, opt);
            bool const build_stage = std::ranges::any_of(starter_report.results, [](runner::CheckResult const& result) {
                return result.kind == catalog::CheckKind::Build && result.verdict == runner::Verdict::Fail;
            });
            if (starter_report.outcome == runner::Outcome::Cancelled)
            {
                row.ok = false;
                row.starter = "cancelled";
                return row;
            }
            if (starter_report.outcome == runner::Outcome::Error)
            {
                row.ok = false;
                row.starter = "error";
                row.detail = runner::render_text(starter_report, s.style);
                return row;
            }
            if (ex.starter_state == catalog::StarterState::FailsBuild)
            {
                if (starter_report.outcome == runner::Outcome::Failed && build_stage)
                    row.starter = "fails build";
                else
                {
                    row.ok = false;
                    row.starter = starter_report.outcome == runner::Outcome::Passed ? "passes all checks" : "builds";
                    row.detail = "the starter is declared fails-build but " + std::string(starter_report.outcome == runner::Outcome::Passed
                                                                                              ? "passes every check"
                                                                                              : "builds successfully");
                }
            }
            else
            {
                if (starter_report.outcome == runner::Outcome::Failed && !build_stage)
                    row.starter = std::format("fails {}/{}", starter_report.total - starter_report.passed, starter_report.total);
                else
                {
                    row.ok = false;
                    row.starter = starter_report.outcome == runner::Outcome::Passed ? "passes all checks" : "fails to build";
                    row.detail = "the starter is declared fails-checks but " + std::string(starter_report.outcome == runner::Outcome::Passed
                                                                                                ? "passes every check"
                                                                                                : "does not build");
                }
            }

            if (ex.staged())
                for (auto const& st : starter_report.stages)
                    if (st.passed)
                    {
                        row.ok = false;
                        row.starter = "passes a stage";
                        row.detail += std::format("the starter already passes stage '{}'; every stage must start failing\n", st.id);
                    }

            auto solution_root = scratch / (slug + "-solution");
            auto sol_started = workspace::start(ex, solution_root);
            if (!sol_started)
            {
                row.ok = false;
                row.solution = "error";
                row.detail += "cannot create the solution workspace: " + sol_started.error().message;
                return row;
            }
            write_overlay(solution_root, ex.solution);
            auto solution_report = runner::run_checks(ex, solution_root, opt);
            bool const checked_stages_pass = std::ranges::all_of(
                solution_report.stages, [](runner::StageResult const& st) { return st.optional || st.checks_total == 0 || st.passed; });
            if (solution_report.outcome == runner::Outcome::Passed && !checked_stages_pass)
            {
                row.ok = false;
                row.detail += "the reference solution passes every check but is not complete\n";
            }
            if (solution_report.outcome == runner::Outcome::Passed)
                row.solution = std::format("passes {}/{}", solution_report.passed, solution_report.total);
            else
            {
                row.ok = false;
                row.solution = solution_report.outcome == runner::Outcome::Cancelled ? "cancelled" : "FAILS";
                if (!row.detail.empty())
                    row.detail += "\n";
                row.detail += "the reference solution does not pass:\n" + runner::render_text(solution_report, s.style);
            }
            if (!ex.questions.empty())
            {
                int passed = 0;
                int total = 0;
                validate_questions(s, ex, backend, scratch, row, passed, total);
                if (total > 0)
                    row.solution += std::format(", programs {}/{}", passed, total);
            }
            row.millis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
            return row;
        }

        inline int cmd_dev(Session& s)
        {
            if (s.args.positionals[0] != "validate")
                return usage_error(s, std::format("unknown dev action '{}' (validate)", text::sanitize(s.args.positionals[0])));
            if (auto f = app::require_content(s.ctx))
                return report_failure(s, *f);
            std::vector<runner::Backend> backends;
            auto const wanted = s.args.value("backend").value_or("dpm");
            if (wanted == "all")
                backends = {runner::Backend::Dpm, runner::Backend::DirectLlvm, runner::Backend::DirectCustom};
            else if (auto b = runner::parse_backend(wanted))
                backends = {*b};
            else
                return usage_error(s, std::format("unknown backend '{}' (dpm, llvm, custom or all)", text::sanitize(wanted)));
            std::vector<catalog::Exercise const*> chosen;
            if (auto id = s.args.value("id"))
            {
                auto found = app::find_exercise(s.ctx, *id);
                if (!found)
                    return report_failure(s, found.error());
                chosen.push_back(*found);
            }
            else
                for (auto const& ex : s.ctx.catalog.exercises())
                    chosen.push_back(&ex);

            std::random_device rd;
            auto const scratch = s.ctx.dirs.cache_dir / "validate" / std::format("run-{:08x}", rd());
            std::error_code ec;
            std::filesystem::create_directories(scratch, ec);
            std::print(s.out(), "Content {}: {} exercise(s) loaded, schema valid\n", s.ctx.source.label, s.ctx.catalog.exercises().size());
            std::vector<ValidationRow> rows;
            std::string const host = s.ctx.os == platform::Os::Windows ? "windows" : "linux";
            bool interrupted = false;
            for (auto const* ex : chosen)
                for (auto backend : backends)
                {
                    if (s.io.cancel && s.io.cancel->load())
                    {
                        interrupted = true;
                        break;
                    }
                    ValidationRow row;
                    row.id = ex->id;
                    row.backend = std::string(runner::to_string(backend));
                    std::string const needed = backend == runner::Backend::DirectCustom ? "custom" : "llvm";
                    if (!ex->supports_platform(host) || !ex->supports_backend(needed))
                    {
                        row.skipped = true;
                        row.starter = "skipped";
                        row.solution = !ex->supports_platform(host) ? "not on this platform" : "backend not listed";
                    }
                    else
                    {
                        std::print(s.out(), "validating {} [{}]...\n", ex->id, row.backend);
                        s.out().flush();
                        row = validate_one(s, *ex, backend, scratch);
                    }
                    rows.push_back(std::move(row));
                }
            std::filesystem::remove_all(scratch, ec);
            std::size_t id_w = 2;
            for (auto const& r : rows)
                id_w = std::max(id_w, text::display_width(r.id));
            std::print(s.out(), "\n{}  {}  {}  {}  {}\n", pad("EXERCISE", id_w), pad("BACKEND", 7), pad("STARTER", 18), pad("SOLUTION", 20), "RESULT");
            int failures = 0;
            for (auto const& r : rows)
            {
                std::string result = r.skipped ? "skip" : r.ok ? "ok" : "FAIL";
                if (!r.ok)
                    ++failures;
                std::print(s.out(), "{}  {}  {}  {}  {}\n", pad(r.id, id_w), pad(r.backend, 7), pad(r.starter, 18), pad(r.solution, 20),
                           paint(s, r.ok ? (r.skipped ? "2" : "32") : "1;31", result));
            }
            for (auto const& r : rows)
                if (!r.ok && !r.detail.empty())
                    std::print(s.out(), "\n{} [{}]:\n{}\n", r.id, r.backend, r.detail);
            std::print(s.out(), "\n{} check(s) of {} exercise(s): {} failed\n", rows.size(), chosen.size(), failures);
            if (interrupted)
                return app::exit_interrupted;
            return failures == 0 ? app::exit_ok : app::exit_failed;
        }

        [[nodiscard]] inline runner::TextStyle make_style(Parsed const& args, app::Context const& ctx, Io const& io)
        {
            runner::TextStyle style;
            bool color = io.stdout_tty && !args.flag("no-color");
            if (auto it = io.env.find("NO_COLOR"); it != io.env.end() && !it->second.empty())
                color = false;
            if (auto it = io.env.find("TERM"); it != io.env.end() && it->second == "dumb")
                color = false;
            if (ctx.settings.ui_theme == "no-color")
                color = false;
            style.color = color;
            style.ascii = args.flag("ascii") || ctx.settings.ui_ascii == "on" || (ctx.settings.ui_ascii == "auto" && !platform::utf8_locale(io.env));
            return style;
        }
    } // namespace detail

    [[nodiscard]] inline int run(std::vector<std::string> const& args, Io& io)
    {
        auto parsed = parse(args);
        if (!parsed)
        {
            std::print(*io.err, "learndc: {}\nTry 'learndc --help' for usage.\n", parsed.error());
            return app::exit_usage;
        }
        if (parsed->help && parsed->command.empty())
        {
            *io.out << help_text();
            return app::exit_ok;
        }
        if (parsed->help)
        {
            *io.out << command_help(*find_command(parsed->command));
            return app::exit_ok;
        }
        if (parsed->command == "help")
        {
            if (auto topic = parsed->positional(0))
            {
                auto const* spec = find_command(*topic);
                if (!spec)
                {
                    std::print(*io.err, "learndc: unknown command '{}'\n", text::sanitize(*topic));
                    return app::exit_usage;
                }
                *io.out << command_help(*spec);
            }
            else
                *io.out << help_text();
            return app::exit_ok;
        }

        app::OpenOptions oo;
        if (auto h = parsed->value("home"))
            oo.home = platform::path_from_utf8(*h);
        if (auto c = parsed->value("content-dir"))
            oo.content_dir = platform::path_from_utf8(*c);
        oo.env = io.env;
        oo.self_exe = io.self_exe;
        auto ctx = app::open(oo);
        if (!ctx)
        {
            std::print(*io.err, "learndc: {}\n", ctx.error().message);
            return ctx.error().code;
        }
        for (auto const& w : ctx->warnings)
            std::print(*io.err, "learndc: warning: {}\n", w);
        ctx->warnings.clear();

        if (parsed->version)
        {
            std::print(*io.out, "learndc {} ({})\n", version_text, git_hash_text);
            if (ctx->has_content())
            {
                std::print(*io.out, "content: {} exercises ({})\n", ctx->catalog.exercises().size(), ctx->source.label);
                return app::exit_ok;
            }
            std::print(*io.err, "learndc: the exercise content could not be loaded:\n{}\n", ctx->content_error);
            return app::exit_data;
        }

        auto const style = detail::make_style(*parsed, *ctx, io);
        if (parsed->command.empty())
        {
            if (io.stdin_tty && io.stdout_tty && io.tui)
                return io.tui(*ctx);
            *io.out << help_text();
            return app::exit_ok;
        }

        detail::Session session{*ctx, *parsed, io, style, std::nullopt};
        using Handler = int (*)(detail::Session&);
        static std::map<std::string, Handler, std::less<>> const handlers = {
            {"list", detail::cmd_list},     {"paths", detail::cmd_paths},       {"search", detail::cmd_search}, {"show", detail::cmd_show},
            {"start", detail::cmd_start},   {"next", detail::cmd_next},         {"edit", detail::cmd_edit},     {"run", detail::cmd_run},
            {"check", detail::cmd_check},   {"watch", detail::cmd_watch},       {"hint", detail::cmd_hint},     {"solution", detail::cmd_solution},
            {"reset", detail::cmd_reset},   {"progress", detail::cmd_progress}, {"review", detail::cmd_review}, {"bookmark", detail::cmd_bookmark},
            {"config", detail::cmd_config}, {"doctor", detail::cmd_doctor},   {"export", detail::cmd_export},
            {"import", detail::cmd_import}, {"dev", detail::cmd_dev},           {"answer", detail::cmd_answer}, {"cards", detail::cmd_cards},
            {"card", detail::cmd_card},
        };
        auto it = handlers.find(parsed->command);
        if (it == handlers.end())
            return app::exit_usage;
        int const code = it->second(session);
        io.out->flush();
        io.err->flush();
        return code;
    }
} // namespace learndc::cli
