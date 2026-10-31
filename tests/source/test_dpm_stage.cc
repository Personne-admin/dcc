#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import dpm.cache;
import dpm.manifest;
import dpm.scaffold;
import dpm.stage;

#include "harness.hh"

namespace
{
    namespace st = dpm::stage;
    namespace sc = dpm::scaffold;
    namespace fs = std::filesystem;

    struct TempDir
    {
        fs::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("dpm_stage_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            fs::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            fs::permissions(path, fs::perms::owner_all, ec);
            dpm::cache::detail::remove_tree(path);
        }

        void write(std::string const& rel, std::string const& content, bool exec = false) const
        {
            auto full = path / rel;
            fs::create_directories(full.parent_path());
            std::ofstream out{full, std::ios::binary};
            out << content;
            out.close();
            if (exec)
                fs::permissions(full, fs::perms::owner_all);
        }

        [[nodiscard]] std::string read(std::string const& rel) const
        {
            std::ifstream in{path / rel, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        }
    };

    fs::path env_path(char const* name)
    {
        char const* value = ::getenv(name);
        return value ? fs::path{value} : fs::path{};
    }

    dpm::manifest::LoadOptions options()
    {
        dpm::manifest::LoadOptions o;
        o.schema_root = env_path("DCC_TEST_DPM_SCHEMA");
        return o;
    }

    bool mentions(std::vector<std::string> const& lines, std::string_view needle)
    {
        return std::ranges::any_of(lines, [&](std::string const& l) { return l.find(needle) != std::string::npos; });
    }

    constexpr std::string_view k_minimal = "dpm::v1::Template { name = \"tiny\" }\n";

