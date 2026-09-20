import std;
import dcdoc.model;
import dcdoc.builder;
import dcdoc.typst.emit;

auto main(int argc, char** argv) -> int
{
    std::vector<std::filesystem::path> roots;
    std::filesystem::path entry;
    std::filesystem::path pdf_out;
    std::filesystem::path typ_out;
    bool dump_model = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg{argv[i]};
        if (arg == "--dump-model")
            dump_model = true;
        else if (arg == "--emit-typ" && i + 1 < argc)
            typ_out = argv[++i];
        else if (arg == "--pdf" && i + 1 < argc)
            pdf_out = argv[++i];
        else if ((arg == "--root" || arg == "-I") && i + 1 < argc)
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

    if (dump_model || (pdf_out.empty() && typ_out.empty()))
        std::print("{}", dcdoc::dump(project));

    if (!typ_out.empty() || !pdf_out.empty())
    {
        std::string typ = dcdoc::typst::render(project);
        std::filesystem::path typ_path = typ_out.empty() ? std::filesystem::path{pdf_out.string() + ".typ"} : typ_out;
        std::ofstream out{typ_path};
        if (!out)
        {
            std::println(std::cerr, "dcdoc: cannot write {}", typ_path.string());
            return 1;
        }

        out << typ;
        out.close();
        if (!pdf_out.empty())
            return dcdoc::typst::compile_pdf(typ_path, pdf_out);
    }

    return 0;
}
