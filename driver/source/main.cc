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
import dcc.config;
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
#define WIN32_LEAN_AND_MEAN
#define NOUSER
#define NOGDI
#include <windows.h>
#endif

#include <array>
#include <cstdio>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace
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
        std::vector<std::filesystem::path> import_paths;
        bool dump_ast{false};
        bool dump_ir{false};
        bool dump_llvm{false};
        bool dump_mir{false};
        bool compile_only{false};
        bool emit_asm_only{false};
        bool shared_library{false};
        bool bounds_check{false};
        bool restricted_check{false};
        bool partial_eval{false};
        bool emit_debug_info{false};
        dcc::backend::DebugFormat debug_format{dcc::backend::DebugFormat::Auto};
        bool help{false};
        bool libdcext{false};
        dcc::target::LibdcextOs libdcext_os{dcc::target::LibdcextOs::Host};
        std::string target_triple;
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
        bool gc_sections{false};
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

    constexpr std::string_view k_choice_model[] = {"default", "small", "kernel", "medium", "large"};
    constexpr std::string_view k_choice_backend[] = {"llvm", "em64t"};
    constexpr std::string_view k_choice_libdcext[] = {"linux", "windows", "freestanding"};
    constexpr std::string_view k_choice_opt[] = {"0", "1", "2", "s"};

    [[noreturn]] void fail_option(std::string const& message)
    {
        std::println(std::cerr, "dcc: error: {}", message);
        std::exit(1);
    }

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
         "target triple (x86_64-elf, x86-elf, x86_64-coff, x86-coff)",
         "host",
         [](Options& o, bool, std::string_view v, char**) { o.target_triple = v; }},

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
         "backend",
         "llvm",
         [](Options& o, bool, std::string_view v, char**) { o.backend_name = v; }},

        {"-flibdcext",
         "",
         {},
         Arg::Optional,
         "<os>",
         k_choice_libdcext,
         Phase::Both,
         "link with libdcext for a hosted os",
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
         [](Options& o, bool, std::string_view v, char**) { o.linker_args.emplace_back(v); }},

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

        {"-fdump-mir", "", {}, Arg::None, "", {}, Phase::Compile, "dump em64t MIR", "", [](Options& o, bool on, std::string_view, char**) { o.dump_mir = on; }},

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

    [[nodiscard]] auto parse_args(int argc, char** argv) -> Options
    {
        Options opts;
        int i = 1;

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
                    fail_option(std::format("unsupported debug-info option: {} (use -g0, -gnone, -g, -g3, -gdwarf, or -gpdb)", arg));

                fail_option(std::format("unknown option: {}", arg));
            }

            ++i;

            if (found->arg == Arg::None && m.has_value)
                fail_option(std::format("option {} does not take a value", found->name));

            if (found->arg == Arg::Joined && !m.has_value)
                fail_option(std::format("option {} requires an attached value, as {}{}", found->name, found->name, found->metavar));

            if ((found->arg == Arg::Required || found->arg == Arg::Glued) && !m.has_value)
            {
                if (i >= argc)
                    fail_option(std::format("option {} requires a value", found->name));

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
                fail_option(std::format("invalid value '{}' for {} (expected: {})", m.value, found->name, choice_list(*found)));

            if (found->phase == Phase::Compile)
                opts.compile_only_flags.emplace_back(arg);

            found->apply(opts, !m.negated, m.value, argv);

            if (opts.help)
                return opts;
        }

        return opts;
    }

    [[nodiscard]] dcc::target::TargetConfig resolve_target_or_exit(Options const& opts)
    {
        std::optional<std::string_view> triple;
        if (!opts.target_triple.empty())
            triple = opts.target_triple;

        std::optional<dcc::target::LibdcextRequest> libdcext;
        if (opts.libdcext)
            libdcext = dcc::target::LibdcextRequest{.enabled = true, .os = opts.libdcext_os};

        auto result = dcc::target::resolve_target(triple, libdcext);
        if (!result)
        {
            std::println(std::cerr, "dcc: error: {}", result.error());
            std::exit(1);
        }

        return *result;
    }

    [[nodiscard]] int get_terminal_width()
    {
        if (char const* columns = std::getenv("COLUMNS"))
        {
            int parsed = 0;
            for (char const* p = columns; *p >= '0' && *p <= '9'; ++p)
                parsed = parsed * 10 + (*p - '0');

            if (parsed > 0)
                return parsed;
        }

#ifndef _WIN32
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1)
            return w.ws_col;
