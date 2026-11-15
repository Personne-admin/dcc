export module learndc.runner;

import std;
import learndc.text;
import learndc.json;
import learndc.platform;
import learndc.catalog;
import learndc.workspace;
import learndc.toolchain;
import learndc.settings;
import dcc.host.process;

export namespace learndc::runner
{
    enum class Backend
    {
        Dpm,
        DirectLlvm,
        DirectCustom,
    };

    [[nodiscard]] inline std::string_view to_string(Backend b)
    {
        switch (b)
        {
            case Backend::Dpm:
                return "dpm";
            case Backend::DirectLlvm:
                return "llvm";
            case Backend::DirectCustom:
                return "custom";
        }
        return "dpm";
    }

    [[nodiscard]] inline std::optional<Backend> parse_backend(std::string_view s)
    {
        if (s == "dpm")
            return Backend::Dpm;
        if (s == "llvm")
            return Backend::DirectLlvm;
        if (s == "custom")
            return Backend::DirectCustom;
        return std::nullopt;
    }

    enum class Verdict
    {
        Pass,
        Fail,
        Error,
        Skipped,
        Cancelled,
    };

    [[nodiscard]] inline std::string_view to_string(Verdict v)
    {
        switch (v)
        {
            case Verdict::Pass:
                return "pass";
            case Verdict::Fail:
                return "fail";
            case Verdict::Error:
                return "error";
            case Verdict::Skipped:
                return "skipped";
            case Verdict::Cancelled:
                return "cancelled";
        }
        return "error";
    }

    enum class Stage
    {
        Toolchain,
        Build,
        Link,
        Launch,
        Run,
        Compare,
        Harness,
        Timeout,
        Cancelled,
        OutputLimit,
        Busy,
        Passed,
    };

    [[nodiscard]] inline std::string_view to_string(Stage s)
    {
        switch (s)
        {
            case Stage::Toolchain:
                return "toolchain";
            case Stage::Build:
                return "build";
            case Stage::Link:
                return "link";
            case Stage::Launch:
                return "launch";
            case Stage::Run:
                return "run";
            case Stage::Compare:
                return "compare";
            case Stage::Harness:
                return "harness";
            case Stage::Timeout:
                return "timeout";
            case Stage::Cancelled:
                return "cancelled";
            case Stage::OutputLimit:
                return "output-limit";
            case Stage::Busy:
                return "busy";
            case Stage::Passed:
                return "passed";
        }
        return "passed";
    }

    enum class Outcome
    {
        Passed,
        Failed,
        Error,
        Cancelled,
    };

    [[nodiscard]] inline std::string_view to_string(Outcome o)
    {
        switch (o)
        {
            case Outcome::Passed:
                return "passed";
            case Outcome::Failed:
                return "failed";
            case Outcome::Error:
                return "error";
            case Outcome::Cancelled:
                return "cancelled";
        }
        return "error";
    }

    struct Location
    {
        std::string file;
        int line = 0;
        int column = 0;
        std::string severity;
        std::string message;
    };

    struct CheckResult
    {
        std::string name;
        catalog::CheckKind kind = catalog::CheckKind::Build;
        Verdict verdict = Verdict::Skipped;
        Stage stage = Stage::Passed;
        std::string summary;
        std::string expected;
        std::string observed;
        std::string expected_visible;
        std::string observed_visible;
        std::optional<int> exit_code;
        std::optional<int> signal;
        bool timed_out = false;
        std::int64_t duration_ms = 0;
        std::string stdout_preview;
        std::string stderr_preview;
        std::string diagnostics;
        std::vector<Location> locations;
        std::string hint;
    };

    struct CheckReport
    {
        std::string exercise;
        int revision = 1;
        std::string workspace;
        std::string backend;
        std::string started_at;
        std::int64_t duration_ms = 0;
        int passed = 0;
        int total = 0;
        Outcome outcome = Outcome::Error;
        std::string first_failure_stage;
        std::vector<CheckResult> results;
        std::string log_path;
        std::string safety_note;
    };

    struct TextStyle
    {
        bool color = false;
        bool ascii = false;
        bool verbose = false;
    };

    struct RunnerOptions
    {
        platform::Dirs dirs;
        settings::Settings settings;
        toolchain::Toolchain tools;
        Backend backend = Backend::Dpm;
        std::atomic<bool> const* cancel = nullptr;
        std::function<void(std::string_view)> on_stage;
        bool include_safety_note = false;
        std::optional<std::chrono::seconds> interactive_timeout;
    };

    struct ProgramSpec
    {
        std::vector<std::string> argv;
        std::filesystem::path cwd;
        std::filesystem::path stdin_file;
        std::chrono::milliseconds timeout{10000};
        std::size_t max_output = 262144;
        std::atomic<bool> const* cancel = nullptr;
        std::map<std::string, std::string> env;
    };

    struct Execution
    {
        dcc::host::process::Result result;
        std::chrono::milliseconds duration{};
        std::chrono::milliseconds timeout{};
        std::size_t max_output = 0;
        bool cancelled = false;
    };

    struct BuildSpec
    {
        Backend backend = Backend::Dpm;
        std::optional<std::filesystem::path> dpm;
        std::optional<std::filesystem::path> dcc;
        std::filesystem::path project_dir;
        std::string entry;
        std::string product;
        std::chrono::seconds timeout{120};
        std::atomic<bool> const* cancel = nullptr;
        std::size_t max_capture = 4u << 20;
    };

    struct BuildOutcome
    {
        bool started = false;
        bool ok = false;
        bool timed_out = false;
        bool cancelled = false;
        bool truncated = false;
        int signal = 0;
        int exit_code = -1;
        std::string output;
        std::string error;
        std::filesystem::path binary;
    };

    struct RunOutcome
    {
        bool ok = false;
        Stage stage = Stage::Passed;
        std::optional<int> exit_code;
        std::optional<int> signal;
        bool cancelled = false;
        bool timed_out = false;
        std::string message;
        std::string diagnostics;
        std::vector<Location> locations;
    };

    inline constexpr std::string_view safety_note_text =
        "learndc compiles and runs your code with your own permissions. Time limits, output limits and process cleanup stop runaway programs, "
        "but they are not a sandbox: only run exercises and code you trust.";

    namespace detail
    {
        inline constexpr std::size_t preview_limit = 4096;
        inline constexpr std::size_t diagnostics_limit = 65536;

        [[nodiscard]] inline std::string slug_of(std::string_view id)
        {
            return text::replace_all(id, "/", "-");
        }

        [[nodiscard]] inline std::string exe_name(std::string_view base)
        {
            return platform::host_os() == platform::Os::Windows ? std::format("{}.exe", base) : std::string(base);
        }

