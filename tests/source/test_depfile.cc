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
        std::string s = p.string();
        if (s.contains('\''))
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

    struct TempDir
    {
        std::filesystem::path path;
        static inline std::atomic<int> s_counter{0};

        TempDir()
        {
            auto tmp = std::filesystem::temp_directory_path();
            auto dir = tmp / ("dcc_test_depfile_" + std::to_string(::getpid()) + "_" + std::to_string(++s_counter));
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

    [[nodiscard]] std::string read_file(std::filesystem::path const& p)
    {
        std::ifstream f{p, std::ios::binary};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    [[nodiscard]] bool file_exists(std::filesystem::path const& p)
    {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    }

    void write_module(TempDir const& td, std::filesystem::path const& relative, std::string_view module_name, std::vector<std::string_view> imports = {})
    {
        std::string content = std::format("module {};\n", module_name);
        for (auto const& imp : imports)
            content += "import " + std::string{imp} + ";\n";
        content += "public i32 q = 1;\n";
        td.write_file(relative, content);
    }

    [[nodiscard]] std::string target_of(std::string const& depfile_content)
    {
        auto colon = depfile_content.find(':');
        if (colon == std::string::npos)
            return {};
        return depfile_content.substr(0, colon);
    }

    [[nodiscard]] std::vector<std::string> deps_of(std::string const& depfile_content)
    {
        auto colon = depfile_content.find(':');
        if (colon == std::string::npos)
            return {};

        std::vector<std::string> out;
        std::istringstream ss{depfile_content.substr(colon + 1)};
        std::string tok;
        while (ss >> tok)
            out.push_back(tok);
        return out;
    }

    [[nodiscard]] bool make_accepts(std::filesystem::path const& depfile, std::filesystem::path const& goal)
    {
        std::error_code ec;
        std::filesystem::remove(goal, ec);
        auto cmd = "make -f " + shell_quote(depfile) + " -n " + shell_quote(goal) + " >/dev/null 2>&1";
        return std::system(cmd.c_str()) == 0;
    }

}

SECTION("Depfile: basic rules");

TEST_CASE("single source produces a one-rule depfile")
{
    TempDir td;
    write_module(td, "single.dc", "single");

    auto src = td.file("single.dc");
    auto obj = td.file("single.o");
    auto dep = td.file("single.d");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(dep) + " " + shell_quote(src));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(dep), obj.string() + ": " + src.string() + "\n");
}

TEST_CASE("direct import is listed after the root")
{
    TempDir td;
    write_module(td, "foo.dc", "foo");
    write_module(td, "app.dc", "app", {"foo"});

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("app.o")) + " --depfile " + shell_quote(td.file("app.d")) + " " +
                     shell_quote(td.file("app.dc")));

    CHECK_EQ(r.rc, 0);
    auto content = read_file(td.file("app.d"));
    auto deps = deps_of(content);
    REQUIRE(deps.size() == 2);
    CHECK_EQ(deps[0], td.file("app.dc").string());
    CHECK_EQ(deps[1], td.file("foo.dc").string());
}

TEST_CASE("transitive import lists the whole chain")
{
    TempDir td;
    write_module(td, "dep_c.dc", "dep_c");
    write_module(td, "dep_b.dc", "dep_b", {"dep_c"});
    write_module(td, "app.dc", "app", {"dep_b"});

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("app.o")) + " --depfile " + shell_quote(td.file("app.d")) + " " +
                     shell_quote(td.file("app.dc")));

    CHECK_EQ(r.rc, 0);
    auto deps = deps_of(read_file(td.file("app.d")));
    REQUIRE(deps.size() == 3);
    CHECK_EQ(deps[0], td.file("app.dc").string());
    CHECK_EQ(deps[1], td.file("dep_b.dc").string());
    CHECK_EQ(deps[2], td.file("dep_c.dc").string());
}

