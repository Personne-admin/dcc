import std;

import dcc.sm;
import dcc.ast;
import dcc.ast.serializer;
import dcc.lex;
import dcc.si;
import dcc.parser;
import dcc.diag;
import dcc.sema;
import dcc.ir;
import dcc.ir.pass;
import dcc.ir.lower;

import dcc.target;
import dcc.backend;
import dcc.session;
#if DCC_ENABLE_LLVM
import dcc.backend.llvm;
#endif
import dcc.backend.em64t;
import dcc.backend.em64t.objwriter;

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace
{
    [[nodiscard]] std::filesystem::path detect_exe_path(char** argv)
    {
        std::error_code ec;

        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec)
        {
            auto resolved = std::filesystem::weakly_canonical(exe, ec);
            if (!ec)
                return resolved;

            return exe;
        }

        std::filesystem::path arg0{argv[0]};

        if (arg0.is_absolute())
        {
            auto resolved = std::filesystem::weakly_canonical(arg0, ec);
            if (!ec)
                return resolved;

            return arg0;
        }

        if (std::string_view{argv[0]}.find('/') != std::string_view::npos)
        {
            auto resolved = std::filesystem::absolute(arg0, ec);
            if (!ec)
            {
                auto wk = std::filesystem::weakly_canonical(resolved, ec);
                if (!ec)
                    return wk;

                return resolved;
            }
            return arg0;
        }

        auto const* path_env = std::getenv("PATH");
        if (path_env)
        {
            std::string_view path_sv{path_env};
            std::size_t pos = 0;
            while (pos < path_sv.size())
            {
                auto colon = path_sv.find(':', pos);
                auto dir = path_sv.substr(pos, colon - pos);
                pos = (colon == std::string_view::npos) ? path_sv.size() : colon + 1;

                if (dir.empty())
                    continue;

                auto candidate = std::filesystem::path{dir} / arg0;
                if (std::filesystem::exists(candidate, ec))
                {
                    auto wk = std::filesystem::weakly_canonical(candidate, ec);
                    if (!ec)
                        return wk;

                    auto abs = std::filesystem::absolute(candidate, ec);
                    if (!ec)
                        return abs;

                    return candidate;
                }
            }
        }

        return arg0;
    }

    [[nodiscard]] std::filesystem::path compute_prefix(std::filesystem::path const& exe_path)
    {
        return exe_path.parent_path().parent_path();
    }

    struct Options
    {
        std::filesystem::path input_file;
        std::filesystem::path output_file;
        std::optional<std::filesystem::path> depfile;
        std::vector<std::filesystem::path> import_paths;
        bool dump_ast{false};
        bool dump_ir{false};
        bool dump_llvm{false};
        bool dump_mir{false};
        bool compile_only{false};
        bool emit_asm_only{false};
        bool shared_library{false};
        bool bounds_check{false};
        bool emit_debug_info{false};
        dcc::backend::DebugFormat debug_format{dcc::backend::DebugFormat::Auto};
        bool help{false};
        bool libdcext{false};
        std::string target_triple;
        bool no_red_zone{false};
        bool no_simd{false};
        bool no_x87{false};
        bool no_stack_protector{false};
        bool no_stack_probe{false};
        bool position_independent_code{false};
        std::optional<dcc::target::CodeModel> code_model;
        std::string target_cpu;
        bool omit_frame_pointer{true};
        std::vector<std::string> injected_decls;
        std::string backend_name = "llvm";
        dcc::ir::pass::OptLevel opt_level{dcc::ir::pass::OptLevel::O0};
    };

    [[nodiscard]] auto parse_args(int argc, char** argv) -> Options
    {
        Options opts;
        int i = 1;

        while (i < argc)
        {
            std::string_view arg{argv[i]};

            if (arg == "-h" || arg == "--help")
            {
                opts.help = true;
                return opts;
            }

            if (arg == "--print-prefix")
            {
                auto exe = detect_exe_path(argv);
                std::println("{}", compute_prefix(exe).string());
                std::exit(0);
            }

            if (arg == "--print-lib-dir")
            {
                auto exe = detect_exe_path(argv);
                std::println("{}", (compute_prefix(exe) / "lib").string());
                std::exit(0);
            }

            if (arg == "--print-include-dir")
            {
                auto exe = detect_exe_path(argv);
                std::println("{}", (compute_prefix(exe) / "include").string());
                std::exit(0);
            }

            if (arg == "-fdump-ast")
            {
                opts.dump_ast = true;
                ++i;
                continue;
            }

            if (arg == "-fdump-ir")
            {
                opts.dump_ir = true;
                ++i;
                continue;
            }

            if (arg == "-fdump-llvm")
            {
                opts.dump_llvm = true;
                ++i;
                continue;
            }

            if (arg == "-fdump-mir")
            {
                opts.dump_mir = true;
                ++i;
                continue;
            }

            if (arg == "-c")
            {
                opts.compile_only = true;
                ++i;
                continue;
            }

            if (arg == "-S")
            {
                opts.emit_asm_only = true;
                ++i;
                continue;
            }

            if (arg == "-shared")
            {
                opts.shared_library = true;
                ++i;
                continue;
            }

            if (arg == "-fbounds-check")
            {
                opts.bounds_check = true;
                ++i;
                continue;
            }

            if (arg == "-flibdcext")
            {
                opts.libdcext = true;
                ++i;
                continue;
            }

            if (arg == "-fno-red-zone")
            {
                opts.no_red_zone = true;
                ++i;
                continue;
            }

            if (arg == "-fno-simd")
            {
                opts.no_simd = true;
                ++i;
                continue;
            }

            if (arg == "-fno-x87")
            {
                opts.no_x87 = true;
                ++i;
                continue;
            }

            if (arg == "-fno-stack-protector")
            {
                opts.no_stack_protector = true;
                ++i;
                continue;
            }

            if (arg == "-fno-stack-probe")
            {
                opts.no_stack_probe = true;
                ++i;
                continue;
            }

            if (arg == "-fomit-frame-pointer")
            {
                opts.omit_frame_pointer = true;
                ++i;
                continue;
            }

            if (arg == "-fno-omit-frame-pointer")
            {
                opts.omit_frame_pointer = false;
                ++i;
                continue;
            }

            if (arg == "-fPIC" || arg == "-fpic" || arg == "-fPIE")
            {
                opts.position_independent_code = true;
                ++i;
                continue;
            }

            if (arg == "-mcmodel" && i + 1 < argc)
            {
                auto parsed = dcc::target::TargetConfig::parse_code_model(argv[i + 1]);
                if (!parsed)
                {
                    std::println(std::cerr, "dcc: invalid mcmodel value '{}' (expected: default, small, kernel, medium, large)", argv[i + 1]);
                    std::exit(1);
                }
                opts.code_model = *parsed;
                i += 2;
                continue;
            }

            if (arg.starts_with("-mcmodel="))
            {
                auto value = arg.substr(9);
                auto parsed = dcc::target::TargetConfig::parse_code_model(value);
                if (!parsed)
                {
                    std::println(std::cerr, "dcc: invalid mcmodel value '{}' (expected: default, small, kernel, medium, large)", value);
                    std::exit(1);
                }
                opts.code_model = *parsed;
                ++i;
                continue;
            }

            if (arg == "-farch" && i + 1 < argc)
            {
                opts.target_cpu = argv[i + 1];
                i += 2;
                continue;
            }

            if (arg.starts_with("-farch="))
            {
                opts.target_cpu = arg.substr(7);
                ++i;
                continue;
            }

            if (arg == "-target" && i + 1 < argc)
            {
                opts.target_triple = argv[++i];
                ++i;
                continue;
            }

            if (arg.starts_with("--target="))
            {
                opts.target_triple = arg.substr(9);
                ++i;
                continue;
            }

            if (arg == "-o" && i + 1 < argc)
            {
                opts.output_file = argv[++i];
                ++i;
                continue;
            }

            if (arg == "--depfile" && i + 1 < argc)
            {
                opts.depfile = argv[++i];
                ++i;
                continue;
            }

            if (arg == "-I" && i + 1 < argc)
            {
                opts.import_paths.emplace_back(argv[++i]);
                ++i;
                continue;
            }

            if (arg.starts_with("-I"))
            {
                opts.import_paths.emplace_back(arg.substr(2));
                ++i;
                continue;
            }

            if ((arg == "-J" || arg == "--inject") && i + 1 < argc)
            {
                opts.injected_decls.emplace_back(argv[++i]);
                ++i;
                continue;
            }

            if (arg.starts_with("-J"))
            {
                opts.injected_decls.emplace_back(arg.substr(2));
                ++i;
                continue;
            }

            if (arg.starts_with("--inject="))
            {
                opts.injected_decls.emplace_back(arg.substr(9));
                ++i;
                continue;
            }

            if (arg == "-g0" || arg == "-gnone")
            {
                opts.emit_debug_info = false;
                opts.debug_format = dcc::backend::DebugFormat::None;
                ++i;
                continue;
            }

            if (arg == "-g3" || arg == "-g")
            {
                opts.emit_debug_info = true;
                opts.debug_format = dcc::backend::DebugFormat::Auto;
                ++i;
                continue;
            }

            if (arg == "-gdwarf")
            {
                opts.emit_debug_info = true;
                opts.debug_format = dcc::backend::DebugFormat::Dwarf;
                ++i;
                continue;
            }

            if (arg == "-gpdb")
            {
                opts.emit_debug_info = true;
                opts.debug_format = dcc::backend::DebugFormat::Pdb;
                ++i;
                continue;
            }

            if (arg.starts_with("-g"))
            {
                std::println(std::cerr, "dcc: unsupported debug-info option: {} (use -g0, -gnone, -g, -g3, -gdwarf, or -gpdb)", arg);
                std::exit(1);
            }

            if (arg == "-fbackend" && i + 1 < argc)
            {
                opts.backend_name = argv[++i];
                ++i;
                continue;
            }

            if (arg.starts_with("-fbackend="))
            {
                opts.backend_name = arg.substr(10);
                ++i;
                continue;
            }

            if (arg == "-O0")
            {
                opts.opt_level = dcc::ir::pass::OptLevel::O0;
                ++i;
                continue;
            }

            if (arg == "-O1")
            {
                opts.opt_level = dcc::ir::pass::OptLevel::O1;
                ++i;
                continue;
            }

            if (arg == "-O2")
            {
                opts.opt_level = dcc::ir::pass::OptLevel::O2;
                ++i;
                continue;
            }

            if (arg == "-Os")
            {
                opts.opt_level = dcc::ir::pass::OptLevel::Os;
                ++i;
                continue;
            }

            if (arg.starts_with("-"))
            {
                std::println(std::cerr, "dcc: unknown option: {}", arg);
                std::exit(1);
            }

            opts.input_file = arg;
            ++i;
        }

        return opts;
    }

    [[nodiscard]] int get_terminal_width()
    {
#ifndef _WIN32
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1)
            return w.ws_col;
