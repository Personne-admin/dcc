import std;
import dpm.extract;

#include "harness.hh"

#include <sys/stat.h>
#include <unistd.h>

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("dpm_extract_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write(std::filesystem::path const& relative, std::string const& content = "x") const
        {
            auto full = path / relative;
            std::filesystem::create_directories(full.parent_path());
            std::ofstream f{full, std::ios::binary};
            f << content;
        }
    };

    bool mentions(std::vector<dpm::extract::Problem> const& problems, std::string_view path, std::string_view message)
    {
        return std::ranges::any_of(problems, [&](auto const& p) { return p.path == path && p.message.find(message) != std::string::npos; });
    }
} // namespace

SECTION("dpm extraction safety");

TEST_CASE("ordinary member names are accepted")
{
    for (std::string_view name : {"a", "a/b/c.dc", "src/x.dc", ".hidden", "a..b", "dir/..name", "name..", "\xc3\xa9/x"})
        CHECK(!dpm::extract::member_name_problem(name).has_value());
}

TEST_CASE("hostile member names are rejected")
{
    CHECK(dpm::extract::member_name_problem("../x").has_value());
    CHECK(dpm::extract::member_name_problem("a/../x").has_value());
    CHECK(dpm::extract::member_name_problem("a/..").has_value());
    CHECK(dpm::extract::member_name_problem("..").has_value());
    CHECK(dpm::extract::member_name_problem("/etc/passwd").has_value());
    CHECK(dpm::extract::member_name_problem("C:/x").has_value());
    CHECK(dpm::extract::member_name_problem("c:x").has_value());
    CHECK(dpm::extract::member_name_problem("a\\b").has_value());
    CHECK(dpm::extract::member_name_problem(std::string{"a\0b", 3}).has_value());
    CHECK(dpm::extract::member_name_problem("").has_value());
    CHECK(dpm::extract::member_name_problem("a//b").has_value());
    CHECK(dpm::extract::member_name_problem(std::string(4097, 'a')).has_value());
    CHECK(!dpm::extract::member_name_problem(std::string(4096, 'a')).has_value());
}

TEST_CASE("a clean tree has no problems")
{
    TempDir td;
    td.write("project.dcon");
    td.write("src/a/b.dc");
    std::filesystem::create_directories(td.path / "empty");
    CHECK(dpm::extract::check_tree(td.path).empty());
}

TEST_CASE("symbolic links are reported, including dangling and directory links")
{
    TempDir td;
    td.write("real/f");
    std::filesystem::create_symlink("real/f", td.path / "file-link");
    std::filesystem::create_symlink("nowhere", td.path / "dangling");
    std::filesystem::create_symlink("real", td.path / "dir-link");
    std::filesystem::create_symlink("/etc", td.path / "abs-link");
    auto problems = dpm::extract::check_tree(td.path);
    CHECK(mentions(problems, "file-link", "symbolic link"));
    CHECK(mentions(problems, "dangling", "symbolic link"));
    CHECK(mentions(problems, "dir-link", "symbolic link"));
    CHECK(mentions(problems, "abs-link", "symbolic link"));
    CHECK_EQ(problems.size(), std::size_t{4});
}

TEST_CASE("hard links are reported")
{
    TempDir td;
    td.write("a");
    std::filesystem::create_hard_link(td.path / "a", td.path / "b");
    auto problems = dpm::extract::check_tree(td.path);
    CHECK(mentions(problems, "a", "hard link"));
    CHECK(mentions(problems, "b", "hard link"));
}

TEST_CASE("fifos are reported")
{
    TempDir td;
    ::mkfifo((td.path / "pipe").c_str(), 0600);
    CHECK(mentions(dpm::extract::check_tree(td.path), "pipe", "special"));
}

TEST_CASE("names differing only in case collide within a directory")
{
    TempDir td;
    td.write("Readme");
    td.write("README");
    td.write("sub/Foo.dc");
    td.write("other/foo.dc");
    auto problems = dpm::extract::check_tree(td.path);
    REQUIRE(problems.size() == std::size_t{1});
    CHECK_EQ(problems[0].path, std::string{"Readme"});
    CHECK(problems[0].message.find("case") != std::string::npos);
}

TEST_CASE("backslashes in names are reported")
{
    TempDir td;
    td.write("a\\b");
    CHECK(mentions(dpm::extract::check_tree(td.path), "a\\b", "backslash"));
}

TEST_CASE("file count and size limits")
{
    TempDir td;
    for (int i = 0; i < 5; ++i)
        td.write("f" + std::to_string(i), "1234567890");
    dpm::extract::Limits limits;
    limits.max_files = 4;
    CHECK(mentions(dpm::extract::check_tree(td.path, limits), "", "files"));
    limits = {};
    limits.max_bytes = 49;
    CHECK(mentions(dpm::extract::check_tree(td.path, limits), "", "bytes"));
    limits.max_bytes = 50;
    limits.max_files = 5;
    CHECK(dpm::extract::check_tree(td.path, limits).empty());
}

TEST_CASE("a path over the length limit is reported")
{
    TempDir td;
    td.write("deep/file");
    dpm::extract::Limits limits;
    limits.max_path = 8;
    CHECK(mentions(dpm::extract::check_tree(td.path, limits), "deep/file", "longer"));
}

TEST_CASE("problems are sorted and the root must be a directory")
{
    TempDir td;
    std::filesystem::create_symlink("x", td.path / "z");
    std::filesystem::create_symlink("x", td.path / "a");
    auto problems = dpm::extract::check_tree(td.path);
    REQUIRE(problems.size() == std::size_t{2});
    CHECK_EQ(problems[0].path, std::string{"a"});
    CHECK(!dpm::extract::check_tree(td.path / "missing").empty());
    CHECK_EQ(problems[0].format(), std::string{"a: symbolic link"});
}
