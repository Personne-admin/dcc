import std;

#include "harness.hh"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    [[nodiscard]] std::string shell_quote(std::filesystem::path const& p)
    {
        return "'" + p.string() + "'";
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

    struct TempDir
    {
        std::filesystem::path path;
        static inline std::atomic<int> s_counter{0};

        TempDir()
        {
            auto tmp = std::filesystem::temp_directory_path();
            auto dir = tmp / ("dcc_test_link_" + std::to_string(::getpid()) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(dir);
            path = std::move(dir);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write_file(std::filesystem::path const& relative, std::string const& content) const
        {
            auto full = path / relative;
            std::error_code ec;
            std::filesystem::create_directories(full.parent_path(), ec);
            std::ofstream f{full};
            f << content;
        }

        [[nodiscard]] std::filesystem::path file(std::filesystem::path const& relative) const { return path / relative; }
    };

    struct RunResult
    {
        int rc;
        std::string output;
    };

    [[nodiscard]] RunResult run_shell(std::string const& command)
    {
        auto* pipe = ::popen(command.c_str(), "r");
        if (!pipe)
            return {.rc = -1, .output = {}};

        std::string output;
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), pipe))
            output += buf;
        int rc = ::pclose(pipe);

        int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        return {code, std::move(output)};
    }

    [[nodiscard]] RunResult run_dcc(std::string const& args)
    {
        auto dcc = dcc_path();
        if (dcc.empty())
            return {-1, {}};

        return run_shell(shell_quote(dcc) + " " + args + " 2>&1");
    }

    [[nodiscard]] bool file_exists(std::filesystem::path const& p)
    {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    }

    [[nodiscard]] bool output_contains(RunResult const& r, std::string_view needle)
    {
        return r.output.find(needle) != std::string::npos;
    }

    [[nodiscard]] int run_program(std::filesystem::path const& exe)
    {
        int rc = std::system((shell_quote(exe)).c_str());
        if (rc == -1)
            return -1;
        if (WIFEXITED(rc))
            return WEXITSTATUS(rc);
        return -2;
    }

    void write_add_module(TempDir const& td)
    {
        td.write_file("add.dc", R"(module add;
public i32 add(i32 a, i32 b) {
    return a + b;
}
)");
        td.write_file("main.dc", R"(module main;
import add;
i32 main() {
    return add::add(40, 2);
}
)");
    }

    void write_extern_pair(TempDir const& td)
    {
        td.write_file("provider.dc", R"(module provider;
@nomangle public i32 provide(i32 x) {
    return x * 2 + 2;
}
)");
        td.write_file("user.dc", R"(module main;
@nomangle extern i32 provide(i32 x);
i32 main() {
    return provide(20);
}
)");
    }

    void write_solo(TempDir const& td)
    {
        td.write_file("solo.dc", R"(module main;
i32 main() {
    return 7;
}
)");
    }

} // namespace

SECTION("Link mode: separate compile and link");

#ifndef _WIN32

TEST_CASE("objects compiled separately link to a binary matching single-shot compile")
{
    TempDir td;
    write_add_module(td);

    auto add_o = td.file("add.o");
    auto main_o = td.file("main.o");
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -c -o " + shell_quote(add_o) + " " + shell_quote(td.file("add.dc"))).rc == 0);
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -c -o " + shell_quote(main_o) + " " + shell_quote(td.file("main.dc"))).rc == 0);

    auto linked = td.file("linked");
    auto r = run_dcc("-flibdcext -fbackend llvm -o " + shell_quote(linked) + " " + shell_quote(main_o) + " " + shell_quote(add_o));
    CHECK_EQ(r.rc, 0);
    CHECK(file_exists(linked));
    CHECK_EQ(run_program(linked), 42);

    auto single = td.file("single");
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -o " + shell_quote(single) + " " + shell_quote(td.file("main.dc"))).rc == 0);
    CHECK_EQ(run_program(single), run_program(linked));
}

TEST_CASE("an undefined cross-object reference fails the link")
{
    TempDir td;
    write_extern_pair(td);

    auto provider_o = td.file("provider.o");
    auto user_o = td.file("user.o");
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -c -o " + shell_quote(provider_o) + " " + shell_quote(td.file("provider.dc"))).rc == 0);
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -c -o " + shell_quote(user_o) + " " + shell_quote(td.file("user.dc"))).rc == 0);

    auto prog = td.file("extprog");
    auto ok = run_dcc("-flibdcext -fbackend llvm -o " + shell_quote(prog) + " " + shell_quote(provider_o) + " " + shell_quote(user_o));
    CHECK_EQ(ok.rc, 0);
    CHECK_EQ(run_program(prog), 42);

    auto bad = td.file("badprog");
    auto r = run_dcc("-flibdcext -fbackend llvm -o " + shell_quote(bad) + " " + shell_quote(user_o));
    CHECK_NE(r.rc, 0);
    CHECK(output_contains(r, "undefined symbol"));
    CHECK(output_contains(r, "provide"));
}

