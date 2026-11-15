import std;
import dpm.build;
import dpm.compdb;
import dpm.manifest;
import dpm.settings;

#include "harness.hh"

namespace
{
    namespace m = dpm::manifest;
    namespace b = dpm::build;

    std::vector<std::string> strs(std::initializer_list<std::string> items)
    {
        return items;
    }

    m::Project base_project()
    {
        m::Project p;
        p.name = "app";
        p.version = "1.0.0";
        p.source = "src";
        m::Product product;
        product.name = "app";
        product.entry = "src/main.dc";
        p.products.push_back(product);
        return p;
    }

    b::Request request_for(m::Project const& project)
    {
        b::Request r;
        r.project = &project;
        r.dcc = "dcc";
        return r;
    }
} // namespace

SECTION("dpm build plans");

TEST_CASE("the debug host executable plan")
{
    auto project = base_project();
    auto plan = b::plan_product(request_for(project), project.products[0]);
    CHECK(plan.argv == strs({"dcc", "src/main.dc", "-o", ".dpm/out/debug/host/app", "-Isrc", "--depfile", ".dpm/out/debug/host/app.d", "--compdb-entry",
                             ".dpm/out/debug/host/app.compdb.json", "-O0", "-fwhole-program", "-flibdcext", "-g", "-fbounds-check"}));
    CHECK_EQ(plan.output.generic_string(), std::string{".dpm/out/debug/host/app"});
}

TEST_CASE("a linker script becomes -T before the settings and is tracked as an extra input")
{
    auto project = base_project();
    project.products[0].linker_script = "link/kernel.ld";
    project.products[0].settings.target = m::Target::X86_64Elf;
    project.products[0].settings.red_zone = m::Tri::Off;
    auto plan = b::plan_product(request_for(project), project.products[0]);
    CHECK(plan.argv == strs({"dcc", "src/main.dc", "-o", ".dpm/out/debug/x86_64-elf/app", "-Isrc", "--depfile", ".dpm/out/debug/x86_64-elf/app.d", "--compdb-entry",
                             ".dpm/out/debug/x86_64-elf/app.compdb.json", "-T", "link/kernel.ld", "-target=x86_64-elf", "-O0", "-fwhole-program", "-g", "-fbounds-check",
                             "-fno-red-zone"}));
    REQUIRE(plan.extra_inputs.size() == std::size_t{1});
    CHECK_EQ(plan.extra_inputs[0].generic_string(), std::string{"link/kernel.ld"});
    auto without = b::plan_product(request_for(base_project()), base_project().products[0]);
    CHECK(without.extra_inputs.empty());
    CHECK(std::ranges::find(without.argv, "-T") == without.argv.end());
}

TEST_CASE("a linker script is resolved against the project root and normalized")
{
    auto project = base_project();
    project.products[0].linker_script = "./a/../link/k.ld";
    auto r = request_for(project);
    r.project_root = "/work/proj";
    auto plan = b::plan_product(r, project.products[0]);
    auto at = std::ranges::find(plan.argv, "-T");
    REQUIRE(at != plan.argv.end());
    CHECK_EQ(*(at + 1), std::string{"/work/proj/link/k.ld"});
    CHECK_EQ(plan.extra_inputs[0].generic_string(), std::string{"/work/proj/link/k.ld"});
}

