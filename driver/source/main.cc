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
import dcc.ir.lower;

import dcc.target;
import dcc.backend;
import dcc.session;
#if DCC_ENABLE_LLVM
import dcc.backend.llvm;
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
        std::vector<std::filesystem::path> import_paths;
        bool dump_ast{false};
        bool dump_ir{false};
        bool dump_llvm{false};
        bool dump_asm{false};
        bool compile_only{false};
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
        bool omit_frame_pointer{true};
        std::vector<std::string> injected_decls;
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

            if (arg == "-fdump-asm")
            {
                opts.dump_asm = true;
                ++i;
                continue;
            }

            if (arg == "-c")
            {
                opts.compile_only = true;
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

            if (arg == "-fPIC" || arg == "-fPIE")
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

    void print_usage()
    {
        std::println(
            "usage: dcc [-I<dir>] [-J<decl>] [-flibdcext] [-fbounds-check] [-fdump-ast] [-fdump-ir] [-fdump-llvm] [-fdump-asm] [-c] "
            "[-g|-g0|-g3|-gdwarf|-gpdb|-gnone] "
            "[-fno-red-zone] [-fno-simd] [-fno-x87] [-fno-stack-protector] [-fno-stack-probe] [-fomit-frame-pointer|-fno-omit-frame-pointer] [-fPIC|-fPIE] "
            "[-mcmodel <model>] "
            "[-target <triple>] [-h] [-o "
            "<file>] <input-file>");
    }

#if DCC_ENABLE_LLVM
    [[nodiscard]] std::filesystem::path output_base(Options const& opts, std::filesystem::path const& input_path)
    {
        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            if (ext == ".ll" || ext == ".s" || ext == ".o")
            {
                auto base = opts.output_file;
                base.replace_extension("");
                return base;
            }

            return opts.output_file;
        }

        return input_path.stem();
    }

    [[nodiscard]] std::optional<dcc::backend::ArtifactKind> artifact_kind_for_extension(std::string_view ext)
    {
        if (ext == ".ll")
            return dcc::backend::ArtifactKind::LlvmIrText;
        if (ext == ".s")
            return dcc::backend::ArtifactKind::AsmText;
        if (ext == ".o")
            return dcc::backend::ArtifactKind::ObjectBytes;
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

    bool write_artifacts(dcc::backend::BackendArtifact const& artifact, Options const& opts, std::filesystem::path const& input_path)
    {
        auto base = output_base(opts, input_path);
        bool ok = true;

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            auto kind = artifact_kind_for_extension(ext);
            if (kind)
            {
                switch (*kind)
                {
                    case dcc::backend::ArtifactKind::LlvmIrText:
                        if (artifact.llvm_ir_text && !write_file(opts.output_file, *artifact.llvm_ir_text))
                        {
                            std::println(std::cerr, "dcc: error: cannot write to '{}'", opts.output_file.string());
                            ok = false;
                        }
                        break;
                    case dcc::backend::ArtifactKind::AsmText:
                        if (artifact.asm_text && !write_file(opts.output_file, *artifact.asm_text))
                        {
                            std::println(std::cerr, "dcc: error: cannot write to '{}'", opts.output_file.string());
                            ok = false;
                        }
                        break;
                    case dcc::backend::ArtifactKind::ObjectBytes:
                        if (artifact.object_bytes && !write_file(opts.output_file, *artifact.object_bytes))
                        {
                            std::println(std::cerr, "dcc: error: cannot write to '{}'", opts.output_file.string());
                            ok = false;
                        }
                        break;
                    default:
                        break;
                }
            }
            else if (artifact.executable_bytes)
            {
                auto path = opts.output_file;
                if (!write_file(path, *artifact.executable_bytes))
                {
                    std::println(std::cerr, "dcc: error: cannot write to '{}'", path.string());
                    ok = false;
                }
                else
                {
                    std::error_code ec;
                    std::filesystem::permissions(path,
                                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                                 std::filesystem::perm_options::add, ec);
                }
            }
        }
        else
        {
            if (artifact.executable_bytes)
            {
                auto path = base;
                path.replace_extension("");
                if (!write_file(path, *artifact.executable_bytes))
                {
                    std::println(std::cerr, "dcc: error: cannot write to '{}'", path.string());
                    ok = false;
                }
                else
                {
                    std::error_code ec;
                    std::filesystem::permissions(path,
                                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                                 std::filesystem::perm_options::add, ec);
                }
            }
        }

        if (artifact.llvm_ir_text)
        {
            if (opts.output_file.empty())
                std::print("{}", *artifact.llvm_ir_text);
            else
            {
                auto path = base;
                path += ".ll";
                if (!write_file(path, *artifact.llvm_ir_text))
                {
                    std::println(std::cerr, "dcc: error: cannot write to '{}'", path.string());
                    ok = false;
                }
            }
        }

        if (artifact.asm_text)
        {
            if (opts.output_file.empty())
                std::print("{}", *artifact.asm_text);
            else
            {
                auto path = base;
                path += ".s";
                if (!write_file(path, *artifact.asm_text))
                {
                    std::println(std::cerr, "dcc: error: cannot write to '{}'", path.string());
                    ok = false;
                }
            }
        }

        if (artifact.object_bytes)
        {
            auto path = base;
            if (path.extension().empty())
                path += ".o";

            if (!write_file(path, *artifact.object_bytes))
            {
                std::println(std::cerr, "dcc: error: cannot write to '{}'", path.string());
                ok = false;
            }
        }

        return ok;
    }

    [[nodiscard]] std::set<dcc::backend::ArtifactKind> desired_artifacts(Options const& opts)
    {
        std::set<dcc::backend::ArtifactKind> kinds;

        if (opts.dump_llvm)
            kinds.insert(dcc::backend::ArtifactKind::LlvmIrText);
        if (opts.dump_asm)
            kinds.insert(dcc::backend::ArtifactKind::AsmText);
        if (opts.compile_only)
            kinds.insert(dcc::backend::ArtifactKind::ObjectBytes);

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            auto kind = artifact_kind_for_extension(ext);
            if (kind)
                kinds.insert(*kind);
        }

        if (kinds.empty() && !opts.dump_llvm && !opts.dump_asm && !opts.compile_only)
            kinds.insert(dcc::backend::ArtifactKind::ExecutableBytes);

        return kinds;
    }