        [[nodiscard]] inline std::string signal_name(int sig)
        {
            switch (sig)
            {
                case 1:
                    return "SIGHUP";
                case 2:
                    return "SIGINT";
                case 4:
                    return "SIGILL";
                case 6:
                    return "SIGABRT";
                case 7:
                    return "SIGBUS";
                case 8:
                    return "SIGFPE";
                case 9:
                    return "SIGKILL";
                case 11:
                    return "SIGSEGV";
                case 13:
                    return "SIGPIPE";
                case 15:
                    return "SIGTERM";
                default:
                    return std::format("signal {}", sig);
            }
        }

        [[nodiscard]] inline std::string exit_note(int code)
        {
            auto const raw = static_cast<std::uint32_t>(code);
            if (raw == 0xC0000005u)
                return " (access violation)";
            if (raw == 0xC00000FDu)
                return " (stack overflow)";
            if (raw == 0xC0000094u)
                return " (integer division by zero)";
            if (raw >= 0xC0000000u)
                return " (Windows exception)";
            return {};
        }

        [[nodiscard]] inline bool is_cancelled(std::atomic<bool> const* cancel)
        {
            return cancel && cancel->load(std::memory_order_relaxed);
        }

        [[nodiscard]] inline std::string preview_of(std::string_view raw)
        {
            return text::sanitize(text::clamp_bytes(raw, preview_limit));
        }

        [[nodiscard]] inline bool whitespace_only_difference(std::string_view a, std::string_view b)
        {
            if (a == b)
                return false;
            auto strip = [](std::string_view s) {
                std::string out;
                for (char c : s)
                    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                        out += c;
                return out;
            };
            return strip(a) == strip(b);
        }

        [[nodiscard]] inline std::string visible_text(std::string_view raw)
        {
            return text::sanitize(text::clamp_bytes(text::visible_whitespace(raw), preview_limit * 2));
        }

        struct Rewrites
        {
            std::vector<std::pair<std::string, std::string>> pairs;

            void add(std::filesystem::path const& from, std::string_view to)
            {
                std::vector<std::string> variants;
                auto push = [&](std::filesystem::path const& p) {
                    auto text_form = platform::path_to_utf8(p);
                    while (text_form.size() > 1 && (text_form.back() == '/' || text_form.back() == '\\'))
                        text_form.pop_back();
                    if (text_form.empty())
                        return;
                    if (std::ranges::find(variants, text_form) == variants.end())
                        variants.push_back(text_form);
                    auto generic = text::replace_all(text_form, "\\", "/");
                    if (std::ranges::find(variants, generic) == variants.end())
                        variants.push_back(generic);
                };
                push(from);
                push(from.lexically_normal());
                std::error_code ec;
                auto canonical = std::filesystem::weakly_canonical(from, ec);
                if (!ec)
                    push(canonical);
                for (auto const& v : variants)
                    pairs.emplace_back(v, std::string(to));
                std::ranges::stable_sort(pairs, [](auto const& a, auto const& b) { return a.first.size() > b.first.size(); });
            }

            [[nodiscard]] std::string apply(std::string_view in) const
            {
                std::string out(in);
                for (auto const& [from, to] : pairs)
                    out = text::replace_all(out, from, to);
                return out;
            }
        };

        [[nodiscard]] inline std::string clean_diagnostics(std::string_view raw, Rewrites const& rewrites)
        {
            auto const sanitized = text::sanitize(raw);
            std::string out;
            for (auto const& line : text::split_lines(sanitized))
            {
                auto const trimmed = text::trim(line);
                if (trimmed.starts_with("Building ") && trimmed.ends_with(")"))
                    continue;
                out += line;
                out += '\n';
            }
            out = rewrites.apply(out);
            auto const trimmed = text::trim(out);
            return text::clamp_bytes(trimmed, diagnostics_limit);
        }

        [[nodiscard]] inline bool is_source_excerpt_line(std::string_view line)
        {
            auto const t = text::trim(line);
            std::size_t i = 0;
            while (i < t.size() && t[i] >= '0' && t[i] <= '9')
                ++i;
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t'))
                ++i;
            return i < t.size() && t[i] == '|';
        }

        [[nodiscard]] inline bool is_link_failure(std::string_view diagnostics)
        {
            for (auto const& line : text::split_lines(diagnostics))
            {
                if (is_source_excerpt_line(line))
                    continue;
                if (line.contains("undefined symbol") || line.contains("ld.lld") || line.contains("lld-link") || line.contains("linker command failed"))
                    return true;
            }
            return false;
        }

        [[nodiscard]] inline int count_errors(std::string_view diagnostics)
        {
            int count = 0;
            for (auto const& line : text::split_lines(diagnostics))
            {
                auto const t = text::trim(line);
                if (t.starts_with("error:") || t.starts_with("error["))
                    ++count;
            }
            return count;
        }

        [[nodiscard]] inline std::optional<int> parse_int(std::string_view s)
        {
            if (s.empty())
                return std::nullopt;
            int value = 0;
            auto const [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
            if (ec != std::errc() || ptr != s.data() + s.size())
                return std::nullopt;
            return value;
        }

        [[nodiscard]] inline std::expected<void, std::string> sync_tree(std::filesystem::path const& dest, std::map<std::string, std::string> const& desired,
                                                                        std::set<std::string> const& keep)
        {
            std::error_code ec;
            std::filesystem::create_directories(dest, ec);
            if (ec)
                return std::unexpected(std::format("cannot create {}: {}", platform::path_to_utf8(dest), ec.message()));
            for (auto const& [rel, bytes] : desired)
            {
                auto const target = dest / platform::path_from_utf8(rel);
                auto const existing = platform::read_file(target);
                if (existing && *existing == bytes)
                    continue;
                std::filesystem::create_directories(target.parent_path(), ec);
                if (ec)
                    return std::unexpected(std::format("cannot create {}: {}", platform::path_to_utf8(target.parent_path()), ec.message()));
                if (std::filesystem::is_symlink(target, ec))
                    std::filesystem::remove(target, ec);
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.flush();
                if (!out)
                    return std::unexpected(std::format("cannot write {}", platform::path_to_utf8(target)));
            }
            std::vector<std::filesystem::path> stale;
            std::filesystem::recursive_directory_iterator it(dest, std::filesystem::directory_options::skip_permission_denied, ec);
            std::filesystem::recursive_directory_iterator const end;
            for (; !ec && it != end; it.increment(ec))
            {
                auto const rel_path = std::filesystem::relative(it->path(), dest, ec);
                if (ec)
                    break;
                auto const generic_u8 = rel_path.generic_u8string();
                std::string const generic(reinterpret_cast<char const*>(generic_u8.data()), generic_u8.size());
                if (it.depth() == 0 && keep.contains(generic))
                {
                    if (it->is_directory(ec))
                        it.disable_recursion_pending();
                    continue;
                }
                auto const status = it->symlink_status(ec);
                if (ec)
                    break;
                if (status.type() == std::filesystem::file_type::directory)
                    continue;
                if (!desired.contains(text::replace_all(generic, "\\", "/")))
                    stale.push_back(it->path());
            }
            for (auto const& path : stale)
                std::filesystem::remove(path, ec);
            return {};
        }

        [[nodiscard]] inline std::map<std::string, std::string> read_workspace(std::filesystem::path const& root, std::string_view prefix = {})
        {
            workspace::Workspace ws;
            ws.root = root;
            std::map<std::string, std::string> out;
            for (auto const& rel : workspace::files(ws))
            {
                if (!prefix.empty() && !rel.starts_with(prefix))
                    continue;
                if (auto bytes = platform::read_file(root / platform::path_from_utf8(rel)))
                    out.emplace(rel, std::move(*bytes));
            }
            return out;
        }

        [[nodiscard]] inline std::string harness_manifest(std::string_view stem)
        {
            return std::format("dpm::v1::Project {{\n    name = \"learndc_check\",\n    version = \"0.1.0\",\n    products = {{\n        {{ name = "
                               "\"check_{0}\", entry = \"tests/{0}.dc\" }},\n    }},\n}}\n",
                               stem);
        }

        [[nodiscard]] inline std::string timeout_text(std::chrono::milliseconds ms)
        {
            auto const seconds = ms.count() / 1000;
            if (ms.count() % 1000 == 0)
                return std::format("{} s", seconds);
            return std::format("{:.1f} s", static_cast<double>(ms.count()) / 1000.0);
        }
    } // namespace detail