TEST_CASE("objects from different backends link together")
{
    TempDir td;
    write_extern_pair(td);

    auto provider_o = td.file("provider_e.o");
    auto user_o = td.file("user.o");
    REQUIRE(run_dcc("-flibdcext -fbackend em64t -c -o " + shell_quote(provider_o) + " " + shell_quote(td.file("provider.dc"))).rc == 0);
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -c -o " + shell_quote(user_o) + " " + shell_quote(td.file("user.dc"))).rc == 0);

    auto prog = td.file("mixedprog");
    auto r = run_dcc("-flibdcext -fbackend llvm -o " + shell_quote(prog) + " " + shell_quote(provider_o) + " " + shell_quote(user_o));
    CHECK_EQ(r.rc, 0);
    CHECK_EQ(run_program(prog), 42);
}

TEST_CASE("linking without -o defaults to the first input stem")
{
    TempDir td;
    write_add_module(td);

    auto add_o = td.file("add.o");
    auto main_o = td.file("main.o");
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -c -o " + shell_quote(add_o) + " " + shell_quote(td.file("add.dc"))).rc == 0);
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -c -o " + shell_quote(main_o) + " " + shell_quote(td.file("main.dc"))).rc == 0);

    std::error_code cec;
    auto old_cwd = std::filesystem::current_path(cec);
    REQUIRE(!cec);
    std::filesystem::current_path(td.path, cec);
    REQUIRE(!cec);
    auto r = run_dcc("-flibdcext -fbackend llvm " + shell_quote(main_o) + " " + shell_quote(add_o));
    std::filesystem::current_path(old_cwd, cec);
    CHECK_EQ(r.rc, 0);
    CHECK(file_exists(td.file("main")));
    CHECK_EQ(run_program(td.file("main")), 42);
}

TEST_CASE("a single object links")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-flibdcext -fbackend llvm -c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto prog = td.file("solobin");
    auto r = run_dcc("-flibdcext -fbackend llvm -o " + shell_quote(prog) + " " + shell_quote(solo_o));
    CHECK_EQ(r.rc, 0);
    CHECK_EQ(run_program(prog), 7);
}

TEST_CASE("bare -flibdcext in link mode uses the host archive")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-flibdcext -c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto prog = td.file("hostbin");
    auto r = run_dcc("-flibdcext -o " + shell_quote(prog) + " " + shell_quote(solo_o));
    CHECK_EQ(r.rc, 0);
    CHECK_EQ(run_program(prog), 7);
}

TEST_CASE("explicit -flibdcext linux selects the linux archive")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-flibdcext=linux -c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    for (auto flag : {"-flibdcext=linux", "-flibdcext linux"})
    {
        auto prog = td.file("linuxbin");
        std::error_code ec;
        std::filesystem::remove(prog, ec);
        auto r = run_dcc(std::string{flag} + " -o " + shell_quote(prog) + " " + shell_quote(solo_o));
        CHECK_EQ(r.rc, 0);
        CHECK_EQ(run_program(prog), 7);
    }
}

TEST_CASE("explicit -flibdcext freestanding names the freestanding archive")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-flibdcext -c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto prog = td.file("freebin");
    auto r = run_dcc("-flibdcext=freestanding -o " + shell_quote(prog) + " " + shell_quote(solo_o));
    CHECK_NE(r.rc, 0);
    CHECK(output_contains(r, "dcext-freestanding-llvm"));
}

TEST_CASE("objects built for mismatched targets fail the link")
{
    TempDir td;
    write_extern_pair(td);

    auto provider_o = td.file("p32.o");
    auto user_o = td.file("user.o");
    REQUIRE(run_dcc("-target x86-elf -c -o " + shell_quote(provider_o) + " " + shell_quote(td.file("provider.dc"))).rc == 0);
    REQUIRE(run_dcc("-flibdcext -c -o " + shell_quote(user_o) + " " + shell_quote(td.file("user.dc"))).rc == 0);

    auto prog = td.file("mm");
    auto r = run_dcc("-flibdcext -o " + shell_quote(prog) + " " + shell_quote(provider_o) + " " + shell_quote(user_o));
    CHECK_NE(r.rc, 0);
    CHECK(output_contains(r, "incompatible"));
}

