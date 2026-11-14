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
