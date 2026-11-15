export module learndc.toolchain;

import std;
import learndc.text;
import learndc.platform;
import learndc.settings;
import dcc.host.process;

export namespace learndc::toolchain
{
    enum class Source
    {
        None,
        Settings,
        Environment,
        Sibling,
        Path,
    };

    [[nodiscard]] inline std::string_view to_string(Source s)
    {
        switch (s)
        {
            case Source::None:
                return "not found";
            case Source::Settings:
                return "settings";
            case Source::Environment:
                return "environment";
            case Source::Sibling:
                return "next to learndc";
            case Source::Path:
                return "PATH";
        }
        return "not found";
    }

    struct Probe
    {
        Source source = Source::None;
        std::string where;
        bool found = false;
        std::string detail;
    };

    struct Located
    {
        std::string name;
        std::optional<std::filesystem::path> path;
        Source source = Source::None;
        std::vector<Probe> searched;
        std::vector<std::string> warnings;

        [[nodiscard]] bool found() const { return path.has_value(); }

        [[nodiscard]] std::string searched_text() const
        {
            std::string out;
            for (auto const& probe : searched)
            {
                if (!out.empty())
                    out += "\n";
                out += std::format("{}: {}{}", to_string(probe.source), probe.where, probe.detail.empty() ? std::string() : std::format(" ({})", probe.detail));
            }
            return out;
        }
    };

    struct Toolchain
    {
        Located dcc;
        Located dpm;
        Located dccd;
    };

    struct Inputs
    {
        settings::Settings settings;
        platform::Env env;
        std::optional<std::filesystem::path> self_exe;
        platform::Os os = platform::host_os();
    };

    namespace detail
    {
        [[nodiscard]] inline std::optional<std::filesystem::path> resolve_named(std::string_view value, Inputs const& in)
        {
            return platform::find_program(value, in.env, in.os);
        }

        [[nodiscard]] inline std::string exe_name(std::string_view name, platform::Os os)
        {
            return os == platform::Os::Windows ? std::format("{}.exe", name) : std::string(name);
        }

        inline void try_override(Located& out, Source source, std::string_view label, std::string_view value, Inputs const& in)
        {
            if (value.empty())
                return;
            Probe probe{source, std::string(label), false, {}};
            auto found = resolve_named(value, in);
            if (found)
            {
                probe.found = true;
                probe.detail = platform::path_to_utf8(*found);
                if (!out.path)
                {
                    out.path = std::move(found);
                    out.source = source;
                }
            }
            else
            {
                probe.detail = std::format("'{}' is not an executable file", text::sanitize(value));
                out.warnings.push_back(
                    std::format("{} is set to '{}' but that is not an executable program; falling back to the next location", label, text::sanitize(value)));
            }
            out.searched.push_back(std::move(probe));
        }
    } // namespace detail

    [[nodiscard]] inline Located locate(std::string_view name, std::string_view setting_value, std::string_view env_name, Inputs const& in)
    {
        Located out;
        out.name = std::string(name);
        if (!setting_value.empty())
            detail::try_override(out, Source::Settings, std::format("tools.{}", name), setting_value, in);
        else
            out.searched.push_back({Source::Settings, std::format("tools.{} (not set)", name), false, {}});
        if (auto env = platform::detail::env_lookup(in.env, env_name, in.os); env && !env->empty())
            detail::try_override(out, Source::Environment, env_name, *env, in);
        else
            out.searched.push_back({Source::Environment, std::format("{} (not set)", env_name), false, {}});
        if (in.self_exe)
        {
            auto const candidate = in.self_exe->parent_path() / platform::path_from_utf8(detail::exe_name(name, in.os));
            Probe probe{Source::Sibling, platform::path_to_utf8(candidate), false, {}};
            if (platform::detail::is_executable_file(candidate))
            {
                probe.found = true;
                if (!out.path)
                {
                    out.path = candidate;
                    out.source = Source::Sibling;
                }
            }
            else
                probe.detail = "not found";
            out.searched.push_back(std::move(probe));
        }
        else
            out.searched.push_back({Source::Sibling, "directory of the learndc executable (unknown)", false, {}});
        {
            Probe probe{Source::Path, std::format("PATH for '{}'", detail::exe_name(name, in.os)), false, {}};
            if (auto found = platform::find_program(name, in.env, in.os))
            {
                probe.found = true;
                probe.detail = platform::path_to_utf8(*found);
                if (!out.path)
                {
                    out.path = std::move(found);
                    out.source = Source::Path;
                }
            }
            else
                probe.detail = "not found";
            out.searched.push_back(std::move(probe));
        }
        return out;
    }