    bool ready()
    {
        return !env_path("DCC_TEST_DPM_SCHEMA").empty() && !env_path("DCC_TEST_DPM_TEMPLATES").empty();
    }
} // namespace

SECTION("dpm template directories");

TEST_CASE("the sample template loads with every file, directory and mode")
{
    if (!ready())
        return;
    auto loaded = st::load_template(env_path("DCC_TEST_DPM_TEMPLATES") / "sample", options());
    REQUIRE(loaded.ok());
    CHECK_EQ(loaded.tmpl.name, std::string{"sample"});
    std::map<std::string, sc::File const*> by_path;
    for (auto const& f : loaded.tree)
        by_path[f.path] = &f;
    for (std::string path : {"project.dcon", "README.md", "src", "src/main.dc", "scripts", "scripts/run.sh", "data/blob.bin", "docs/@@name@@.md", ".gitignore", "skip/ignored.txt"})
        CHECK(by_path.contains(path));
    REQUIRE(by_path.contains("scripts/run.sh"));
    CHECK(by_path["scripts/run.sh"]->executable);
    CHECK(!by_path["README.md"]->executable);
    CHECK(by_path["src"]->directory);
    CHECK(by_path["data/blob.bin"]->content == std::string("raw @@name@@ \0 bytes\n", 21));
}

TEST_CASE("the sample template passes the template check without warnings")
{
    if (!ready())
        return;
    auto loaded = st::load_template(env_path("DCC_TEST_DPM_TEMPLATES") / "sample", options());
    auto checked = st::check_template(loaded, options());
    CHECK(checked.ok());
    CHECK(checked.warnings.empty());
    CHECK_EQ(checked.files, std::size_t{7});
}

TEST_CASE("a missing or damaged template.dcon is reported")
{
    if (!ready())
        return;
    TempDir td;
    CHECK(mentions(st::load_template(td.path / "absent", options()).problems, "not a directory"));
    CHECK(mentions(st::load_template(td.path, options()).problems, "has no template.dcon"));
    td.write("template.dcon", "dpm::v1::Template { name = \"X\" }\n");
    CHECK(mentions(st::load_template(td.path, options()).problems, "not a valid template name"));
    td.write("template.dcon", "dpm::v1::Template {");
    CHECK(!st::load_template(td.path, options()).ok());
}

TEST_CASE("the template root must be a directory")
{
    if (!ready())
        return;
    TempDir td;
    td.write("template.dcon", std::string{k_minimal});
    CHECK(mentions(st::load_template(td.path, options()).problems, "template root 'files' is not a directory"));
    td.write("files", "a file");
    CHECK(mentions(st::load_template(td.path, options()).problems, "template root 'files' is not a directory"));
}

TEST_CASE("a custom root is used and a .git directory at its top is skipped")
{
    if (!ready())
        return;
    TempDir td;
    td.write("template.dcon", "dpm::v1::Template { name = \"tiny\", root = \"payload\" }\n");
    td.write("payload/a.txt", "a");
    td.write("payload/.git/config", "x");
    td.write("payload/sub/.git/keep", "y");
    td.write("files/ignored.txt", "z");
    auto loaded = st::load_template(td.path, options());
    REQUIRE(loaded.ok());
    std::set<std::string> paths;
    for (auto const& f : loaded.tree)
        paths.insert(f.path);
    CHECK(paths.contains("a.txt"));
    CHECK(!paths.contains(".git"));
    CHECK(!paths.contains(".git/config"));
    CHECK(paths.contains("sub/.git/keep"));
    CHECK(!paths.contains("ignored.txt"));
}

TEST_CASE("symbolic links and special files in a template are refused")
{
    if (!ready())
        return;
    TempDir td;
    td.write("template.dcon", std::string{k_minimal});
    td.write("files/a.txt", "a");
    std::error_code ec;
    fs::create_symlink("a.txt", td.path / "files" / "link", ec);
    REQUIRE(!ec);
    auto loaded = st::load_template(td.path, options());
    CHECK(!loaded.ok());
    CHECK(mentions(loaded.problems, "link: symbolic links are not allowed"));
    fs::remove(td.path / "files" / "link");
    REQUIRE(::mkfifo((td.path / "files" / "pipe").c_str(), 0600) == 0);
    auto with_fifo = st::load_template(td.path, options());
    CHECK(!with_fifo.ok());
    CHECK(mentions(with_fifo.problems, "pipe: only regular files and directories"));
}

TEST_CASE("names that differ only in case are refused in the template tree")
{
    if (!ready())
        return;
    TempDir td;
    td.write("template.dcon", std::string{k_minimal});
    td.write("files/Readme", "a");
    td.write("files/README", "b");
    auto loaded = st::load_template(td.path, options());
    CHECK(!loaded.ok());
    CHECK(mentions(loaded.problems, "differs only in case"));
}

TEST_CASE("the template check reports placeholders that name no variable, with file and line")
{
    if (!ready())
        return;
    TempDir td;
    td.write("template.dcon", "dpm::v1::Template { name = \"tiny\", substitute = { \"*.md\" } }\n");
    td.write("files/a.md", "one\ntwo @@nothing@@\n");
    auto checked = st::check_template(st::load_template(td.path, options()), options());
    CHECK(!checked.ok());
    CHECK(mentions(checked.problems, "a.md:2: unknown variable 'nothing'"));
}

TEST_CASE("warnings name unused variables and patterns that match nothing")
{
    if (!ready())
        return;
    TempDir td;
    td.write("template.dcon", "dpm::v1::Template { name = \"tiny\", substitute = { \"*.md\", \"*.zzz\" }, exclude = { \"gone/**\" },"
                              " variables = { { name = \"used\", default_value = \"x\" }, { name = \"idle\", default_value = \"y\" } },"
                              " next_steps = \"@@used@@\" }\n");
    td.write("files/a.md", "text\n");
    auto checked = st::check_template(st::load_template(td.path, options()), options());
    CHECK(checked.ok());
    CHECK(mentions(checked.warnings, "'*.zzz' matches no file"));
    CHECK(mentions(checked.warnings, "'gone/**' matches no file"));
    CHECK(mentions(checked.warnings, "variable 'idle' is not used"));
    CHECK(!mentions(checked.warnings, "variable 'used'"));
    CHECK(!mentions(checked.warnings, "'*.md'"));
}

TEST_CASE("a generated project.dcon that does not load is a template error")
{
    if (!ready())
        return;
    TempDir td;
    td.write("template.dcon", "dpm::v1::Template { name = \"tiny\", substitute = { \"project.dcon\" } }\n");
    td.write("files/project.dcon", "dpm::v1::Project { name = \"@@name@@\", version = \"nope\" }\n");
    auto checked = st::check_template(st::load_template(td.path, options()), options());
    CHECK(!checked.ok());
    CHECK(mentions(checked.problems, "generated project.dcon"));
    CHECK(mentions(checked.problems, "not a valid version"));
}

TEST_CASE("a template cannot generate a .git entry")
{
    if (!ready())
        return;
    TempDir td;
    td.write("template.dcon", "dpm::v1::Template { name = \"tiny\", variables = { { name = \"dir\", default_value = \"x\" } } }\n");
    CHECK(!st::load_template(td.path, options()).ok());
    TempDir other;
    other.write("template.dcon", "dpm::v1::Template { name = \"tiny\", variables = { { name = \"part\", default_value = \".git\" } } }\n");
    other.write("files/@@part@@/config", "x");
    auto checked = st::check_template(st::load_template(other.path, options()), options());
    CHECK(!checked.ok());
    CHECK(mentions(checked.problems, "cannot generate .git"));
}

namespace
{
    std::vector<sc::File> two_files()
    {
        std::vector<sc::File> files;
        files.push_back({"a.txt", "alpha"});
        files.push_back({"sub/b.sh", "#!/bin/sh\n", true});
        files.push_back({"empty", "", false, false, true});
        return files;
    }
} // namespace

TEST_CASE("a new project is written, with modes, into a directory that does not exist yet")
{
    TempDir td;
    auto target = td.path / "deep" / "er" / "proj";
    auto written = st::write_new(target, two_files());
    REQUIRE(written.ok);
    CHECK_EQ(td.read("deep/er/proj/a.txt"), std::string{"alpha"});
    CHECK_EQ(td.read("deep/er/proj/sub/b.sh"), std::string{"#!/bin/sh\n"});
    CHECK(fs::is_directory(target / "empty"));
    struct stat sa{};
    struct stat sb{};
    REQUIRE(::stat((target / "a.txt").c_str(), &sa) == 0);
    REQUIRE(::stat((target / "sub/b.sh").c_str(), &sb) == 0);
    CHECK((sa.st_mode & 0111) == 0);
    CHECK((sb.st_mode & 0100) != 0);
    for (auto const& e : fs::directory_iterator(td.path / "deep" / "er"))
        CHECK_EQ(e.path().filename().string(), std::string{"proj"});
}

TEST_CASE("an empty existing directory, with or without .git, is filled")
{
    TempDir td;
    fs::create_directories(td.path / "empty_dir");
    REQUIRE(st::write_new(td.path / "empty_dir", two_files()).ok);
    CHECK_EQ(td.read("empty_dir/a.txt"), std::string{"alpha"});
    td.write("with_git/.git/HEAD", "ref");
    REQUIRE(st::write_new(td.path / "with_git", two_files()).ok);
    CHECK_EQ(td.read("with_git/.git/HEAD"), std::string{"ref"});
    CHECK_EQ(td.read("with_git/a.txt"), std::string{"alpha"});
    std::size_t entries = 0;
    for ([[maybe_unused]] auto const& e : fs::directory_iterator(td.path / "with_git"))
        ++entries;
    CHECK_EQ(entries, std::size_t{4});
}

TEST_CASE("a non-empty directory or a file is left untouched")
{
    TempDir td;
    td.write("busy/keep.txt", "mine");
    auto refused = st::write_new(td.path / "busy", two_files());
    CHECK(!refused.ok);
    CHECK(refused.error.find("not empty") != std::string::npos);
    CHECK_EQ(td.read("busy/keep.txt"), std::string{"mine"});
    std::size_t entries = 0;
    for ([[maybe_unused]] auto const& e : fs::directory_iterator(td.path / "busy"))
        ++entries;
    CHECK_EQ(entries, std::size_t{1});
    td.write("plain", "x");
    CHECK(!st::write_new(td.path / "plain", two_files()).ok);
    CHECK_EQ(td.read("plain"), std::string{"x"});
}

TEST_CASE("a failure leaves no staging directory and no half-written project")
{
    if (::geteuid() == 0)
        return;
    TempDir td;
    fs::create_directories(td.path / "locked");
    fs::permissions(td.path / "locked", fs::perms::owner_read | fs::perms::owner_exec);
    auto written = st::write_new(td.path / "locked" / "proj", two_files());
    CHECK(!written.ok);
    fs::permissions(td.path / "locked", fs::perms::owner_all);
    std::size_t entries = 0;
    for ([[maybe_unused]] auto const& e : fs::directory_iterator(td.path / "locked"))
        ++entries;
    CHECK_EQ(entries, std::size_t{0});
}

TEST_CASE("a trailing slash on the target is accepted")
{
    TempDir td;
    REQUIRE(st::write_new(td.path / "slashed/", two_files()).ok);
    CHECK_EQ(td.read("slashed/a.txt"), std::string{"alpha"});
}