    namespace detail
    {
        struct Header
        {
            std::string severity;
            std::string message;
        };

        [[nodiscard]] inline std::optional<Header> parse_header(std::string_view line)
        {
            auto const header = text::trim(line);
            for (std::string_view const sev : {"error", "warning", "note"})
            {
                if (!header.starts_with(sev))
                    continue;
                auto after = header.substr(sev.size());
                if (after.starts_with("["))
                {
                    auto close = after.find(']');
                    if (close == std::string_view::npos)
                        continue;
                    after = after.substr(close + 1);
                }
                if (!after.starts_with(":"))
                    continue;
                return Header{std::string(sev), std::string(text::trim(after.substr(1)))};
            }
            return std::nullopt;
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<Location> parse_locations(std::string_view diagnostics)
    {
        std::vector<Location> out;
        auto const lines = text::split_lines(diagnostics);
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            auto const header = detail::parse_header(lines[i]);
            if (!header)
                continue;
            for (std::size_t j = i + 1; j < lines.size() && j <= i + 2; ++j)
            {
                if (detail::parse_header(lines[j]))
                    break;
                auto const pointer = text::trim(lines[j]);
                if (!pointer.starts_with("--> "))
                    continue;
                auto const spec = text::trim(pointer.substr(4));
                auto const last = spec.rfind(':');
                if (last == std::string_view::npos || last == 0)
                    break;
                auto const prev = spec.substr(0, last).rfind(':');
                if (prev == std::string_view::npos || prev == 0)
                    break;
                auto const col = detail::parse_int(spec.substr(last + 1));
                auto const line = detail::parse_int(spec.substr(prev + 1, last - prev - 1));
                if (!col || !line)
                    break;
                Location loc;
                loc.file = std::string(spec.substr(0, prev));
                loc.line = *line;
                loc.column = *col;
                loc.severity = header->severity;
                loc.message = header->message;
                out.push_back(std::move(loc));
                break;
            }
        }
        return out;
    }

    [[nodiscard]] inline bool output_matches(std::string_view expected, std::string_view observed, catalog::StdoutMatch match, catalog::LineEndings endings)
    {
        std::string e(expected);
        std::string o(observed);
        if (endings == catalog::LineEndings::Portable)
        {
            e = text::collapse_crlf(e);
            o = text::collapse_crlf(o);
        }
        switch (match)
        {
            case catalog::StdoutMatch::Exact:
                return e == o;
            case catalog::StdoutMatch::Trimmed:
                return text::trim(e) == text::trim(o);
            case catalog::StdoutMatch::Contains:
                return o.find(e) != std::string::npos;
        }
        return false;
    }

    [[nodiscard]] inline Execution execute_program(ProgramSpec const& spec)
    {
        dcc::host::process::Command command;
        command.argv = spec.argv;
        command.cwd = spec.cwd;
        command.stdin_file = spec.stdin_file;
        command.timeout = spec.timeout;
        command.max_capture = spec.max_output;
        command.max_error_capture = spec.max_output;
        command.stop_on_error_overflow = true;
        command.cancel = spec.cancel;
        command.inherit_env = true;
        command.env = spec.env;
        command.grace = std::chrono::milliseconds(1000);
        Execution ex;
        ex.timeout = spec.timeout;
        ex.max_output = spec.max_output;
        auto const begin = std::chrono::steady_clock::now();
        ex.result = dcc::host::process::run(command);
        ex.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
        ex.cancelled = detail::is_cancelled(spec.cancel) && (ex.result.aborted || !ex.result.started);
        return ex;
    }

    [[nodiscard]] inline CheckResult judge_run(catalog::Check const& check, Execution const& ex)
    {
        CheckResult r;
        r.name = check.name;
        r.kind = check.kind;
        r.duration_ms = ex.duration.count();
        r.hint = check.hint;
        auto const& res = ex.result;
        r.stdout_preview = detail::preview_of(res.out);
        r.stderr_preview = detail::preview_of(res.err);
        r.timed_out = res.timed_out;
        if (res.signal != 0)
            r.signal = res.signal;
        if (res.started && res.signal == 0 && !res.timed_out && !res.aborted)
            r.exit_code = res.exit_code;
        if (!res.started)
        {
            r.verdict = ex.cancelled ? Verdict::Cancelled : Verdict::Error;
            r.stage = ex.cancelled ? Stage::Cancelled : Stage::Launch;
            r.summary = std::format("the program could not be started: {}", res.error.empty() ? "unknown error" : res.error);
            r.hint.clear();
            return r;
        }
        if (ex.cancelled)
        {
            r.verdict = Verdict::Cancelled;
            r.stage = Stage::Cancelled;
            r.summary = "cancelled while the program was running";
            r.hint.clear();
            return r;
        }
        if (res.timed_out)
        {
            r.verdict = Verdict::Fail;
            r.stage = Stage::Timeout;
            r.summary =
                std::format("the program did not finish within {}; look for a loop that never ends or input it waits for", detail::timeout_text(ex.timeout));
            return r;
        }
        if (res.truncated)
        {
            r.verdict = Verdict::Fail;
            r.stage = Stage::OutputLimit;
            r.summary = std::format("the program printed more than {} KiB; it was stopped", ex.max_output / 1024);
            return r;
        }
        if (res.signal != 0)
        {
            r.verdict = Verdict::Fail;
            r.stage = Stage::Run;
            r.summary = std::format("the program was stopped by {} (it crashed)", detail::signal_name(res.signal));
            return r;
        }
        bool const exit_ok = res.exit_code == check.expect_exit;
        bool output_ok = true;
        if (check.expect_stdout)
            output_ok = output_matches(*check.expect_stdout, res.out, check.stdout_match, check.line_endings);
        bool stderr_ok = true;
        if (check.expect_stderr_contains && !check.expect_stderr_contains->empty())
            stderr_ok = res.err.find(*check.expect_stderr_contains) != std::string::npos;
        if (exit_ok && output_ok && stderr_ok)
        {
            r.verdict = Verdict::Pass;
            r.stage = Stage::Passed;
            r.summary = "passed";
            r.hint.clear();
            return r;
        }
        r.verdict = Verdict::Fail;
        if (!exit_ok)
        {
            r.stage = Stage::Run;
            r.summary =
                std::format("the program exited with code {}{} but code {} was expected", res.exit_code, detail::exit_note(res.exit_code), check.expect_exit);
            return r;
        }
        r.stage = Stage::Compare;
        if (!output_ok)
        {
            std::string const& expected = *check.expect_stdout;
            r.expected = text::sanitize(text::clamp_bytes(expected, detail::preview_limit));
            r.observed = text::sanitize(text::clamp_bytes(res.out, detail::preview_limit));
            bool const contains = check.stdout_match == catalog::StdoutMatch::Contains;
            bool const ws_only = detail::whitespace_only_difference(expected, res.out);
            if (ws_only && !contains)
            {
                r.expected_visible = detail::visible_text(expected);
                r.observed_visible = detail::visible_text(res.out);
                r.summary = "the output has the right text but differs in whitespace or line endings (shown visibly below)";
            }
            else if (contains)
                r.summary = "the output does not contain the expected text";
            else
                r.summary = "the output is different from what was expected";
            return r;
        }
        r.expected = *check.expect_stderr_contains;
        r.observed = r.stderr_preview;
        r.summary = "the error output does not contain the expected text";
        return r;
    }

