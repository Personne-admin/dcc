#include <cstdio>

import std;
import dcc.target;
import dcc.backend.i8086.link;

namespace
{
    struct Options
    {
        std::vector<std::string> inputs;
        std::filesystem::path output;
        std::string base{"?:0000"};
        std::string stack_reserve;
        std::string model{"small"};
        std::vector<std::string> extra_args;
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
            else if (argument.starts_with("-fbase="))
                options.base = std::string{argument.substr(7)};
            else if (argument.starts_with("-fstack-reserve="))
                options.stack_reserve = std::string{argument.substr(16)};
            else if (argument.starts_with("-mcmodel="))
                options.model = std::string{argument.substr(9)};
            else if (argument.starts_with("-Wl,"))
            {
                auto value = argument.substr(4);
                for (;;)
                {
                    auto comma = value.find(',');
                    options.extra_args.emplace_back(value.substr(0, comma));
                    if (comma == std::string_view::npos)
                        break;
                    value.remove_prefix(comma + 1);
                }
            }
            else if (argument.starts_with("-"))
                return std::nullopt;
            else
                options.inputs.push_back(std::string{argument});
        }
        if (options.inputs.empty() || options.output.empty())
            return std::nullopt;
        return options;
    }

    int self_check()
    {
        auto marker = dcc::backend::i8086::model_marker(dcc::target::CodeModel::Small);
        auto script = dcc::backend::i8086::linker_script(dcc::backend::i8086::LinkOptions{});
        bool const ok = !marker.empty() && script.find("OUTPUT_FORMAT(binary)") != std::string::npos && script.find(".start") != std::string::npos;
        std::println("  RESULT  {}/1 i8086 link oracle checks passed", ok ? 1 : 0);
        return ok ? 0 : 1;
    }

} // namespace

int main(int argc, char** argv)
{
    if (argc == 1)
        return self_check();

    auto options = parse(argc, argv);
    if (!options)
    {
        std::println(std::cerr, "usage: i8086_link_oracle [-mcmodel=MODEL] [-fbase=SEG:OFF] [-fstack-reserve=N] [-Wl,ARG]... -o OUTPUT INPUT...");
        return 2;
    }
    dcc::backend::i8086::LinkOptions link;
    link.model = options->model == "unreal"     ? dcc::target::CodeModel::Unreal
                 : options->model == "unreal32" ? dcc::target::CodeModel::Unreal32
                 : options->model == "default"  ? dcc::target::CodeModel::Default
                                                : dcc::target::CodeModel::Small;
    auto base = dcc::backend::i8086::parse_link_base(options->base);
    if (!base)
    {
        std::println(std::cerr, "dcc: error: {}", base.error());
        return 1;
    }
    link.base = *base;
    if (!options->stack_reserve.empty())
    {
        auto reserve = dcc::backend::i8086::parse_stack_reserve(options->stack_reserve);
        if (!reserve)
        {
            std::println(std::cerr, "dcc: error: {}", reserve.error());
            return 1;
        }
        link.stack_reserve = *reserve;
    }
    auto linked = dcc::backend::i8086::link_flat_binary(options->inputs, options->output.string(), link, options->extra_args);
    if (!linked)
    {
        std::println(std::cerr, "dcc: error: linking failed:\n{}", linked.error());
        return 1;
    }
    return 0;
}
