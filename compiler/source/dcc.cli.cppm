module;

#include <cstdlib>

export module dcc.cli;

import std;
import dcc.target;
import dcc.backend;
import dcc.ir.pass;
import dcc.config;

export namespace dcc::cli
{
    enum class Arg : std::uint8_t
    {
        None,
        Required,
        Optional,
        Glued,
        Joined
    };

    enum class Phase : std::uint8_t
    {
        Compile,
        Link,
        Both,
        Immediate
    };

    struct Options
    {
        std::vector<std::filesystem::path> input_files;
        std::filesystem::path output_file;
        std::optional<std::filesystem::path> depfile;
        std::optional<std::filesystem::path> compdb_entry;
        std::optional<std::filesystem::path> merge_compdb;
        std::vector<std::filesystem::path> import_paths;
        std::vector<std::string> file_prefix_maps;
        bool dump_ast{false};
        bool dump_ir{false};
        bool dump_llvm{false};
        bool dump_mir{false};
        bool compile_only{false};
        bool emit_asm_only{false};
        bool shared_library{false};
        bool bounds_check{false};
        bool far_check{false};
        bool restricted_check{false};
        bool partial_eval{false};
        bool whole_program{false};
        bool whole_program_explicit{false};
        bool emit_debug_info{false};
        dcc::backend::DebugFormat debug_format{dcc::backend::DebugFormat::Auto};
        bool help{false};
        bool libdcext{false};
        dcc::target::LibdcextOs libdcext_os{dcc::target::LibdcextOs::Host};
        std::string target_triple;
        bool function_sections{false};
        bool data_sections{false};
        bool red_zone{true};
        bool simd{true};
        bool x87{true};
        bool stack_protector{true};
        bool stack_probe{true};
        bool position_independent_code{false};
        std::optional<dcc::target::CodeModel> code_model;
        std::string target_cpu;
        bool omit_frame_pointer{true};
        std::vector<std::string> injected_decls;
        std::string backend_name = "llvm";
        dcc::ir::pass::OptLevel opt_level{dcc::ir::pass::OptLevel::O0};
        std::vector<std::string> compile_only_flags;
        std::vector<std::string> library_paths;
        std::vector<std::string> libraries;
        std::vector<std::string> linker_args;
        std::string linker_script;
        std::string entry_symbol;
        bool print_reachability{false};
        bool gc_sections{false};
        std::optional<std::string> link_base;
        std::optional<std::string> stack_reserve;
        bool print_linker_script{false};
    };

    struct OptionSpec
    {
        std::string_view name;
        std::string_view negated;
        std::span<std::string_view const> aliases;
        Arg arg;
        std::string_view metavar;
        std::span<std::string_view const> choices;
        Phase phase;
        std::string_view help;
        std::string_view default_text;
        void (*apply)(Options&, bool, std::string_view, char**);
    };

    constexpr std::string_view k_alias_pic[] = {"-fPIC", "-fpic", "-fPIE"};
    constexpr std::string_view k_alias_debug[] = {"-g3"};
    constexpr std::string_view k_alias_nodebug[] = {"-gnone"};
    constexpr std::string_view k_alias_inject[] = {"--inject"};
    constexpr std::string_view k_alias_target[] = {"--target"};
    constexpr std::string_view k_alias_help[] = {"--help"};

    constexpr std::string_view k_choice_model[] = {"default", "small", "kernel", "medium", "large", "unreal", "unreal32"};
    constexpr std::string_view k_choice_backend[] = {"llvm", "custom"};
    constexpr std::string_view k_choice_libdcext[] = {"linux", "windows", "freestanding"};
    constexpr std::string_view k_choice_opt[] = {"0", "1", "2", "s"};

    struct CliError
    {
        std::string message;
    };

    [[noreturn]] void fail_option(std::string const& message) { throw CliError{message}; }

