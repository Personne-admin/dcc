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

TEST_CASE("Win64 stack probes preserve large frames on a fresh thread")
{
    if (!windows())
        return;

    auto const source = os_test::fixture("win64-stack-probe.dc");
    REQUIRE(!source.empty());
    for (auto backend : {"llvm", "custom"})
        for (auto optimization : {"-O0", "-O2"})
            CHECK_EQ(os_test::run_modules({{"main.dc", source, {}, optimization}}, true, backend).status, 42);
}

TEST_CASE("LLVM based pointers access per-thread FS storage")
{
    if (windows())
        return;
    auto const source = os_test::fixture("based-fs-linux.dc");
    REQUIRE(!source.empty());
    for (auto optimization : {"-O0", "-O2"})
        CHECK_EQ(os_test::run_modules({{"main.dc", source, {}, optimization}}, false, "llvm").status, 42);
}

TEST_CASE("LLVM based pointers access Win64 TEB through GS")
{
    if (!windows())
        return;
    auto const source = os_test::fixture("based-gs-windows.dc");
    REQUIRE(!source.empty());
    for (auto optimization : {"-O0", "-O2"})
        CHECK_EQ(os_test::run_modules({{"main.dc", source, {}, optimization}}, true, "llvm").status, 42);
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

TEST_CASE("custom backend aggregates of 1, 2, 4 and 8 bytes follow the C return convention")
{
    std::string const peer =
        "#include <stdint.h>\n"
        "\n"
        "struct S1 { uint8_t b[1]; };\n"
        "struct S2 { uint8_t b[2]; };\n"
        "struct S3 { uint8_t b[3]; };\n"
        "struct S4 { uint8_t b[4]; };\n"
        "struct S8 { uint8_t b[8]; };\n"
        "struct FS { float x; float y; };\n"
        "\n"
        "struct S1 dc_r1(int32_t seed);\n"
        "struct S2 dc_r2(int32_t seed);\n"
        "struct S3 dc_r3(int32_t seed);\n"
        "struct S4 dc_r4(int32_t seed);\n"
        "struct S8 dc_r8(int32_t seed);\n"
        "struct FS dc_rf(int32_t seed);\n"
        "\n"
        "struct S1 c_r1(int32_t seed) { struct S1 r; for (int i = 0; i < 1; ++i) r.b[i] = (uint8_t)(seed + i * 3); return r; }\n"
        "struct S2 c_r2(int32_t seed) { struct S2 r; for (int i = 0; i < 2; ++i) r.b[i] = (uint8_t)(seed + i * 3); return r; }\n"
        "struct S3 c_r3(int32_t seed) { struct S3 r; for (int i = 0; i < 3; ++i) r.b[i] = (uint8_t)(seed + i * 3); return r; }\n"
        "struct S4 c_r4(int32_t seed) { struct S4 r; for (int i = 0; i < 4; ++i) r.b[i] = (uint8_t)(seed + i * 3); return r; }\n"
        "struct S8 c_r8(int32_t seed) { struct S8 r; for (int i = 0; i < 8; ++i) r.b[i] = (uint8_t)(seed + i * 3); return r; }\n"
        "struct FS c_rf(int32_t seed) { struct FS r; r.x = (float)seed + 0.5f; r.y = (float)seed + 1.25f; return r; }\n"
        "\n"
        "int c_check_dc(void)\n"
        "{\n"
        "    struct S1 a = dc_r1(10);\n"
        "    struct S2 b = dc_r2(20);\n"
        "    struct S3 c = dc_r3(30);\n"
        "    struct S4 d = dc_r4(40);\n"
        "    struct S8 e = dc_r8(50);\n"
        "    struct FS f = dc_rf(60);\n"
        "    if (a.b[0] != 10) return 1;\n"
        "    for (int i = 0; i < 2; ++i) if (b.b[i] != 20 + i * 3) return 2;\n"
        "    for (int i = 0; i < 3; ++i) if (c.b[i] != 30 + i * 3) return 3;\n"
        "    for (int i = 0; i < 4; ++i) if (d.b[i] != 40 + i * 3) return 4;\n"
        "    for (int i = 0; i < 8; ++i) if (e.b[i] != 50 + i * 3) return 5;\n"
        "    if (f.x != 60.5f || f.y != 61.25f) return 6;\n"
        "    return 0;\n"
        "}\n";
    std::string const main =
        "module main;\n"
        "\n"
        "public struct S1 {\n"
        "    u8[1] b;\n"
        "}\n"
        "\n"
        "public struct S2 {\n"
        "    u8[2] b;\n"
        "}\n"
        "\n"
        "public struct S3 {\n"
        "    u8[3] b;\n"
        "}\n"
        "\n"
        "public struct S4 {\n"
        "    u8[4] b;\n"
        "}\n"
        "\n"
        "public struct S8 {\n"
        "    u8[8] b;\n"
        "}\n"
        "\n"
        "public struct FS {\n"
        "    f32 x;\n"
        "    f32 y;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public extern S1 c_r1(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern S2 c_r2(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern S3 c_r3(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern S4 c_r4(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern S8 c_r8(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern FS c_rf(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern i32 c_check_dc();\n"
        "\n"
        "@nomangle\n"
        "public S1 dc_r1(i32 seed) {\n"
        "    S1 r;\n"
        "    for usize i = 0; i < 1; i++ {\n"
        "        r.b[i] = (seed + (i as i32) * 3) as u8;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public S2 dc_r2(i32 seed) {\n"
        "    S2 r;\n"
        "    for usize i = 0; i < 2; i++ {\n"
        "        r.b[i] = (seed + (i as i32) * 3) as u8;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public S3 dc_r3(i32 seed) {\n"
        "    S3 r;\n"
        "    for usize i = 0; i < 3; i++ {\n"
        "        r.b[i] = (seed + (i as i32) * 3) as u8;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public S4 dc_r4(i32 seed) {\n"
        "    S4 r;\n"
        "    for usize i = 0; i < 4; i++ {\n"
        "        r.b[i] = (seed + (i as i32) * 3) as u8;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public S8 dc_r8(i32 seed) {\n"
        "    S8 r;\n"
        "    for usize i = 0; i < 8; i++ {\n"
        "        r.b[i] = (seed + (i as i32) * 3) as u8;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public FS dc_rf(i32 seed) {\n"
        "    FS r;\n"
        "    r.x = (seed as f32) + (0.5 as f32);\n"
        "    r.y = (seed as f32) + (1.25 as f32);\n"
        "    return r;\n"
        "}\n"
        "\n"
        "public i32 main() {\n"
        "    S1 v1 = c_r1(10);\n"
        "    for usize i = 0; i < 1; i++ {\n"
        "        if v1.b[i] != ((10 + (i as i32) * 3) as u8) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "    S2 v2 = c_r2(20);\n"
        "    for usize i = 0; i < 2; i++ {\n"
        "        if v2.b[i] != ((20 + (i as i32) * 3) as u8) {\n"
        "            return 2;\n"
        "        }\n"
        "    }\n"
        "    S3 v3 = c_r3(30);\n"
        "    for usize i = 0; i < 3; i++ {\n"
        "        if v3.b[i] != ((30 + (i as i32) * 3) as u8) {\n"
        "            return 3;\n"
        "        }\n"
        "    }\n"
        "    S4 v4 = c_r4(40);\n"
        "    for usize i = 0; i < 4; i++ {\n"
        "        if v4.b[i] != ((40 + (i as i32) * 3) as u8) {\n"
        "            return 4;\n"
        "        }\n"
        "    }\n"
        "    S8 v8 = c_r8(50);\n"
        "    for usize i = 0; i < 8; i++ {\n"
        "        if v8.b[i] != ((50 + (i as i32) * 3) as u8) {\n"
        "            return 5;\n"
        "        }\n"
        "    }\n"
        "    FS f = c_rf(60);\n"
        "    if f.x != (60.5 as f32) || f.y != (61.25 as f32) {\n"
        "        return 6;\n"
        "    }\n"
        "    if c_check_dc() != 0 {\n"
        "        return 7;\n"
        "    }\n"
        "    return 42;\n"
        "}\n";
    auto r = os_test::run_modules({{"peer.c", peer}, {"main.dc", main}}, windows(), "custom");
    CHECK_EQ(r.status, 42);
}

TEST_CASE("llvm backend aggregates up to 16 bytes and slices follow the C calling convention")
{
    std::string const peer =
        "#include <stdint.h>\n"
        "\n"
        "struct S2 { uint8_t b[2]; };\n"
        "struct S3 { uint8_t b[3]; };\n"
        "struct S12 { uint8_t b[12]; };\n"
        "struct S16 { uint8_t b[16]; };\n"
        "struct FS { float x; float y; };\n"
        "struct MX { double a; int64_t b; };\n"
        "struct F2 { double x; double y; };\n"
        "struct SL { const uint8_t* p; uint64_t n; };\n"
        "\n"
        "uint64_t dc_take(struct S2 a, struct S3 b, struct S12 c, struct S16 d, struct FS e, struct MX f, struct SL g, int32_t h);\n"
        "struct S2 dc_r2(int32_t seed);\n"
        "struct S3 dc_r3(int32_t seed);\n"
        "struct F2 dc_rf2(int32_t seed);\n"
        "struct MX dc_rmx(int32_t seed);\n"
        "struct SL dc_rsl(int32_t seed);\n"
        "\n"
        "static uint64_t fold(uint64_t h, const uint8_t* p, int n) { for (int i = 0; i < n; ++i) h = h * 31 + p[i]; return h; }\n"
        "\n"
        "uint64_t c_take(struct S2 a, struct S3 b, struct S12 c, struct S16 d, struct FS e, struct MX f, struct SL g, int32_t h)\n"
        "{\n"
        "    uint64_t r = fold(0, a.b, 2);\n"
        "    r = fold(r, b.b, 3);\n"
        "    r = fold(r, c.b, 12);\n"
        "    r = fold(r, d.b, 16);\n"
        "    r = r * 31 + (uint64_t)(e.x * 4.0f) + (uint64_t)(e.y * 4.0f);\n"
        "    r = r * 31 + (uint64_t)(f.a * 4.0) + (uint64_t)f.b;\n"
        "    r = fold(r * 31 + g.n, g.p, (int)g.n);\n"
        "    return r * 31 + (uint64_t)(int64_t)h;\n"
        "}\n"
        "\n"
        "struct S2 c_r2(int32_t seed) { struct S2 r = {{(uint8_t)seed, (uint8_t)(seed + 1)}}; return r; }\n"
        "struct S3 c_r3(int32_t seed) { struct S3 r = {{(uint8_t)seed, (uint8_t)(seed + 1), (uint8_t)(seed + 2)}}; return r; }\n"
        "struct F2 c_rf2(int32_t seed) { struct F2 r = {seed + 0.5, seed + 0.25}; return r; }\n"
        "struct MX c_rmx(int32_t seed) { struct MX r = {seed + 0.75, seed * 3}; return r; }\n"
        "static const uint8_t sl_data[8] = {11, 22, 33, 44, 55, 66, 77, 88};\n"
        "struct SL c_rsl(int32_t seed) { struct SL r = {sl_data + seed, (uint64_t)(8 - seed)}; return r; }\n"
        "\n"
        "int c_check_dc(void)\n"
        "{\n"
        "    struct S2 a = {{1, 2}};\n"
        "    struct S3 b = {{3, 4, 5}};\n"
        "    struct S12 c;\n"
        "    struct S16 d;\n"
        "    for (int i = 0; i < 12; ++i) c.b[i] = (uint8_t)(10 + i);\n"
        "    for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(40 + i);\n"
        "    struct FS e = {1.5f, 2.25f};\n"
        "    struct MX f = {3.5, -7};\n"
        "    struct SL g = {sl_data, 5};\n"
        "    if (dc_take(a, b, c, d, e, f, g, -9) != c_take(a, b, c, d, e, f, g, -9)) return 1;\n"
        "    struct S2 r2 = dc_r2(20);\n"
        "    if (r2.b[0] != 20 || r2.b[1] != 21) return 2;\n"
        "    struct S3 r3 = dc_r3(30);\n"
        "    if (r3.b[0] != 30 || r3.b[1] != 31 || r3.b[2] != 32) return 3;\n"
        "    struct F2 rf = dc_rf2(40);\n"
        "    if (rf.x != 40.5 || rf.y != 40.25) return 4;\n"
        "    struct MX rm = dc_rmx(50);\n"
        "    if (rm.a != 50.75 || rm.b != 150) return 5;\n"
        "    struct SL rs = dc_rsl(2);\n"
        "    if (rs.n != 6 || rs.p[0] != 33) return 6;\n"
        "    return 0;\n"
        "}\n";
    std::string const main =
        "module main;\n"
        "\n"
        "public struct S2 {\n"
        "    u8[2] b;\n"
        "}\n"
        "\n"
        "public struct S3 {\n"
        "    u8[3] b;\n"
        "}\n"
        "\n"
        "public struct S12 {\n"
        "    u8[12] b;\n"
        "}\n"
        "\n"
        "public struct S16 {\n"
        "    u8[16] b;\n"
        "}\n"
        "\n"
        "public struct FS {\n"
        "    f32 x;\n"
        "    f32 y;\n"
        "}\n"
        "\n"
        "public struct MX {\n"
        "    f64 a;\n"
        "    i64 b;\n"
        "}\n"
        "\n"
        "public struct F2 {\n"
        "    f64 x;\n"
        "    f64 y;\n"
        "}\n"
        "\n"
        "u8[8] sl_data = {11, 22, 33, 44, 55, 66, 77, 88};\n"
        "\n"
        "@nomangle\n"
        "public extern u64 c_take(S2 a, S3 b, S12 c, S16 d, FS e, MX f, [] const u8 g, i32 h);\n"
        "\n"
        "@nomangle\n"
        "public extern S2 c_r2(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern S3 c_r3(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern F2 c_rf2(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern MX c_rmx(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern [] const u8 c_rsl(i32 seed);\n"
        "\n"
        "@nomangle\n"
        "public extern i32 c_check_dc();\n"
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
        "public u64 dc_take(S2 a, S3 b, S12 c, S16 d, FS e, MX f, [] const u8 g, i32 h) {\n"
        "    u64 r = fold(0, a.b);\n"
        "    r = fold(r, b.b);\n"
        "    r = fold(r, c.b);\n"
        "    r = fold(r, d.b);\n"
        "    r = r * 31 + ((e.x * (4.0 as f32)) as u64) + ((e.y * (4.0 as f32)) as u64);\n"
        "    r = r * 31 + ((f.a * 4.0) as u64) + (f.b as u64);\n"
        "    r = fold(r * 31 + (g.len as u64), g);\n"
        "    return r * 31 + ((h as i64) as u64);\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public S2 dc_r2(i32 seed) {\n"
        "    S2 r;\n"
        "    r.b[0] = seed as u8;\n"
        "    r.b[1] = (seed + 1) as u8;\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public S3 dc_r3(i32 seed) {\n"
        "    S3 r;\n"
        "    r.b[0] = seed as u8;\n"
        "    r.b[1] = (seed + 1) as u8;\n"
        "    r.b[2] = (seed + 2) as u8;\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public F2 dc_rf2(i32 seed) {\n"
        "    F2 r;\n"
        "    r.x = (seed as f64) + 0.5;\n"
        "    r.y = (seed as f64) + 0.25;\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public MX dc_rmx(i32 seed) {\n"
        "    MX r;\n"
        "    r.a = (seed as f64) + 0.75;\n"
        "    r.b = (seed * 3) as i64;\n"
        "    return r;\n"
        "}\n"
        "\n"
        "@nomangle\n"
        "public [] const u8 dc_rsl(i32 seed) {\n"
        "    return sl_data[(seed as usize)..8];\n"
        "}\n"
        "\n"
        "public i32 main() {\n"
        "    S2 a;\n"
        "    a.b[0] = 1;\n"
        "    a.b[1] = 2;\n"
        "    S3 b;\n"
        "    b.b[0] = 3;\n"
        "    b.b[1] = 4;\n"
        "    b.b[2] = 5;\n"
        "    S12 c;\n"
        "    for usize i = 0; i < 12; i++ {\n"
        "        c.b[i] = (10 + i) as u8;\n"
        "    }\n"
        "    S16 d;\n"
        "    for usize i = 0; i < 16; i++ {\n"
        "        d.b[i] = (40 + i) as u8;\n"
        "    }\n"
        "    FS e;\n"
        "    e.x = 1.5 as f32;\n"
        "    e.y = 2.25 as f32;\n"
        "    MX f;\n"
        "    f.a = 3.5;\n"
        "    f.b = -7;\n"
        "    if c_take(a, b, c, d, e, f, sl_data[0..5], -9) != dc_take(a, b, c, d, e, f, sl_data[0..5], -9) {\n"
        "        return 1;\n"
        "    }\n"
        "    S2 r2 = c_r2(20);\n"
        "    if r2.b[0] != 20 || r2.b[1] != 21 {\n"
        "        return 2;\n"
        "    }\n"
        "    S3 r3 = c_r3(30);\n"
        "    if r3.b[0] != 30 || r3.b[1] != 31 || r3.b[2] != 32 {\n"
        "        return 3;\n"
        "    }\n"
        "    F2 rf = c_rf2(40);\n"
        "    if rf.x != 40.5 || rf.y != 40.25 {\n"
        "        return 4;\n"
        "    }\n"
        "    MX rm = c_rmx(50);\n"
        "    if rm.a != 50.75 || rm.b != 150 {\n"
        "        return 5;\n"
        "    }\n"
        "    [] const u8 rs = c_rsl(2);\n"
        "    if rs.len != 6 || rs[0] != 33 {\n"
        "        return 6;\n"
        "    }\n"
        "    if c_check_dc() != 0 {\n"
        "        return 7;\n"
        "    }\n"
        "    return 42;\n"
        "}\n";
    auto r = os_test::run_modules({{"peer.c", peer}, {"main.dc", main}}, windows(), "llvm");
    CHECK_EQ(r.status, 42);
}
