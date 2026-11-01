export module dpm.build;

import std;
import dpm.cache;
import dpm.manifest;
import dpm.settings;

export namespace dpm::build
{
    using manifest::Kind;
    using manifest::Product;
    using manifest::Project;
    using manifest::Settings;
    using manifest::Target;

    struct PackageRoot
    {
        std::string location;
        std::filesystem::path source_dir;
        bool dev{};
    };

    struct Request
    {
        Project const* project{};
        std::filesystem::path project_root{"."};
        std::vector<PackageRoot> packages;
        std::string profile{"debug"};
        settings::Overrides overrides;
        std::filesystem::path dcc{"dcc"};
        bool include_dev{};
    };

    struct Plan
    {
        std::vector<std::string> argv;
        std::filesystem::path output;
        std::filesystem::path depfile;
        std::filesystem::path compdb_entry;
        std::vector<std::filesystem::path> extra_inputs;
        Settings settings;
    };

    [[nodiscard]] inline std::optional<Target> parse_target(std::string_view name)
    {
        for (auto target : {Target::Host, Target::X86_64Elf, Target::X86_64Coff, Target::X86Elf, Target::X86Coff, Target::I8086Binary, Target::I8086Mz})
        {
            auto triple = settings::triple(target);
            if (name == (triple.empty() ? std::string_view{"host"} : triple))
                return target;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::string target_dir(Target target)
    {
        auto triple = settings::triple(target);
        return triple.empty() ? "host" : std::string{triple};
    }

    [[nodiscard]] inline bool host_is_windows()
    {
#if defined(_WIN32)
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] inline std::string artifact_name(Product const& product, Target target)
    {
        bool const windows = target == Target::X86_64Coff || target == Target::X86Coff || (target == Target::Host && host_is_windows());
        bool const flat = target == Target::I8086Binary;
        if (product.kind == Kind::SharedLibrary)
            return windows ? product.name + ".dll" : "lib" + product.name + ".so";
        if (flat)
            return product.name + ".bin";
        if (target == Target::I8086Mz)
            return product.name + ".exe";
        return windows ? product.name + ".exe" : product.name;
    }

    [[nodiscard]] inline std::filesystem::path output_dir(std::string const& profile, Target target)
    {
        return std::filesystem::path{".dpm"} / "out" / profile / target_dir(target);
    }

    namespace detail
    {
        inline std::vector<std::string> import_roots(Request const& request)
        {
            std::vector<std::string> roots;
            std::vector<PackageRoot const*> sorted;
            for (auto const& p : request.packages)
                if (request.include_dev || !p.dev)
                    sorted.push_back(&p);
            std::ranges::sort(sorted, [](auto const* a, auto const* b) { return a->location < b->location; });
            for (auto const* p : sorted)
                roots.push_back("-I" + p->source_dir.generic_string());
            roots.push_back("-I" + (request.project_root / request.project->source).lexically_normal().generic_string());
            return roots;
        }

        inline Plan compose(Request const& request, Settings const& effective, std::string const& entry, std::filesystem::path const& output,
                            std::vector<std::string> const& extra_product_flags)
        {
            Plan plan;
            plan.settings = effective;
            plan.output = output;
            plan.depfile = output;
            plan.depfile += ".d";
            plan.compdb_entry = output;
            plan.compdb_entry += ".compdb.json";
            plan.argv.push_back(request.dcc.generic_string());
            plan.argv.push_back(entry);
            plan.argv.push_back("-o");
            plan.argv.push_back(output.generic_string());
            for (auto& root : import_roots(request))
                plan.argv.push_back(std::move(root));
            plan.argv.push_back("--depfile");
            plan.argv.push_back(plan.depfile.generic_string());
            plan.argv.push_back("--compdb-entry");
            plan.argv.push_back(plan.compdb_entry.generic_string());
            for (auto const& flag : extra_product_flags)
                plan.argv.push_back(flag);
            for (auto const& flag : settings::dcc_flags(effective))
                plan.argv.push_back(flag);
            return plan;
        }

    } // namespace detail

    [[nodiscard]] inline Plan plan_product(Request const& request, Product const& product)
    {
        auto effective = settings::effective(*request.project, request.profile, &product, request.overrides);
        auto output = output_dir(request.profile, effective.target) / artifact_name(product, effective.target);
        auto entry = (std::filesystem::path{request.project_root} / product.entry).lexically_normal().generic_string();
        auto flags = settings::product_flags(product);
        std::vector<std::filesystem::path> extra_inputs;
        if (!product.linker_script.empty())
        {
            auto script = (std::filesystem::path{request.project_root} / product.linker_script).lexically_normal();
            flags.emplace_back("-T");
            flags.push_back(script.generic_string());
            extra_inputs.push_back(script);
        }
        auto plan = detail::compose(request, effective, entry, output, flags);
        plan.extra_inputs = std::move(extra_inputs);
        return plan;
    }

    [[nodiscard]] inline Plan plan_test(Request const& request, std::filesystem::path const& test_file)
    {
        auto effective = settings::effective(*request.project, request.profile, nullptr, request.overrides);
        bool const windows =
            effective.target == Target::X86_64Coff || effective.target == Target::X86Coff || (effective.target == Target::Host && host_is_windows());
        auto name = test_file.stem().string() + (windows ? ".exe" : "");
        auto output = output_dir(request.profile, effective.target) / "tests" / name;
        return detail::compose(request, effective, test_file.generic_string(), output, {});
    }

    [[nodiscard]] inline std::optional<std::vector<PackageRoot>> package_roots(manifest::Lock const& lock, cache::Cache const& cache,
                                                                               std::filesystem::path const& project_root,
                                                                               std::filesystem::path const& schema_root, bool allow_file_urls,
                                                                               std::string& error)
    {
        std::vector<PackageRoot> roots;
        for (auto const& p : lock.packages)
        {
            std::filesystem::path root;
            if (p.location.starts_with("path:"))
                root = (project_root / p.location.substr(5)).lexically_normal();
            else
                root = cache.src_path(p.hash);
            std::ifstream in{root / "project.dcon", std::ios::binary};
            if (!in)
            {
                error = std::format("{}@{} is not available at {}", p.location, p.version, root.string());
                return std::nullopt;
            }
            std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            manifest::LoadOptions options;
            options.file_name = (root / "project.dcon").string();
            options.schema_root = schema_root;
            options.project_root = root;
            options.allow_file_urls = allow_file_urls;
            auto loaded = manifest::load(text, options);
            if (!loaded.ok || !loaded.project)
            {
                error = std::format("{}: {}", p.location, loaded.diagnostics.empty() ? "invalid manifest" : loaded.diagnostics.front().format());
                return std::nullopt;
            }
            roots.push_back({p.location, (root / loaded.project->source).lexically_normal(), p.dev});
        }
        return roots;
    }

} // namespace dpm::build