    [[nodiscard]] inline Toolchain resolve(Inputs const& in)
    {
        Toolchain t;
        t.dcc = locate("dcc", in.settings.tools_dcc, "LEARNDC_DCC", in);
        t.dpm = locate("dpm", in.settings.tools_dpm, "LEARNDC_DPM", in);
        t.dccd = locate("dccd", {}, "LEARNDC_DCCD", in);
        return t;
    }

    [[nodiscard]] inline Toolchain resolve(settings::Settings const& s)
    {
        return resolve(Inputs{s, platform::current_env(), platform::executable_path(), platform::host_os()});
    }

    [[nodiscard]] inline std::optional<std::string> parse_version(std::string_view output)
    {
        for (auto const& line : text::split_lines(output))
        {
            auto const words = text::split(std::string(text::trim(line)), ' ');
            for (auto const& word : words)
            {
                std::size_t i = 0;
                while (i < word.size() && (word[i] == 'v' || word[i] == 'V'))
                    ++i;
                std::size_t digits = 0;
                std::size_t dots = 0;
                std::size_t j = i;
                while (j < word.size() && ((word[j] >= '0' && word[j] <= '9') || word[j] == '.'))
                {
                    if (word[j] == '.')
                        ++dots;
                    else
                        ++digits;
                    ++j;
                }
                if (dots >= 1 && digits >= 2 && j > i && word[i] >= '0' && word[i] <= '9')
                    return word.substr(i, j - i);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::array<long, 3>> version_triple(std::string_view v)
    {
        std::array<long, 3> out{0, 0, 0};
        auto const parts = text::split(v, '.');
        if (parts.empty() || parts.size() > 3)
            return std::nullopt;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (parts[i].empty() || parts[i].size() > 9 || !std::ranges::all_of(parts[i], [](char c) { return c >= '0' && c <= '9'; }))
                return std::nullopt;
            out[i] = std::stol(parts[i]);
        }
        return out;
    }

    [[nodiscard]] inline bool version_at_least(std::string_view actual, std::string_view required)
    {
        auto a = version_triple(actual);
        auto r = version_triple(required);
        if (!a || !r)
            return false;
        return *a >= *r;
    }

    struct VersionInfo
    {
        std::string line;
        std::optional<std::string> number;
    };

    [[nodiscard]] inline std::expected<VersionInfo, text::Error> version(std::filesystem::path const& program,
                                                                         std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        dcc::host::process::Command cmd;
        cmd.argv = {platform::path_to_utf8(program), "--version"};
        cmd.timeout = timeout;
        cmd.max_capture = 64u << 10;
        cmd.grace = std::chrono::milliseconds(200);
        auto result = dcc::host::process::run(cmd);
        if (!result.started)
            return std::unexpected(text::Error{std::format("cannot run '{}': {}", platform::path_to_utf8(program), text::sanitize(result.error))});
        if (result.timed_out)
            return std::unexpected(
                text::Error{std::format("'{} --version' did not finish within {} seconds", platform::path_to_utf8(program), timeout.count() / 1000)});
        if (result.exit_code != 0)
            return std::unexpected(text::Error{std::format("'{} --version' exited with status {}", platform::path_to_utf8(program), result.exit_code)});
        auto const clean = text::sanitize(result.out.empty() ? result.err : result.out);
        std::string first;
        for (auto const& line : text::split_lines(clean))
            if (!text::trim(line).empty())
            {
                first = std::string(text::trim(line));
                break;
            }
        if (first.empty())
            return std::unexpected(text::Error{std::format("'{} --version' printed nothing", platform::path_to_utf8(program))});
        return VersionInfo{first, parse_version(first)};
    }
} // namespace learndc::toolchain