TEST_CASE("diamond import lists the shared module once")
{
    TempDir td;
    write_module(td, "dep_d.dc", "dep_d");
    write_module(td, "dep_b.dc", "dep_b", {"dep_d"});
    write_module(td, "dep_c.dc", "dep_c", {"dep_d"});
    write_module(td, "app.dc", "app", {"dep_b", "dep_c"});

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("app.o")) + " --depfile " + shell_quote(td.file("app.d")) + " " +
                     shell_quote(td.file("app.dc")));

    CHECK_EQ(r.rc, 0);
    auto deps = deps_of(read_file(td.file("app.d")));
    REQUIRE(deps.size() == 4);
    CHECK_EQ(deps[0], td.file("app.dc").string());
    auto rest = std::vector<std::string>{deps.begin() + 1, deps.end()};
    auto sorted = rest;
    std::sort(sorted.begin(), sorted.end());
    CHECK(rest == sorted);
    CHECK_EQ(rest.size(), std::set<std::string>(rest.begin(), rest.end()).size());
}

SECTION("Depfile: resolution");

TEST_CASE("-I import root resolves and appears in the depfile")
{
    TempDir td;
    write_module(td, "lib/helper.dc", "helper");
    write_module(td, "app.dc", "app", {"helper"});

    auto r = run_dcc("-c -target x86_64-elf -I " + shell_quote(td.file("lib")) + " -o " + shell_quote(td.file("app.o")) + " --depfile " +
                     shell_quote(td.file("app.d")) + " " + shell_quote(td.file("app.dc")));

    CHECK_EQ(r.rc, 0);
    auto deps = deps_of(read_file(td.file("app.d")));
    REQUIRE(deps.size() == 2);
    CHECK_EQ(deps[0], td.file("app.dc").string());
    CHECK_EQ(deps[1], td.file("lib/helper.dc").string());
}

TEST_CASE("cross-directory import resolves via -I")
{
    TempDir td;
    write_module(td, "libs/util.dc", "util");
    write_module(td, "src/main.dc", "main", {"util"});

    auto r = run_dcc("-c -target x86_64-elf -I " + shell_quote(td.file("libs")) + " -o " + shell_quote(td.file("main.o")) + " --depfile " +
                     shell_quote(td.file("main.d")) + " " + shell_quote(td.file("src/main.dc")));

    CHECK_EQ(r.rc, 0);
    auto deps = deps_of(read_file(td.file("main.d")));
    REQUIRE(deps.size() == 2);
    CHECK_EQ(deps[0], td.file("src/main.dc").string());
    CHECK_EQ(deps[1], td.file("libs/util.dc").string());
}

