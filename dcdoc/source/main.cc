import std;
import dcdoc.model;
import dcdoc.builder;

auto main(int argc, char** argv) -> int
{
    std::vector<std::filesystem::path> roots;
    std::filesystem::path entry;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg{argv[i]};
        if ((arg == "--root" || arg == "-I") && i + 1 < argc)
            roots.emplace_back(argv[++i]);
        else if (arg.starts_with("--root="))
            roots.emplace_back(arg.substr(7));
        else if (arg.starts_with("-I"))
            roots.emplace_back(arg.substr(2));
        else if (entry.empty())
            entry = arg;
        else
            return 2;
    }

    if (entry.empty())
        return 2;

    if (roots.empty() && entry.has_parent_path())
        roots.push_back(entry.parent_path());

    dcdoc::Builder builder{entry, std::move(roots)};
    dcdoc::Project project = builder.build();
    if (project.modules.empty())
        return 1;

    for (auto const& e : project.file_errors)
        std::println(std::cerr, "dcdoc: {}: {}", e.file, e.message);

    std::print("{}", dcdoc::dump(project));

    return 0;
}