    constexpr OptionSpec k_options[] = {
        {"-I",
         "",
         {},
         Arg::Glued,
         "<dir>",
         {},
         Phase::Compile,
         "add import search path",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.import_paths.emplace_back(v); }},

        {"-J",
         "",
         k_alias_inject,
         Arg::Glued,
         "<decl>",
         {},
         Phase::Compile,
         "inject a declaration",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.injected_decls.emplace_back(v); }},

        {"-o", "", {}, Arg::Required, "<file>", {}, Phase::Both, "output file", "", [](Options& o, bool, std::string_view v, char**) { o.output_file = v; }},

        {"-c",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "compile to object file only",
         "",
         [](Options& o, bool on, std::string_view, char**) { o.compile_only = on; }},

        {"-S",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "emit assembly only",
         "",
         [](Options& o, bool on, std::string_view, char**) { o.emit_asm_only = on; }},

        {"-shared",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "build a shared library (.so / .dll)",
         "",
         [](Options& o, bool on, std::string_view, char**) { o.shared_library = on; }},

        {"--compdb-entry", "", {}, Arg::Required, "<file>", {}, Phase::Compile, "write a JSON compilation database entry", "",
         [](Options& o, bool, std::string_view v, char**) { o.compdb_entry = std::filesystem::path{v}; }},
        {"--merge-compdb", "", {}, Arg::Required, "<out>", {}, Phase::Immediate, "merge JSON entry files into a compilation database", "",
         [](Options& o, bool, std::string_view v, char**) { o.merge_compdb = std::filesystem::path{v}; }},

        {"--depfile",
         "",
         {},
         Arg::Required,
         "<file>",
         {},
         Phase::Compile,
         "write Make-compatible module dependencies",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.depfile = std::filesystem::path{v}; }},

        {"-target",
         "",
         k_alias_target,
         Arg::Required,
         "<triple>",
         {},
         Phase::Both,
         "target triple (x86_64-elf, x86-elf, x86_64-coff, x86-coff, i8086-binary)",
         "host",
         [](Options& o, bool, std::string_view v, char**) { o.target_triple = v; }},

        {"-ffile-prefix-map",
         "",
         {},
         Arg::Required,
         "<old>=<new>",
         {},
         Phase::Compile,
         "remap emitted source paths (repeatable, longest prefix wins)",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.file_prefix_maps.emplace_back(v); }},

        {"-farch",
         "",
         {},
         Arg::Required,
         "<cpu>",
         {},
         Phase::Compile,
         "CPU baseline (pentium, i686, generic, native, ...)",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.target_cpu = v; }},

        {"-mcmodel",
         "",
         {},
         Arg::Required,
         "<model>",
         k_choice_model,
         Phase::Compile,
         "code model",
         "default",
         [](Options& o, bool, std::string_view v, char**) { o.code_model = dcc::target::TargetConfig::parse_code_model(v); }},

        {"-fbackend",
         "",
         {},
         Arg::Required,
         "<name>",
         k_choice_backend,
         Phase::Both,
         "backend (llvm, or custom for the native code generator)",
         "llvm",
         [](Options& o, bool, std::string_view v, char**) { o.backend_name = v; }},

        {"-flibdcext",
         "",
         {},
         Arg::Optional,
         "<os>",
         k_choice_libdcext,
         Phase::Both,
         "link with libdcext for the selected os",
         "host",
         [](Options& o, bool on, std::string_view v, char**) {
             o.libdcext = on;
             o.libdcext_os = v.empty() ? dcc::target::LibdcextOs::Host : *dcc::target::parse_libdcext_os(v);
         }},

        {"-O",
         "",
         {},
         Arg::Joined,
         "<level>",
         k_choice_opt,
         Phase::Compile,
         "optimization level",
         "0",
         [](Options& o, bool, std::string_view v, char**) {
             if (v == "0")
                 o.opt_level = dcc::ir::pass::OptLevel::O0;
             else if (v == "1")
                 o.opt_level = dcc::ir::pass::OptLevel::O1;
             else if (v == "2")
                 o.opt_level = dcc::ir::pass::OptLevel::O2;
             else
                 o.opt_level = dcc::ir::pass::OptLevel::Os;
         }},

        {"-ffar-check",
         "-fno-far-check",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "checked far-to-near pointer conversions",
         "off",
         [](Options& o, bool on, std::string_view, char**) { o.far_check = on; }},

        {"-fbounds-check",
         "-fno-bounds-check",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "bounds checking",
         "off",
         [](Options& o, bool on, std::string_view, char**) { o.bounds_check = on; }},

        {"-fpartial-eval",
         "-fno-partial-eval",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "call-site partial evaluation",
         "off",
         [](Options& o, bool on, std::string_view, char**) { o.partial_eval = on; }},

        {"-fwhole-program",
         "-fno-whole-program",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "lower the whole import graph into one output, keeping only reachable symbols",
         "off",
         [](Options& o, bool on, std::string_view, char**) {
             o.whole_program = on;
             o.whole_program_explicit = true;
         }},

        {"-frestricted-check",
         "-fno-restricted-check",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "restricted-value cast checks",
         "off",
         [](Options& o, bool on, std::string_view, char**) { o.restricted_check = on; }},

        {"-fred-zone",
         "-fno-red-zone",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "red zone",
         "on",
         [](Options& o, bool on, std::string_view, char**) { o.red_zone = on; }},

        {"-ffunction-sections", "-fno-function-sections", {}, Arg::None, "", {}, Phase::Compile, "per-function sections", "off",
         [](Options& o, bool on, std::string_view, char**) { o.function_sections = on; }},
        {"-fdata-sections", "-fno-data-sections", {}, Arg::None, "", {}, Phase::Compile, "per-global sections", "off",
         [](Options& o, bool on, std::string_view, char**) { o.data_sections = on; }},

        {"-fsimd", "-fno-simd", {}, Arg::None, "", {}, Phase::Compile, "SIMD", "on", [](Options& o, bool on, std::string_view, char**) { o.simd = on; }},

        {"-fx87", "-fno-x87", {}, Arg::None, "", {}, Phase::Compile, "x87 FPU", "on", [](Options& o, bool on, std::string_view, char**) { o.x87 = on; }},

        {"-fstack-protector",
         "-fno-stack-protector",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "stack protector",
         "on",
         [](Options& o, bool on, std::string_view, char**) { o.stack_protector = on; }},

        {"-fstack-probe",
         "-fno-stack-probe",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "stack probing",
         "on",
         [](Options& o, bool on, std::string_view, char**) { o.stack_probe = on; }},

        {"-fomit-frame-pointer",
         "-fno-omit-frame-pointer",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "frame pointer omission",
         "on",
         [](Options& o, bool on, std::string_view, char**) { o.omit_frame_pointer = on; }},

        {"-fpic",
         "-fno-pic",
         k_alias_pic,
         Arg::None,
         "",
         {},
         Phase::Compile,
         "position-independent code",
         "off",
         [](Options& o, bool on, std::string_view, char**) { o.position_independent_code = on; }},

        {"-g",
         "",
         k_alias_debug,
         Arg::None,
         "",
         {},
         Phase::Compile,
         "emit debug info",
         "off",
         [](Options& o, bool, std::string_view, char**) {
             o.emit_debug_info = true;
             o.debug_format = dcc::backend::DebugFormat::Auto;
         }},

        {"-g0",
         "",
         k_alias_nodebug,
         Arg::None,
         "",
         {},
         Phase::Compile,
         "no debug info",
         "",
         [](Options& o, bool, std::string_view, char**) {
             o.emit_debug_info = false;
             o.debug_format = dcc::backend::DebugFormat::None;
         }},

        {"-gdwarf",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "debug info in DWARF format, implies -g",
         "",
         [](Options& o, bool, std::string_view, char**) {
             o.emit_debug_info = true;
             o.debug_format = dcc::backend::DebugFormat::Dwarf;
         }},

        {"-gpdb",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "debug info in PDB format, implies -g",
         "",
         [](Options& o, bool, std::string_view, char**) {
             o.emit_debug_info = true;
             o.debug_format = dcc::backend::DebugFormat::Pdb;
         }},

        {"-L",
         "",
         {},
         Arg::Glued,
         "<dir>",
         {},
         Phase::Both,
         "add library search path",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.library_paths.emplace_back(v); }},

        {"-l",
         "",
         {},
         Arg::Glued,
         "<name>",
         {},
         Phase::Both,
         "link a library",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.libraries.emplace_back(v); }},

        {"-T",
         "",
         {},
         Arg::Required,
         "<script>",
         {},
         Phase::Both,
         "linker script",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.linker_script = v; }},

        {"-e",
         "",
         {},
         Arg::Required,
         "<symbol>",
         {},
         Phase::Both,
         "entry symbol",
         "",
         [](Options& o, bool, std::string_view v, char**) { o.entry_symbol = v; }},

        {"--print-reachability",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "print why each whole-program function and global was kept or dropped",
         "",
         [](Options& o, bool, std::string_view, char**) { o.print_reachability = true; }},

        {"-fbase",
         "",
         {},
         Arg::Required,
         "<seg>:<off>",
         {},
         Phase::Both,
         "i8086 load address in hexadecimal; '?' for an unknown segment",
         "?:0",
         [](Options& o, bool, std::string_view v, char**) { o.link_base = std::string{v}; }},

        {"-fstack-reserve",
         "",
         {},
         Arg::Required,
         "<bytes>",
         {},
         Phase::Both,
         "i8086 small model: stack bytes kept free above the image",
         "4096",
         [](Options& o, bool, std::string_view v, char**) { o.stack_reserve = std::string{v}; }},

        {"--print-linker-script",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Both,
         "print the reference i8086 flat linker script for the selected -mcmodel and -fbase; final images use the native writer", 
         "",
         [](Options& o, bool on, std::string_view, char**) { o.print_linker_script = on; }},

        {"--gc-sections",
         "--no-gc-sections",
         {},
         Arg::None,
         "",
         {},
         Phase::Both,
         "discard unreferenced sections",
         "off",
         [](Options& o, bool on, std::string_view, char**) { o.gc_sections = on; }},

        {"-Wl,",
         "",
         {},
         Arg::Joined,
         "<arg>",
         {},
         Phase::Both,
         "pass <arg> to the linker",
         "",
         [](Options& o, bool, std::string_view v, char**) {
             for (;;)
             {
                 auto comma = v.find(',');
                 o.linker_args.emplace_back(v.substr(0, comma));
                 if (comma == std::string_view::npos)
                     break;
                 v.remove_prefix(comma + 1);
             }
         }},

        {"-fdump-ast",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Compile,
         "dump AST and exit",
         "",
         [](Options& o, bool on, std::string_view, char**) { o.dump_ast = on; }},

        {"-fdump-ir", "", {}, Arg::None, "", {}, Phase::Compile, "dump IR and exit", "", [](Options& o, bool on, std::string_view, char**) { o.dump_ir = on; }},

        {"-fdump-llvm", "", {}, Arg::None, "", {}, Phase::Compile, "dump LLVM IR", "", [](Options& o, bool on, std::string_view, char**) { o.dump_llvm = on; }},

        {"-fdump-mir", "", {}, Arg::None, "", {}, Phase::Compile, "dump custom backend MIR", "", [](Options& o, bool on, std::string_view, char**) { o.dump_mir = on; }},

        {"-h", "", k_alias_help, Arg::None, "", {}, Phase::Immediate, "show this help", "", [](Options& o, bool, std::string_view, char**) { o.help = true; }},

        {"--version",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Immediate,
         "print compiler version",
         "",
         [](Options&, bool, std::string_view, char**) {
#ifndef DCC_VERSION
#define DCC_VERSION "unknown"
#endif
#ifndef DCC_GIT_HASH
#define DCC_GIT_HASH "unknown"
#endif
             std::println("dcc {} ({})", DCC_VERSION, DCC_GIT_HASH);
             std::exit(0);
         }},

        {"--print-prefix",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Immediate,
         "print installation prefix",
         "",
         [](Options&, bool, std::string_view, char** argv) {
             std::println("{}", dcc::config::current_prefix(argv).path.string());
             std::exit(0);
         }},

        {"--print-prefix-source",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Immediate,
         "print how the prefix was determined",
         "",
         [](Options&, bool, std::string_view, char** argv) {
             std::println("{}", dcc::config::to_string(dcc::config::current_prefix(argv).source));
             std::exit(0);
         }},

        {"--print-lib-dir",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Immediate,
         "print library directory",
         "",
         [](Options&, bool, std::string_view, char** argv) {
             std::println("{}", (dcc::config::current_prefix(argv).path / "lib").string());
             std::exit(0);
         }},

        {"--print-include-dir",
         "",
         {},
         Arg::None,
         "",
         {},
         Phase::Immediate,
         "print include directory",
         "",
         [](Options&, bool, std::string_view, char** argv) {
             std::println("{}", (dcc::config::current_prefix(argv).path / "include").string());
             std::exit(0);
         }},
    };

    struct OptionMatch
    {
        bool matched{false};
        bool negated{false};
        bool has_value{false};
        std::string_view value;
    };

    [[nodiscard]] OptionMatch match_name(OptionSpec const& spec, std::string_view name, bool negated, std::string_view arg)
    {
        if (name.empty())
            return {};

        if (arg == name)
            return {true, negated, false, {}};

        if (arg.size() <= name.size() || !arg.starts_with(name))
            return {};

        if (arg[name.size()] == '=' && spec.arg != Arg::None)
            return {true, negated, true, arg.substr(name.size() + 1)};

        if (spec.arg == Arg::Glued || spec.arg == Arg::Joined)
            return {true, negated, true, arg.substr(name.size())};

        return {};
    }

    [[nodiscard]] OptionMatch match_option(OptionSpec const& spec, std::string_view arg)
    {
        if (auto m = match_name(spec, spec.name, false, arg); m.matched)
            return m;

        if (auto m = match_name(spec, spec.negated, true, arg); m.matched)
            return m;

        for (auto alias : spec.aliases)
            if (auto m = match_name(spec, alias, false, arg); m.matched)
                return m;

        return {};
    }

    [[nodiscard]] bool is_joined(OptionSpec const& spec)
    {
        return spec.arg == Arg::Glued || spec.arg == Arg::Joined;
    }

    [[nodiscard]] bool is_choice(OptionSpec const& spec, std::string_view value)
    {
        return std::ranges::find(spec.choices, value) != spec.choices.end();
    }

    [[nodiscard]] std::string choice_list(OptionSpec const& spec)
    {
        std::string out;
        for (std::size_t k = 0; k < spec.choices.size(); ++k)
        {
            if (k)
                out += ", ";
            out += spec.choices[k];
        }
        return out;
    }

    [[nodiscard]] auto parse_args_impl(int argc, char** argv, std::vector<std::string>* lenient_errors) -> Options
    {
        Options opts;
        int i = 1;

        auto report = [&](std::string message) {
            if (lenient_errors)
                lenient_errors->push_back(std::move(message));
            else
                fail_option(message);
        };

        while (i < argc)
        {
            std::string_view arg{argv[i]};

            if (!arg.starts_with("-") || arg == "-")
            {
                opts.input_files.emplace_back(arg);
                ++i;
                continue;
            }

            OptionSpec const* found = nullptr;
            OptionMatch m;

            for (auto const& spec : k_options)
            {
                if (is_joined(spec))
                    continue;

                if (auto candidate = match_option(spec, arg); candidate.matched)
                {
                    found = &spec;
                    m = candidate;
                    break;
                }
            }

            if (!found)
            {
                for (auto const& spec : k_options)
                {
                    if (!is_joined(spec))
                        continue;

                    if (auto candidate = match_option(spec, arg); candidate.matched)
                    {
                        found = &spec;
                        m = candidate;
                        break;
                    }
                }
            }

            if (!found)
            {
                if (arg.starts_with("-g"))
                    report(std::format("unsupported debug-info option: {} (use -g0, -gnone, -g, -g3, -gdwarf, or -gpdb)", arg));
                else
                    report(std::format("unknown option: {}", arg));

                ++i;
                continue;
            }

            ++i;

            if (found->arg == Arg::None && m.has_value)
            {
                report(std::format("option {} does not take a value", found->name));
                continue;
            }

            if (found->arg == Arg::Joined && !m.has_value)
            {
                report(std::format("option {} requires an attached value, as {}{}", found->name, found->name, found->metavar));
                continue;
            }

            if ((found->arg == Arg::Required || found->arg == Arg::Glued) && !m.has_value)
            {
                if (i >= argc)
                {
                    report(std::format("option {} requires a value", found->name));
                    continue;
                }

                m.value = argv[i];
                m.has_value = true;
                ++i;
            }

            if (found->arg == Arg::Optional && !m.has_value && i < argc && is_choice(*found, argv[i]))
            {
                m.value = argv[i];
                m.has_value = true;
                ++i;
            }

            if (m.has_value && !found->choices.empty() && !is_choice(*found, m.value))
            {
                report(std::format("invalid value '{}' for {} (expected: {})", m.value, found->name, choice_list(*found)));
                continue;
            }

            if (found->phase == Phase::Compile)
                opts.compile_only_flags.emplace_back(arg);

            found->apply(opts, !m.negated, m.value, argv);

            if (opts.help)
                return opts;
        }

        return opts;
    }

    [[nodiscard]] auto parse_args(int argc, char** argv) -> Options
    {
        return parse_args_impl(argc, argv, nullptr);
    }

    [[nodiscard]] auto parse_args(std::span<std::string const> args) -> Options
    {
        std::vector<std::string> owned{args.begin(), args.end()};
        std::vector<char*> argv;
        argv.reserve(owned.size());
        for (auto& arg : owned)
            argv.push_back(arg.data());
        return parse_args_impl(static_cast<int>(argv.size()), argv.data(), nullptr);
    }

    struct ParseOutcome
    {
        Options options;
        std::vector<std::string> errors;
    };

    [[nodiscard]] inline ParseOutcome parse_args_lenient(std::span<std::string const> args)
    {
        std::vector<std::string> owned{args.begin(), args.end()};
        std::vector<char*> argv;
        argv.reserve(owned.size());
        for (auto& arg : owned)
            argv.push_back(arg.data());
        ParseOutcome outcome;
        outcome.options = parse_args_impl(static_cast<int>(argv.size()), argv.data(), &outcome.errors);
        return outcome;
    }

    [[nodiscard]] std::optional<dcc::target::TargetConfig> resolve_target_config(Options const& opts, std::string& error)
    {
        std::optional<std::string_view> triple;
        if (!opts.target_triple.empty())
            triple = opts.target_triple;

        std::optional<dcc::target::LibdcextRequest> libdcext;
        if (opts.libdcext)
            libdcext = dcc::target::LibdcextRequest{.enabled = true, .os = opts.libdcext_os};

        auto resolved = dcc::target::resolve_target(triple, libdcext);
        if (!resolved)
        {
            error = resolved.error();
            return std::nullopt;
        }

        if (auto configure_error = resolved->configure(opts.code_model.value_or(dcc::target::CodeModel::Default), opts.target_cpu))
        {
            error = *configure_error;
            return std::nullopt;
        }

        resolved->no_simd = !opts.simd;
        resolved->no_x87 = !opts.x87;
        return *resolved;
    }
}