TEST_CASE("prerequisite with a colon is escaped and resolvable")
{
    TempDir td;
    write_module(td, "lib:dir/col.dc", "col");
    write_module(td, "app.dc", "app", {"col"});

    auto obj = td.file("app.o");
    auto dep = td.file("d.d");
    auto r = run_dcc("-c -target x86_64-elf -I " + shell_quote(td.file("lib:dir")) + " -o " + shell_quote(obj) + " --depfile " + shell_quote(dep) +
                     " " + shell_quote(td.file("app.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(dep), obj.string() + ": " + td.file("app.dc").string() + " " + td.path.string() + "/lib\\:dir/col.dc\n");
    CHECK(make_accepts(dep, obj));
}

TEST_CASE("prerequisite with backslash before a colon keeps both")
{
    TempDir td;
    write_module(td, "b\\:dir/colb.dc", "colb");
    write_module(td, "app.dc", "app", {"colb"});

    auto obj = td.file("app.o");
    auto dep = td.file("d.d");
    auto r = run_dcc("-c -target x86_64-elf -I " + shell_quote(td.file("b\\:dir")) + " -o " + shell_quote(obj) + " --depfile " + shell_quote(dep) +
                     " " + shell_quote(td.file("app.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(dep), obj.string() + ": " + td.file("app.dc").string() + " " + td.path.string() + "/b\\\\\\:dir/colb.dc\n");
    CHECK(make_accepts(dep, obj));
}

SECTION("Depfile: stability and ordering");

TEST_CASE("repeated runs produce byte-identical depfiles")
{
    TempDir td;
    write_module(td, "dep.dc", "dep");
    write_module(td, "app.dc", "app", {"dep"});

    auto args =
        "-c -target x86_64-elf -o " + shell_quote(td.file("app.o")) + " --depfile " + shell_quote(td.file("app.d")) + " " + shell_quote(td.file("app.dc"));

    CHECK_EQ(run_dcc(args).rc, 0);
    auto first = read_file(td.file("app.d"));
    CHECK_EQ(run_dcc(args).rc, 0);
    CHECK_EQ(first, read_file(td.file("app.d")));
}

TEST_CASE("root is first and the rest are sorted")
{
    TempDir td;
    write_module(td, "zebra.dc", "zebra");
    write_module(td, "alpha.dc", "alpha");
    write_module(td, "app.dc", "app", {"zebra", "alpha"});

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("app.o")) + " --depfile " + shell_quote(td.file("app.d")) + " " +
                     shell_quote(td.file("app.dc")));

    CHECK_EQ(r.rc, 0);
    auto content = read_file(td.file("app.d"));
    auto deps = deps_of(content);
    REQUIRE(deps.size() == 3);
    CHECK_EQ(deps[0], td.file("app.dc").string());
    CHECK_EQ(deps[1], td.file("alpha.dc").string());
    CHECK_EQ(deps[2], td.file("zebra.dc").string());
    CHECK_EQ(target_of(content), td.file("app.o").string());
}

SECTION("Depfile: escaping");

TEST_CASE("spaces, hash, dollar, and backslash are escaped in target and prerequisites")
{
    TempDir td;
    write_module(td, "s p#ce$dir\\bs/es.dc", "es");
    write_module(td, "root.dc", "root", {"es"});

    auto target = td.file("o ut#put$fi le\\3.o");
    auto depfile = td.file("de p#d$e.d");
    auto root = td.file("root.dc");
    auto prereq = td.file("s p#ce$dir\\bs/es.dc");

    auto r = run_dcc("-c -target x86_64-elf -I " + shell_quote(td.file("s p#ce$dir\\bs")) + " -o " + shell_quote(target) + " --depfile " +
                     shell_quote(depfile) + " " + shell_quote(root));

    CHECK_EQ(r.rc, 0);
    auto expected_target = td.path.string() + "/o\\ ut\\#put$$fi\\ le\\3.o";
    auto expected_prereq = td.path.string() + "/s\\ p\\#ce$$dir\\bs/es.dc";
    auto expected_root = root.string();
    CHECK_EQ(read_file(depfile), expected_target + ": " + expected_root + " " + expected_prereq + "\n");
    CHECK(make_accepts(depfile, target));
}

TEST_CASE("final prerequisite ending in one backslash gets a trailing separator")
{
    TempDir td;
    td.write_file("trail\\", "module trail;\npublic i32 q = 1;\n");

    auto src = td.file("trail\\");
    auto obj = td.file("trail.o");
    auto dep = td.file("trail.d");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(dep) + " " + shell_quote(src));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(dep), obj.string() + ": " + src.string() + " \n");
    CHECK(make_accepts(dep, obj));
}

TEST_CASE("final prerequisite ending in multiple backslashes keeps the whole run")
{
    TempDir td;
    td.write_file("trail\\\\", "module trail2;\npublic i32 q = 1;\n");

    auto src = td.file("trail\\\\");
    auto obj = td.file("trail.o");
    auto dep = td.file("trail.d");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(dep) + " " + shell_quote(src));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(dep), obj.string() + ": " + src.string() + " \n");
    CHECK(make_accepts(dep, obj));
}

TEST_CASE("prerequisite ending in backslash before another prerequisite")
{
    TempDir td;
    write_module(td, "aaa.dc", "aaa");
    td.write_file("trail\\", "module trail3;\nimport aaa;\npublic i32 q = 1;\n");

    auto src = td.file("trail\\");
    auto obj = td.file("trail.o");
    auto dep = td.file("trail.d");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(dep) + " " + shell_quote(src));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(dep), obj.string() + ": " + src.string() + "\\ " + td.file("aaa.dc").string() + "\n");
    CHECK(make_accepts(dep, obj));
}

