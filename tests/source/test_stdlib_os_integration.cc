import std;

#include "harness.hh"
#include "stdlib_os.hh"

namespace
{
    bool windows()
    {
        return std::getenv("DCC_TEST_WINDOWS") != nullptr;
    }

} // namespace

SECTION("libdcext OS integration");

TEST_CASE("shared file, heap, clock and process contracts")
{
    CHECK_EQ(os_test::run(os_test::fixture("logic.dc"), windows()).status, 0);
    auto r = os_test::run(os_test::fixture("integration.dc"), windows());
    CHECK_EQ(r.status, 0);
    CHECK(r.out.contains("stdio ok"));
    CHECK(r.err.contains("stderr ok"));
}

TEST_CASE("exit and explicit panic handler in child processes")
{
    CHECK_EQ(os_test::run("module main; import std::os::process; public i32 main() { std::os::process::exit(42); return 1; }", windows()).status, 42);
    auto r = os_test::run("module main; import std::os::process; import std::debug; public i32 main() { std::os::process::install_panic_handler(); "
                          "std::debug::panic(\"explicit panic\"); return 1; }",
                          windows());
    CHECK_EQ(r.status, 134);
    CHECK(r.err.contains("panic: explicit panic"));
}

TEST_CASE("crt0 installs formatted panic handler before any OS call")
{
    auto r = os_test::run("module main; import std::debug; public i32 main() { std::debug::panic(\"startup panic\"); return 1; }", windows());
    CHECK_EQ(r.status, 134);
    CHECK(r.err.contains("panic: startup panic"));
    CHECK(r.err.contains("main.dc:"));
}

TEST_CASE("abort terminates through SIGABRT or TerminateProcess")
{
    CHECK_EQ(os_test::run("module main; import std::os::process; public i32 main() { std::os::process::abort(); return 1; }", windows()).status, 134);
}

TEST_CASE("Windows child standard output reaches an inherited pipe")
{
    if (windows())
        CHECK_EQ(os_test::run(os_test::fixture("pipe-redirection.dc"), true).status, 0);
}
