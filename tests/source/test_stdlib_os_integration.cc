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

TEST_CASE("custom backend callees preserve callee-saved general registers clobbered by aggregate copies")
{
    std::string const work =
        "module work;\n"
        "\n"
        "public struct Big {\n"
        "    u8[512] b;\n"
        "}\n"
        "\n"
        "Big make(u64 seed) {\n"
        "    Big r;\n"
        "    for usize i = 0; i < 512; i++ {\n"
        "        r.b[i] = ((seed + (i as u64) * 7) & 255) as u8;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "public u64 churn(u64 seed) {\n"
        "    Big first = make(seed);\n"
        "    Big copy = first;\n"
        "    u64 h = 0;\n"
        "    for usize i = 0; i < 512; i++ {\n"
        "        h = h + (copy.b[i] as u64);\n"
        "    }\n"
        "    f64 x0 = (seed as f64) * 0.5;\n"
        "    f64 x1 = x0 + 1.0;\n"
        "    f64 x2 = x1 + 1.0;\n"
        "    f64 x3 = x2 + 1.0;\n"
        "    f64 x4 = x3 + 1.0;\n"
        "    f64 x5 = x4 + 1.0;\n"
        "    f64 x6 = x5 + 1.0;\n"
        "    f64 x7 = x6 + 1.0;\n"
        "    f64 x8 = x7 + 1.0;\n"
        "    f64 x9 = x8 + 1.0;\n"
        "    f64 x10 = x9 + 1.0;\n"
        "    f64 x11 = x10 + 1.0;\n"
        "    f64 x12 = x11 + 1.0;\n"
        "    f64 x13 = x12 + 1.0;\n"
        "    f64 x14 = x13 + 1.0;\n"
        "    f64 x15 = x14 + 1.0;\n"
        "    f64 acc = x0 * x15 + x1 * x14 + x2 * x13 + x3 * x12 + x4 * x11 + x5 * x10 + x6 * x9 + x7 * x8;\n"
        "    return h + (acc as u64);\n"
        "}\n";
    std::string const main =
        "module main;\n"
        "\n"
        "import work;\n"
        "\n"
        "@nomangle\n"
        "public u64 churn_seed = 9;\n"
        "\n"
        "@nomangle\n"
        "public u64[8] held = {3, 5, 7, 11, 13, 17, 19, 23};\n"
        "\n"
        "public i32 main() {\n"
        "    u64 a = held[0];\n"
        "    u64 b = held[1];\n"
        "    u64 c = held[2];\n"
        "    u64 d = held[3];\n"
        "    u64 e = held[4];\n"
        "    u64 f = held[5];\n"
        "    u64 g = held[6];\n"
        "    u64 k = held[7];\n"
        "    u64 got = work::churn(churn_seed);\n"
        "    if a * 2 + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + g * 17 + k * 19 != 1257 {\n"
        "        return 1;\n"
        "    }\n"
        "    if got != 66262 {\n"
        "        return 3;\n"
        "    }\n"
        "    return 42;\n"
        "}\n";
    auto r = os_test::run_modules({{"work.dc", work, "custom", "-O2"}, {"main.dc", main, "llvm", "-O2"}}, windows(), "llvm");
    CHECK_EQ(r.status, 42);
}

TEST_CASE("custom backend callees preserve xmm6 through xmm15 on the execution target")
{
    std::string const work =
        "module work;\n"
        "\n"
        "public struct Big {\n"
        "    u8[512] b;\n"
        "}\n"
        "\n"
        "Big make(u64 seed) {\n"
        "    Big r;\n"
        "    for usize i = 0; i < 512; i++ {\n"
        "        r.b[i] = ((seed + (i as u64) * 7) & 255) as u8;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "public u64 churn(u64 seed) {\n"
        "    Big first = make(seed);\n"
        "    Big copy = first;\n"
        "    u64 h = 0;\n"
        "    for usize i = 0; i < 512; i++ {\n"
        "        h = h + (copy.b[i] as u64);\n"
        "    }\n"
        "    f64 x0 = (seed as f64) * 0.5;\n"
        "    f64 x1 = x0 + 1.0;\n"
        "    f64 x2 = x1 + 1.0;\n"
        "    f64 x3 = x2 + 1.0;\n"
        "    f64 x4 = x3 + 1.0;\n"
        "    f64 x5 = x4 + 1.0;\n"
        "    f64 x6 = x5 + 1.0;\n"
        "    f64 x7 = x6 + 1.0;\n"
        "    f64 x8 = x7 + 1.0;\n"
        "    f64 x9 = x8 + 1.0;\n"
        "    f64 x10 = x9 + 1.0;\n"
        "    f64 x11 = x10 + 1.0;\n"
        "    f64 x12 = x11 + 1.0;\n"
        "    f64 x13 = x12 + 1.0;\n"
        "    f64 x14 = x13 + 1.0;\n"
        "    f64 x15 = x14 + 1.0;\n"
        "    f64 acc = x0 * x15 + x1 * x14 + x2 * x13 + x3 * x12 + x4 * x11 + x5 * x10 + x6 * x9 + x7 * x8;\n"
        "    return h + (acc as u64);\n"
        "}\n";
    std::string const main =
        "module main;\n"
        "\n"
        "import work;\n"
        "\n"
        "@nomangle\n"
        "public u64 churn_seed = 9;\n"
        "\n"
        "@nomangle\n"
        "public f64[8] held = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5};\n"
        "\n"
        "public i32 main() {\n"
        "    f64 p = held[0];\n"
        "    f64 q = held[1];\n"
        "    f64 r = held[2];\n"
        "    f64 t = held[3];\n"
        "    f64 u = held[4];\n"
        "    f64 v = held[5];\n"
        "    f64 w = held[6];\n"
        "    f64 x = held[7];\n"
        "    u64 got = work::churn(churn_seed);\n"
        "    if p * 2.0 + q * 3.0 + r * 5.0 + t * 7.0 + u * 11.0 + v * 13.0 + w * 17.0 + x * 19.0 != 493.5 {\n"
        "        return 2;\n"
        "    }\n"
        "    if got != 66262 {\n"
        "        return 3;\n"
        "    }\n"
        "    return 42;\n"
        "}\n";
    auto r = os_test::run_modules({{"work.dc", work, "custom", "-O2"}, {"main.dc", main, "llvm", "-O2"}}, windows(), "llvm");
    CHECK_EQ(r.status, 42);
}

TEST_CASE("custom backend aggregates of 3, 5, 6 and 7 bytes follow the C argument convention")
{
    std::string const peer =
        "#include <stdint.h>\n"
        "\n"
        "struct S3 { uint8_t b[3]; };\n"
        "struct S5 { uint8_t b[5]; };\n"
        "struct S6 { uint8_t b[6]; };\n"
        "struct S7 { uint8_t b[7]; };\n"
        "\n"
        "uint64_t dc_take(struct S3 a, struct S5 b, struct S6 c, struct S7 d, int32_t e);\n"
        "\n"
        "uint64_t c_take(struct S3 a, struct S5 b, struct S6 c, struct S7 d, int32_t e)\n"
        "{\n"
        "    uint64_t h = 0;\n"
        "    for (int i = 0; i < 3; ++i) h = h * 31 + a.b[i];\n"
        "    for (int i = 0; i < 5; ++i) h = h * 31 + b.b[i];\n"
        "    for (int i = 0; i < 6; ++i) h = h * 31 + c.b[i];\n"
        "    for (int i = 0; i < 7; ++i) h = h * 31 + d.b[i];\n"
        "    return h * 31 + (uint64_t)(int64_t)e;\n"
        "}\n"
        "\n"
        "int c_call_dc(void)\n"
        "{\n"
        "    struct S3 a = {{1, 2, 3}};\n"
        "    struct S5 b = {{4, 5, 6, 7, 8}};\n"
        "    struct S6 c = {{9, 10, 11, 12, 13, 14}};\n"
        "    struct S7 d = {{15, 16, 17, 18, 19, 20, 21}};\n"
        "    return dc_take(a, b, c, d, -5) == c_take(a, b, c, d, -5) ? 0 : 1;\n"
        "}\n";
    std::string const main =
        "module main;\n"
        "\n"
        "public struct S3 {\n"
        "    u8[3] b;\n"
        "}\n"
        "\n"
        "public struct S5 {\n"
        "    u8[5] b;\n"
        "}\n"
        "\n"
        "public struct S6 {\n"
        "    u8[6] b;\n"
        "}\n"
        "\n"
        "public struct S7 {\n"
        "    u8[7] b;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public extern u64 c_take(S3 a, S5 b, S6 c, S7 d, i32 e);\n"
        "\n"
        "@nomangle\n"
        "public extern i32 c_call_dc();\n"
        "\n"
        "u64 fold(u64 h, [] const u8 bytes) {\n"
        "    u64 r = h;\n"
        "    for usize i = 0; i < bytes.len; i++ {\n"
        "        r = r * 31 + (bytes[i] as u64);\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public u64 dc_take(S3 a, S5 b, S6 c, S7 d, i32 e) {\n"
        "    u64 h = fold(0, a.b);\n"
        "    h = fold(h, b.b);\n"
        "    h = fold(h, c.b);\n"
        "    h = fold(h, d.b);\n"
        "    return h * 31 + ((e as i64) as u64);\n"
        "}\n"
        "\n"
        "public i32 main() {\n"
        "    S3 a;\n"
        "    S5 b;\n"
        "    S6 c;\n"
        "    S7 d;\n"
        "    for usize i = 0; i < 3; i++ {\n"
        "        a.b[i] = (i + 1) as u8;\n"
        "    }\n"
        "    for usize i = 0; i < 5; i++ {\n"
        "        b.b[i] = (i + 4) as u8;\n"
        "    }\n"
        "    for usize i = 0; i < 6; i++ {\n"
        "        c.b[i] = (i + 9) as u8;\n"
        "    }\n"
        "    for usize i = 0; i < 7; i++ {\n"
        "        d.b[i] = (i + 15) as u8;\n"
        "    }\n"
        "    if c_take(a, b, c, d, -5) != dc_take(a, b, c, d, -5) {\n"
        "        return 1;\n"
        "    }\n"
        "    if c_call_dc() != 0 {\n"
        "        return 2;\n"
        "    }\n"
        "    return 42;\n"
        "}\n";
    auto r = os_test::run_modules({{"peer.c", peer}, {"main.dc", main}}, windows(), "custom");
    CHECK_EQ(r.status, 42);
}
