export module learndc.editor;

import std;
import learndc.text;
import learndc.platform;
import learndc.settings;
import dcc.host.process;

export namespace learndc::editor
{
    enum class Kind
    {
        Terminal,
        Gui,
        Manual,
    };

    [[nodiscard]] inline std::string_view to_string(Kind k)
    {
        switch (k)
        {
            case Kind::Terminal:
                return "terminal";
            case Kind::Gui:
                return "gui";
            case Kind::Manual:
                return "manual";
        }
        return "manual";
    }

    enum class Source
    {
        Override,
        Settings,
        Visual,
        Editor,
        Detected,
        Manual,
    };

    [[nodiscard]] inline std::string_view to_string(Source s)
    {
        switch (s)
        {
            case Source::Override:
                return "--editor";
            case Source::Settings:
                return "settings";
            case Source::Visual:
                return "VISUAL";
            case Source::Editor:
                return "EDITOR";
            case Source::Detected:
                return "detected";
            case Source::Manual:
                return "manual";
        }
        return "manual";
    }

    enum class Syntax
    {
        Posix,
        Windows,
    };

    [[nodiscard]] inline Syntax syntax_for(platform::Os os)
    {
        return os == platform::Os::Windows ? Syntax::Windows : Syntax::Posix;
    }

    struct EditorChoice
    {
        std::vector<std::string> argv_template;
        std::vector<std::string> folder_template;
        Kind kind = Kind::Manual;
        Source source = Source::Manual;
        std::string label;
        bool wait = false;
        std::string problem;
        std::vector<std::string> warnings;

        [[nodiscard]] bool manual() const { return kind == Kind::Manual || argv_template.empty(); }
    };

    struct Placeholders
    {
        std::string file;
        std::string workspace;
        int line = 1;
        int column = 1;
    };

    struct Detected
    {
        std::string preset;
        std::filesystem::path path;
    };

    namespace detail
    {
        inline constexpr std::string_view shell_characters = "|&;<>$`()";

        [[nodiscard]] inline std::string meta_message(char c)
        {
            return std::format("the command contains the shell character '{}'; learndc starts the editor directly without a shell, so put the character inside "
                               "quotes or remove it",
                               c);
        }

        [[nodiscard]] inline bool is_blank(char c)
        {
            return c == ' ' || c == '\t';
        }

        [[nodiscard]] inline std::string base_name_of(std::string_view program)
        {
            auto const slash = program.find_last_of("/\\");
            auto name = std::string(slash == std::string_view::npos ? program : program.substr(slash + 1));
            name = text::lower(name);
            for (std::string_view const suffix : {".exe", ".cmd", ".bat"})
                if (name.size() > suffix.size() && name.ends_with(suffix))
                    name.resize(name.size() - suffix.size());
            return name;
        }

        [[nodiscard]] inline bool has_placeholder(std::vector<std::string> const& argv)
        {
            return std::ranges::any_of(argv, [](std::string const& a) { return a.contains("{file}"); });
        }

        [[nodiscard]] inline Kind guess_kind(std::string_view base)
        {
            static constexpr std::array<std::string_view, 12> gui = {"code", "codium",  "code-insiders", "subl", "sublime_text", "gedit",
                                                                     "kate", "notepad", "notepad++",     "zed",  "atom",         "mousepad"};
            return std::ranges::find(gui, base) != gui.end() ? Kind::Gui : Kind::Terminal;
        }
    } // namespace detail

