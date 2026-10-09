import std;

#include "harness.hh"

#include <stdio.h>
#include <sys/wait.h>

namespace
{
    [[nodiscard]] std::string shell_quote(std::filesystem::path const& p)
    {
        std::string s = p.string();
        if (s.find('\'') != std::string::npos)
            return '"' + s + '"';

        return "'" + s + "'";
    }

    [[nodiscard]] std::filesystem::path self_exe_path()
    {
        std::error_code ec;
        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec)
            return std::filesystem::weakly_canonical(exe, ec);

        return std::filesystem::path{};
    }

    [[nodiscard]] std::filesystem::path dcc_path()
    {
        auto exe = self_exe_path();
        if (exe.empty())
            return {};

        return exe.parent_path().parent_path() / "dcc";
    }

    [[nodiscard]] std::filesystem::path expected_prefix()
    {
        auto exe = self_exe_path();
        if (exe.empty())
            return {};

        return exe.parent_path().parent_path().parent_path();
    }

    [[nodiscard]] std::string run_driver_flag(std::string const& flag)
    {
        auto dcc = dcc_path();
        if (dcc.empty())
            return {};

        std::string cmd = shell_quote(dcc) + " " + flag + " 2>/dev/null";

        auto* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe)
            return {};

        std::string result;
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), pipe))
            result += buf;
        int rc = ::pclose(pipe);

        if (rc != 0)
            return {};

        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();

        return result;
    }

    [[nodiscard, maybe_unused]] std::pair<int, std::string> run_dcc(std::string const& args)
    {
        auto dcc = dcc_path();
        if (dcc.empty())
            return {-1, {}};

        std::string cmd = "timeout 60 " + shell_quote(dcc) + " " + args + " 2>&1";

        auto* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe)
            return {-1, {}};

        std::string output;
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), pipe))
            output += buf;
        int rc = ::pclose(pipe);

        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        return {exit_code, output};
    }

} // anonymous namespace

SECTION("Driver prefix diagnostics");

TEST_CASE("--print-prefix matches build directory")
{
    auto prefix = run_driver_flag("--print-prefix");
    REQUIRE(!prefix.empty());

    auto expected = std::filesystem::weakly_canonical(expected_prefix());
    CHECK_EQ(prefix, expected.string());
}

TEST_CASE("--print-lib-dir matches build/lib")
{
    auto lib_dir = run_driver_flag("--print-lib-dir");
    REQUIRE(!lib_dir.empty());

    auto expected = std::filesystem::weakly_canonical(expected_prefix() / "lib");
    CHECK_EQ(lib_dir, expected.string());
}

TEST_CASE("--print-include-dir matches build/include")
{
    auto inc_dir = run_driver_flag("--print-include-dir");
    REQUIRE(!inc_dir.empty());

    auto expected = std::filesystem::weakly_canonical(expected_prefix() / "include");
    CHECK_EQ(inc_dir, expected.string());
}

TEST_CASE("--version exits 0 and reports the release version")
{
    auto version = run_driver_flag("--version");
    REQUIRE(!version.empty());
    CHECK(version.starts_with("dcc " DCC_EXPECTED_VERSION " ("));
}