    [[nodiscard]] inline BuildOutcome build_program(BuildSpec const& spec)
    {
        BuildOutcome outcome;
        dcc::host::process::Command command;
        command.cwd = spec.project_dir;
        command.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(spec.timeout);
        command.max_capture = spec.max_capture;
        command.max_error_capture = spec.max_capture;
        command.cancel = spec.cancel;
        command.inherit_env = true;
        command.grace = std::chrono::milliseconds(1000);
        if (spec.backend == Backend::Dpm)
        {
            if (!spec.dpm)
            {
                outcome.error = "dpm was not found";
                return outcome;
            }
            command.argv = {platform::path_to_utf8(*spec.dpm), "build", "--offline"};
            if (spec.dcc)
                command.env["DCC"] = platform::path_to_utf8(*spec.dcc);
            outcome.binary = spec.project_dir / ".dpm" / "out" / "debug" / "host" / platform::path_from_utf8(detail::exe_name(spec.product));
            std::error_code ec;
            std::filesystem::remove(outcome.binary, ec);
        }
        else
        {
            if (!spec.dcc)
            {
                outcome.error = "dcc was not found";
                return outcome;
            }
            auto const dir = std::format(".dpm/out/direct-{}", to_string(spec.backend));
            std::error_code ec;
            std::filesystem::create_directories(spec.project_dir / platform::path_from_utf8(dir), ec);
            auto const out_rel = std::format("{}/{}", dir, detail::exe_name(spec.product));
            command.argv = {platform::path_to_utf8(*spec.dcc), spec.entry, "-o", out_rel, "-Isrc", "-O0", "-fwhole-program", "-flibdcext"};
            if (spec.backend == Backend::DirectLlvm)
                command.argv.push_back("-g");
            command.argv.push_back("-fbounds-check");
            command.argv.push_back(spec.backend == Backend::DirectLlvm ? "-fbackend=llvm" : "-fbackend=custom");
            outcome.binary = spec.project_dir / platform::path_from_utf8(out_rel);
            std::filesystem::remove(outcome.binary, ec);
        }
        auto const result = dcc::host::process::run(command);
        outcome.started = result.started;
        outcome.exit_code = result.exit_code;
        outcome.timed_out = result.timed_out;
        outcome.truncated = result.truncated;
        outcome.signal = result.signal;
        outcome.cancelled = detail::is_cancelled(spec.cancel) && result.aborted;
        outcome.output = result.out;
        if (!result.err.empty())
        {
            if (!outcome.output.empty() && !outcome.output.ends_with('\n'))
                outcome.output += '\n';
            outcome.output += result.err;
        }
        outcome.error = result.error;
        outcome.ok = result.started && result.exit_code == 0 && result.signal == 0 && !result.timed_out && !result.aborted && !result.truncated &&
                     std::filesystem::exists(outcome.binary);
        if (result.started && result.exit_code == 0 && !outcome.ok && outcome.error.empty() && !result.timed_out && !result.aborted)
            outcome.error = std::format("the build finished but produced no program at {}", platform::path_to_utf8(outcome.binary));
        return outcome;
    }

    [[nodiscard]] inline std::filesystem::path lock_path(platform::Dirs const& dirs, catalog::Exercise const& ex)
    {
        return dirs.staging_dir / platform::path_from_utf8(detail::slug_of(ex.id) + ".lock");
    }

    namespace detail
    {
        struct Session
        {
            catalog::Exercise const& ex;
            std::filesystem::path ws_root;
            RunnerOptions const& opt;
            std::filesystem::path stage_root;
            std::filesystem::path project_dir;
            std::filesystem::path harness_dir;
            std::filesystem::path stdin_path;
            Rewrites rewrites;
            std::string log;
            std::optional<BuildOutcome> project_build;
            std::map<std::string, BuildOutcome> harness_builds;

            Session(catalog::Exercise const& e, std::filesystem::path root, RunnerOptions const& o) : ex(e), ws_root(std::move(root)), opt(o)
            {
                stage_root = opt.dirs.staging_dir / platform::path_from_utf8(slug_of(ex.id));
                project_dir = stage_root / "project";
                harness_dir = stage_root / "harness";
                stdin_path = stage_root / "stdin.txt";
                auto const root_text = platform::path_to_utf8(ws_root);
                rewrites.add(harness_dir / "tests", "checks");
                rewrites.add(harness_dir / "src", root_text + "/src");
                rewrites.add(project_dir, root_text);
                rewrites.add(harness_dir, root_text);
            }

            void stage(std::string_view message) const
            {
                if (opt.on_stage)
                    opt.on_stage(message);
            }

            [[nodiscard]] bool cancelled() const { return is_cancelled(opt.cancel); }

            [[nodiscard]] std::size_t max_output() const { return static_cast<std::size_t>(std::max(1, opt.settings.checks_max_output_kib)) * 1024u; }

            [[nodiscard]] std::chrono::seconds build_timeout() const
            {
                return std::chrono::seconds(ex.build_timeout_seconds.value_or(opt.settings.checks_build_timeout_seconds));
            }

