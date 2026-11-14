import std;
#include "harness.hh"
#include "stdlib_os.hh"

TEST_CASE("spawn options preserve child configuration and distinguish exec failures")
{
    auto source = os_test::fixture("process-options.dc");
    REQUIRE(!source.empty());
    for (auto backend : {"llvm", "custom"})
        for (auto opt : {"-O0", "-O2"})
            CHECK_EQ(os_test::run(source, false, std::format("-fbackend {} {}", backend, opt)).status, 0);
}

TEST_CASE("owning commands preserve arguments, lifecycle and allocation failure cleanup")
{
    auto source = os_test::fixture("command.dc");
    REQUIRE(!source.empty());
    for (auto backend : {"llvm", "custom"})
        for (auto opt : {"-O0", "-O2"})
            for (auto mode : {"", "-fwhole-program"})
            {
                auto flags = std::format("-fbackend {} {} {}", backend, opt, mode);
                CHECK_EQ(os_test::run(source, false, flags).status, 0);
                CHECK_EQ(os_test::run(os_test::fixture("command-error.dc"), false, flags).status, 0);
            }
}

TEST_CASE("commands support Windows code generation and freestanding import isolation")
{
    auto root = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path().parent_path();
    auto dir = std::filesystem::temp_directory_path() /
               std::format("dcc-command-{}-{}", getpid(), std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{dir};
    auto source = dir / "main.dc";
    { std::ofstream out{source}; out << os_test::fixture("command-windows.dc"); }
    for (auto backend : {"llvm", "custom"})
        for (auto opt : {"-O0", "-O2"})
        {
            auto object = dir / "main.o";
            auto command = std::format("timeout 60 {} -flibdcext windows -target x86_64-coff -fwhole-program -fbackend {} {} -c {} -o {}",
                                       os_test::quote(root / "bin/dcc"), backend, opt, os_test::quote(source), os_test::quote(object));
            CHECK_EQ(std::system(command.c_str()), 0);
            CHECK(std::filesystem::exists(object));
        }
    { std::ofstream out{source}; out << "module main; import std::process; @nomangle public i32 entry() { return 0; }"; }
    for (auto backend : {"llvm", "custom"})
    {
        auto object = dir / "freestanding.o";
        auto command = std::format("timeout 60 {} -target x86_64-elf -fno-prelude -I{} -fwhole-program -fbackend {} -c {} -o {}",
                                   os_test::quote(root / "bin/dcc"), os_test::quote(root / "include"), backend,
                                   os_test::quote(source), os_test::quote(object));
        CHECK_EQ(std::system(command.c_str()), 0);
    }
}

TEST_CASE("documented command example builds and launches a child")
{
    auto root = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path().parent_path().parent_path();
    auto text = os_test::read(root / "docs/process.md");
    auto start = text.find("```dc\n");
    REQUIRE(start != std::string::npos);
    start += 6;
    auto end = text.find("```", start);
    REQUIRE(end != std::string::npos);
    auto result = os_test::run(text.substr(start, end - start));
    CHECK_EQ(result.status, 0);
    CHECK(result.out.starts_with("git version "));
}
