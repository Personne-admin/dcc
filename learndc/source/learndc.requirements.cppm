export module learndc.requirements;

import std;
import learndc.text;
import learndc.platform;
import dcc.host.process;

export namespace learndc::requirements
{
    struct Finding
    {
        std::string id;
        std::string name;
        bool ok = false;
        bool informational = false;
        std::string detail;
        std::string fix;
    };

    struct Status
    {
        std::string id;
        std::string title;
        bool ok = false;
        std::vector<Finding> findings;
    };

    [[nodiscard]] inline bool is_optional(std::string_view id)
    {
        return id == "sdl3";
    }

    namespace detail
    {
        [[nodiscard]] inline std::vector<std::filesystem::path> split_list(std::string_view value, char sep)
        {
            std::vector<std::filesystem::path> out;
            for (auto part : std::views::split(value, sep))
            {
                std::string_view const piece(part.begin(), part.end());
                if (!piece.empty())
                    out.push_back(platform::path_from_utf8(std::string(piece)));
            }
            return out;
        }

        [[nodiscard]] inline std::vector<std::filesystem::path> linux_library_dirs()
        {
            std::vector<std::filesystem::path> dirs;
            for (auto const* var : {"LIBRARY_PATH", "LD_LIBRARY_PATH"})
                if (auto v = platform::getenv(var))
                    for (auto& d : split_list(*v, ':'))
                        dirs.push_back(std::move(d));
            for (auto const* d : {"/usr/local/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/usr/local/lib64", "/lib64",
                                  "/usr/lib64", "/usr/local/lib", "/lib", "/usr/lib"})
                dirs.emplace_back(d);
            return dirs;
        }

        [[nodiscard]] inline std::optional<std::filesystem::path> find_in(std::vector<std::filesystem::path> const& dirs, std::string_view name)
        {
            std::error_code ec;
            for (auto const& d : dirs)
            {
                auto const candidate = d / platform::path_from_utf8(std::string(name));
                if (std::filesystem::exists(candidate, ec))
                    return candidate;
            }
            return std::nullopt;
        }
    } // namespace detail

    [[nodiscard]] inline Finding sdl3_library(platform::Os os)
    {
        Finding f;
        f.id = "library";
        f.name = "SDL3 library";
        if (os == platform::Os::Windows)
        {
            f.detail = "the SDL3 lessons support Linux for now";
            f.fix = "Use a Linux system (or WSL with a graphical environment) for the game development path.";
            return f;
        }
        auto const dirs = detail::linux_library_dirs();
        auto const link = detail::find_in(dirs, "libSDL3.so");
        auto const runtime = detail::find_in(dirs, "libSDL3.so.0");
        if (link && runtime)
        {
            f.ok = true;
            f.detail = std::format("{} (runtime {})", platform::path_to_utf8(*link), platform::path_to_utf8(*runtime));
            return f;
        }
        if (runtime)
        {
            f.detail =
                std::format("only the runtime library {} was found; linking needs libSDL3.so from the development package", platform::path_to_utf8(*runtime));
            f.fix = "Install the SDL3 development package (see the setup steps below).";
            return f;
        }
        f.detail = "libSDL3.so was not found in the library directories dcc searches";
        f.fix = "Install SDL3 (see the setup steps below). If SDL3 is installed elsewhere, add its lib directory to LIBRARY_PATH and LD_LIBRARY_PATH.";
        return f;
    }

