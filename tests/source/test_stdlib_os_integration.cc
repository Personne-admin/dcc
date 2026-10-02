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

TEST_CASE("os::random fills buffers from OS entropy")
{
    CHECK_EQ(os_test::run(os_test::fixture("random.dc"), windows()).status, 0);
}

TEST_CASE("os::time civil calendar passes on the execution target")
{
    CHECK_EQ(os_test::run(os_test::fixture("time-civil.dc"), windows()).status, 0);
}

TEST_CASE("os::path lexical helpers pass on the execution target")
{
    CHECK_EQ(os_test::run(os_test::fixture("path-lexical.dc"), windows()).status, 0);
}

TEST_CASE("os::path wide transcoding passes on the execution target")
{
    CHECK_EQ(os_test::run(os_test::fixture("path-wide.dc"), windows()).status, 0);
}

TEST_CASE("os::file rename, make_directory_all, predicates and metadata")
{
    CHECK_EQ(os_test::run(os_test::fixture("file-ops.dc"), windows()).status, 0);
}

TEST_CASE("os::process environment, args and working directory")
{
    CHECK_EQ(os_test::run(os_test::fixture("process-env.dc"), windows()).status, 0);
}

TEST_CASE("os::process spawnvectors, quoting, errors and allocator discipline")
{
    CHECK_EQ(os_test::run(os_test::fixture("process-spawn.dc"), windows()).status, 0);
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

TEST_CASE("custom backend jump tables link and run on the execution target")
{
    auto r = os_test::run_modules({{"main.dc",
                                    "module main;\n\ni32 classify(i32 v) {\n    return match v {\n        0 => 10,\n        1 => 20,\n        2 => 30,\n"
                                    "        3 => 41,\n        4 => 50,\n        5 => 60,\n        6 => 70,\n        7 => 42,\n        _ => 0,\n    };\n}\n\n"
                                    "public i32 main() {\n    return classify(7);\n}\n"}},
                                  windows(), "custom");
    CHECK_EQ(r.status, 42);
}

TEST_CASE("custom backend objects sharing template instances link and run on the execution target")
{
    std::string const templates = "module t;\n\npublic T twice(T)(T x) {\n    return x + x;\n}\n\npublic T scale(T)(T v) {\n    return match v {\n"
                                  "        0 => 2,\n        1 => 3,\n        2 => 5,\n        3 => 7,\n        4 => 11,\n        5 => 13,\n        6 => 17,\n"
                                  "        7 => 40,\n        _ => 0,\n    };\n}\n";
    std::string const a = "module a;\n\nimport t;\n\npublic i32 fa(i32 x) {\n    return t::twice(x) + t::scale(x);\n}\n";
    std::string const b = "module b;\n\nimport t;\n\npublic i32 fb(i32 x) {\n    return t::scale(x) - t::twice(x);\n}\n";
    std::string const main = "module main;\n\nimport a;\nimport b;\n\npublic i32 main() {\n    return a::fa(1) + b::fb(7) + 11;\n}\n";
    auto r = os_test::run_modules({{"t.dc", templates}, {"a.dc", a}, {"b.dc", b}, {"main.dc", main}}, windows(), "custom");
    CHECK_EQ(r.status, 42);
}

TEST_CASE("custom backend programs link against the custom libdcext on the execution target")
{
    auto r = os_test::run_modules({{"main.dc", "module main;\n\nimport std::os::file;\nimport std::result;\n\nusing std::os::{ file };\n\n"
                                               "public i32 main() {\n    file::File out = file::stdout();\n"
                                               "    if !out.write_all(\"hello from dcc\\n\").is_ok() {\n        return 1;\n    }\n    return 42;\n}\n"}},
                                  windows(), "custom");
    CHECK_EQ(r.status, 42);
    CHECK(r.out.contains("hello from dcc"));
}

TEST_CASE("custom backend struct returns through memory work on the execution target")
{
    auto r = os_test::run_modules({{"main.dc", "module main;\n\nstruct Triple {\n    i64 a;\n    i64 b;\n    i64 c;\n}\n\n"
                                               "Triple make(i64 seed, i64 step) {\n    return Triple { a = seed, b = seed + step, c = seed + step * 2 };\n}\n\n"
                                               "public i32 main() {\n    Triple t = make(10, 3);\n    return (t.a + t.b + t.c + 3) as i32;\n}\n"}},
                                  windows(), "custom");
    CHECK_EQ(r.status, 42);
}
