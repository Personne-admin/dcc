import std;
import dcc.config;
import dpm.manifest;

namespace
{
    constexpr int k_exit_usage = 2;
    constexpr int k_exit_manifest = 3;

    std::filesystem::path schema_root(char** argv)
    {
        if (char const* env = std::getenv("DPM_SCHEMA_ROOT"))
            return env;
        return dcc::config::current_prefix(argv).path / "include";
    }

    int run_check(char** argv, std::span<char const* const> args)
    {
        bool tree = false;
        std::filesystem::path file{"project.dcon"};
        for (auto arg : args)
        {
            if (std::string_view{arg} == "--tree")
                tree = true;
            else if (std::string_view{arg}.starts_with("-"))
            {
                std::println(std::cerr, "dpm check: unknown option {}", arg);
                return k_exit_usage;
            }
            else
                file = arg;
        }
        std::ifstream in{file, std::ios::binary};
        if (!in)
        {
            std::println(std::cerr, "dpm: cannot read {}", file.string());
            return k_exit_manifest;
        }
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        dpm::manifest::LoadOptions options;
        options.file_name = file.string();
        options.schema_root = schema_root(argv);
        options.project_root = file.has_parent_path() ? file.parent_path() : std::filesystem::path{"."};
        auto result = dpm::manifest::load(text, options);
        for (auto const& diagnostic : result.diagnostics)
            std::println(std::cerr, "{}", diagnostic.format());
        if (!result.ok)
            return k_exit_manifest;
        if (tree)
            std::print("{}", result.tree.dump(true));
        return 0;
    }
} // namespace

auto main(int argc, char** argv) -> int
{
    if (argc >= 2 && std::string_view{argv[1]} == "check")
        return run_check(argv, std::span<char const* const>{argv + 2, static_cast<std::size_t>(argc - 2)});
    std::println(std::cerr, "usage: dpm check [--tree] [project.dcon]");
    return k_exit_usage;
}