#endif

SECTION("Link mode: argument validation");

TEST_CASE("mixing sources and objects is an error")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto r = run_dcc("-o " + shell_quote(td.file("m")) + " " + shell_quote(td.file("solo.dc")) + " " + shell_quote(solo_o));
    CHECK_NE(r.rc, 0);
    CHECK(output_contains(r, "cannot mix"));
}

TEST_CASE("-c cannot be combined with object inputs")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto r = run_dcc("-c -o " + shell_quote(td.file("x")) + " " + shell_quote(solo_o));
    CHECK_NE(r.rc, 0);
    CHECK(output_contains(r, "-c cannot be used in link mode"));
}

TEST_CASE("-S and -shared are rejected in link mode")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto r_asm = run_dcc("-S -o " + shell_quote(td.file("x")) + " " + shell_quote(solo_o));
    CHECK_NE(r_asm.rc, 0);
    CHECK(output_contains(r_asm, "-S cannot be used in link mode"));

    auto r_shared = run_dcc("-shared -o " + shell_quote(td.file("x")) + " " + shell_quote(solo_o));
    CHECK_NE(r_shared.rc, 0);
    CHECK(output_contains(r_shared, "-shared is not supported in link mode"));
}

TEST_CASE("compile-only options are rejected in link mode")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto r = run_dcc("-O2 -fbounds-check -o " + shell_quote(td.file("x")) + " " + shell_quote(solo_o));
    CHECK_NE(r.rc, 0);
    CHECK(output_contains(r, "only apply when compiling sources"));
    CHECK(output_contains(r, "-O2"));
    CHECK(output_contains(r, "-fbounds-check"));

    auto r_inc = run_dcc("-I. -o " + shell_quote(td.file("x")) + " " + shell_quote(solo_o));
    CHECK_NE(r_inc.rc, 0);
    CHECK(output_contains(r_inc, "-I"));

    auto r_dep = run_dcc("--depfile " + shell_quote(td.file("x.d")) + " -o " + shell_quote(td.file("x")) + " " + shell_quote(solo_o));
    CHECK_NE(r_dep.rc, 0);
    CHECK(output_contains(r_dep, "--depfile"));

    auto r_dbg = run_dcc("-g -o " + shell_quote(td.file("x")) + " " + shell_quote(solo_o));
    CHECK_NE(r_dbg.rc, 0);
    CHECK(output_contains(r_dbg, "-g"));
}

TEST_CASE("missing object inputs are an error")
{
    TempDir td;
    auto r = run_dcc("-o " + shell_quote(td.file("x")) + " " + shell_quote(td.file("nope.o")));
    CHECK_NE(r.rc, 0);
    CHECK(output_contains(r, "cannot find input file"));
}

TEST_CASE("linking with no inputs is an error")
{
    TempDir td;
    auto r = run_dcc("-o " + shell_quote(td.file("x")));
    CHECK_NE(r.rc, 0);
}

TEST_CASE("non-x86_64 link targets are rejected")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto r32 = run_dcc("-target x86-elf -o " + shell_quote(td.file("x32")) + " " + shell_quote(solo_o));
    CHECK_NE(r32.rc, 0);
    CHECK(output_contains(r32, "only supported for x86_64-elf"));

    auto r_coff = run_dcc("-target x86_64-coff -o " + shell_quote(td.file("xcoff")) + " " + shell_quote(solo_o));
    CHECK_NE(r_coff.rc, 0);
    CHECK(output_contains(r_coff, "COFF"));

    auto r_win = run_dcc("-flibdcext=windows -o " + shell_quote(td.file("xwin")) + " " + shell_quote(solo_o));
    CHECK_NE(r_win.rc, 0);
    CHECK(output_contains(r_win, "COFF"));
}

TEST_CASE("unknown backends are rejected in link mode")
{
    TempDir td;
    write_solo(td);

    auto solo_o = td.file("solo.o");
    REQUIRE(run_dcc("-c -o " + shell_quote(solo_o) + " " + shell_quote(td.file("solo.dc"))).rc == 0);

    auto r = run_dcc("-fbackend frobnicate -o " + shell_quote(td.file("x")) + " " + shell_quote(solo_o));
    CHECK_NE(r.rc, 0);
    CHECK(output_contains(r, "invalid value"));
}

TEST_CASE("help documents link mode")
{
    auto r = run_dcc("--help");
    CHECK_EQ(r.rc, 0);
    CHECK(output_contains(r, "input1.o"));
}
