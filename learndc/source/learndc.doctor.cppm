export module learndc.doctor;

import std;
import learndc.text;
import learndc.json;
import learndc.platform;
import learndc.settings;
import learndc.catalog;
import learndc.toolchain;
import learndc.editor;
import learndc.runner;

export namespace learndc::doctor
{
    enum class Status
    {
        Ok,
        Warn,
        Error,
        Info,
    };

    [[nodiscard]] inline std::string_view to_string(Status s)
    {
        switch (s)
        {
            case Status::Ok:
                return "ok";
            case Status::Warn:
                return "warn";
            case Status::Error:
                return "error";
            case Status::Info:
                return "info";
        }
        return "info";
    }

    struct Item
    {
        std::string id;
        std::string label;
        Status status = Status::Info;
        std::string detail;
        std::string fix;
    };

    struct DoctorReport
    {
        std::vector<Item> items;

        [[nodiscard]] Item const* find(std::string_view id) const
        {
            for (auto const& item : items)
                if (item.id == id)
                    return &item;
            return nullptr;
        }

        [[nodiscard]] int count(Status s) const
        {
            return static_cast<int>(std::ranges::count_if(items, [&](Item const& i) { return i.status == s; }));
        }

        [[nodiscard]] bool has_errors() const { return count(Status::Error) > 0; }
    };

    struct DoctorOptions
    {
        platform::Dirs dirs;
        settings::Settings settings;
        platform::Env env;
        toolchain::Toolchain tools;
        catalog::ContentSource const* content = nullptr;
        std::optional<std::string> editor_override;
        platform::Os os = platform::host_os();
        bool build = false;
        runner::Backend backend = runner::Backend::Dpm;
        std::atomic<bool> const* cancel = nullptr;
        std::optional<bool> stdin_tty;
        std::optional<bool> stdout_tty;
        std::optional<platform::TerminalSize> terminal;
    };

    namespace detail
    {
        inline constexpr std::string_view probe_source =
            "module main;\n\npublic Status(Error) main() {\n    println(\"learndc doctor ok\")?;\n    return Status::Ok;\n}\n";

        inline constexpr std::string_view probe_manifest = "dpm::v1::Project {\n    name = \"doctor_probe\",\n    version = \"0.1.0\",\n    products = {\n     "
                                                           "   { name = \"doctor_probe\", entry = \"src/main.dc\" },\n    },\n}\n";

        [[nodiscard]] inline std::string tool_fix(std::string_view name, std::string_view key, std::string_view env_name)
        {
            return std::format("Install the DC toolchain so that {0} is on your PATH or next to learndc, or set {1} in the learndc settings, or set the {2} "
                               "environment variable to the full path of {0}.",
                               name, key, env_name);
        }

        inline void check_tool(DoctorReport& report, std::string_view id, std::string_view name, toolchain::Located const& located, std::string_view key,
                               std::string_view env_name, bool required)
        {
            Item item;
            item.id = std::string(id);
            item.label = std::string(name);
            if (!located.found())
            {
                item.status = required ? Status::Error : Status::Info;
                item.detail = std::format("not found. Searched:\n{}", located.searched_text());
                if (required)
                    item.fix = tool_fix(name, key, env_name);
                else
                    item.detail = std::format("not found (optional). Searched:\n{}", located.searched_text());
                report.items.push_back(std::move(item));
                return;
            }
            auto const where = platform::path_to_utf8(*located.path);
            auto const version = toolchain::version(*located.path);
            if (!version)
            {
                item.status = Status::Warn;
                item.detail = std::format("{} (from {}) did not report a version: {}", where, toolchain::to_string(located.source), version.error().message);
                item.fix = std::format("Check that {} runs from a terminal, or point {} / {} at a working copy.", where, key, env_name);
            }
            else
            {
                item.status = Status::Ok;
                item.detail = std::format("{} at {} (from {})", version->line, where, toolchain::to_string(located.source));
            }
            for (auto const& warning : located.warnings)
                item.detail += std::format("\nwarning: {}", warning);
            report.items.push_back(std::move(item));
        }