    [[nodiscard]] inline std::expected<std::vector<std::string>, text::Error> parse_command_line(std::string_view line, Syntax syntax)
    {
        std::vector<std::string> args;
        std::string current;
        bool in_token = false;
        auto flush = [&]() {
            if (in_token)
                args.push_back(std::move(current));
            current.clear();
            in_token = false;
        };
        auto fail = [](std::string message) -> std::expected<std::vector<std::string>, text::Error> {
            return std::unexpected(text::Error{std::move(message)});
        };
        if (syntax == Syntax::Posix)
        {
            enum class State
            {
                Plain,
                Single,
                Double,
            } state = State::Plain;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                char const c = line[i];
                if (c == '\n' || c == '\r')
                    return fail("the command must be on a single line");
                if (state == State::Single)
                {
                    if (c == '\'')
                        state = State::Plain;
                    else
                        current += c;
                    continue;
                }
                if (state == State::Double)
                {
                    if (c == '"')
                        state = State::Plain;
                    else if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\' || line[i + 1] == '$' || line[i + 1] == '`'))
                        current += line[++i];
                    else if (c == '$' || c == '`')
                        return fail(detail::meta_message(c));
                    else
                        current += c;
                    continue;
                }
                if (detail::is_blank(c))
                {
                    flush();
                    continue;
                }
                if (c == '\'')
                {
                    state = State::Single;
                    in_token = true;
                    continue;
                }
                if (c == '"')
                {
                    state = State::Double;
                    in_token = true;
                    continue;
                }
                if (c == '\\')
                {
                    if (i + 1 >= line.size())
                        return fail("the command ends with a lone backslash");
                    current += line[++i];
                    in_token = true;
                    continue;
                }
                if (detail::shell_characters.find(c) != std::string_view::npos)
                    return fail(detail::meta_message(c));
                current += c;
                in_token = true;
            }
            if (state != State::Plain)
                return fail("the command has an unterminated quote");
            flush();
        }
        else
        {
            bool quoted = false;
            std::size_t i = 0;
            while (i < line.size())
            {
                char const c = line[i];
                if (c == '\n' || c == '\r')
                    return fail("the command must be on a single line");
                if (c == '\\')
                {
                    std::size_t slashes = 0;
                    while (i < line.size() && line[i] == '\\')
                    {
                        ++slashes;
                        ++i;
                    }
                    if (i < line.size() && line[i] == '"')
                    {
                        current.append(slashes / 2, '\\');
                        if (slashes % 2 == 1)
                        {
                            current += '"';
                            ++i;
                        }
                    }
                    else
                        current.append(slashes, '\\');
                    in_token = true;
                    continue;
                }
                if (c == '"')
                {
                    quoted = !quoted;
                    in_token = true;
                    ++i;
                    continue;
                }
                if (!quoted && detail::is_blank(c))
                {
                    flush();
                    ++i;
                    continue;
                }
                if (!quoted && detail::shell_characters.find(c) != std::string_view::npos)
                    return fail(detail::meta_message(c));
                current += c;
                in_token = true;
                ++i;
            }
            if (quoted)
                return fail("the command has an unterminated quote");
            flush();
        }
        if (args.empty())
            return fail("the command is empty");
        if (args.front().empty())
            return fail("the program name is empty");
        return args;
    }

    [[nodiscard]] inline std::string expand_argument(std::string_view arg, Placeholders const& ph)
    {
        std::string out;
        std::size_t i = 0;
        while (i < arg.size())
        {
            if (arg[i] == '{')
            {
                auto const close = arg.find('}', i);
                if (close != std::string_view::npos)
                {
                    auto const name = arg.substr(i + 1, close - i - 1);
                    if (name == "file")
                    {
                        out += ph.file;
                        i = close + 1;
                        continue;
                    }
                    if (name == "workspace")
                    {
                        out += ph.workspace;
                        i = close + 1;
                        continue;
                    }
                    if (name == "line")
                    {
                        out += std::to_string(ph.line);
                        i = close + 1;
                        continue;
                    }
                    if (name == "column")
                    {
                        out += std::to_string(ph.column);
                        i = close + 1;
                        continue;
                    }
                }
            }
            out += arg[i];
            ++i;
        }
        return out;
    }

    [[nodiscard]] inline std::vector<std::string> expand(std::vector<std::string> const& argv_template, Placeholders const& ph)
    {
        std::vector<std::string> out;
        out.reserve(argv_template.size());
        for (auto const& arg : argv_template)
            out.push_back(expand_argument(arg, ph));
        return out;
    }

    [[nodiscard]] inline std::vector<std::string> preset_names()
    {
        return {"nvim", "vim", "nano", "code", "codium", "notepad"};
    }

    [[nodiscard]] inline std::optional<EditorChoice> preset(std::string_view name, bool wait, std::string_view program = {})
    {
        EditorChoice c;
        c.label = std::string(name);
        c.wait = wait;
        auto const prog = [&](std::string_view fallback) { return program.empty() ? std::string(fallback) : std::string(program); };
        if (name == "nvim" || name == "vim")
        {
            c.argv_template = {prog(name), "+{line}", "{file}"};
            c.kind = Kind::Terminal;
        }
        else if (name == "nano")
        {
            c.argv_template = {prog("nano"), "+{line},{column}", "{file}"};
            c.kind = Kind::Terminal;
        }
        else if (name == "code" || name == "codium")
        {
            c.argv_template = {prog(name)};
            if (wait)
                c.argv_template.push_back("--wait");
            c.argv_template.push_back("--goto");
            c.argv_template.push_back("{file}:{line}:{column}");
            c.folder_template = {prog(name), "{workspace}"};
            c.kind = Kind::Gui;
        }
        else if (name == "notepad")
        {
            c.argv_template = {prog("notepad.exe"), "{file}"};
            c.kind = Kind::Gui;
        }
        else
            return std::nullopt;
        return c;
    }

    [[nodiscard]] inline std::vector<Detected> detect(platform::Env const& env, platform::Os os = platform::host_os())
    {
        std::vector<Detected> out;
        for (auto const& name : preset_names())
        {
            if (name == "notepad" && os != platform::Os::Windows)
                continue;
            if (auto found = platform::find_program(name, env, os))
                out.push_back(Detected{name, *found});
        }
        return out;
    }

    namespace detail
    {
        inline void apply_kind(EditorChoice& choice, settings::Settings const& s)
        {
            if (s.editor_kind == "terminal")
                choice.kind = Kind::Terminal;
            else if (s.editor_kind == "gui")
                choice.kind = Kind::Gui;
            choice.wait = s.editor_wait;
        }

        [[nodiscard]] inline EditorChoice from_words(std::vector<std::string> words, Source source, settings::Settings const& s)
        {
            EditorChoice choice;
            auto const base = base_name_of(words.front());
            auto known = preset(base, s.editor_wait, words.front());
            if (known && words.size() == 1)
                choice = *known;
            else
            {
                choice.label = base;
                choice.kind = guess_kind(base);
                choice.argv_template = std::move(words);
                if (!has_placeholder(choice.argv_template))
                    choice.argv_template.push_back("{file}");
            }
            choice.source = source;
            apply_kind(choice, s);
            return choice;
        }

        [[nodiscard]] inline EditorChoice manual_choice(Source source, std::string problem = {})
        {
            EditorChoice c;
            c.kind = Kind::Manual;
            c.source = source;
            c.label = "manual";
            c.problem = std::move(problem);
            return c;
        }
    } // namespace detail

    [[nodiscard]] inline EditorChoice resolve(settings::Settings const& s, platform::Env const& env, std::optional<std::string> const& override_value,
                                              platform::Os os = platform::host_os())
    {
        auto const syntax = syntax_for(os);
        if (override_value && !override_value->empty())
        {
            if (*override_value == "manual")
                return detail::manual_choice(Source::Override);
            auto words = parse_command_line(*override_value, syntax);
            if (!words)
                return detail::manual_choice(Source::Override, std::format("--editor: {}", words.error().message));
            return detail::from_words(std::move(*words), Source::Override, s);
        }
        if (s.editor_preset != "auto")
        {
            if (s.editor_preset == "manual")
                return detail::manual_choice(Source::Settings);
            if (s.editor_preset == "custom")
            {
                if (s.editor_command.empty() || s.editor_command.front().empty())
                    return detail::manual_choice(Source::Settings, "editor.preset is custom but editor.command is empty");
                EditorChoice c;
                c.argv_template = s.editor_command;
                if (!detail::has_placeholder(c.argv_template))
                    c.argv_template.push_back("{file}");
                c.label = detail::base_name_of(s.editor_command.front());
                c.kind = detail::guess_kind(c.label);
                c.source = Source::Settings;
                detail::apply_kind(c, s);
                return c;
            }
            if (auto p = preset(s.editor_preset, s.editor_wait))
            {
                p->source = Source::Settings;
                detail::apply_kind(*p, s);
                return *p;
            }
            return detail::manual_choice(Source::Settings, std::format("unknown editor preset '{}'", s.editor_preset));
        }
        std::vector<std::string> warnings;
        for (auto const& [name, source] :
             {std::pair<std::string_view, Source>{"VISUAL", Source::Visual}, std::pair<std::string_view, Source>{"EDITOR", Source::Editor}})
        {
            auto const it = env.find(std::string(name));
            if (it == env.end() || text::trim(it->second).empty())
                continue;
            auto words = parse_command_line(it->second, syntax);
            if (!words)
            {
                warnings.push_back(std::format("{} ignored: {}", name, words.error().message));
                continue;
            }
            auto choice = detail::from_words(std::move(*words), source, s);
            choice.warnings = std::move(warnings);
            return choice;
        }
        auto const found = detect(env, os);
        if (!found.empty())
        {
            auto p = preset(found.front().preset, s.editor_wait);
            if (p)
            {
                p->source = Source::Detected;
                detail::apply_kind(*p, s);
                p->warnings = std::move(warnings);
                return *p;
            }
        }
        auto manual = detail::manual_choice(Source::Manual);
        manual.warnings = std::move(warnings);
        return manual;
    }

    enum class Mode
    {
        Terminal,
        GuiDetached,
        GuiWait,
        Manual,
    };

    [[nodiscard]] inline Mode mode_of(EditorChoice const& choice)
    {
        if (choice.manual())
            return Mode::Manual;
        if (choice.kind == Kind::Terminal)
            return Mode::Terminal;
        return choice.wait ? Mode::GuiWait : Mode::GuiDetached;
    }

    struct LaunchOptions
    {
        bool folder = false;
        std::atomic<bool> const* cancel = nullptr;
        std::chrono::milliseconds timeout{std::chrono::hours(12)};
    };

    struct LaunchResult
    {
        bool ok = false;
        bool cancelled = false;
        Mode mode = Mode::Manual;
        std::string message;
        std::string shown_path;
        std::optional<int> exit_code;
        std::vector<std::string> argv;
    };

    [[nodiscard]] inline LaunchResult launch(EditorChoice const& choice, Placeholders const& ph, LaunchOptions const& options = {})
    {
        LaunchResult out;
        out.mode = mode_of(choice);
        out.shown_path = options.folder ? ph.workspace : ph.file;
        if (out.mode == Mode::Manual)
        {
            out.message = std::format("Open {} in your editor", out.shown_path);
            if (!choice.problem.empty())
                out.message += std::format(" ({})", choice.problem);
            return out;
        }
        Placeholders effective = ph;
        std::vector<std::string> const* tmpl = &choice.argv_template;
        if (options.folder)
        {
            if (!choice.folder_template.empty())
                tmpl = &choice.folder_template;
            else if (choice.kind == Kind::Terminal)
            {
                out.message = "this editor runs in the terminal and cannot open a folder; open a file instead";
                return out;
            }
            else
                effective.file = ph.workspace;
        }
        out.argv = expand(*tmpl, effective);
        std::filesystem::path const cwd = ph.workspace.empty() ? std::filesystem::path() : platform::path_from_utf8(ph.workspace);
        if (out.mode == Mode::GuiDetached)
        {
            auto spawned = platform::spawn_detached(out.argv, cwd);
            if (!spawned)
            {
                out.message = std::format("could not start {}: {}", out.argv.front(), spawned.error().message);
                return out;
            }
            out.ok = true;
            out.message = std::format("Opened {} in {}", out.shown_path, choice.label);
            return out;
        }
        dcc::host::process::Command command;
        command.argv = out.argv;
        command.cwd = cwd;
        command.inherit_env = true;
        command.cancel = options.cancel;
        command.timeout = options.timeout;
        if (out.mode == Mode::Terminal)
        {
            command.passthrough = true;
            command.own_group = true;
        }
        else
            command.stdout_file = platform::host_os() == platform::Os::Windows ? "NUL" : "/dev/null";
        auto const result = dcc::host::process::run(command);
        if (!result.started)
        {
            out.message = std::format("could not start {}: {}", out.argv.front(), result.error.empty() ? "unknown error" : result.error);
            return out;
        }
        if (result.aborted)
        {
            out.cancelled = true;
            out.message = "cancelled";
            return out;
        }
        if (result.timed_out)
        {
            out.message = "the editor was stopped because it ran too long";
            return out;
        }
        if (result.signal != 0)
        {
            out.message = std::format("the editor was stopped by signal {}", result.signal);
            return out;
        }
        out.exit_code = result.exit_code;
        out.ok = result.exit_code == 0;
        out.message = out.ok ? std::format("Closed {}", choice.label) : std::format("the editor exited with code {}", result.exit_code);
        return out;
    }
} // namespace learndc::editor