TEST_CASE("a misplaced module declaration is a parse error, not an endless recovery loop")
{
    auto dir = std::filesystem::temp_directory_path() / std::format("dcc_module_recovery_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    struct Case
    {
        std::string_view name;
        std::string_view source;
    };
    for (auto const& c : {Case{"after_error.dc", "+\nmodule m;\n"}, Case{"second.dc", "module m;\nmodule n;\nvoid f() {}\n"},
                          Case{"static_if.dc", "module m;\nstatic if true {\n    module n;\n}\n"},
                          Case{"fixture_text.dc", "=== FILE: main.dc ===\nmodule m;\n"}})
    {
        auto src = dir / c.name;
        std::ofstream{src} << c.source;
        auto [code, output] = run_dcc("-fdump-ir " + shell_quote(src));
        CHECK(code == 1);
        CHECK(output.find("module declaration must be the first declaration in the file") != std::string::npos);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("the i8086 backend emits elf32 objects, assembly and mir")
{
    auto dir = std::filesystem::temp_directory_path() / std::format("dcc_i8086_backend_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    auto src = dir / "main.dc";
    {
        std::ofstream file{src};
        file << "module test;\n@nomangle public i32 dcc_main() { return 42; }\n";
    }

    auto obj = dir / "main.o";
    auto [code, output] = run_dcc("-target i8086-binary -fbackend custom -c -o " + shell_quote(obj) + " " + shell_quote(src));
    CHECK(code == 0);
    std::ifstream in{obj, std::ios::binary};
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), {}};
    REQUIRE(bytes.size() > 20);
    CHECK(bytes[4] == 1);
    CHECK(bytes[18] == 3);

    auto asm_path = dir / "main.s";
    auto [asm_code, asm_log] = run_dcc("-target i8086-binary -fbackend custom -S -o " + shell_quote(asm_path) + " " + shell_quote(src));
    CHECK(asm_code == 0);
    std::ifstream asm_in{asm_path};
    std::string asm_output{std::istreambuf_iterator<char>(asm_in), {}};
    CHECK(asm_output.find("bits 16") != std::string::npos);
    CHECK(asm_output.find("global dcc_main:function") != std::string::npos);

    auto [mir_code, mir_output] = run_dcc("-target i8086-binary -fbackend custom -fdump-mir " + shell_quote(src));
    CHECK(mir_code == 0);
    CHECK(mir_output.find("func dcc_main") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("the i8086 backend rejects code it cannot select yet")
{
    auto src = std::filesystem::temp_directory_path() / "dcc_i8086_unsupported.dc";
    {
        std::ofstream file{src};
        file << "module test;\npublic u8 f(u8* p) { return *p; }\n";
    }

    auto [code, output] = run_dcc("-target i8086-binary -fbackend custom -c -o /dev/null " + shell_quote(src));
    CHECK(code == 1);
    CHECK(output.find("i8086 backend: a 2-byte non-integer value in function `") != std::string::npos);
    CHECK(output.find("is not supported yet") != std::string::npos);

    std::filesystem::remove(src);
}

TEST_CASE("the i8086 target dumps ir in every code model")
{
    auto src = std::filesystem::temp_directory_path() / "dcc_i8086_dump_ir.dc";
    {
        std::ofstream file{src};
        file << "module test;\npublic usize f(u8* p, usize i) { return p[i] as usize + i; }";
    }

    for (auto model : {"", "-mcmodel=small", "-mcmodel=unreal", "-mcmodel=unreal32"})
    {
        auto [code, output] = run_dcc("-target i8086-binary " + std::string{model} + " -fdump-ir " + shell_quote(src));
        CHECK(code == 0);
        CHECK(output.find("module \"test\"") != std::string::npos);
        CHECK(output.find("ret usize") != std::string::npos);
        CHECK(output.find("no backend for target") == std::string::npos);
    }

    std::filesystem::remove(src);
}

TEST_CASE("the llvm backend rejects the i8086 target")
{
    auto src = std::filesystem::temp_directory_path() / "dcc_llvm_i8086_target.dc";
    {
        std::ofstream file{src};
        file << "module test;\nvoid f() {}\n";
    }

    for (auto flags : {"-c -o /dev/null", "-fbackend llvm -c -o /dev/null"})
    {
        auto [code, output] = run_dcc("-target i8086-binary " + std::string{flags} + " " + shell_quote(src));
        CHECK(code != 0);
        CHECK(output.find("LLVM backend does not support target 'i8086-binary'") != std::string::npos);
        CHECK(output.find("no backend for target") == std::string::npos);
    }

    std::filesystem::remove(src);
}

SECTION("Driver -flibdcext");

#if DCC_ENABLE_LLVM
TEST_CASE("-flibdcext -c compiles a trivial module")
{
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto src = tmp_dir / "test_flibdcext_c.dc";
    auto obj = tmp_dir / "test_flibdcext_c.o";

    {
        std::ofstream f{src};
        f << "module test;\n";
    }

    auto rc = run_dcc("-flibdcext -c -target x86_64-elf -o " + shell_quote(obj) + " " + shell_quote(src)).first;
    std::filesystem::remove(src);
    std::filesystem::remove(obj);

    CHECK_EQ(rc, 0);
}

TEST_CASE("-flibdcext produces a working executable")
{
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto src = tmp_dir / "test_flibdcext_exe.dc";
    auto exe = tmp_dir / "test_flibdcext_exe";

    {
        std::ofstream f{src};
        f << "module test;\n";
        f << "@nomangle\n";
        f << "public void _start() { while (true) {} }\n";
    }

    auto [rc, out] = run_dcc("-flibdcext -target x86_64-elf -o " + shell_quote(exe) + " " + shell_quote(src));

    std::filesystem::remove(src);
    bool exe_exists = std::filesystem::exists(exe);
    {
        std::error_code ec;
        std::filesystem::remove(exe, ec);
    }

    CHECK_EQ(rc, 0);
    CHECK(exe_exists);
}

TEST_CASE("DCC_WARN_ARRAY_DECAY warns on implicit array to slice copies")
{
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto src = tmp_dir / "test_array_decay.dc";
    auto obj = tmp_dir / "test_array_decay.o";

    {
        std::ofstream f{src};
        f << "module test;\n";
        f << "void take([] u8 s) {}\n";
        f << "public i32 main() {\n";
        f << "    u8[4] buf;\n";
        f << "    take(buf);\n";
        f << "    [] u8 view = buf;\n";
        f << "    take(view);\n";
        f << "    return 0;\n";
        f << "}\n";
    }

    auto dcc = dcc_path();
    REQUIRE(!dcc.empty());
    auto base = shell_quote(dcc) + " -flibdcext -c -target x86_64-elf -o " + shell_quote(obj) + " " + shell_quote(src);

    auto run_with = [&](std::string const& env_prefix) {
        auto* pipe = ::popen((env_prefix + base + " 2>&1").c_str(), "r");
        std::string output;
        char buf[4096];
        while (pipe && std::fgets(buf, sizeof(buf), pipe))
            output += buf;
        int rc = pipe ? ::pclose(pipe) : -1;
        return std::pair{WIFEXITED(rc) ? WEXITSTATUS(rc) : -1, output};
    };

    auto [rc_off, out_off] = run_with("");
    CHECK_EQ(rc_off, 0);
    CHECK(out_off.find("implicit copy") == std::string::npos);

    auto [rc_on, out_on] = run_with("DCC_WARN_ARRAY_DECAY=1 ");
    CHECK_EQ(rc_on, 0);
    CHECK(out_on.find("implicit copy of array as slice") != std::string::npos);

    std::filesystem::remove(src);
    std::filesystem::remove(obj);
}

TEST_CASE("DCC_WARN_ARRAY_DECAY reports repeated template instantiations once")
{
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto src = tmp_dir / "test_array_decay_dedup.dc";
    auto obj = tmp_dir / "test_array_decay_dedup.o";

    {
        std::ofstream f{src};
        f << "module test;\n";
        f << "void take([] u8 s) {}\n";
        f << "void helper(T)(T v) {\n";
        f << "    u8[4] buf;\n";
        f << "    take(buf);\n";
        f << "}\n";
        f << "public i32 main() {\n";
        f << "    helper(1 as i32);\n";
        f << "    helper(1 as i64);\n";
        f << "    helper(1 as u8);\n";
        f << "    return 0;\n";
        f << "}\n";
    }

    auto dcc = dcc_path();
    REQUIRE(!dcc.empty());
    auto base = shell_quote(dcc) + " -flibdcext -c -target x86_64-elf -o " + shell_quote(obj) + " " + shell_quote(src);

    auto* pipe = ::popen(("DCC_WARN_ARRAY_DECAY=1 " + base + " 2>&1").c_str(), "r");
    std::string output;
    char buf[4096];
    while (pipe && std::fgets(buf, sizeof(buf), pipe))
        output += buf;
    int rc = pipe ? ::pclose(pipe) : -1;
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    CHECK_EQ(code, 0);
    std::size_t count = 0;
    std::string needle = "implicit copy of array as slice";
    for (std::size_t pos = output.find(needle); pos != std::string::npos; pos = output.find(needle, pos + needle.size()))
        ++count;
    CHECK_EQ(count, 1u);

    std::filesystem::remove(src);
    std::filesystem::remove(obj);
}
#endif
