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

    inline constexpr std::string_view default_dynamic_linker = "/lib64/ld-linux-x86-64.so.2";

    struct LinkPlan
    {
        bool dynamic{};
        bool shared_inputs{};
        std::vector<std::string> search_paths;
        std::vector<std::filesystem::path> libraries;
    };

    namespace link_detail
    {
        [[nodiscard]] inline bool is_shared_object_name(std::string_view name)
        {
            auto file = std::filesystem::path{name}.filename().string();
            return file.ends_with(".so") || file.find(".so.") != std::string::npos;
        }

        [[nodiscard]] inline bool is_regular(std::filesystem::path const& path)
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec) && !ec;
        }

        [[nodiscard]] inline std::vector<std::string> linker_arg_paths(std::vector<std::string> const& args)
        {
            std::vector<std::string> paths;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                std::string_view arg = args[i];
                if (arg == "-L" && i + 1 < args.size())
                    paths.push_back(args[++i]);
                else if (arg.starts_with("--library-path="))
                    paths.emplace_back(arg.substr(15));
                else if (arg.starts_with("-L") && arg.size() > 2)
                    paths.emplace_back(arg.substr(2));
            }
            return paths;
        }

        inline void append_unique(std::vector<std::string>& out, std::string const& path)
        {
            if (!path.empty() && std::ranges::find(out, path) == out.end())
                out.push_back(path);
        }

        [[nodiscard]] inline bool dynamic_capable(target::TargetConfig const& t)
        {
            return t.object_format == target::ObjectFormat::Elf && t.arch == target::Arch::X86_64 && t.os == target::Os::Linux;
        }
    }

    [[nodiscard]] inline std::vector<std::string> system_library_paths()
    {
        std::vector<std::string> paths;
#ifndef _WIN32
        if (char const* env = std::getenv("LIBRARY_PATH"))
        {
            std::string_view rest{env};
            while (!rest.empty())
            {
                auto colon = rest.find(':');
                link_detail::append_unique(paths, std::string{rest.substr(0, colon)});
                if (colon == std::string_view::npos)
                    break;
                rest.remove_prefix(colon + 1);
            }
        }
        for (auto dir : {"/usr/local/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/usr/local/lib64", "/lib64",
                         "/usr/lib64", "/usr/local/lib", "/lib", "/usr/lib"})
        {
            std::error_code ec;
            if (std::filesystem::is_directory(dir, ec))
                link_detail::append_unique(paths, dir);
        }
#endif
        return paths;
    }

    struct LibraryMatch
    {
        std::filesystem::path file;
        bool shared{};
    };

    [[nodiscard]] inline std::optional<LibraryMatch> find_library(std::string_view name, std::vector<std::string> const& dirs, bool allow_shared)
    {
        for (auto const& dir : dirs)
        {
            std::filesystem::path base{dir};
            if (name.starts_with(':'))
            {
                auto exact = base / std::string{name.substr(1)};
                if (link_detail::is_regular(exact))
                    return LibraryMatch{exact, link_detail::is_shared_object_name(exact.string())};
                continue;
            }
            if (allow_shared)
                if (auto so = base / std::format("lib{}.so", name); link_detail::is_regular(so))
                    return LibraryMatch{so, true};
            if (auto a = base / std::format("lib{}.a", name); link_detail::is_regular(a))
                return LibraryMatch{a, false};
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::expected<LinkPlan, std::string> plan_link(BackendOptions const& opts)
    {
        LinkPlan plan;
        auto const& t = opts.target;
        bool const capable = link_detail::dynamic_capable(t);
        auto user_paths = executable_library_paths(opts);
        for (auto const& path : link_detail::linker_arg_paths(opts.linker_args))
            link_detail::append_unique(user_paths, path);
        if (t.object_format == target::ObjectFormat::Coff)
        {
            if (!opts.runtime_paths.empty())
                return std::unexpected(std::string{"-rpath is not supported for PE executables; Windows finds DLLs next to the executable and on PATH"});
            if (!opts.dynamic_linker.empty())
                return std::unexpected(std::string{"-dynamic-linker is not supported for PE executables"});
            plan.search_paths = executable_library_paths(opts);
            plan.dynamic = opts.link_mode != LinkMode::Static;
            for (auto const& lib : opts.libraries)
            {
                bool found = false;
                for (auto const& dir : user_paths)
                {
                    std::filesystem::path base{dir};
                    for (auto const& candidate : {base / std::format("lib{}.dll.a", lib), base / std::format("{}.dll.a", lib), base / std::format("lib{}.a", lib),
                                                  base / std::format("{}.lib", lib), base / std::format("lib{}.lib", lib)})
                        if (!found && link_detail::is_regular(candidate))
                        {
                            plan.libraries.push_back(candidate);
                            found = true;
                        }
                    if (found)
                        break;
                }
            }
            return plan;
        }
        auto system_paths = capable ? system_library_paths() : std::vector<std::string>{};
        auto all_paths = user_paths;
        for (auto const& path : system_paths)
            link_detail::append_unique(all_paths, path);

        bool wants_dynamic = opts.link_mode == LinkMode::Dynamic || !opts.runtime_paths.empty() || !opts.dynamic_linker.empty();
        std::string reason = opts.link_mode == LinkMode::Dynamic ? "-dynamic" : !opts.runtime_paths.empty() ? "-rpath" : "-dynamic-linker";
        for (auto const& object : opts.additional_objects)
            if (link_detail::is_shared_object_name(object))
            {
                plan.shared_inputs = true;
                if (!wants_dynamic)
                    reason = "the input '" + object + "' is a shared object";
                wants_dynamic = true;
            }
        for (auto const& lib : opts.libraries)
        {
            auto match = find_library(lib, opts.link_mode == LinkMode::Static ? user_paths : all_paths, opts.link_mode != LinkMode::Static);
            if (!match && opts.link_mode == LinkMode::Static)
                if (auto shared = find_library(lib, user_paths, true); shared && shared->shared)
                    return std::unexpected(std::format("-l{} resolves only to the shared object '{}', and -static links only lib{}.a archives; drop -static to "
                                                       "link it dynamically",
                                                       lib, shared->file.string(), lib));
            if (!match)
                continue;
            plan.libraries.push_back(match->file);
            plan.shared_inputs = plan.shared_inputs || match->shared;
            if (match->shared && opts.link_mode == LinkMode::Auto && !wants_dynamic)
            {
                wants_dynamic = true;
                reason = std::format("-l{} resolves to the shared object '{}'", lib, match->file.string());
            }
        }
        if (opts.link_mode == LinkMode::Static)
        {
            if (!opts.runtime_paths.empty())
                return std::unexpected(std::string{"-rpath needs a dynamically linked executable; it cannot be combined with -static"});
            if (!opts.dynamic_linker.empty())
                return std::unexpected(std::string{"-dynamic-linker cannot be combined with -static"});
            wants_dynamic = false;
        }
        if (wants_dynamic && capable && !plan.shared_inputs)
            return std::unexpected(std::format("{} needs a shared library to link, but no linked library resolved to lib<name>.so and no .so input was given",
                                               reason));
        if (wants_dynamic && !capable)
            return std::unexpected(std::format("dynamic linking is supported only for x86_64 Linux targets, not '{}' ({}); link static libraries or use a "
                                               "hosted x86_64 Linux target",
                                               t.triple, reason));
        plan.dynamic = wants_dynamic;
        plan.search_paths = plan.dynamic ? all_paths : executable_library_paths(opts);
        return plan;
    }

    [[nodiscard]] inline host::process::Result link_executable(std::filesystem::path const& object, std::filesystem::path const& output,
                                                               BackendOptions const& opts)
    {
        bool const coff = opts.target.object_format == target::ObjectFormat::Coff;
        bool const windows = coff && opts.target.os == target::Os::Windows && opts.link_windows_imports;
        auto planned = plan_link(opts);
        if (!planned)
        {
            host::process::Result result;
            result.error = planned.error();
            return result;
        }
        auto const& plan = *planned;
        auto paths = plan.search_paths;
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
        else if (plan.dynamic)
            add({"-m", "elf_x86_64", "--dynamic-linker", opts.dynamic_linker.empty() ? std::string{default_dynamic_linker} : opts.dynamic_linker,
                 "--eh-frame-hdr", "--fatal-warnings"});
        else
            add({"-m", opts.target.arch == target::Arch::X86 ? "elf_i386" : "elf_x86_64", "--static", "--no-dynamic-linker", "--fatal-warnings"});
        add({"-o", output.string(), object.string()});
        for (auto const& path : opts.additional_objects)
            add({path});
        for (auto const& path : paths)
            add({"-L" + path});
        for (auto const& path : opts.runtime_paths)
            add({"-rpath", path});
        if (coff && opts.link_mode == LinkMode::Static)
            add({"-Bstatic"});
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
        else if (!result.ok() && (result.err.find("unable to find library -l") != std::string::npos || result.out.find("unable to find library -l") != std::string::npos))
        {
            auto join = [](std::vector<std::string> const& list) {
                std::string out;
                for (auto const& path : list)
                    out += (out.empty() ? "" : ", ") + path;
                return out;
            };
            result.error += std::format("library search path: {}", paths.empty() ? std::string{"none; pass -L <dir>"} : join(paths));
            if (!plan.dynamic && !coff && link_detail::dynamic_capable(opts.target) && opts.link_mode != LinkMode::Static)
                result.error += std::format("; no lib<name>.so was found there or in the system library directories ({}) either", join(system_library_paths()));
        }
        return result;
    }
    [[nodiscard]] inline host::process::Result link_shared_library(std::filesystem::path const& object, std::filesystem::path const& output,
                                                                   BackendOptions const& opts)
    {
        host::process::Command command;
        command.inherit_env = true;
        command.argv = {host::process::linker_path("ld.lld"), "-m", "elf_x86_64", "--shared", "-o", output.string(), object.string()};
        for (auto const& path : opts.additional_objects)
            command.argv.push_back(path);
        for (auto const& path : opts.library_paths)
            command.argv.push_back("-L" + path);
        for (auto const& path : opts.runtime_paths)
        {
            command.argv.push_back("-rpath");
            command.argv.push_back(path);
        }
        for (auto const& lib : opts.libraries)
            command.argv.push_back("-l" + lib);
        for (auto const& arg : opts.linker_args)
            command.argv.push_back(arg);
        auto result = host::process::run(command);
        if (!result.started && result.error.starts_with("cannot find "))
            result.error = std::format("linker '{}' not found; dcc needs ld.lld beside dcc or on PATH to write shared libraries", command.argv.front());
        return result;
    }
} // namespace dcc::backend::object