TEST_CASE("target with one backslash before percent is an exact explicit rule")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto obj = td.file("a\\%b.o");
    auto dep = td.file("d.d");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(dep) + " " + shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(dep), td.path.string() + "/a\\\\\\%b.o: " + td.file("src.dc").string() + "\n");
    CHECK(make_accepts(dep, obj));
}

TEST_CASE("target with two backslashes before percent is an exact explicit rule")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto obj = td.file("a\\\\%b.o");
    auto dep = td.file("d.d");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(dep) + " " + shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(dep), td.path.string() + "/a\\\\\\\\\\%b.o: " + td.file("src.dc").string() + "\n");
    CHECK(make_accepts(dep, obj));
}

SECTION("Depfile: output targets");

TEST_CASE("exact -o path is the depfile target")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto obj = td.file("custom_name.o");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(td.file("d.d")) + " " + shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(target_of(read_file(td.file("d.d"))), obj.string());
}

TEST_CASE("default -c output derives the target from the input stem")
{
    TempDir td;
    write_module(td, "stemmed.dc", "stemmed");

    auto cmd = "cd " + shell_quote(td.path) + " && " + shell_quote(dcc_path()) + " -c -target x86_64-elf --depfile stemmed.d stemmed.dc 2>&1";
    auto r = run_shell(cmd);

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(td.file("stemmed.d")), "stemmed.o: " + td.file("stemmed.dc").string() + "\n");
    CHECK(file_exists(td.file("stemmed.o")));
}

TEST_CASE("-S output is the depfile target")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto asm_path = td.file("src.s");
    auto r = run_dcc("-S -target x86_64-elf -o " + shell_quote(asm_path) + " --depfile " + shell_quote(td.file("d.d")) + " " + shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(target_of(read_file(td.file("d.d"))), asm_path.string());
}

TEST_CASE("executable output is the depfile target")
{
    TempDir td;
    std::string content = "module ex;\n@nomangle\npublic void _start() { while (true) {} }\n";
    td.write_file("ex.dc", content);

    auto exe = td.file("runme");
    auto r = run_dcc("-target x86_64-elf -o " + shell_quote(exe) + " --depfile " + shell_quote(td.file("d.d")) + " " + shell_quote(td.file("ex.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(target_of(read_file(td.file("d.d"))), exe.string());
}

TEST_CASE("shared output is the depfile target")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto so = td.file("libsrc.so");
    auto r = run_dcc("-fbackend em64t -shared -target x86_64-elf -o " + shell_quote(so) + " --depfile " + shell_quote(td.file("d.d")) + " " +
                     shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(target_of(read_file(td.file("d.d"))), so.string());
}

#if DCC_ENABLE_LLVM
TEST_CASE("LLVM and em64t backends produce equivalent dependencies")
{
    TempDir td;
    write_module(td, "dep.dc", "dep");
    write_module(td, "app.dc", "app", {"dep"});

    auto obj = td.file("app.o");
    auto depfile = td.file("app.d");

    auto r1 = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(depfile) + " " + shell_quote(td.file("app.dc")));
    CHECK_EQ(r1.rc, 0);
    auto llvm_content = read_file(depfile);

    auto r2 =
        run_dcc("-fbackend em64t -c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(depfile) + " " + shell_quote(td.file("app.dc")));
    CHECK_EQ(r2.rc, 0);
    CHECK_EQ(llvm_content, read_file(depfile));
}
#endif

SECTION("Depfile: failure behavior");

TEST_CASE("failed import leaves an existing depfile untouched")
{
    TempDir td;
    td.write_file("keep.d", "ORIGINAL BYTES\n");
    td.write_file("bad.dc", "module bad;\nimport does_not_exist_xyz;\npublic i32 q = 1;\n");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("bad.o")) + " --depfile " + shell_quote(td.file("keep.d")) + " " +
                     shell_quote(td.file("bad.dc")));

    CHECK_NE(r.rc, 0);
    CHECK_EQ(read_file(td.file("keep.d")), "ORIGINAL BYTES\n");
    CHECK(!file_exists(td.file("bad.o")));
}

TEST_CASE("syntax error leaves an existing depfile untouched")
{
    TempDir td;
    td.write_file("keep.d", "ORIGINAL BYTES\n");
    td.write_file("bad.dc", "module bad;\nthis is not dcc syntax &&&\n");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("bad.o")) + " --depfile " + shell_quote(td.file("keep.d")) + " " +
                     shell_quote(td.file("bad.dc")));

    CHECK_NE(r.rc, 0);
    CHECK_EQ(read_file(td.file("keep.d")), "ORIGINAL BYTES\n");
}