    [[nodiscard]] inline Finding sdl3_package(std::optional<std::filesystem::path> const& dpm, std::filesystem::path const& project_dir)
    {
        Finding f;
        f.id = "package";
        f.name = "DC bindings (dpm package sdl3)";
        if (!dpm)
        {
            f.detail = "dpm was not found, so the package cannot be checked";
            f.fix = "Install dpm next to learndc or put it on your PATH.";
            return f;
        }
        dcc::host::process::Command cmd;
        cmd.argv = {platform::path_to_utf8(*dpm), "compile-commands"};
        cmd.cwd = project_dir;
        cmd.inherit_env = true;
        cmd.timeout = std::chrono::seconds(20);
        auto const r = dcc::host::process::run(cmd);
        if (!r.started)
        {
            f.detail = "dpm could not be started: " + r.error;
            return f;
        }
        bool const uncached = r.err.find("dpm fetch") != std::string::npos;
        if (r.exit_code != 0 || uncached)
        {
            f.detail = uncached ? "the sdl3 package is not in the dpm cache yet" : text::sanitize(std::string(text::trim(r.err)));
            f.fix = "Download it once with: learndc setup sdl3 --fetch (this uses the network).";
            return f;
        }
        f.ok = true;
        f.detail = "in the dpm cache";
        return f;
    }

    [[nodiscard]] inline Finding display(platform::Os os)
    {
        Finding f;
        f.id = "display";
        f.name = "Display";
        f.informational = true;
        if (os == platform::Os::Windows)
        {
            f.ok = true;
            f.detail = "windows open on the desktop";
            return f;
        }
        auto const x = platform::getenv("DISPLAY");
        auto const wayland = platform::getenv("WAYLAND_DISPLAY");
        if ((x && !x->empty()) || (wayland && !wayland->empty()))
        {
            f.ok = true;
            f.detail = wayland && !wayland->empty() ? "Wayland session" : "X11 session";
            return f;
        }
        f.detail = "no graphical session (DISPLAY and WAYLAND_DISPLAY are unset): checks still run, because they render off screen, but learndc run "
                   "cannot show a window";
        f.fix = "Run learndc from a desktop terminal to see your games.";
        return f;
    }

    [[nodiscard]] inline Status check_sdl3(platform::Os os, std::optional<std::filesystem::path> const& dpm, std::filesystem::path const& project_dir)
    {
        Status s;
        s.id = "sdl3";
        s.title = "SDL3 (optional, for the game development path)";
        s.findings.push_back(sdl3_library(os));
        if (s.findings.back().ok && !project_dir.empty())
            s.findings.push_back(sdl3_package(dpm, project_dir));
        s.findings.push_back(display(os));
        s.ok = std::ranges::all_of(s.findings, [](Finding const& f) { return f.ok || f.informational; });
        return s;
    }

    [[nodiscard]] inline std::string setup_guide(platform::Os os)
    {
        if (os == platform::Os::Windows)
            return "The game development path uses the SDL3 shared library through DPM and currently supports Linux only.\n";
        return "Setting up SDL3 for the game development path:\n"
               "  1. Install the SDL3 library with its development files (DC uses the shared library libSDL3.so; C headers are not needed):\n"
               "       Debian 13 / Ubuntu 25.04 or newer:  sudo apt install libsdl3-dev\n"
               "       Fedora:                            sudo dnf install SDL3-devel\n"
               "       Arch Linux:                        sudo pacman -S sdl3\n"
               "       openSUSE Tumbleweed:               sudo zypper install SDL3-devel\n"
               "     Elsewhere, build SDL3 from https://github.com/libsdl-org/SDL and install it.\n"
               "  2. Download the DC bindings (the dpm package sdl3) once: learndc setup sdl3 --fetch\n"
               "     This is the only step that uses the network; lessons build offline afterwards.\n"
               "  3. Check the result: learndc setup sdl3\n";
    }

    [[nodiscard]] inline std::string describe(Status const& s)
    {
        std::string out = std::format("{}: {}\n", s.title, s.ok ? "ready" : "setup needed");
        for (auto const& f : s.findings)
        {
            out += std::format("  {} {}: {}\n", f.ok ? "ok  " : (f.informational ? "note" : "MISS"), f.name, f.detail);
            if (!f.ok && !f.fix.empty())
                out += std::format("       {}\n", f.fix);
        }
        return out;
    }
} // namespace learndc::requirements
