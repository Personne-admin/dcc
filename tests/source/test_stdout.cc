import std;
#include "harness.hh"
#include "stdlib_os.hh"

TEST_CASE("stdout formatting supports hooks and long output while legacy printing stays on stderr")
{
    auto source = os_test::fixture("stdout-print.dc");
    REQUIRE(!source.empty());
    auto expected = "Hello, world!\n42 true 1.5 !\nbadge(7)\n" + std::string(4096, 'x') + "\nend\n\n";
    for (auto backend : {"llvm", "custom"})
    {
#if !DCC_ENABLE_LLVM
        if (backend == std::string_view{"llvm"}) continue;
#endif
        for (auto opt : {"-O0", "-O2"})
            for (auto mode : {"", "-fwhole-program"})
            {
                auto run = os_test::run(source, false, std::format("-fbackend {} {} {}", backend, opt, mode));
                CHECK_EQ(run.status, 0);
                CHECK_EQ(run.out, expected);
                CHECK_EQ(run.err, "legacy\n");
            }
    }
}

TEST_CASE("stdout errors preserve formatting and OS details through partial writes and propagation")
{
    auto source = os_test::fixture("stdout-errors.dc");
    REQUIRE(!source.empty());
    for (auto backend : {"llvm", "custom"})
    {
#if !DCC_ENABLE_LLVM
        if (backend == std::string_view{"llvm"}) continue;
#endif
        for (auto opt : {"-O0", "-O2"})
            for (auto mode : {"", "-fwhole-program"})
            {
                auto run = os_test::run(source, false, std::format("-fbackend {} {} {}", backend, opt, mode));
                CHECK_EQ(run.status, 0);
                CHECK(run.out.empty());
                CHECK(run.err.empty());
            }
    }
}