TEST_CASE("output write failure leaves an existing depfile untouched")
{
    TempDir td;
    td.write_file("keep.d", "ORIGINAL BYTES\n");
    write_module(td, "src.dc", "src");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("no_dir") / "out.o") + " --depfile " + shell_quote(td.file("keep.d")) + " " +
                     shell_quote(td.file("src.dc")));

    CHECK_NE(r.rc, 0);
    CHECK_EQ(read_file(td.file("keep.d")), "ORIGINAL BYTES\n");
}

TEST_CASE("depfile write failure yields a diagnostic and nonzero status")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("src.o")) + " --depfile " + shell_quote(td.file("no_dir") / "d.d") + " " +
                     shell_quote(td.file("src.dc")));

    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("cannot write dependency file") != std::string::npos);
    CHECK(file_exists(td.file("src.o")));
}

TEST_CASE("depfile destination equal to the output artifact is rejected")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto obj = td.file("same.o");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(obj) + " " + shell_quote(td.file("src.dc")));

    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("same as the output artifact") != std::string::npos);
    CHECK(!file_exists(obj));
}

TEST_CASE("depfile destination equal to the root input is rejected and preserved")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto src = td.file("src.dc");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("src.o")) + " --depfile " + shell_quote(src) + " " + shell_quote(src));

    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("same as an input source file") != std::string::npos);
    CHECK_EQ(read_file(src), "module src;\npublic i32 q = 1;\n");
    CHECK(!file_exists(td.file("src.o")));
}
TEST_CASE("depfile destination equal to an imported module is rejected and preserved")
{
    TempDir td;
    write_module(td, "foo.dc", "foo");
    write_module(td, "app.dc", "app", {"foo"});

    auto dep = td.file("foo.dc");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("app.o")) + " --depfile " + shell_quote(dep) + " " +
                     shell_quote(td.file("app.dc")));

    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("same as an input source file") != std::string::npos);
    CHECK_EQ(read_file(dep), "module foo;\npublic i32 q = 1;\n");
}

TEST_CASE("tab in the output target is rejected with a diagnostic")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto dep = td.file("d.d");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("out\t.o")) + " --depfile " + shell_quote(dep) + " " +
                     shell_quote(td.file("src.dc")));

    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("cannot write dependency file") != std::string::npos);
    CHECK(r.output.find("tab") != std::string::npos);
    CHECK(!file_exists(dep));
}

TEST_CASE("tab in a source path is rejected with a diagnostic")
{
    TempDir td;
    td.write_file("src\t.dc", "module s;\npublic i32 q = 1;\n");

    auto dep = td.file("d.d");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("src.o")) + " --depfile " + shell_quote(dep) + " " +
                     shell_quote(td.file("src\t.dc")));

    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("cannot write dependency file") != std::string::npos);
    CHECK(r.output.find("tab") != std::string::npos);
    CHECK(!file_exists(dep));
}

TEST_CASE("no --depfile leaves no depfile side effect")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto obj = td.file("src.o");
    auto depfile = td.file("src.d");
    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(obj) + " " + shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK(file_exists(obj));
    CHECK(!file_exists(depfile));
}

SECTION("Depfile: option handling");

TEST_CASE("missing option argument is an option error")
{
    auto r = run_dcc("--depfile");
    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("unknown option: --depfile") != std::string::npos);
}