        inline void check_dir(DoctorReport& report, std::string_view id, std::string_view label, std::filesystem::path const& dir)
        {
            Item item;
            item.id = std::string(id);
            item.label = std::string(label);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            auto const text_path = platform::path_to_utf8(dir);
            if (ec)
            {
                item.status = Status::Error;
                item.detail = std::format("{} cannot be created: {}", text_path, ec.message());
                item.fix = "Make the directory writable, or choose another location with --home DIR or the LEARNDC_HOME environment variable.";
                report.items.push_back(std::move(item));
                return;
            }
            std::random_device rd;
            auto const probe = dir / platform::path_from_utf8(std::format(".learndc-probe-{:08x}", rd()));
            auto created = platform::create_new_file(probe, "ok");
            std::filesystem::remove(probe, ec);
            if (!created)
            {
                item.status = Status::Error;
                item.detail = std::format("{} is not writable: {}", text_path, created.error().message);
                item.fix = "Make the directory writable, or choose another location with --home DIR or the LEARNDC_HOME environment variable.";
            }
            else
            {
                item.status = Status::Ok;
                item.detail = std::format("{} is writable", text_path);
            }
            report.items.push_back(std::move(item));
        }

        inline void check_content(DoctorReport& report, catalog::ContentSource const* content)
        {
            Item item;
            item.id = "content";
            item.label = "exercise content";
            if (!content)
            {
                item.status = Status::Info;
                item.detail = "content was not loaded for this check";
                report.items.push_back(std::move(item));
                return;
            }
            auto loaded = catalog::Catalog::load(*content);
            if (!loaded)
            {
                item.status = Status::Error;
                auto const& errors = loaded.error();
                item.detail = std::format("{} problem{} in {}; first: {}", errors.size(), errors.size() == 1 ? "" : "s", content->label,
                                          errors.empty() ? std::string("unknown") : errors.front().format());
                item.fix = "Reinstall learndc, or fix the content directory given with --content-dir or LEARNDC_CONTENT_DIR.";
            }
            else
            {
                item.status = Status::Ok;
                item.detail = std::format("{} exercises in {} tracks ({})", loaded->exercises().size(), loaded->tracks().size(), content->label);
            }
            report.items.push_back(std::move(item));
        }

        inline void check_editor(DoctorReport& report, DoctorOptions const& options)
        {
            Item item;
            item.id = "editor";
            item.label = "editor";
            auto const choice = editor::resolve(options.settings, options.env, options.editor_override, options.os);
            if (choice.manual())
            {
                item.status = Status::Warn;
                item.detail = choice.problem.empty() ? "no editor was found; learndc will show the file path for you to open yourself" : choice.problem;
                item.fix = "Set editor.preset (nvim, vim, nano, code, codium, notepad or custom) in the learndc settings, or set the EDITOR or VISUAL "
                           "environment variable.";
            }
            else
            {
                auto const program = choice.argv_template.front();
                auto const found = platform::find_program(program, options.env, options.os);
                item.detail = std::format("{} ({}, chosen by {}): {}", choice.label, editor::to_string(choice.kind), editor::to_string(choice.source), program);
                if (!found)
                {
                    item.status = Status::Warn;
                    item.detail += " was not found on PATH";
                    item.fix = "Install that editor, or change editor.preset / editor.command in the learndc settings, or unset EDITOR and VISUAL.";
                }
                else
                    item.status = Status::Ok;
            }
            for (auto const& warning : choice.warnings)
                item.detail += std::format("\nwarning: {}", warning);
            report.items.push_back(std::move(item));
        }

