import std;
import dcdoc.model;
import dcdoc.builder;
import dcdoc.typst.emit;
import dcdoc.markdown.emit;

auto main(int argc, char** argv) -> int
{
    std::vector<std::filesystem::path> roots;
    std::filesystem::path entry;
    std::filesystem::path pdf_out;
    std::filesystem::path typ_out;
    std::filesystem::path md_out;
    bool md_is_dir = false;
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
        else if (arg == "--markdown" && i + 1 < argc)
        {
            std::string_view raw{argv[++i]};
            md_is_dir = raw.ends_with("/");
            md_out = raw;
        }
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

    dcdoc::Builder builder{entry, std::move(roots), argv[0]};
    dcdoc::Project project = builder.build();
    if (project.modules.empty())
        return 1;

    for (auto const& e : project.file_errors)
        std::println(std::cerr, "dcdoc: {}: {}", e.file, e.message);

    if (dump_model || (pdf_out.empty() && typ_out.empty() && md_out.empty()))
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
        {
            int rc = dcdoc::typst::compile_pdf(typ_path, pdf_out);
            if (rc != 0)
                return rc;
        }
    }

    if (!md_out.empty())
    {
        std::error_code ec;
        bool is_dir = md_is_dir || (std::filesystem::exists(md_out, ec) && std::filesystem::is_directory(md_out, ec));
        int rc = dcdoc::markdown::write_markdown(project, md_out, is_dir);
        if (rc != 0)
            return rc;
    }

    return 0;
}