            [[nodiscard]] std::chrono::milliseconds run_timeout(catalog::Check const& check) const
            {
                int const seconds = std::max(1, std::min(check.max_seconds, opt.settings.checks_timeout_seconds));
                return std::chrono::milliseconds(seconds * 1000);
            }

            void note(std::string_view title, std::string_view body)
            {
                log += std::format("==== {} ====\n", title);
                log += text::clamp_bytes(body, max_output());
                if (!log.ends_with('\n'))
                    log += '\n';
            }

            [[nodiscard]] std::optional<std::string> missing_tool() const
            {
                if (opt.backend == Backend::Dpm)
                {
                    if (!opt.tools.dpm.found())
                        return std::format("dpm was not found. Searched:\n{}\nSet tools.dpm in the learndc settings or the LEARNDC_DPM environment variable, "
                                           "or put dpm on your PATH.",
                                           opt.tools.dpm.searched_text());
                    return std::nullopt;
                }
                if (!opt.tools.dcc.found())
                    return std::format("dcc was not found. Searched:\n{}\nSet tools.dcc in the learndc settings or the LEARNDC_DCC environment variable, or "
                                       "put dcc on your PATH.",
                                       opt.tools.dcc.searched_text());
                return std::nullopt;
            }

            [[nodiscard]] BuildSpec spec_for(std::filesystem::path const& dir, std::string_view entry, std::string_view product) const
            {
                BuildSpec spec;
                spec.backend = opt.backend;
                spec.dpm = opt.tools.dpm.path;
                spec.dcc = opt.tools.dcc.path;
                spec.project_dir = dir;
                spec.entry = std::string(entry);
                spec.product = std::string(product);
                spec.timeout = build_timeout();
                spec.cancel = opt.cancel;
                return spec;
            }

            [[nodiscard]] std::expected<void, std::string> sync_project()
            {
                auto files = read_workspace(ws_root);
                return sync_tree(project_dir, files, {".dpm", "project.lock.dcon", "compile_commands.json"});
            }

            [[nodiscard]] std::expected<void, std::string> sync_harness(std::string const& stem, std::string const& source)
            {
                auto files = read_workspace(ws_root, "src/");
                files.emplace(std::format("tests/{}.dc", stem), source);
                files.emplace("project.dcon", harness_manifest(stem));
                return sync_tree(harness_dir, files, {".dpm", "project.lock.dcon", "compile_commands.json"});
            }

            BuildOutcome& build_project()
            {
                if (!project_build)
                {
                    stage("Building");
                    auto outcome = build_program(spec_for(project_dir, ex.entry_file, ex.product));
                    note("build", outcome.output);
                    project_build = std::move(outcome);
                }
                return *project_build;
            }

            BuildOutcome& build_harness(std::string const& stem)
            {
                auto it = harness_builds.find(stem);
                if (it != harness_builds.end())
                    return it->second;
                stage(std::format("Building checks program {}", stem));
                auto outcome = build_program(spec_for(harness_dir, std::format("tests/{}.dc", stem), std::format("check_{}", stem)));
                note(std::format("build harness {}", stem), outcome.output);
                return harness_builds.emplace(stem, std::move(outcome)).first->second;
            }
        };

        [[nodiscard]] inline CheckResult skipped(catalog::Check const& check, std::string_view why)
        {
            CheckResult r;
            r.name = check.name;
            r.kind = check.kind;
            r.verdict = Verdict::Skipped;
            r.stage = Stage::Passed;
            r.summary = std::string(why);
            return r;
        }

        [[nodiscard]] inline CheckResult cancelled_result(catalog::Check const& check)
        {
            CheckResult r;
            r.name = check.name;
            r.kind = check.kind;
            r.verdict = Verdict::Cancelled;
            r.stage = Stage::Cancelled;
            r.summary = "cancelled before this check ran";
            return r;
        }

        [[nodiscard]] inline CheckResult build_failure(catalog::Check const& check, BuildOutcome const& build, Rewrites const& rewrites, std::string_view what)
        {
            CheckResult r;
            r.name = check.name;
            r.kind = check.kind;
            r.hint = check.hint;
            auto const combined = build.output.empty() ? build.error : build.output;
            r.diagnostics = clean_diagnostics(combined, rewrites);
            r.locations = parse_locations(r.diagnostics);
            if (build.cancelled)
            {
                r.verdict = Verdict::Cancelled;
                r.stage = Stage::Cancelled;
                r.summary = std::format("cancelled while {} was building", what);
                r.hint.clear();
                return r;
            }
            if (!build.started)
            {
                r.verdict = Verdict::Error;
                r.stage = Stage::Toolchain;
                r.summary = std::format("the build tool could not be started: {}", build.error.empty() ? "unknown error" : build.error);
                r.hint.clear();
                return r;
            }
            r.verdict = Verdict::Fail;
            if (build.timed_out)
            {
                r.stage = Stage::Timeout;
                r.summary = std::format("{} did not finish building in time", what);
                return r;
            }
            if (build.signal != 0)
            {
                r.verdict = Verdict::Error;
                r.stage = Stage::Toolchain;
                r.hint.clear();
                r.summary = std::format("the build tool was stopped by {} (it crashed)", signal_name(build.signal));
                return r;
            }
            if (build.truncated)
            {
                r.verdict = Verdict::Error;
                r.stage = Stage::OutputLimit;
                r.hint.clear();
                r.summary = "the build printed more output than learndc keeps";
                return r;
            }
            if (is_link_failure(r.diagnostics))
            {
                r.stage = Stage::Link;
                r.summary = std::format("{} compiled but could not be linked", what);
                return r;
            }
            r.stage = Stage::Build;
            if (build.exit_code == 0)
            {
                r.verdict = Verdict::Error;
                r.hint.clear();
                r.summary = build.error.empty() ? std::string("the build produced no program") : build.error;
                return r;
            }
            int const errors = count_errors(r.diagnostics);
            if (errors > 0)
                r.summary = std::format("{} did not compile: {} {}", what, errors, errors == 1 ? "error" : "errors");
            else
                r.summary = std::format("{} did not compile", what);
            return r;
        }

        [[nodiscard]] inline std::string describe_log(std::string_view title, std::vector<std::string> const& argv)
        {
            std::string line = std::format("$ {}", title);
            for (auto const& a : argv)
                line += " " + a;
            return line;
        }