        inline void check_terminal(DoctorReport& report, DoctorOptions const& options)
        {
            bool const in_tty = options.stdin_tty.value_or(platform::stdin_is_tty());
            bool const out_tty = options.stdout_tty.value_or(platform::stdout_is_tty());
            {
                Item item;
                item.id = "terminal.tty";
                item.label = "terminal";
                item.status = in_tty && out_tty ? Status::Ok : Status::Info;
                item.detail =
                    in_tty && out_tty ? "interactive terminal" : "not an interactive terminal; the full-screen interface needs one, the commands still work";
                report.items.push_back(std::move(item));
            }
            if (options.os != platform::Os::Windows)
            {
                Item item;
                item.id = "terminal.term";
                item.label = "TERM";
                auto const it = options.env.find("TERM");
                if (it == options.env.end() || it->second.empty() || it->second == "dumb")
                {
                    item.status = Status::Warn;
                    item.detail = it == options.env.end() ? "TERM is not set" : std::format("TERM is '{}'", it->second);
                    item.fix = "Run learndc in a real terminal emulator, or set TERM (for example TERM=xterm-256color).";
                }
                else
                {
                    item.status = Status::Ok;
                    item.detail = it->second;
                }
                report.items.push_back(std::move(item));
            }
            {
                Item item;
                item.id = "terminal.size";
                item.label = "terminal size";
                auto const size = options.terminal ? options.terminal : platform::terminal_size();
                if (!size)
                {
                    item.status = Status::Info;
                    item.detail = "unknown (no terminal attached)";
                }
                else if (size->columns < 80 || size->rows < 24)
                {
                    item.status = Status::Warn;
                    item.detail = std::format("{}x{}; the compact layout will be used", size->columns, size->rows);
                    item.fix = "Enlarge the terminal window to at least 80x24 for the full layout.";
                }
                else
                {
                    item.status = Status::Ok;
                    item.detail = std::format("{}x{}", size->columns, size->rows);
                }
                report.items.push_back(std::move(item));
            }
            {
                Item item;
                item.id = "terminal.utf8";
                item.label = "text encoding";
                if (options.os == platform::Os::Windows || platform::utf8_locale(options.env))
                {
                    item.status = Status::Ok;
                    item.detail = "UTF-8";
                }
                else
                {
                    item.status = Status::Warn;
                    item.detail = "the locale is not UTF-8; learndc will draw with ASCII characters";
                    item.fix = "Set LANG=C.UTF-8 (or another UTF-8 locale), or set ui.ascii to on to silence this.";
                }
                report.items.push_back(std::move(item));
            }
            {
                Item item;
                item.id = "terminal.color";
                item.label = "color";
                bool const no_color = options.env.contains("NO_COLOR") && !options.env.at("NO_COLOR").empty();
                item.status = Status::Info;
                item.detail = no_color ? "NO_COLOR is set; colors are disabled" : "colors enabled when writing to a terminal";
                report.items.push_back(std::move(item));
            }
        }

        inline void check_build(DoctorReport& report, DoctorOptions const& options)
        {
            Item item;
            item.id = "build";
            item.label = "test build";
            auto const dir = options.dirs.cache_dir / "doctor" / "hello";
            auto write = [&](std::filesystem::path const& path, std::string_view data) -> bool {
                auto const existing = platform::read_file(path);
                if (existing && *existing == data)
                    return true;
                return platform::atomic_write(path, data).has_value();
            };
            if (!write(dir / "project.dcon", probe_manifest) || !write(dir / "src" / "main.dc", probe_source))
            {
                item.status = Status::Error;
                item.detail = std::format("could not write the test project under {}", platform::path_to_utf8(dir));
                item.fix = "Make the cache directory writable, or choose another with --home DIR or LEARNDC_HOME.";
                report.items.push_back(std::move(item));
                return;
            }
            runner::BuildSpec spec;
            spec.backend = options.backend;
            spec.dpm = options.tools.dpm.path;
            spec.dcc = options.tools.dcc.path;
            spec.project_dir = dir;
            spec.entry = "src/main.dc";
            spec.product = "doctor_probe";
            spec.timeout = std::chrono::seconds(options.settings.checks_build_timeout_seconds);
            spec.cancel = options.cancel;
            auto const build = runner::build_program(spec);
            if (!build.ok)
            {
                item.status = Status::Error;
                auto const output = text::sanitize(build.output.empty() ? build.error : build.output);
                item.detail = std::format("building a hello program failed:\n{}", text::clamp_bytes(text::trim(output), 1500));
                item.fix = "Check that dcc and dpm come from the same installation (tools.dcc and tools.dpm settings, LEARNDC_DCC and LEARNDC_DPM variables).";
                report.items.push_back(std::move(item));
                return;
            }
            runner::ProgramSpec run;
            run.argv = {platform::path_to_utf8(build.binary)};
            run.cwd = dir;
            run.timeout = std::chrono::seconds(10);
            run.max_output = 4096;
            run.cancel = options.cancel;
            auto const execution = runner::execute_program(run);
            if (execution.result.ok() && execution.result.out.contains("learndc doctor ok"))
            {
                item.status = Status::Ok;
                item.detail = std::format("built and ran a hello program in {} ms", execution.duration.count());
            }
            else
            {
                item.status = Status::Error;
                item.detail = std::format("the test program was built but did not run correctly (exit {}, {})", execution.result.exit_code,
                                          execution.result.error.empty() ? "no error text" : execution.result.error);
                item.fix = "Make sure the cache directory is not mounted noexec; set LEARNDC_HOME to another location if it is.";
            }
            report.items.push_back(std::move(item));
        }
    } // namespace detail