#endif
        return 80;
    }

    void print_usage()
    {
        const struct OptionHelp
        {
            std::string_view flag;
            std::string_view desc;
        } options[] = {{"-I<dir>", "add import search path"},
                       {"-J<decl>", "inject a declaration"},
                       {"-o <file>", "output file"},
                       {"--depfile <file>", "write Make-compatible module dependencies"},
                       {"-c", "compile to object file only"},
                       {"-S", "emit assembly only"},
                       {"-shared", "build a shared library (.so / .dll)"},
                       {"-fdump-ast", "dump AST and exit"},
                       {"-fdump-ir", "dump IR and exit"},
                       {"-fdump-llvm", "dump LLVM IR"},
                       {"-fdump-mir", "dump em64t MIR"},
                       {"-flibdcext", "link with libdcext"},
                       {"-fbounds-check", "enable bounds checking"},
                       {"-fbackend <name>", "select backend (llvm, em64t)"},
                       {"-O0|-O1|-O2|-Os", "optimization level"},
                       {"-g, -g0, -g3", "debug info level"},
                       {"-gdwarf", "DWARF debug info"},
                       {"-gpdb", "PDB debug info"},
                       {"-gnone", "no debug info"},
                       {"-fno-red-zone", "disable red zone"},
                       {"-fno-simd", "disable SIMD"},
                       {"-fno-x87", "disable x87 FPU"},
                       {"-fno-stack-protector", "disable stack protector"},
                       {"-fno-stack-probe", "disable stack probing"},
                       {"-fPIC | -fPIE", "position-independent code"},
                       {"-mcmodel <model>", "code model (default, small, kernel, medium, large)"},
                       {"-farch <cpu>", "target CPU baseline (pentium, i686, generic, native, ...)"},
                       {"-target <triple>", "target triple"},
                       {"-h, --help", "show this help"},
                       {"-fomit-frame-pointer | -fno-omit-frame-pointer", "toggle frame pointer omission"}};

        int const term_width = get_terminal_width();
        int const flag_col_width = 24;
        int const desc_col_width = std::max(20, term_width - flag_col_width - 4);

        std::println("usage: dcc [options] <input-file>\n");
        std::println("options:");

        for (auto const& opt : options)
        {
            if (opt.flag.length() >= flag_col_width)
            {
                std::println("  {}", opt.flag);
                std::print("{:<{}}", "", flag_col_width + 2);
            }
            else
                std::print("  {:<{}}", opt.flag, flag_col_width);

            std::string_view desc = opt.desc;
            bool first_line = true;

            while (!desc.empty())
            {
                if (!first_line)
                    std::print("{:<{}}", "", flag_col_width + 2);

                if (desc.length() <= static_cast<std::size_t>(desc_col_width))
                {
                    std::println("{}", desc);
                    break;
                }

                auto wrap_pos = desc.find_last_of(" \t", static_cast<std::size_t>(desc_col_width));
                if (wrap_pos == std::string_view::npos)
                    wrap_pos = static_cast<std::size_t>(desc_col_width);

                std::println("{}", desc.substr(0, wrap_pos));

                desc = desc.substr(wrap_pos);
                auto first_non_space = desc.find_first_not_of(" \t");
                if (first_non_space != std::string_view::npos)
                    desc = desc.substr(first_non_space);
                else
                    desc = "";

                first_line = false;
            }
        }
    }

    [[nodiscard]] std::filesystem::path output_base(Options const& opts, std::filesystem::path const& input_path)
    {
        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            if (ext == ".ll" || ext == ".mir" || ext == ".s" || ext == ".o" || ext == ".a" || ext == ".so" || ext == ".dll")
            {
                auto base = opts.output_file;
                base.replace_extension("");
                return base;
            }

            return opts.output_file;
        }

        return input_path.stem();
    }

    [[nodiscard]] std::optional<std::filesystem::path> primary_output_path(Options const& opts, std::filesystem::path const& input_path,
                                                                           dcc::target::TargetConfig const& target)
    {
        auto base = output_base(opts, input_path);

        if (opts.emit_asm_only)
        {
            auto path = opts.output_file.empty() ? base : opts.output_file;
            if (opts.output_file.empty())
                path += ".s";
            return path;
        }

        if (opts.compile_only)
        {
            auto path = opts.output_file.empty() ? base : opts.output_file;
            if (opts.output_file.empty())
                path += ".o";
            return path;
        }

        if (opts.dump_llvm || opts.dump_mir)
            return opts.output_file.empty() ? std::nullopt : std::optional{opts.output_file};

        if (!opts.output_file.empty())
            return opts.output_file;

        if (opts.shared_library)
        {
            auto path = base;
            if (target.object_format == dcc::target::ObjectFormat::Coff)
                path += ".dll";
            else
                path += ".so";
            return path;
        }

        auto path = base;
        path.replace_extension("");
        return path;
    }

    [[nodiscard]] std::optional<dcc::backend::ArtifactKind> artifact_kind_for_extension(std::string_view ext)
    {
        if (ext == ".ll")
            return dcc::backend::ArtifactKind::LlvmIrText;
        if (ext == ".mir")
            return dcc::backend::ArtifactKind::MirText;
        if (ext == ".s")
            return dcc::backend::ArtifactKind::AsmText;
        if (ext == ".o")
            return dcc::backend::ArtifactKind::ObjectBytes;
        if (ext == ".a")
            return dcc::backend::ArtifactKind::ArchiveBytes;
        if (ext == ".so" || ext == ".dll")
            return dcc::backend::ArtifactKind::SharedLibraryBytes;
        return std::nullopt;
    }

    bool write_file(std::filesystem::path const& path, std::string_view content)
    {
        std::ofstream out{path, std::ios::binary};
        if (!out)
            return false;

        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        return true;
    }

    bool write_file(std::filesystem::path const& path, std::span<std::byte const> data)
    {
        std::ofstream out{path, std::ios::binary};
        if (!out)
            return false;

        out.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
        return true;
    }

    bool write_artifacts(dcc::backend::BackendArtifact const& artifact, Options const& opts, std::filesystem::path const& input_path,
                         dcc::target::TargetConfig const& target, std::filesystem::path* primary_out = nullptr)
    {
        auto base = output_base(opts, input_path);
        auto primary = primary_output_path(opts, input_path, target);

        if (primary_out)
            primary_out->clear();

        auto do_write = [&](std::filesystem::path const& path, auto const& content) -> bool {
            if (!write_file(path, content))
            {
                std::println(std::cerr, "dcc: error: cannot write to '{}'", path.string());
                return false;
            }
            return true;
        };

        if (opts.emit_asm_only)
        {
            if (artifact.asm_text)
            {
                if (primary_out)
                    *primary_out = *primary;
                return do_write(*primary, *artifact.asm_text);
            }
            return true;
        }

        if (opts.compile_only)
        {
            if (artifact.object_bytes)
            {
                if (primary_out)
                    *primary_out = *primary;
                return do_write(*primary, *artifact.object_bytes);
            }
            return true;
        }

        if (opts.dump_llvm || opts.dump_mir)
        {
            bool const has_llvm = artifact.llvm_ir_text.has_value();
            bool const has_mir = artifact.mir_text.has_value();

            if (opts.output_file.empty())
            {
                if (has_llvm)
                    std::print("{}", *artifact.llvm_ir_text);
                if (has_mir)
                    std::print("{}", *artifact.mir_text);
            }
            else if (has_llvm && has_mir)
            {
                auto dump_ext = opts.output_file.extension().string();
                if (dump_ext == ".mir")
                {
                    if (!do_write(opts.output_file, *artifact.mir_text))
                        return false;

                    if (primary_out)
                        *primary_out = opts.output_file;

                    auto llvm_path = base;
                    llvm_path += ".ll";
                    if (!do_write(llvm_path, *artifact.llvm_ir_text))
                        return false;
                }
                else
                {
                    if (!do_write(opts.output_file, *artifact.llvm_ir_text))
                        return false;

                    if (primary_out)
                        *primary_out = opts.output_file;

                    auto mir_path = base;
                    mir_path += ".mir";
                    if (!do_write(mir_path, *artifact.mir_text))
                        return false;
                }
            }
            else if (has_llvm)
            {
                if (!do_write(opts.output_file, *artifact.llvm_ir_text))
                    return false;

                if (primary_out)
                    *primary_out = opts.output_file;
            }
            else if (has_mir)
            {
                if (!do_write(opts.output_file, *artifact.mir_text))
                    return false;

                if (primary_out)
                    *primary_out = opts.output_file;
            }
            return true;
        }

        bool ok = true;
        bool mir_written_via_extension = false;
        bool asm_written_via_extension = false;
        bool obj_written_via_extension = false;
        bool llvm_written_via_extension = false;
        bool archive_written_via_extension = false;

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            auto kind = artifact_kind_for_extension(ext);
            if (kind)
            {
                switch (*kind)
                {
                    case dcc::backend::ArtifactKind::LlvmIrText:
                        if (artifact.llvm_ir_text)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.llvm_ir_text))
                                ok = false;
                        }
                        llvm_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::MirText:
                        if (artifact.mir_text)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.mir_text))
                                ok = false;
                        }
                        mir_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::AsmText:
                        if (artifact.asm_text)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.asm_text))
                                ok = false;
                        }
                        asm_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::ObjectBytes:
                        if (artifact.object_bytes)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.object_bytes))
                                ok = false;
                        }
                        obj_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::ArchiveBytes:
                        if (artifact.archive_bytes)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.archive_bytes))
                                ok = false;
                        }
                        archive_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::SharedLibraryBytes:
                        if (artifact.shared_library_bytes)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.shared_library_bytes))
                                ok = false;
                        }
                        break;
                    default:
                        break;
                }
            }
            else if (artifact.executable_bytes)
            {
                if (primary_out)
                    *primary_out = opts.output_file;
                if (!do_write(opts.output_file, *artifact.executable_bytes))
                    ok = false;
                else
                {
                    std::error_code ec;
                    std::filesystem::permissions(opts.output_file,
                                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                                 std::filesystem::perm_options::add, ec);
                }
            }
        }
        else if (artifact.executable_bytes)
        {
            auto path = *primary;
            if (primary_out)
                *primary_out = path;
            if (!do_write(path, *artifact.executable_bytes))
                ok = false;
            else
            {
                std::error_code ec;
                std::filesystem::permissions(path,
                                             std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                             std::filesystem::perm_options::add, ec);
            }
        }
        else if (artifact.shared_library_bytes)
        {
            auto path = *primary;
            if (primary_out)
                *primary_out = path;
            if (!do_write(path, *artifact.shared_library_bytes))
                ok = false;
        }

        if (artifact.llvm_ir_text && !llvm_written_via_extension)
        {
            if (opts.output_file.empty())
                std::print("{}", *artifact.llvm_ir_text);
            else
            {
                auto path = base;
                path += ".ll";
                if (!do_write(path, *artifact.llvm_ir_text))
                    ok = false;
            }
        }

        if (artifact.mir_text && !mir_written_via_extension)
        {
            if (opts.output_file.empty())
                std::print("{}", *artifact.mir_text);
            else
            {
                auto path = base;
                path += ".mir";
                if (!do_write(path, *artifact.mir_text))
                    ok = false;
            }
        }

        if (artifact.asm_text && !asm_written_via_extension)
        {
            if (opts.output_file.empty())
                std::print("{}", *artifact.asm_text);
            else
            {
                auto path = base;
                path += ".s";
                if (!do_write(path, *artifact.asm_text))
                    ok = false;
            }
        }

        if (artifact.object_bytes && !obj_written_via_extension)
        {
            auto path = base;
            if (path.extension().empty())
                path += ".o";
            if (!do_write(path, *artifact.object_bytes))
                ok = false;
        }

        if (artifact.archive_bytes && !archive_written_via_extension)
        {
            auto path = base;
            if (path.extension().empty())
                path += ".a";
            if (!do_write(path, *artifact.archive_bytes))
                ok = false;
        }

        return ok;
    }

    [[nodiscard]] std::vector<std::filesystem::path> collect_depfile_dependencies(dcc::sema::ModuleInfo const* root, dcc::sema::ModuleGraph const& graph,
                                                                                  dcc::sm::SourceManager const& sm)
    {
        std::vector<std::filesystem::path> deps;
        std::unordered_set<dcc::sm::FileId> seen_ids;
        std::unordered_set<std::string> seen_paths;

        auto push_unique = [&](dcc::sema::ModuleInfo const* m) -> bool {
            auto const* file = sm.get(m->file_id);
            if (!file || file->kind() != dcc::sm::FileKind::Disk)
                return false;

            if (m->file_id != dcc::sm::FileId::Invalid && !seen_ids.insert(m->file_id).second)
                return false;

            if (!seen_paths.insert(m->file_path.string()).second)
                return false;

            deps.push_back(m->file_path);
            return true;
        };

        if (root)
            push_unique(root);

        for (auto const& m : graph.all())
            push_unique(m.get());

        if (deps.size() > 1)
            std::sort(deps.begin() + 1, deps.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) { return a.string() < b.string(); });

        return deps;
    }

    [[nodiscard]] std::string escape_make_path(std::string_view path, bool is_target, bool at_line_end)
    {
        std::string out;
        out.reserve(path.size() + 16);

        std::size_t i = 0;
        while (i < path.size())
        {
            char c = path[i];
            if (c == '\\')
            {
                std::size_t run = 0;
                while (i + run < path.size() && path[i + run] == '\\')
                    ++run;

                char next = (i + run < path.size()) ? path[i + run] : '\0';
                if (next == ' ' || next == '\t' || next == '#')
                    out.append(2 * run, '\\');
                else if (next == ':')
                    out.append(2 * run, '\\');
                else if (is_target && next == '%')
                    out.append(2 * run, '\\');
                else if (next == '\0')
                    out.append((is_target || !at_line_end) ? 2 * run : run, '\\');
                else
                    out.append(run, '\\');

                i += run;
                continue;
            }

            switch (c)
            {
                case '$':
                    out += "$$";
                    break;
                case '#':
                    out += "\\#";
                    break;
                case ' ':
                    out += "\\ ";
                    break;
                case '\t':
                    out += "\\\t";
                    break;
                case ':':
                    out += "\\:";
                    break;
                case '%':
                    out += is_target ? "\\%" : "%";
                    break;
                default:
                    out += c;
                    break;
            }
            ++i;
        }

        return out;
    }

    [[nodiscard]] std::optional<std::string> serialize_depfile_rule(std::filesystem::path const& target, std::span<std::filesystem::path const> deps)
    {
        auto bad_path = [](std::filesystem::path const& p) {
            auto s = p.string();
            return s.find('\n') != std::string::npos || s.find('\r') != std::string::npos || s.find('\t') != std::string::npos;
        };

        if (bad_path(target))
            return std::nullopt;

        std::string rule = escape_make_path(target.string(), true, true);
        rule += ':';

        for (std::size_t i = 0; i < deps.size(); ++i)
        {
            if (bad_path(deps[i]))
                return std::nullopt;

            rule += ' ';
            rule += escape_make_path(deps[i].string(), false, i + 1 == deps.size());
        }

        if (!deps.empty() && !deps.back().string().empty() && deps.back().string().back() == '\\')
            rule += ' ';

        rule += '\n';
        return rule;
    }

    [[nodiscard]] bool same_file_path(std::filesystem::path const& a, std::filesystem::path const& b)
    {
        std::error_code ec_a;
        std::error_code ec_b;
        auto ca = std::filesystem::weakly_canonical(a, ec_a);
        auto cb = std::filesystem::weakly_canonical(b, ec_b);
        return !ec_a && !ec_b && ca == cb;
    }

    bool write_depfile_atomic(std::filesystem::path const& dest, std::string_view content)
    {
        auto parent = dest.parent_path();
        static std::atomic<int> s_temp_seq{0};

#ifndef _WIN32
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            auto tmp_path = parent / (dest.filename().string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(++s_temp_seq));

            int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (fd < 0)
            {
                if (errno == EEXIST)
                    continue;

                return false;
            }

            std::size_t off = 0;
            while (off < content.size())
            {
                auto n = ::write(fd, content.data() + off, content.size() - off);
                if (n <= 0)
                {
                    if (n < 0 && errno == EINTR)
                        continue;

                    ::close(fd);
                    std::error_code rm_ec;
                    std::filesystem::remove(tmp_path, rm_ec);
                    return false;
                }
                off += static_cast<std::size_t>(n);
            }

            if (::close(fd) != 0)
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            std::error_code ec;
            std::filesystem::rename(tmp_path, dest, ec);
            if (ec)
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            return true;
        }
        return false;