        inline CheckResult run_one(Session& s, catalog::Check const& check)
        {
            CheckResult r;
            r.name = check.name;
            r.kind = check.kind;
            auto const started = std::chrono::steady_clock::now();
            auto finish = [&](CheckResult result) {
                if (result.duration_ms == 0)
                    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                return result;
            };
            switch (check.kind)
            {
                case catalog::CheckKind::Build: {
                    auto& build = s.build_project();
                    if (build.ok)
                    {
                        r.verdict = Verdict::Pass;
                        r.stage = Stage::Passed;
                        r.summary = "the program compiled";
                        r.diagnostics = clean_diagnostics(build.output, s.rewrites);
                        r.locations = parse_locations(r.diagnostics);
                        return finish(std::move(r));
                    }
                    auto result = build_failure(check, build, s.rewrites, "your program");
                    return finish(std::move(result));
                }
                case catalog::CheckKind::Diagnostic: {
                    auto& build = s.build_project();
                    if (build.cancelled)
                        return finish(build_failure(check, build, s.rewrites, "your program"));
                    if (!build.started)
                        return finish(build_failure(check, build, s.rewrites, "your program"));
                    r.diagnostics = clean_diagnostics(build.output.empty() ? build.error : build.output, s.rewrites);
                    r.locations = parse_locations(r.diagnostics);
                    if (build.timed_out || build.signal != 0 || build.truncated || (build.exit_code == 0 && !build.ok))
                        return finish(build_failure(check, build, s.rewrites, "your program"));
                    bool const succeeded = build.ok;
                    r.hint = check.hint;
                    if (succeeded != check.expect_build_success)
                    {
                        r.verdict = Verdict::Fail;
                        r.stage = Stage::Build;
                        r.summary = check.expect_build_success ? "the program should compile but it does not"
                                                               : "the program should be rejected by the compiler but it compiled";
                        return finish(std::move(r));
                    }
                    if (!check.diagnostic_contains.empty() && r.diagnostics.find(check.diagnostic_contains) == std::string::npos)
                    {
                        r.verdict = Verdict::Fail;
                        r.stage = Stage::Compare;
                        r.summary = "the compiler messages do not mention the expected text";
                        r.expected = check.diagnostic_contains;
                        r.observed = text::clamp_bytes(r.diagnostics, preview_limit);
                        return finish(std::move(r));
                    }
                    r.verdict = Verdict::Pass;
                    r.stage = Stage::Passed;
                    r.summary = "the compiler behaved as expected";
                    r.hint.clear();
                    return finish(std::move(r));
                }
                case catalog::CheckKind::Run:
                case catalog::CheckKind::Harness: {
                    bool const harness = check.kind == catalog::CheckKind::Harness;
                    auto& project = s.build_project();
                    if (!project.ok)
                    {
                        if (project.cancelled)
                            return finish(cancelled_result(check));
                        return finish(skipped(check, "not run because the program did not build"));
                    }
                    std::filesystem::path binary = project.binary;
                    std::filesystem::path cwd = s.project_dir;
                    if (harness)
                    {
                        auto const stem = check.harness_stem();
                        auto const key = std::format("checks/{}.dc", stem);
                        auto const source = s.ex.check_files.find(key);
                        if (stem.empty() || source == s.ex.check_files.end())
                        {
                            r.verdict = Verdict::Error;
                            r.stage = Stage::Harness;
                            r.summary = std::format("the checks program {} is missing from the exercise content", check.harness);
                            return finish(std::move(r));
                        }
                        if (!s.harness_builds.contains(stem))
                        {
                            s.stage("Preparing checks program");
                            if (auto synced = s.sync_harness(stem, source->second); !synced)
                            {
                                r.verdict = Verdict::Error;
                                r.stage = Stage::Harness;
                                r.summary = std::format("could not prepare the checks program: {}", synced.error());
                                return finish(std::move(r));
                            }
                        }
                        auto& hb = s.build_harness(stem);
                        if (!hb.ok)
                        {
                            auto result = build_failure(check, hb, s.rewrites, "the checks program");
                            if (result.verdict == Verdict::Fail)
                                result.summary = "the checks program could not be built against your code: " + result.summary;
                            return finish(std::move(result));
                        }
                        binary = hb.binary;
                        cwd = s.harness_dir;
                    }
                    if (s.cancelled())
                        return finish(cancelled_result(check));
                    ProgramSpec spec;
                    spec.argv.push_back(platform::path_to_utf8(binary));
                    for (auto const& a : check.args)
                        spec.argv.push_back(a);
                    spec.cwd = cwd;
                    spec.timeout = s.run_timeout(check);
                    spec.max_output = s.max_output();
                    spec.cancel = s.opt.cancel;
                    if (!check.stdin_text.empty())
                    {
                        std::ofstream in(s.stdin_path, std::ios::binary | std::ios::trunc);
                        in.write(check.stdin_text.data(), static_cast<std::streamsize>(check.stdin_text.size()));
                        in.flush();
                        if (in)
                            spec.stdin_file = s.stdin_path;
                    }
                    s.stage(std::format("Running {}", check.name));
                    auto execution = execute_program(spec);
                    s.note(std::format("run {}", check.name),
                           std::format("{}\nexit={} signal={} timed_out={}\n--- stdout ---\n{}\n--- stderr ---\n{}", describe_log("run", spec.argv),
                                       execution.result.exit_code, execution.result.signal, execution.result.timed_out,
                                       text::clamp_bytes(execution.result.out, s.max_output()), text::clamp_bytes(execution.result.err, s.max_output())));
                    auto result = judge_run(check, execution);
                    if (harness && (result.stage == Stage::Run || result.stage == Stage::Compare) && result.verdict == Verdict::Fail)
                        result.stage = Stage::Harness;
                    return result;
                }
            }
            return finish(std::move(r));
        }

        inline void finalize(CheckReport& report, std::chrono::steady_clock::time_point begin)
        {
            report.passed = 0;
            bool any_error = false;
            bool any_cancel = false;
            for (auto const& r : report.results)
            {
                if (r.verdict == Verdict::Pass)
                    ++report.passed;
                if (r.verdict == Verdict::Error)
                    any_error = true;
                if (r.verdict == Verdict::Cancelled)
                    any_cancel = true;
                if (report.first_failure_stage.empty() && r.verdict != Verdict::Pass && r.verdict != Verdict::Skipped)
                    report.first_failure_stage = std::string(to_string(r.stage));
            }
            report.total = static_cast<int>(report.results.size());
            if (any_cancel)
                report.outcome = Outcome::Cancelled;
            else if (any_error)
                report.outcome = Outcome::Error;
            else if (report.passed == report.total && report.total > 0)
                report.outcome = Outcome::Passed;
            else
                report.outcome = Outcome::Failed;
            report.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
        }
    } // namespace detail