TEST_CASE("product library paths runtime paths and link mode become dcc link flags")
{
    auto project = base_project();
    project.products[0].link = {"SDL3"};
    project.products[0].library_paths = {"native libs/./x64", "/opt/sdl 3/lib"};
    project.products[0].runtime_paths = {"$ORIGIN/../lib", "/opt/sdl 3/lib"};
    project.products[0].link_mode = m::LinkMode::Dynamic;
    auto r = request_for(project);
    r.project_root = "/work/proj";
    auto plan = b::plan_product(r, project.products[0]);
    auto at = std::ranges::find(plan.argv, "-lSDL3");
    REQUIRE(at != plan.argv.end());
    CHECK(std::vector<std::string>(at, at + 8) ==
          strs({"-lSDL3", "-L/work/proj/native libs/x64", "-L/opt/sdl 3/lib", "-rpath", "$ORIGIN/../lib", "-rpath", "/opt/sdl 3/lib", "-dynamic"}));
    project.products[0].link_mode = m::LinkMode::Static;
    auto static_plan = b::plan_product(r, project.products[0]);
    CHECK(std::ranges::find(static_plan.argv, "-static") != static_plan.argv.end());
    CHECK(std::ranges::find(static_plan.argv, "-dynamic") == static_plan.argv.end());
    auto plain = b::plan_product(request_for(base_project()), base_project().products[0]);
    CHECK(std::ranges::none_of(plain.argv, [](std::string const& a) { return a == "-static" || a == "-dynamic" || a == "-rpath" || a.starts_with("-L"); }));
}

TEST_CASE("the release plan uses release defaults")
{
    auto project = base_project();
    auto r = request_for(project);
    r.profile = "release";
    auto plan = b::plan_product(r, project.products[0]);
    CHECK(plan.argv == strs({"dcc", "src/main.dc", "-o", ".dpm/out/release/host/app", "-Isrc", "--depfile", ".dpm/out/release/host/app.d", "--compdb-entry",
                             ".dpm/out/release/host/app.compdb.json", "-O2", "-fwhole-program", "-flibdcext"}));
}

TEST_CASE("dependency roots are sorted by location, exclude dev packages for builds and precede the project root")
{
    auto project = base_project();
    auto r = request_for(project);
    r.packages = {{"path:../a", "../a/src", false}, {"github.com/u/c", "/cache/src/cc/src", true}, {"github.com/u/b", "/cache/src/bb/lib", false}};
    auto plan = b::plan_product(r, project.products[0]);
    CHECK(plan.argv[4] == "-I/cache/src/bb/lib");
    CHECK(plan.argv[5] == "-I../a/src");
    CHECK(plan.argv[6] == "-Isrc");
    CHECK(plan.argv[7] == "--depfile");
    r.include_dev = true;
    auto with_dev = b::plan_product(r, project.products[0]);
    CHECK(with_dev.argv[4] == "-I/cache/src/bb/lib");
    CHECK(with_dev.argv[5] == "-I/cache/src/cc/src");
    CHECK(with_dev.argv[6] == "-I../a/src");
    CHECK(with_dev.argv[7] == "-Isrc");
}

TEST_CASE("targets choose the output directory and artifact name")
{
    auto project = base_project();
    project.products[0].settings.target = m::Target::I8086Mz;
    project.products[0].settings.base = "0800:0100";
    auto plan = b::plan_product(request_for(project), project.products[0]);
    CHECK_EQ(plan.output.generic_string(), std::string{".dpm/out/debug/i8086-mz/app.exe"});
    CHECK(plan.argv == strs({"dcc", "src/main.dc", "-o", ".dpm/out/debug/i8086-mz/app.exe", "-Isrc", "--depfile", ".dpm/out/debug/i8086-mz/app.exe.d", "--compdb-entry",
                             ".dpm/out/debug/i8086-mz/app.exe.compdb.json", "-target=i8086-mz", "-O0", "-fwhole-program", "-g", "-fbounds-check", "-fbase=0800:0100"}));
    auto flat = base_project();
    flat.products[0].settings.target = m::Target::I8086Binary;
    CHECK_EQ(b::plan_product(request_for(flat), flat.products[0]).output.generic_string(), std::string{".dpm/out/debug/i8086-binary/app.bin"});
    auto windows = base_project();
    windows.products[0].settings.target = m::Target::X86_64Coff;
    auto wplan = b::plan_product(request_for(windows), windows.products[0]);
    CHECK_EQ(wplan.output.generic_string(), std::string{".dpm/out/debug/x86_64-coff/app.exe"});
    CHECK(std::ranges::find(wplan.argv, "-flibdcext") == wplan.argv.end());
    CHECK(std::ranges::find(wplan.argv, "-target=x86_64-coff") != wplan.argv.end());
    auto elf = base_project();
    elf.products[0].settings.target = m::Target::X86_64Elf;
    elf.products[0].settings.libdcext = m::Libdcext::Freestanding;
    auto eplan = b::plan_product(request_for(elf), elf.products[0]);
    CHECK_EQ(eplan.output.generic_string(), std::string{".dpm/out/debug/x86_64-elf/app"});
    CHECK(std::ranges::find(eplan.argv, "-flibdcext=freestanding") != eplan.argv.end());
}