#else
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            auto tmp_path = parent / (dest.filename().string() + ".tmp." + std::to_string(::GetCurrentProcessId()) + "." + std::to_string(++s_temp_seq));

            HANDLE h = ::CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                DWORD err = ::GetLastError();
                if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
                    continue;
                return false;
            }

            std::size_t total = 0;
            bool ok = true;
            while (total < content.size())
            {
                DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - total, static_cast<std::size_t>(0xFFFFFFFFu)));
                DWORD written = 0;
                if (!::WriteFile(h, content.data() + total, chunk, &written, nullptr) || written == 0)
                {
                    ok = false;
                    break;
                }
                total += written;
            }

            if (!ok)
            {
                ::CloseHandle(h);
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            if (!::CloseHandle(h))
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            if (!::MoveFileExW(tmp_path.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            return true;
        }
        return false;
#endif
    }

    bool emit_depfile(std::filesystem::path const& depfile_path, std::filesystem::path const& target_path, dcc::sema::ModuleInfo const* root,
                      dcc::sema::ModuleGraph const& graph, dcc::sm::SourceManager const& sm)
    {
        if (target_path.empty())
        {
            std::println(std::cerr, "dcc: error: --depfile requires a filesystem output artifact");
            return false;
        }

        auto deps = collect_depfile_dependencies(root, graph, sm);

        if (same_file_path(depfile_path, target_path))
        {
            std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as the output artifact", depfile_path.string());
            return false;
        }

        for (auto const& dep : deps)
        {
            if (same_file_path(depfile_path, dep))
            {
                std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as an input source file '{}'", depfile_path.string(), dep.string());
                return false;
            }
        }

        auto rule = serialize_depfile_rule(target_path, deps);
        if (!rule)
        {
            std::println(std::cerr, "dcc: error: cannot write dependency file '{}': a path contains a newline, carriage-return, or tab character",
                         depfile_path.string());
            return false;
        }

        if (!write_depfile_atomic(depfile_path, *rule))
        {
            std::println(std::cerr, "dcc: error: cannot write dependency file '{}'", depfile_path.string());
            return false;
        }

        return true;
    }

    [[nodiscard]] std::set<dcc::backend::ArtifactKind> desired_artifacts(Options const& opts)
    {
        std::set<dcc::backend::ArtifactKind> kinds;

        if (opts.emit_asm_only)
        {
            kinds.insert(dcc::backend::ArtifactKind::AsmText);
            return kinds;
        }

        if (opts.compile_only)
        {
            kinds.insert(dcc::backend::ArtifactKind::ObjectBytes);
            return kinds;
        }

        if (opts.dump_llvm)
            kinds.insert(dcc::backend::ArtifactKind::LlvmIrText);
        if (opts.dump_mir)
            kinds.insert(dcc::backend::ArtifactKind::MirText);

        if (!kinds.empty())
            return kinds;

        if (opts.shared_library)
        {
            kinds.insert(dcc::backend::ArtifactKind::SharedLibraryBytes);
            return kinds;
        }

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            auto kind = artifact_kind_for_extension(ext);
            if (kind)
                kinds.insert(*kind);
            else
                kinds.insert(dcc::backend::ArtifactKind::ExecutableBytes);
            return kinds;
        }

        kinds.insert(dcc::backend::ArtifactKind::ExecutableBytes);
        return kinds;
    }

    [[nodiscard]] bool backend_needed(Options const& opts)
    {
        if (opts.dump_llvm || opts.dump_mir || opts.compile_only || opts.emit_asm_only)
            return true;

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            if (ext == ".ll" || ext == ".mir" || ext == ".s" || ext == ".o" || ext == ".a")
                return true;

            return true;
        }

        return !opts.dump_ir;
    }

    [[nodiscard]] bool writes_output_artifact(Options const& opts)
    {
        if (!backend_needed(opts))
            return false;

        if (opts.compile_only || opts.emit_asm_only)
            return true;

        if (opts.dump_llvm || opts.dump_mir)
            return !opts.output_file.empty();

        return true;
    }

} // anonymous namespace

