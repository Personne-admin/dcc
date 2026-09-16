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

        std::string cmd = shell_quote(dcc) + " " + args + " 2>&1";

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
    CHECK(version.find("0.3.0") != std::string::npos);
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