#endif

    [[nodiscard]] bool backend_needed(Options const& opts)
    {
        if (opts.dump_llvm || opts.dump_asm || opts.compile_only)
            return true;

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            if (ext == ".ll" || ext == ".s" || ext == ".o")
                return true;

            if (ext == ".a")
            {
                std::println(std::cerr, "dcc: error: static archive output is not supported"); // TODO use ar
                std::exit(1);
            }
            return true;
        }

        return !opts.dump_ir;
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

    std::error_code ec;
    auto input_path = std::filesystem::canonical(opts.input_file, ec);
    if (ec)
    {
        std::println(std::cerr, "dcc: error: cannot find input file '{}'", opts.input_file.string());
        return 1;
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
#if DCC_ENABLE_LLVM
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

            auto kinds = desired_artifacts(opts);
            if (kinds.empty())
            {
                std::println(std::cerr, "dcc: error: no output artifact requested; use -fdump-llvm, -fdump-asm, -c, or -o");
                return 1;
            }

            dcc::backend::BackendOptions backend_opts;
            backend_opts.target = target;
            backend_opts.requested_artifacts = kinds;
            backend_opts.emit_debug_info = opts.emit_debug_info;
            backend_opts.debug_format = opts.debug_format;
            backend_opts.omit_frame_pointer = opts.omit_frame_pointer;
            backend_opts.source_manager = &session.source_manager();

            if (opts.libdcext && kinds.contains(dcc::backend::ArtifactKind::ExecutableBytes))
            {
                backend_opts.library_paths.push_back((prefix / "lib").string());
                backend_opts.libraries.push_back("dcext");
            }

            auto backend = dcc::backend::make_llvm_backend();
            auto artifact = backend->emit(*ir_mod, backend_opts);

            if (!artifact.diagnostics.empty())
            {
                for (auto const& d : artifact.diagnostics)
                    std::println(std::cerr, "dcc: error: backend: {}", d.message);

                return 1;
            }

            if (!write_artifacts(artifact, opts, input_path))
                return 1;
#else
            std::println(std::cerr, "dcc: error: LLVM support not compiled into this build of dcc");
            return 1;
#endif
        }
    }

    return 0;
}
