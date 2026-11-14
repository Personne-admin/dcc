export module dcc.backend.object.link;

import std;
import dcc.backend;
import dcc.target;
import dcc.config;
import dcc.host.process;

export namespace dcc::backend::object
{
    inline constexpr std::array<std::string_view, 4> windows_import_libraries{"kernel32", "ws2_32", "advapi32", "shell32"};

    [[nodiscard]] inline std::vector<std::string> executable_library_paths(BackendOptions const& opts)
    {
        std::vector<std::string> paths(opts.library_paths.begin(), opts.library_paths.end());
#ifdef _WIN32
        if (opts.target.object_format == target::ObjectFormat::Coff)
            paths.push_back((config::current_prefix(nullptr).path / "lib").string());
#endif
        return paths;
    }

    [[nodiscard]] inline std::optional<std::string> missing_windows_import_library(std::vector<std::string> const& paths)
    {
        for (auto name : windows_import_libraries)
        {
            auto file = std::format("lib{}.a", name);
            bool found = std::ranges::any_of(paths, [&](std::string const& path) {
                std::error_code ec;
                return std::filesystem::is_regular_file(std::filesystem::path{path} / file, ec);
            });
            if (!found)
            {
                std::string searched;
                for (auto const& path : paths)
                    searched += (searched.empty() ? "" : ", ") + path;
                return std::format("Windows import library '{}' not found (searched: {}); install the dcc Windows toolchain or pass -L with a MinGW-w64 "
                                   "x86_64 import library directory",
                                   file, searched.empty() ? "no library paths" : searched);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] inline host::process::Result link_executable(std::filesystem::path const& object, std::filesystem::path const& output,
                                                               BackendOptions const& opts)
    {
        bool const coff = opts.target.object_format == target::ObjectFormat::Coff;
        bool const windows = coff && opts.target.os == target::Os::Windows && opts.link_windows_imports;
        auto paths = executable_library_paths(opts);
        if (windows)
            if (auto missing = missing_windows_import_library(paths))
            {
                host::process::Result result;
                result.error = *missing;
                return result;
            }
        host::process::Command command;
        command.inherit_env = true;
        command.argv = {host::process::linker_path("ld.lld")};
        auto add = [&](std::initializer_list<std::string> args) { command.argv.insert(command.argv.end(), args.begin(), args.end()); };
        if (coff)
            add({"-m", "i386pep", "--subsystem", "console", "--entry", "_start"});
        else
            add({"--static", "--no-dynamic-linker", "--fatal-warnings"});
        add({"-o", output.string(), object.string()});
        for (auto const& path : opts.additional_objects)
            add({path});
        for (auto const& path : paths)
            add({"-L" + path});
        for (auto const& lib : opts.libraries)
            add({"-l" + lib});
        for (auto const& arg : opts.linker_args)
            add({arg});
        if (windows)
            for (auto name : windows_import_libraries)
                add({"-l" + std::string{name}});
        auto result = host::process::run(command);
        if (!result.started && result.error.starts_with("cannot find "))
            result.error = std::format("linker '{}' not found; dcc needs ld.lld beside dcc or on PATH to write executables", command.argv.front());
        return result;
    }
} // namespace dcc::backend::object
