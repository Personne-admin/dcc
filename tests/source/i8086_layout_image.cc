#include <cstdio>

import std;
import dcc.cli;
import dcc.session;
import dcc.ir;
import dcc.ir.pass;
import dcc.ir.lower;
import dcc.target;
import dcc.backend;
import dcc.backend.i8086;
import dcc.backend.i8086.place;
import dcc.backend.i8086.link;

namespace
{
    namespace fs = std::filesystem;

    struct Options
    {
        fs::path source;
        fs::path output;
        std::string model;
        std::string cpu;
        std::string base;
        std::string stack_reserve;
        std::vector<std::string> prefix_maps;
        dcc::ir::pass::OptLevel opt{dcc::ir::pass::OptLevel::O0};
        bool function_sections{false};
        bool data_sections{false};
    };

    [[nodiscard]] std::optional<Options> parse(int argc, char** argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            std::string_view argument{argv[index]};
            if (argument == "-o")
            {
                if (++index >= argc)
                    return std::nullopt;
                options.output = argv[index];
            }
            else if (argument.starts_with("-mcmodel="))
                options.model = std::string{argument.substr(9)};
            else if (argument.starts_with("-farch="))
                options.cpu = std::string{argument.substr(7)};
            else if (argument.starts_with("-fbase="))
                options.base = std::string{argument.substr(7)};
            else if (argument.starts_with("-fstack-reserve="))
                options.stack_reserve = std::string{argument.substr(16)};
            else if (argument.starts_with("-ffile-prefix-map="))
                options.prefix_maps.push_back(std::string{argument.substr(18)});
            else if (argument == "-ffunction-sections")
                options.function_sections = true;
            else if (argument == "-fdata-sections")
                options.data_sections = true;
            else if (argument == "-O0")
                options.opt = dcc::ir::pass::OptLevel::O0;
            else if (argument == "-O1")
                options.opt = dcc::ir::pass::OptLevel::O1;
            else if (argument == "-O2")
                options.opt = dcc::ir::pass::OptLevel::O2;
            else if (argument == "-Os")
                options.opt = dcc::ir::pass::OptLevel::Os;
            else if (!argument.starts_with('-'))
                options.source = argv[index];
            else
                return std::nullopt;
        }
        if (options.source.empty() || options.output.empty())
            return std::nullopt;
        return options;
    }

    struct Built
    {
        dcc::backend::BackendArtifact artifact;
        dcc::target::TargetConfig target;
    };

    [[nodiscard]] std::expected<Built, int> build(Options const& options)
    {
        dcc::cli::Options cli;
        cli.target_triple = "i8086-binary";
        cli.backend_name = "custom";
        cli.function_sections = options.function_sections;
        cli.data_sections = options.data_sections;
        cli.opt_level = options.opt;
        cli.target_cpu = options.cpu;
        if (!options.model.empty())
        {
            auto model = dcc::target::TargetConfig::parse_code_model(options.model);
            if (!model)
            {
                std::println(std::cerr, "dcc: error: unknown -mcmodel value '`{}`'", options.model);
                return std::unexpected(1);
            }
            cli.code_model = *model;
        }
        std::string error;
        auto target = dcc::cli::resolve_target_config(cli, error);
        if (!target)
        {
            std::println(std::cerr, "dcc: error: {}", error);
            return std::unexpected(1);
        }
        target->function_sections = options.function_sections;
        target->data_sections = options.data_sections;

        dcc::session::CompilerSession session;
        for (auto const& mapping : options.prefix_maps)
        {
            auto separator = mapping.find('=');
            if (separator == std::string::npos)
            {
                std::println(std::cerr, "dcc: error: -ffile-prefix-map requires OLD=NEW");
                return std::unexpected(1);
            }
            session.source_manager().add_file_prefix_map(mapping.substr(0, separator), mapping.substr(separator + 1));
        }
        dcc::session::CompileOptions compile;
        compile.target = *target;
        compile.arena_initial_size = 256 * 1024;
        compile.import_roots.push_back(options.source.parent_path());
        auto analyzed = session.analyze_entry(options.source, compile);
        if (analyzed.has_errors || !analyzed.module)
            return std::unexpected(1);
        auto* sema = session.sema_context();
        if (!sema)
        {
            std::println(std::cerr, "dcc: error: internal error (no sema context)");
            return std::unexpected(1);
        }
        dcc::ir::IrContext context{256 * 1024, &compile.target};
        auto lowerer = std::make_unique<dcc::ir::lower::Lowerer>(context, &sema->spec_registry(), &sema->graph(), false, &session.source_manager(),
                                                                 &sema->types(), false, false, false);
        lowerer->set_whole_program(false);
        auto* module = lowerer->lower_module(*analyzed.module);
        if (!lowerer->lower_errors().empty())
        {
            for (auto const& message : lowerer->lower_errors())
                std::println(std::cerr, "dcc: error: {}", message);
            return std::unexpected(1);
        }
        dcc::backend::BackendOptions backend;
        backend.target = *target;
        backend.requested_artifacts = {dcc::backend::ArtifactKind::ObjectBytes};
        backend.opt_level = options.opt;
        backend.source_manager = &session.source_manager();
        Built built;
        built.artifact = dcc::backend::make_i8086_backend()->emit(*module, backend);
        built.target = *target;
        if (!built.artifact.object_bytes || !built.artifact.image)
        {
            for (auto const& diagnostic : built.artifact.diagnostics)
                std::println(std::cerr, "dcc: error: {}", diagnostic.message);
            if (built.artifact.diagnostics.empty())
                std::println(std::cerr, "dcc: error: i8086 backend produced no object");
            return std::unexpected(1);
        }
        return built;
    }

    [[nodiscard]] std::expected<dcc::backend::i8086::FlatLayoutOptions, std::string> layout_options(Options const& options,
                                                                                                    dcc::target::TargetConfig const& target)
    {
        dcc::backend::i8086::FlatLayoutOptions layout;
        layout.model = target.code_model;
        auto base = options.base.empty() ? std::string{"?:0000"} : options.base;
        auto parsed = dcc::backend::i8086::parse_link_base(base);
        if (!parsed)
            return std::unexpected(parsed.error());
        layout.segment = parsed->segment;
        layout.offset = parsed->offset;
        if (!options.stack_reserve.empty())
        {
            auto reserve = dcc::backend::i8086::parse_stack_reserve(options.stack_reserve);
            if (!reserve)
                return std::unexpected(reserve.error());
            layout.stack_reserve = *reserve;
        }
        return layout;
    }

    [[nodiscard]] bool write_bytes(fs::path const& path, std::vector<std::uint8_t> const& bytes)
    {
        std::ofstream out{path, std::ios::binary};
        out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    }

    [[nodiscard]] std::vector<std::uint8_t> read_bytes(fs::path const& path)
    {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, int> native_image(Options const& options)
    {
        auto built = build(options);
        if (!built)
            return std::unexpected(built.error());
        auto layout = layout_options(options, built->target);
        if (!layout)
        {
            std::println(std::cerr, "dcc: error: {}", layout.error());
            return std::unexpected(1);
        }
        auto placed = dcc::backend::i8086::layout_flat_image(*built->artifact.image, *layout);
        if (!placed)
        {
            std::println(std::cerr, "dcc: error: backend: linking failed:\n{}", placed.error());
            return std::unexpected(1);
        }
        return placed->bytes;
    }

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, int> legacy_image(Options const& options)
    {
        auto built = build(options);
        if (!built)
            return std::unexpected(built.error());
        auto layout = layout_options(options, built->target);
        if (!layout)
        {
            std::println(std::cerr, "dcc: error: {}", layout.error());
            return std::unexpected(1);
        }
        auto directory = fs::temp_directory_path() / std::format("dcc-i8086-legacy-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::error_code ec;
        fs::create_directories(directory, ec);
        struct Cleanup
        {
            fs::path path;
            ~Cleanup()
            {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } cleanup{directory};
        auto object = directory / "input.o";
        {
            std::ofstream out{object, std::ios::binary};
            out.write(reinterpret_cast<char const*>(built->artifact.object_bytes->data()), static_cast<std::streamsize>(built->artifact.object_bytes->size()));
        }
        auto binary = directory / "image.bin";
        dcc::backend::i8086::LinkOptions link;
        link.base.segment = layout->segment;
        link.base.offset = layout->offset;
        link.stack_reserve = layout->stack_reserve;
        link.model = layout->model;
        auto linked = dcc::backend::i8086::link_flat_binary({object.string()}, binary.string(), link, {});
        if (!linked)
        {
            std::println(std::cerr, "dcc: error: backend: linking failed:\n{}", linked.error());
            return std::unexpected(1);
        }
        return read_bytes(binary);
    }

    int self_check()
    {
        std::error_code ec;
        auto directory = fs::temp_directory_path(ec) / std::format("dcc-i8086-layout-self-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(directory, ec);
        struct Cleanup
        {
            fs::path path;
            ~Cleanup()
            {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } cleanup{directory};
        auto source = directory / "main.dc";
        {
            std::ofstream out{source};
            out << "module main;\n"
                   "@nomangle public u16 value = 19;\n"
                   "@nomangle public u16^ pointer = &value;\n"
                   "@nomangle public u16[97] zero;\n"
                   "@nomangle public i32 dcc_main() { return *pointer as i32; }\n";
        }
        int checks = 0;
        int failed = 0;
        for (std::string_view model : {"small", "unreal", "unreal32"})
            for (bool sections : {false, true})
            {
                Options options;
                options.source = source;
                options.output = directory / "native.bin";
                options.model = std::string{model};
                options.base = "1000:0000";
                options.function_sections = sections;
                options.data_sections = sections;
                options.opt = dcc::ir::pass::OptLevel::O2;
                auto native = native_image(options);
                auto legacy = legacy_image(options);
                ++checks;
                if (!native || !legacy || native->empty() || *native != *legacy)
                {
                    ++failed;
                    std::println(std::cerr, "    FAIL  i8086 flat layout image differs from the ld.lld image (model {}, sections {})", model, sections);
                }
            }
        std::println("  RESULT  {}/{} i8086 flat layout checks passed", checks - failed, checks);
        return failed == 0 ? 0 : 1;
    }

} // namespace

int main(int argc, char** argv)
{
    if (argc == 1)
        return self_check();

    auto options = parse(argc, argv);
    if (!options)
    {
        std::println(std::cerr, "usage: i8086_layout_image [-mcmodel=MODEL] [-farch=CPU] [-O0|-O2|-Os] [-ffunction-sections] [-fdata-sections] "
                                "[-fbase=SEG:OFF] [-fstack-reserve=N] -o OUTPUT SOURCE");
        return 2;
    }

    auto image = native_image(*options);
    if (!image)
        return image.error();

    if (!write_bytes(options->output, *image))
    {
        std::println(std::cerr, "dcc: error: cannot write '`{}`'", options->output.string());
        return 1;
    }

    return 0;
}