TEST_CASE("duplicate --depfile options are last-wins")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto first = td.file("first.d");
    auto second = td.file("second.d");
    auto r = run_dcc("--depfile " + shell_quote(first) + " --depfile " + shell_quote(second) + " -c -target x86_64-elf -o " + shell_quote(td.file("src.o")) +
                     " " + shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK(!file_exists(first));
    CHECK(file_exists(second));
}

TEST_CASE("dump-only invocation with no -o is rejected")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto depfile = td.file("d.d");
    auto r = run_dcc("-fdump-llvm --depfile " + shell_quote(depfile) + " " + shell_quote(td.file("src.dc")));

    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("--depfile requires a filesystem output artifact") != std::string::npos);
    CHECK(!file_exists(depfile));
}

#if DCC_ENABLE_LLVM
TEST_CASE("dump invocation with -o uses that exact target")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto out = td.file("dump.ll");
    auto depfile = td.file("d.d");
    auto r = run_dcc("-fdump-llvm -o " + shell_quote(out) + " --depfile " + shell_quote(depfile) + " " + shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(target_of(read_file(depfile)), out.string());
}
#endif
TEST_CASE("pure -fdump-ir with no -o rejects --depfile")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto depfile = td.file("d.d");
    auto r = run_dcc("-fdump-ir --depfile " + shell_quote(depfile) + " " + shell_quote(td.file("src.dc")));

    CHECK_NE(r.rc, 0);
    CHECK(r.output.find("--depfile requires a filesystem output artifact") != std::string::npos);
    CHECK(!file_exists(depfile));
}

TEST_CASE("pure -fdump-ir with -o writes the executable and targets it")
{
    TempDir td;
    td.write_file("ex.dc", "module ex;\n@nomangle\npublic void _start() { while (true) {} }\n");

    auto exe = td.file("runme");
    auto depfile = td.file("out.d");
    auto r = run_dcc("-fdump-ir -target x86_64-elf -o " + shell_quote(exe) + " --depfile " + shell_quote(depfile) + " " +
                     shell_quote(td.file("ex.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK(file_exists(exe));
    CHECK_EQ(target_of(read_file(depfile)), exe.string());
}

TEST_CASE("-fdump-ir combined with -c produces a real depfile target")
{
    TempDir td;
    write_module(td, "src.dc", "src");

    auto obj = td.file("out.o");
    auto depfile = td.file("out.d");
    auto r = run_dcc("-fdump-ir -c -target x86_64-elf -o " + shell_quote(obj) + " --depfile " + shell_quote(depfile) + " " +
                     shell_quote(td.file("src.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(target_of(read_file(depfile)), obj.string());
    CHECK(file_exists(obj));
}


TEST_CASE("virtual core module is excluded from dependencies")
{
    TempDir td;
    td.write_file("vc.dc", "module vc;\nimport core;\npublic i32 q = 1;\n");

    auto r = run_dcc("-c -target x86_64-elf -o " + shell_quote(td.file("vc.o")) + " --depfile " + shell_quote(td.file("vc.d")) + " " +
                     shell_quote(td.file("vc.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(td.file("vc.d")), td.file("vc.o").string() + ": " + td.file("vc.dc").string() + "\n");
}

TEST_CASE("injected declarations are not listed as dependencies")
{
    TempDir td;
    write_module(td, "jd.dc", "jd");

    auto r = run_dcc("-J 'public i32 injected_value = 9;' -c -target x86_64-elf -o " + shell_quote(td.file("jd.o")) + " --depfile " +
                     shell_quote(td.file("jd.d")) + " " + shell_quote(td.file("jd.dc")));

    CHECK_EQ(r.rc, 0);
    CHECK_EQ(read_file(td.file("jd.d")), td.file("jd.o").string() + ": " + td.file("jd.dc").string() + "\n");
}

SECTION("Depfile: help");

TEST_CASE("help lists the --depfile option with exact description")
{
    auto r = run_dcc("--help");
    CHECK_EQ(r.rc, 0);
    CHECK(r.output.find("--depfile <file>") != std::string::npos);
    CHECK(r.output.find("write Make-compatible module dependencies") != std::string::npos);
}
