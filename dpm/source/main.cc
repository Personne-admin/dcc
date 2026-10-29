import std;
import dpm.manifest;

namespace
{
    int run_spike(std::filesystem::path const& file, std::filesystem::path const& schema_root)
    {
        std::ifstream in{file, std::ios::binary};
        if (!in)
        {
            std::println(std::cerr, "dpm: cannot read {}", file.string());
            return 3;
        }
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        auto result = dpm::manifest::spike_evaluate(text, schema_root);
        for (auto const& error : result.errors)
            std::println(std::cerr, "{}:{}", file.string(), error);
        if (!result.ok)
            return 3;
        std::print("{}", result.tree);
        return 0;
    }
} // namespace

auto main(int argc, char** argv) -> int
{
    if (argc == 4 && std::string_view{argv[1]} == "--spike")
        return run_spike(argv[2], argv[3]);
    std::println(std::cerr, "usage: dpm --spike <project.dcon> <schema-root>");
    return 2;
}