#endif
        return 80;
    }

    [[nodiscard]] std::string spec_flag_text(OptionSpec const& spec)
    {
        std::string out;

        if (!spec.negated.empty() && spec.name.starts_with("-f") && spec.negated == std::format("-fno-{}", spec.name.substr(2)))
            out = std::format("-f[no-]{}", spec.name.substr(2));
        else if (!spec.negated.empty())
            out = std::format("{} | {}", spec.name, spec.negated);
        else
            out = std::string{spec.name};

        if (spec.arg == Arg::None)
            return out;

        if (spec.arg == Arg::Glued || spec.arg == Arg::Joined)
            return out + std::string{spec.metavar};

        if (spec.arg == Arg::Optional)
            return std::format("{} [{}]", out, spec.metavar);

        return std::format("{} {}", out, spec.metavar);
    }

    [[nodiscard]] std::string spec_desc_text(OptionSpec const& spec)
    {
        std::string out{spec.help};

        if (!spec.choices.empty())
            out += std::format(" (one of: {})", choice_list(spec));

        if (!spec.aliases.empty())
        {
            out += spec.aliases.size() == 1 ? " (alias: " : " (aliases: ";
            for (std::size_t k = 0; k < spec.aliases.size(); ++k)
            {
                if (k)
                    out += ", ";
                out += spec.aliases[k];
            }
            out += ")";
        }

        if (!spec.default_text.empty())
            out += std::format(" [default: {}]", spec.default_text);

        return out;
    }

    void print_usage()
    {
        int const term_width = get_terminal_width();
        int const flag_col_width = 24;
        int const desc_col_width = std::max(20, term_width - flag_col_width - 4);

        std::println("usage: dcc [options] <input-file>");
        std::println("       dcc -o <output> <input1.o> [<input2.o> ...] [link options]");
        std::println("");
        std::println("options:");

        for (auto const& spec : k_options)
        {
            auto const flag = spec_flag_text(spec);
            auto const desc_text = spec_desc_text(spec);

            if (flag.length() >= static_cast<std::size_t>(flag_col_width))
            {
                std::println("  {}", flag);
                std::print("{:<{}}", "", flag_col_width + 2);
            }
            else
                std::print("  {:<{}}", flag, flag_col_width);

            std::string_view desc = desc_text;
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

    [[nodiscard]] bool is_object_file(std::filesystem::path const& p)
    {
        return p.extension() == ".o";
    }

    [[nodiscard]] std::string shell_quote(std::string const& s)
    {
#ifdef _WIN32
        bool needs_quotes = s.empty();
        for (char c : s)
            if (c == ' ' || c == '\t' || c == '"' || c == '&' || c == '|' || c == '<' || c == '>' || c == '^')
                needs_quotes = true;

        if (!needs_quotes)
            return s;

        std::string out{'"'};
        std::size_t i = 0;
        while (i < s.size())
        {
            std::size_t slashes = 0;
            while (i < s.size() && s[i] == '\\')
            {
                ++slashes;
                ++i;
            }

            if (i == s.size())
            {
                out.append(slashes * 2, '\\');
                break;
            }

            if (s[i] == '"')
            {
                out.append(slashes * 2 + 1, '\\');
                out += '"';
                ++i;
                continue;
            }

            out.append(slashes, '\\');
            out += s[i];
            ++i;
        }

        out += '"';
        return out;
#else
        std::string out{'\''};
        for (char c : s)
        {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += '\'';
        return out;
#endif
    }

    [[nodiscard]] std::string libdcext_library_name(dcc::target::TargetConfig const& target, std::string_view backend_name)
    {
        return std::format("dcext-{}-{}", dcc::target::os_name(target.os), backend_name);
    }

    [[nodiscard]] std::vector<std::string> explicit_linker_args(Options const& opts)
    {
        std::vector<std::string> args;

        if (!opts.linker_script.empty())
            args.push_back(std::format("--script={}", opts.linker_script));

        if (!opts.entry_symbol.empty())
            args.push_back(std::format("--entry={}", opts.entry_symbol));

        if (opts.gc_sections)
            args.emplace_back("--gc-sections");

        for (auto const& extra : opts.linker_args)
            args.push_back(extra);

        return args;
    }

    int run_link_mode(Options const& opts, char** argv)
    {
        if (opts.compile_only)
        {
            std::println(std::cerr, "dcc: error: -c cannot be used in link mode (all inputs are object files); pass a .dc source to -c, or link without -c");
            return 1;
        }

        if (opts.emit_asm_only)
        {
            std::println(std::cerr, "dcc: error: -S cannot be used in link mode (all inputs are already-compiled object files)");
            return 1;
        }

        if (opts.shared_library)
        {
            std::println(std::cerr, "dcc: error: -shared is not supported in link mode; link mode produces executables only");
            return 1;
        }

        if (!opts.compile_only_flags.empty())
        {
            std::string list;
            for (std::size_t k = 0; k < opts.compile_only_flags.size(); ++k)
            {
                if (k)
                    list += ", ";
                list += opts.compile_only_flags[k];
            }
            std::println(std::cerr, "dcc: error: option(s) {} only apply when compiling sources and cannot be used in link mode", list);
            return 1;
        }

        dcc::target::TargetConfig target = resolve_target_or_exit(opts);

        if (target.object_format == dcc::target::ObjectFormat::Coff)
        {
            std::println(std::cerr,
                         "dcc: error: link mode does not support COFF targets (target: '{}'); executable linking is currently only supported for x86_64-elf",
                         target.triple);
            return 1;
        }

        if (target.triple != "x86_64-elf")
        {
            std::println(std::cerr, "dcc: error: executable linking is currently only supported for x86_64-elf (target: '{}')", target.triple);
            return 1;
        }

        std::error_code ec;
        std::vector<std::string> objects;
        objects.reserve(opts.input_files.size());
        for (auto const& in : opts.input_files)
        {
            auto canon = std::filesystem::canonical(in, ec);
            if (ec)
            {
                std::println(std::cerr, "dcc: error: cannot find input file '{}'", in.string());
                return 1;
            }
            objects.push_back(canon.string());
        }

        std::filesystem::path output_path;
        if (!opts.output_file.empty())
            output_path = opts.output_file;
        else
            output_path = std::filesystem::path{objects.front()}.replace_extension("").filename();

        for (auto const& in : opts.input_files)
        {
            if (same_file_path(output_path, in))
            {
                std::println(std::cerr, "dcc: error: output file '{}' is the same as an input object file", output_path.string());
                return 1;
            }
        }

        auto prefix = dcc::config::current_prefix(argv).path;

        std::string cmd = "ld.lld --static --no-dynamic-linker --fatal-warnings -o ";
        cmd += shell_quote(output_path.string());
        for (auto const& obj : objects)
        {
            cmd += " ";
            cmd += shell_quote(obj);
        }

        for (auto const& dir : opts.library_paths)
        {
            cmd += " ";
            cmd += shell_quote(std::format("-L{}", dir));
        }

        for (auto const& lib : opts.libraries)
        {
            cmd += " ";
            cmd += shell_quote(std::format("-l{}", lib));
        }

        for (auto const& extra : explicit_linker_args(opts))
        {
            cmd += " ";
            cmd += shell_quote(extra);
        }

        if (opts.libdcext)
        {
            cmd += " -L";
            cmd += shell_quote((prefix / "lib").string());
            cmd += " -l";
            cmd += libdcext_library_name(target, opts.backend_name);
        }

        cmd += " 2>&1";

        std::array<char, 4096> buf{};
        std::string captured;
        auto* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
        {
            std::println(std::cerr, "dcc: error: failed to invoke linker");
            return 1;
        }
        while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
            captured += buf.data();

        int rc = pclose(pipe);
        if (rc != 0)
        {
            if (!captured.empty())
                std::println(std::cerr, "dcc: error: {}", std::string{"linking failed:"} + std::string(1, static_cast<char>(10)) + captured);
            else
                std::println(std::cerr, "dcc: error: linker failed with unknown error");
            return 1;
        }

        std::filesystem::permissions(output_path, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, ec);
        return 0;
    }

} // anonymous namespace

auto main(int argc, char** argv) -> int
{
    auto opts = parse_args(argc, argv);

    if (opts.help || opts.input_files.empty())
    {
        print_usage();
        return opts.help ? 0 : 1;
    }

    bool const any_object = std::ranges::any_of(opts.input_files, is_object_file);
    bool const all_objects = std::ranges::all_of(opts.input_files, is_object_file);

    if (any_object && !all_objects)
    {
        std::println(std::cerr, "dcc: error: cannot mix source files and object files in one invocation; compile sources with -c first, then link the objects");
        return 1;
    }

    if (all_objects)
        return run_link_mode(opts, argv);

    if (opts.input_files.size() > 1)
    {
        std::println(std::cerr, "dcc: error: multiple input files are not supported in compile mode; compile each file with -c and link the objects");
        return 1;
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
    auto input_path = std::filesystem::canonical(opts.input_files.front(), ec);
    if (ec)
    {
        std::println(std::cerr, "dcc: error: cannot find input file '{}'", opts.input_files.front().string());
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

        auto dep_target = resolve_target_or_exit(opts);

        auto dep_target_path = primary_output_path(opts, input_path, dep_target);
        if (dep_target_path && same_file_path(*opts.depfile, *dep_target_path))
        {
            std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as the output artifact", opts.depfile->string());
            return 1;
        }
    }

    dcc::session::CompilerSession session;

    auto prefix = dcc::config::current_prefix(argv).path;

    dcc::session::CompileOptions compile_opts;
    compile_opts.arena_initial_size = 256 * 1024;
    compile_opts.injected_decls = std::move(opts.injected_decls);

    compile_opts.target = resolve_target_or_exit(opts);

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

    bool const measure = std::getenv("DCC_BENCH_STATS") != nullptr;
    auto phase_start = std::chrono::steady_clock::now();
    auto phase = [&](std::string_view name) {
        auto now = std::chrono::steady_clock::now();
        if (measure)
            std::println(std::cerr, "DCC_BENCH phase {} {}", name, std::chrono::duration<double>(now - phase_start).count());
        phase_start = now;
    };
    auto result = session.analyze_entry(input_path, compile_opts);
    phase("frontend");
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
                                                                 &sema->types(), opts.restricted_check, opts.partial_eval);
        auto* ir_mod = lowerer->lower_module(*module);

        phase("lowering");
        if (measure)
            dcc::ir::pass::benchmark_stats(*ir_mod, "before");

        if (opts.dump_ir)
            std::println("{}", dcc::ir::IrSerializer::dump(ir_mod));

        if (need_backend)
        {
            phase_start = std::chrono::steady_clock::now();
            dcc::target::TargetConfig target = resolve_target_or_exit(opts);

            target.no_red_zone = !opts.red_zone;
            target.no_simd = !opts.simd;
            target.no_x87 = !opts.x87;
            target.no_stack_protector = !opts.stack_protector;
            target.no_stack_probe = !opts.stack_probe;
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

            for (auto const& dir : opts.library_paths)
                backend_opts.library_paths.push_back(dir);

            for (auto const& lib : opts.libraries)
                backend_opts.libraries.push_back(lib);

            for (auto const& extra : explicit_linker_args(opts))
                backend_opts.linker_args.push_back(extra);

            if (opts.libdcext &&
                (kinds.contains(dcc::backend::ArtifactKind::ExecutableBytes) || kinds.contains(dcc::backend::ArtifactKind::SharedLibraryBytes)))
            {
                backend_opts.library_paths.push_back((prefix / "lib").string());
                backend_opts.libraries.push_back(libdcext_library_name(target, opts.backend_name));
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
                        std::println(std::cerr, "dcc: error: static archive output is not supported for COFF target");
                        return 1;
                    }
                    backend_opts.requested_artifacts.erase(dcc::backend::ArtifactKind::ArchiveBytes);
                    backend_opts.requested_artifacts.insert(dcc::backend::ArtifactKind::ObjectBytes);
                }

                auto backend = dcc::backend::make_llvm_backend();
                auto artifact = backend->emit(*ir_mod, backend_opts);
                phase("backend");
                std::ignore = dcc::backend::validate_requested_artifacts(backend_opts.requested_artifacts, artifact);

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
                phase("backend");
                std::ignore = dcc::backend::validate_requested_artifacts(backend_opts.requested_artifacts, artifact);

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
        }
    }

    return 0;
}