    [[nodiscard]] inline CheckReport run_checks(catalog::Exercise const& ex, std::filesystem::path const& source_root, RunnerOptions const& opt)
    {
        auto const begin = std::chrono::steady_clock::now();
        CheckReport report;
        report.exercise = ex.id;
        report.revision = ex.revision;
        report.workspace = platform::path_to_utf8(source_root);
        report.backend = std::string(to_string(opt.backend));
        report.started_at = platform::now_utc_iso8601();
        if (opt.include_safety_note)
            report.safety_note = std::string(safety_note_text);

        auto fail_all = [&](Stage stage, Verdict verdict, std::string const& message) {
            bool first = true;
            for (auto const& check : ex.checks)
            {
                if (first)
                {
                    CheckResult r;
                    r.name = check.name;
                    r.kind = check.kind;
                    r.verdict = verdict;
                    r.stage = stage;
                    r.summary = message;
                    report.results.push_back(std::move(r));
                    first = false;
                }
                else
                    report.results.push_back(detail::skipped(check, "not run because the first check could not start"));
            }
            detail::finalize(report, begin);
        };

        detail::Session s(ex, source_root, opt);

        std::error_code ec;
        std::filesystem::create_directories(opt.dirs.staging_dir, ec);
        auto lock = platform::FileLock::try_acquire(lock_path(opt.dirs, ex));
        if (!lock)
        {
            fail_all(Stage::Busy, Verdict::Error,
                     lock.error().busy ? "another check of this exercise is already running; wait for it to finish"
                                       : "cannot take the check lock: " + lock.error().message);
            return report;
        }

        if (auto message = s.missing_tool())
        {
            fail_all(Stage::Toolchain, Verdict::Error, *message);
            return report;
        }

        s.stage("Preparing files");
        if (auto synced = s.sync_project(); !synced)
        {
            fail_all(Stage::Build, Verdict::Error, "could not prepare the build directory: " + synced.error());
            return report;
        }
        s.log = std::format("learndc check log\nexercise: {} (revision {})\nworkspace: {}\nbackend: {}\nstarted: {}\n\n", ex.id, ex.revision, report.workspace,
                            report.backend, report.started_at);

        bool stop = false;
        std::size_t index = 0;
        for (auto const& check : ex.checks)
        {
            ++index;
            if (stop || s.cancelled())
            {
                stop = true;
                report.results.push_back(detail::cancelled_result(check));
                continue;
            }
            s.stage(std::format("Check {} of {}: {}", index, ex.checks.size(), check.name));
            auto result = detail::run_one(s, check);
            if (result.verdict == Verdict::Cancelled)
                stop = true;
            report.results.push_back(std::move(result));
        }
        detail::finalize(report, begin);

        auto const log_file = opt.dirs.logs_dir / platform::path_from_utf8(detail::slug_of(ex.id)) / "last-check.log";
        s.log += std::format("\noutcome: {} ({} of {} passed)\n", to_string(report.outcome), report.passed, report.total);
        if (platform::atomic_write(log_file, s.log))
            report.log_path = platform::path_to_utf8(log_file);
        return report;
    }

    [[nodiscard]] inline RunOutcome build_and_run_interactive(catalog::Exercise const& ex, std::filesystem::path const& source_root, RunnerOptions const& opt,
                                                              std::vector<std::string> const& args = {})
    {
        RunOutcome out;
        detail::Session s(ex, source_root, opt);
        std::error_code ec;
        std::filesystem::create_directories(opt.dirs.staging_dir, ec);
        auto lock = platform::FileLock::try_acquire(lock_path(opt.dirs, ex));
        if (!lock)
        {
            out.stage = Stage::Busy;
            out.message = lock.error().busy ? "another check of this exercise is already running" : "cannot take the check lock: " + lock.error().message;
            return out;
        }
        if (auto message = s.missing_tool())
        {
            out.stage = Stage::Toolchain;
            out.message = *message;
            return out;
        }
        s.stage("Preparing files");
        if (auto synced = s.sync_project(); !synced)
        {
            out.stage = Stage::Build;
            out.message = "could not prepare the build directory: " + synced.error();
            return out;
        }
        auto& build = s.build_project();
        if (!build.ok)
        {
            catalog::Check check;
            check.name = "build";
            auto failure = detail::build_failure(check, build, s.rewrites, "your program");
            out.stage = failure.stage;
            out.cancelled = failure.verdict == Verdict::Cancelled;
            out.timed_out = build.timed_out;
            out.message = failure.summary;
            out.diagnostics = failure.diagnostics;
            out.locations = failure.locations;
            return out;
        }
        out.diagnostics = detail::clean_diagnostics(build.output, s.rewrites);
        out.locations = parse_locations(out.diagnostics);
        dcc::host::process::Command command;
        command.argv.push_back(platform::path_to_utf8(build.binary));
        for (auto const& a : args)
            command.argv.push_back(a);
        command.cwd = source_root;
        command.passthrough = true;
        command.own_group = true;
        command.inherit_env = true;
        command.cancel = opt.cancel;
        command.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(opt.interactive_timeout.value_or(std::chrono::hours(24)));
        s.stage("Running");
        auto const result = dcc::host::process::run(command);
        if (!result.started)
        {
            out.stage = Stage::Launch;
            out.message = "the program could not be started: " + result.error;
            return out;
        }
        if (result.aborted)
        {
            out.stage = Stage::Cancelled;
            out.cancelled = true;
            out.message = "cancelled";
            return out;
        }
        if (result.timed_out)
        {
            out.stage = Stage::Timeout;
            out.timed_out = true;
            out.message = "the program was stopped because it ran too long";
            return out;
        }
        if (result.signal != 0)
        {
            out.stage = Stage::Run;
            out.signal = result.signal;
            out.message = std::format("the program was stopped by {}", detail::signal_name(result.signal));
            return out;
        }
        out.exit_code = result.exit_code;
        out.stage = Stage::Passed;
        out.ok = result.exit_code == 0;
        out.message = std::format("the program exited with code {}{}", result.exit_code, detail::exit_note(result.exit_code));
        return out;
    }

    [[nodiscard]] inline json::Json to_json(Location const& loc)
    {
        auto j = json::Json::make_object();
        j.set("file", loc.file);
        j.set("line", loc.line);
        j.set("column", loc.column);
        j.set("severity", loc.severity);
        j.set("message", loc.message);
        return j;
    }

    [[nodiscard]] inline json::Json to_json(CheckResult const& r)
    {
        auto j = json::Json::make_object();
        j.set("name", r.name);
        j.set("kind", std::string(catalog::to_string(r.kind)));
        j.set("verdict", std::string(to_string(r.verdict)));
        j.set("stage", std::string(to_string(r.stage)));
        j.set("summary", r.summary);
        j.set("expected", r.expected);
        j.set("observed", r.observed);
        j.set("expected_visible", r.expected_visible);
        j.set("observed_visible", r.observed_visible);
        j.set("exit_code", r.exit_code ? json::Json(*r.exit_code) : json::Json());
        j.set("signal", r.signal ? json::Json(*r.signal) : json::Json());
        j.set("timed_out", r.timed_out);
        j.set("duration_ms", r.duration_ms);
        j.set("stdout_preview", r.stdout_preview);
        j.set("stderr_preview", r.stderr_preview);
        j.set("diagnostics", r.diagnostics);
        auto locations = json::Json::make_array();
        for (auto const& loc : r.locations)
            locations.push(to_json(loc));
        j.set("locations", std::move(locations));
        j.set("hint", r.hint);
        return j;
    }