TEST_CASE("shared libraries carry their link flags before the settings and extra flags come last")
{
    auto project = base_project();
    project.products[0].kind = m::Kind::SharedLibrary;
    project.products[0].link = {"m", "z"};
    project.settings.extra_flags = {"-fno-simd"};
    project.products[0].settings.extra_flags = {"-fno-x87"};
    auto plan = b::plan_product(request_for(project), project.products[0]);
    CHECK_EQ(plan.output.generic_string(), std::string{".dpm/out/debug/host/libapp.so"});
    CHECK(plan.argv == strs({"dcc", "src/main.dc", "-o", ".dpm/out/debug/host/libapp.so", "-Isrc", "--depfile", ".dpm/out/debug/host/libapp.so.d", "--compdb-entry",
                             ".dpm/out/debug/host/libapp.so.compdb.json", "-shared", "-lm", "-lz", "-O0", "-fwhole-program", "-flibdcext", "-g", "-fbounds-check", "-fno-simd", "-fno-x87"}));
}

TEST_CASE("custom profiles inherit and command line overrides win")
{
    auto project = base_project();
    m::Profile fast;
    fast.name = "fast";
    fast.inherits = m::ProfileBase::Release;
    fast.settings.optimize = m::Optimize::Size;
    project.profiles.push_back(fast);
    auto r = request_for(project);
    r.profile = "fast";
    auto plan = b::plan_product(r, project.products[0]);
    CHECK_EQ(plan.output.generic_string(), std::string{".dpm/out/fast/host/app"});
    CHECK(std::ranges::find(plan.argv, "-Os") != plan.argv.end());
    CHECK(std::ranges::find(plan.argv, "-fwhole-program") != plan.argv.end());
    r.overrides.optimize = m::Optimize::O1;
    r.overrides.target = m::Target::X86Elf;
    auto overridden = b::plan_product(r, project.products[0]);
    CHECK(std::ranges::find(overridden.argv, "-O1") != overridden.argv.end());
    CHECK(std::ranges::find(overridden.argv, "-target=x86-elf") != overridden.argv.end());
    CHECK_EQ(overridden.output.generic_string(), std::string{".dpm/out/fast/x86-elf/app"});
}

TEST_CASE("test plans compile the test file with dev roots and no product flags")
{
    auto project = base_project();
    project.products[0].kind = m::Kind::SharedLibrary;
    auto r = request_for(project);
    r.include_dev = true;
    r.packages = {{"github.com/u/c", "/cache/src/cc/src", true}};
    auto plan = b::plan_test(r, "tests/basic.dc");
    CHECK(plan.argv == strs({"dcc", "tests/basic.dc", "-o", ".dpm/out/debug/host/tests/basic", "-I/cache/src/cc/src", "-Isrc", "--depfile",
                             ".dpm/out/debug/host/tests/basic.d", "--compdb-entry", ".dpm/out/debug/host/tests/basic.compdb.json", "-O0", "-fwhole-program", "-flibdcext", "-g", "-fbounds-check"}));
}

TEST_CASE("targets parse from triples")
{
    CHECK(b::parse_target("x86_64-elf") == m::Target::X86_64Elf);
    CHECK(b::parse_target("i8086-mz") == m::Target::I8086Mz);
    CHECK(b::parse_target("host") == m::Target::Host);
    CHECK(!b::parse_target("arm64-linux").has_value());
}