    [[nodiscard]] inline DoctorReport run_doctor(DoctorOptions const& options)
    {
        DoctorReport report;
        detail::check_tool(report, "dcc", "dcc", options.tools.dcc, "tools.dcc", "LEARNDC_DCC", true);
        detail::check_tool(report, "dpm", "dpm", options.tools.dpm, "tools.dpm", "LEARNDC_DPM", true);
        detail::check_tool(report, "dccd", "dccd", options.tools.dccd, "", "LEARNDC_DCCD", false);
        detail::check_dir(report, "dir.config", "config directory", options.dirs.config_dir);
        detail::check_dir(report, "dir.data", "data directory", options.dirs.data_dir);
        detail::check_dir(report, "dir.cache", "cache directory", options.dirs.cache_dir);
        detail::check_content(report, options.content);
        detail::check_editor(report, options);
        detail::check_terminal(report, options);
        {
            Item item;
            item.id = "safety";
            item.label = "safety";
            item.status = Status::Info;
            item.detail = std::string(runner::safety_note_text);
            report.items.push_back(std::move(item));
        }
        if (options.build)
        {
            bool const tools_ok = options.backend == runner::Backend::Dpm ? options.tools.dpm.found() : options.tools.dcc.found();
            if (tools_ok)
                detail::check_build(report, options);
            else
            {
                Item item;
                item.id = "build";
                item.label = "test build";
                item.status = Status::Error;
                item.detail = "skipped because the toolchain was not found";
                item.fix = "Fix the dcc and dpm entries above first.";
                report.items.push_back(std::move(item));
            }
        }
        return report;
    }

    [[nodiscard]] inline json::Json to_json(DoctorReport const& report)
    {
        auto j = json::Json::make_object();
        auto items = json::Json::make_array();
        for (auto const& item : report.items)
        {
            auto o = json::Json::make_object();
            o.set("id", item.id);
            o.set("label", item.label);
            o.set("status", std::string(to_string(item.status)));
            o.set("detail", item.detail);
            o.set("fix", item.fix);
            items.push(std::move(o));
        }
        j.set("items", std::move(items));
        j.set("errors", report.count(Status::Error));
        j.set("warnings", report.count(Status::Warn));
        j.set("ok", !report.has_errors());
        return j;
    }

    [[nodiscard]] inline std::string render_text(DoctorReport const& report, runner::TextStyle const& style = {})
    {
        auto paint = [&](std::string_view code, std::string_view body) -> std::string {
            if (!style.color)
                return std::string(body);
            return std::format("\x1b[{}m{}\x1b[0m", code, body);
        };
        std::string out;
        for (auto const& item : report.items)
        {
            std::string_view tag;
            std::string_view code;
            switch (item.status)
            {
                case Status::Ok:
                    tag = "ok";
                    code = "32";
                    break;
                case Status::Warn:
                    tag = "warn";
                    code = "33";
                    break;
                case Status::Error:
                    tag = "ERROR";
                    code = "1;31";
                    break;
                case Status::Info:
                    tag = "info";
                    code = "36";
                    break;
            }
            out += std::format("[{}] {}\n", paint(code, tag), item.label);
            for (auto const& line : text::split_lines(item.detail))
                out += std::format("      {}\n", line);
            if (!item.fix.empty())
                out += std::format("      {} {}\n", paint("1", "Fix:"), item.fix);
        }
        out += std::format("\n{} error{}, {} warning{}\n", report.count(Status::Error), report.count(Status::Error) == 1 ? "" : "s", report.count(Status::Warn),
                           report.count(Status::Warn) == 1 ? "" : "s");
        return out;
    }
} // namespace learndc::doctor