    [[nodiscard]] inline json::Json to_json(CheckReport const& report)
    {
        auto j = json::Json::make_object();
        j.set("exercise", report.exercise);
        j.set("revision", report.revision);
        j.set("workspace", report.workspace);
        j.set("backend", report.backend);
        j.set("started_at", report.started_at);
        j.set("duration_ms", report.duration_ms);
        j.set("passed", report.passed);
        j.set("total", report.total);
        j.set("outcome", std::string(to_string(report.outcome)));
        j.set("first_failure_stage", report.first_failure_stage);
        auto results = json::Json::make_array();
        for (auto const& r : report.results)
            results.push(to_json(r));
        j.set("results", std::move(results));
        j.set("log_path", report.log_path);
        j.set("safety_note", report.safety_note);
        return j;
    }

    namespace detail
    {
        [[nodiscard]] inline std::string paint(TextStyle const& style, std::string_view code, std::string_view body)
        {
            if (!style.color)
                return std::string(body);
            return std::format("\x1b[{}m{}\x1b[0m", code, body);
        }

        inline void block(std::string& out, std::string_view indent, std::string_view label, std::string_view body, TextStyle const& style)
        {
            out += std::format("{}{}\n", indent, paint(style, "1", label));
            auto const bar = style.ascii ? "|" : "\xE2\x94\x82";
            if (body.empty())
            {
                out += std::format("{}  {} (empty)\n", indent, bar);
                return;
            }
            std::string_view rest = body;
            while (!rest.empty())
            {
                auto const nl = rest.find('\n');
                auto const line = rest.substr(0, nl);
                out += std::format("{}  {} {}\n", indent, bar, line);
                if (nl == std::string_view::npos)
                    break;
                rest.remove_prefix(nl + 1);
            }
        }

        [[nodiscard]] inline std::string relative_to(std::string_view workspace_root, std::string_view file)
        {
            auto root = std::string(workspace_root);
            if (!root.empty() && root.back() != '/' && root.back() != '\\')
                root += '/';
            if (file.starts_with(root))
                return std::string(file.substr(root.size()));
            return std::string(file);
        }

        [[nodiscard]] inline std::string clamp_lines(std::string_view s, std::size_t max_lines, std::size_t max_bytes)
        {
            auto lines = text::split_lines(s);
            std::string out;
            std::size_t shown = 0;
            for (auto const& line : lines)
            {
                if (shown == max_lines)
                    break;
                out += line;
                out += '\n';
                ++shown;
            }
            if (!out.empty())
                out.pop_back();
            if (lines.size() > shown)
                out += std::format("\n[... {} more lines]", lines.size() - shown);
            return text::clamp_bytes(out, max_bytes);
        }
    } // namespace detail

    [[nodiscard]] inline std::string render_text(CheckReport const& report, TextStyle const& style = {})
    {
        std::string out;
        auto const noun = report.total == 1 ? "check" : "checks";
        std::string headline = std::format("{} of {} {} passed", report.passed, report.total, noun);
        if (report.outcome == Outcome::Cancelled)
            headline += " (cancelled)";
        std::string_view const color = report.outcome == Outcome::Passed ? "1;32" : report.outcome == Outcome::Cancelled ? "1;33" : "1;31";
        out += std::format("{}: {}\n", report.exercise, detail::paint(style, color, headline));
        for (auto const& r : report.results)
        {
            std::string label;
            std::string glyph;
            std::string_view code = "0";
            switch (r.verdict)
            {
                case Verdict::Pass:
                    label = "pass";
                    glyph = style.ascii ? "[ok]" : "\xE2\x9C\x93";
                    code = "32";
                    break;
                case Verdict::Fail:
                    label = "FAIL";
                    glyph = style.ascii ? "[x]" : "\xE2\x9C\x97";
                    code = "31";
                    break;
                case Verdict::Error:
                    label = "ERROR";
                    glyph = style.ascii ? "[!]" : "!";
                    code = "1;31";
                    break;
                case Verdict::Skipped:
                    label = "skipped";
                    glyph = style.ascii ? "[-]" : "\xE2\x97\x8B";
                    code = "2";
                    break;
                case Verdict::Cancelled:
                    label = "cancelled";
                    glyph = style.ascii ? "[-]" : "\xE2\x97\x8B";
                    code = "33";
                    break;
            }
            out += std::format("  {} {}  {}\n", detail::paint(style, code, glyph), detail::paint(style, code, label), r.name);
            if (r.verdict == Verdict::Pass)
                continue;
            std::string const indent = "      ";
            if (!r.summary.empty())
                out += std::format("{}{} (stage: {})\n", indent, r.summary, to_string(r.stage));
            if (!r.expected.empty() || !r.observed.empty())
            {
                bool const visible = !r.expected_visible.empty() || !r.observed_visible.empty();
                std::string expected = r.expected;
                std::string observed = r.observed;
                if (visible)
                {
                    expected = style.ascii ? text::visible_whitespace_ascii(r.expected) : r.expected_visible;
                    observed = style.ascii ? text::visible_whitespace_ascii(r.observed) : r.observed_visible;
                }
                detail::block(out, indent, visible ? "Expected (whitespace shown):" : "Expected:", expected, style);
                detail::block(out, indent, visible ? "Observed (whitespace shown):" : "Observed:", observed, style);
            }
            else if (!r.stdout_preview.empty() && r.stage != Stage::Build && r.stage != Stage::Link)
                detail::block(out, indent, "Program output:", r.stdout_preview, style);
            if (!r.stderr_preview.empty() && r.stage != Stage::Build && r.stage != Stage::Link && (r.stage == Stage::Run || r.stage == Stage::Harness))
                detail::block(out, indent, "Error output:", r.stderr_preview, style);
            for (auto const& loc : r.locations)
            {
                if (loc.severity == "note")
                    continue;
                out += std::format("{}at {}:{}:{} {}: {}\n", indent, detail::relative_to(report.workspace, loc.file), loc.line, loc.column, loc.severity,
                                   loc.message);
            }
            if (!r.diagnostics.empty() && (r.verdict != Verdict::Pass))
            {
                auto const body = style.verbose ? r.diagnostics : detail::clamp_lines(r.diagnostics, 30, 3000);
                detail::block(out, indent, "Compiler output:", body, style);
            }
            if (!r.hint.empty() && r.verdict == Verdict::Fail)
                out += std::format("{}{} {}\n", indent, detail::paint(style, "1;36", "Hint:"), r.hint);
        }
        if (!report.log_path.empty())
            out += std::format("Full log: {}\n", report.log_path);
        if (!report.safety_note.empty())
            out += std::format("Note: {}\n", report.safety_note);
        return out;
    }
} // namespace learndc::runner