auto main(int argc, char** argv) -> int
{
    auto opts = parse_args(argc, argv);

    if (opts.help || opts.input_file.empty())
    {
        print_usage();
        return opts.help ? 0 : 1;
    }

    if (opts.compile_only && opts.emit_asm_only)
    {
        std::println(std::cerr, "dcc: error: cannot specify both -c and -S");
        return 1;
    }

    if (opts.shared_library)
    {
        if (opts.compile_only || opts.emit_asm_only)
        {
            std::println(std::cerr, "dcc: error: -shared is mutually exclusive with -c and -S");
            return 1;
        }

        if (!opts.position_independent_code)
        {
            std::println(std::cerr, "dcc: warning: -shared implies -fPIC");
            opts.position_independent_code = true;
        }
    }

    if (opts.depfile && !writes_output_artifact(opts))
    {
        std::println(std::cerr, "dcc: error: --depfile requires a filesystem output artifact");
        return 1;
    }

    std::error_code ec;
    auto input_path = std::filesystem::canonical(opts.input_file, ec);
    if (ec)
    {
        std::println(std::cerr, "dcc: error: cannot find input file '{}'", opts.input_file.string());
        return 1;
    }

    if (opts.depfile)
    {
        if (same_file_path(*opts.depfile, input_path))
        {
            std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as an input source file '{}'", opts.depfile->string(),
                         input_path.string());
            return 1;
        }

        dcc::target::TargetConfig dep_target = dcc::target::TargetConfig::host_default();
        if (!opts.target_triple.empty())
        {
            auto parsed = dcc::target::TargetConfig::parse_triple(opts.target_triple);
            if (parsed)
                dep_target = *parsed;
        }

        auto dep_target_path = primary_output_path(opts, input_path, dep_target);
        if (dep_target_path && same_file_path(*opts.depfile, *dep_target_path))
        {
            std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as the output artifact", opts.depfile->string());
            return 1;
        }
    }

    dcc::session::CompilerSession session;

    auto prefix = compute_prefix(detect_exe_path(argv));

    dcc::session::CompileOptions compile_opts;
    compile_opts.arena_initial_size = 256 * 1024;
    compile_opts.injected_decls = std::move(opts.injected_decls);

    if (!opts.target_triple.empty())
    {
        auto parsed = dcc::target::TargetConfig::parse_triple(opts.target_triple);
        if (parsed)
            compile_opts.target = *parsed;
        else
        {
            std::println(std::cerr, "dcc: error: unsupported target triple '{}'", opts.target_triple);
            return 1;
        }
    }

    compile_opts.import_roots.push_back(input_path.parent_path());

    for (auto& p : opts.import_paths)
    {
        auto canonical = std::filesystem::weakly_canonical(p, ec);
        if (!ec)
            compile_opts.import_roots.push_back(std::move(canonical));
    }

    if (opts.libdcext)
        compile_opts.import_roots.push_back(prefix / "include");

    compile_opts.inject_libdcext_prelude = opts.libdcext;

    auto result = session.analyze_entry(input_path, compile_opts);
    auto* module = result.module;

    if (result.has_errors)
        return 1;

    if (!module)
    {
        std::println(std::cerr, "dcc: error: internal error");
        return 1;
    }

    if (opts.dump_ast && module->tu)
        std::println("{}", dcc::ast::AstSerializer::dump(module->tu));

    bool need_backend = backend_needed(opts);
    if (opts.dump_ir || need_backend)
    {
        auto* sema = session.sema_context();
        if (!sema)
        {
            std::println(std::cerr, "dcc: error: internal error (no sema context)");
            return 1;
        }

        dcc::ir::IrContext ir_ctx{256 * 1024, &compile_opts.target};
        auto lowerer = std::make_unique<dcc::ir::lower::Lowerer>(ir_ctx, &sema->spec_registry(), &sema->graph(), opts.bounds_check, &session.source_manager(),
                                                                 &sema->types());
        auto* ir_mod = lowerer->lower_module(*module);

        if (opts.dump_ir)
            std::println("{}", dcc::ir::IrSerializer::dump(ir_mod));

        if (need_backend)
        {
            dcc::target::TargetConfig target;
            if (!opts.target_triple.empty())
            {
                auto parsed = dcc::target::TargetConfig::parse_triple(opts.target_triple);
                if (!parsed)
                {
                    std::println(std::cerr, "dcc: error: unsupported target triple '{}'", opts.target_triple);
                    return 1;
                }
                target = *parsed;
            }
            else
                target = dcc::target::TargetConfig::host_default();

            target.no_red_zone = opts.no_red_zone;
            target.no_simd = opts.no_simd;
            target.no_x87 = opts.no_x87;
            target.no_stack_protector = opts.no_stack_protector;
            target.no_stack_probe = opts.no_stack_probe;
            target.position_independent_code = opts.position_independent_code;
            if (opts.code_model)
                target.code_model = *opts.code_model;

            if (!opts.target_cpu.empty())
            {
                if (target.arch == dcc::target::Arch::X86_64 || target.arch == dcc::target::Arch::X86)
                {
                    if (!dcc::target::TargetConfig::is_x86_cpu_allowed(opts.target_cpu))
                    {
                        std::println(std::cerr, "dcc: error: unknown CPU '{}' for target arch", opts.target_cpu);
                        return 1;
                    }
                }
                target.cpu = opts.target_cpu;
            }

            if (opts.shared_library)
            {
                if (target.object_format != dcc::target::ObjectFormat::Elf && target.object_format != dcc::target::ObjectFormat::Coff)
                {
                    std::println(std::cerr, "dcc: error: shared library output is only supported for ELF targets");
                    return 1;
                }
            }

            auto kinds = desired_artifacts(opts);
            if (kinds.empty())
            {
                std::println(std::cerr, "dcc: error: no output artifact requested; use -fdump-llvm, -fdump-mir, -S, -c, or -o");
                return 1;
            }

            dcc::backend::BackendOptions backend_opts;
            backend_opts.target = target;
            backend_opts.requested_artifacts = kinds;
            backend_opts.emit_debug_info = opts.emit_debug_info;
            backend_opts.debug_format = opts.debug_format;
            backend_opts.omit_frame_pointer = opts.omit_frame_pointer;
            backend_opts.opt_level = opts.opt_level;
            backend_opts.source_manager = &session.source_manager();

            if (opts.libdcext &&
                (kinds.contains(dcc::backend::ArtifactKind::ExecutableBytes) || kinds.contains(dcc::backend::ArtifactKind::SharedLibraryBytes)))
            {
                backend_opts.library_paths.push_back((prefix / "lib").string());
                backend_opts.libraries.push_back("dcext");
            }

            if (opts.backend_name == "llvm")
            {
                if (kinds.contains(dcc::backend::ArtifactKind::MirText))
                {
                    std::println(std::cerr, "dcc: error: LLVM backend does not support MIR output");
                    return 1;
                }

                if (kinds.contains(dcc::backend::ArtifactKind::SharedLibraryBytes))
                {
                    std::println(std::cerr, "dcc: error: LLVM backend does not support shared library output; use -fbackend em64t");
                    return 1;
                }

#if DCC_ENABLE_LLVM
                bool need_archive_wrap = kinds.contains(dcc::backend::ArtifactKind::ArchiveBytes);
                if (need_archive_wrap)
                {
                    if (target.object_format == dcc::target::ObjectFormat::Coff)
                    {
                        std::println(std::cerr, "dcc: error: static archive output is not supported for COFF target"); // TODO
                        return 1;
                    }
                    backend_opts.requested_artifacts.erase(dcc::backend::ArtifactKind::ArchiveBytes);
                    backend_opts.requested_artifacts.insert(dcc::backend::ArtifactKind::ObjectBytes);
                }

                auto backend = dcc::backend::make_llvm_backend();
                auto artifact = backend->emit(*ir_mod, backend_opts);

                if (!artifact.diagnostics.empty())
                {
                    for (auto const& d : artifact.diagnostics)
                        std::println(std::cerr, "dcc: error: backend: {}", d.message);

                    return 1;
                }

                if (need_archive_wrap)
                {
                    if (artifact.object_bytes)
                    {
                        std::string member_name{ir_mod->name};
                        if (member_name.empty())
                            member_name = "module";
                        member_name += ".o";

                        std::vector<std::uint8_t> obj_data;
                        obj_data.reserve(artifact.object_bytes->size());
                        for (auto b : *artifact.object_bytes)
                            obj_data.push_back(static_cast<std::uint8_t>(b));

                        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> archive_members;
                        archive_members.emplace_back(std::move(member_name), std::move(obj_data));
                        auto archive_data = dcc::backend::em64t::write_archive_elf(archive_members);

                        std::vector<std::byte> archive_bytes;
                        archive_bytes.reserve(archive_data.size());
                        for (auto b : archive_data)
                            archive_bytes.push_back(static_cast<std::byte>(b));
                        artifact.archive_bytes = std::move(archive_bytes);
                    }

                    artifact.object_bytes.reset();
                }

                std::filesystem::path primary_output;
                if (!write_artifacts(artifact, opts, input_path, target, &primary_output))
                    return 1;

                if (opts.depfile && !emit_depfile(*opts.depfile, primary_output, module, sema->graph(), session.source_manager()))
                    return 1;
#else
                std::println(std::cerr, "dcc: error: LLVM support not compiled into this build of dcc");
                return 1;
#endif
            }
            else if (opts.backend_name == "em64t")
            {
                if (kinds.contains(dcc::backend::ArtifactKind::LlvmIrText))
                {
                    std::println(std::cerr, "dcc: error: em64t backend does not support LLVM IR output");
                    return 1;
                }

                auto backend = dcc::backend::make_em64t_backend();
                auto artifact = backend->emit(*ir_mod, backend_opts);

                if (!artifact.diagnostics.empty())
                {
                    for (auto const& d : artifact.diagnostics)
                        std::println(std::cerr, "dcc: error: backend: {}", d.message);

                    return 1;
                }

                std::filesystem::path primary_output;
                if (!write_artifacts(artifact, opts, input_path, target, &primary_output))
                    return 1;

                if (opts.depfile && !emit_depfile(*opts.depfile, primary_output, module, sema->graph(), session.source_manager()))
                    return 1;
            }
            else
            {
                std::println(std::cerr, "dcc: error: unknown backend '{}'", opts.backend_name);
                return 1;
            }
        }
    }

    return 0;
}