TEST_CASE("package native libraries propagate to products and tests in dependency order")
{
    auto project = base_project();
    project.link = {"root", "repeat", "repeat"};
    project.products[0].link = {"product"};
    auto r = request_for(project);
    r.packages = {
        {"a", "/a/src", false, {"leaf"}, {}},
        {"z", "/z/src", false, {"binding"}, {"a"}},
        {"dev", "/dev/src", true, {"devlib"}, {"z"}},
    };
    auto libraries = [](b::Plan const& plan)
    {
        std::vector<std::string> result;
        for (auto const& arg : plan.argv)
            if (arg.starts_with("-l"))
                result.push_back(arg);
        return result;
    };
    CHECK(libraries(b::plan_product(r, project.products[0])) == strs({"-lproduct", "-lroot", "-lrepeat", "-lrepeat", "-lbinding", "-lleaf"}));
    CHECK(libraries(b::plan_test(r, "tests/basic.dc")) == strs({"-lroot", "-lrepeat", "-lrepeat", "-lbinding", "-lleaf"}));
    r.include_dev = true;
    CHECK(libraries(b::plan_test(r, "tests/basic.dc")) == strs({"-lroot", "-lrepeat", "-lrepeat", "-ldevlib", "-lbinding", "-lleaf"}));
    project.products[0].kind = m::Kind::SharedLibrary;
    CHECK(libraries(b::plan_product(r, project.products[0])) == strs({"-lproduct", "-lroot", "-lrepeat", "-lrepeat", "-ldevlib", "-lbinding", "-lleaf"}));
    std::ranges::reverse(r.packages);
    CHECK(libraries(b::plan_test(r, "tests/basic.dc")) == strs({"-lroot", "-lrepeat", "-lrepeat", "-ldevlib", "-lbinding", "-lleaf"}));
}

SECTION("dpm compile commands");

TEST_CASE("compile commands plan every product before the first lock and write nothing")
{
    char const* schema = std::getenv("DCC_TEST_DPM_SCHEMA");
    REQUIRE(schema != nullptr);
    auto const root = std::filesystem::temp_directory_path() /
                      std::format("dpm_compdb_{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), std::random_device{}());
    std::filesystem::create_directories(root / "src");
    for (auto const* name : {"main.dc", "tool.dc"})
        std::ofstream{root / "src" / name} << "module main;\n";
    {
        std::ofstream out{root / "project.dcon"};
        out << "dpm::v1::Project {\n    name = \"app\",\n    version = \"0.1.0\",\n    products = {\n"
               "        { name = \"app\", entry = \"src/main.dc\" },\n        { name = \"tool\", entry = \"src/tool.dc\" },\n    },\n"
               "    dependencies = {\n        { location = \"github.com/u/a\", version = \"1.2.0\" },\n    },\n}\n";
    }
    dpm::compdb::Options options;
    options.schema_root = schema;
    options.cache_root = root / "cache";
    auto const result = dpm::compdb::plan(root, options);
    CHECK(result.ok);
    REQUIRE(result.entries.size() == 2);
    CHECK_EQ(result.entries[0].product, std::string{"app"});
    CHECK_EQ(result.entries[1].product, std::string{"tool"});
    CHECK(result.entries[0].file == root / "src" / "main.dc");
    CHECK(std::ranges::contains(result.entries[0].arguments, std::string{"-flibdcext"}));
    CHECK(std::ranges::any_of(result.problems, [](std::string const& p) { return p.find("project.lock.dcon does not exist yet") != std::string::npos; }));
    CHECK(!dpm::compdb::inputs_changed(result.inputs));
    std::vector<std::string> names;
    for (auto const& entry : std::filesystem::directory_iterator{root})
        names.push_back(entry.path().filename().string());
    std::ranges::sort(names);
    CHECK(names == strs({"project.dcon", "src"}));
    {
        std::ofstream out{root / "project.lock.dcon"};
        out << "dpm::v1::Lock {}\n";
    }
    CHECK(dpm::compdb::inputs_changed(result.inputs));
    std::filesystem::remove_all(root);
}
