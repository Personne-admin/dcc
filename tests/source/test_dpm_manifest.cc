import std;
import dpm.manifest;

#include "harness.hh"

struct TempDir
{
    std::filesystem::path path;
    static inline int s_counter{0};

    TempDir()
    {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("dpm_test_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    void write_file(std::filesystem::path const& relative, std::string const& content) const
    {
        auto full = path / relative;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream f{full};
        f << content;
    }
};

constexpr std::string_view k_schema = R"(module dpm::v1;

public enum Optimize : u8 { Default, Speed, Size }

public struct Dependency {
    []const char location;
    []const char version;
}

public const []const char DefaultName = "shared";
public const Dependency DefaultDependency = { location = "github.com/x/y", version = "2.0.0" };

public struct Project {
    []const char name;
    []const char version;
    Optimize optimize;
    bool flag;
    i32 count;
    []const Dependency dependencies;
}
)";

[[nodiscard]] bool contains(std::string const& haystack, std::string_view needle)
{
    return haystack.contains(needle);
}

SECTION("dpm manifest spike");

TEST_CASE("sema and ctfe evaluate a manifest initializer into a tree")
{
    TempDir td;
    td.write_file("dpm/v1.dc", std::string{k_schema});
    auto result = dpm::manifest::spike_evaluate(R"(dpm::v1::Project {
    // comment inside the manifest
    name = "hello",
    version = "1.0.0",
    optimize = dpm::v1::Optimize::Speed,
    flag = true,
    count = -3,
    dependencies = {
        { location = "github.com/a/b", version = "1.2.0" },
        { location = "github.com/c/d", version = "0.3.1" },
    },
})",
                                                td.path);
    REQUIRE(result.ok);
    CHECK(result.errors.empty());
    CHECK(contains(result.tree, "name = \"hello\""));
    CHECK(contains(result.tree, "optimize = Optimize::Speed"));
    CHECK(contains(result.tree, "flag = true"));
    CHECK(contains(result.tree, "count = -3"));
    CHECK(contains(result.tree, "location = \"github.com/c/d\""));
}

TEST_CASE("references to other constants are resolved by the evaluator")
{
    TempDir td;
    td.write_file("dpm/v1.dc", std::string{k_schema});
    auto result = dpm::manifest::spike_evaluate(R"({ name = dpm::v1::DefaultName, version = "1", optimize = dpm::v1::Optimize::Size, flag = false, count = 1,
        dependencies = { dpm::v1::DefaultDependency, }, })",
                                                td.path);
    REQUIRE(result.ok);
    CHECK(contains(result.tree, "name = \"shared\""));
    CHECK(contains(result.tree, "location = \"github.com/x/y\""));
    CHECK(contains(result.tree, "version = \"2.0.0\""));
}

TEST_CASE("an initializer without the type prefix evaluates against the declared type")
{
    TempDir td;
    td.write_file("dpm/v1.dc", std::string{k_schema});
    auto result = dpm::manifest::spike_evaluate(
        R"({ name = "a", version = "1", optimize = dpm::v1::Optimize::Size, flag = false, count = 0, dependencies = {}, })", td.path);
    REQUIRE(result.ok);
    CHECK(contains(result.tree, "optimize = Optimize::Size"));
    CHECK(contains(result.tree, "dependencies = []"));
}

TEST_CASE("sema errors are reported with user line and column")
{
    TempDir td;
    td.write_file("dpm/v1.dc", std::string{k_schema});
    auto mismatch = dpm::manifest::spike_evaluate("{\n    name = 3,\n}", td.path);
    CHECK(!mismatch.ok);
    bool found_mismatch = false;
    for (auto const& error : mismatch.errors)
        if (error.starts_with("2:") && contains(error, "type mismatch"))
            found_mismatch = true;
    CHECK(found_mismatch);
    auto unknown = dpm::manifest::spike_evaluate("{\n    name = \"a\",\n    versoin = \"1\",\n}", td.path);
    CHECK(!unknown.ok);
    bool found_unknown = false;
    for (auto const& error : unknown.errors)
        if (error.starts_with("3:") && contains(error, "versoin"))
            found_unknown = true;
    CHECK(found_unknown);
}

TEST_CASE("errors on the first line have the wrapper prefix removed from the column")
{
    TempDir td;
    td.write_file("dpm/v1.dc", std::string{k_schema});
    auto result = dpm::manifest::spike_evaluate("{ bogus = 1 }", td.path);
    CHECK(!result.ok);
    bool found = false;
    for (auto const& error : result.errors)
        if (error.starts_with("1:3:") && contains(error, "bogus"))
            found = true;
    CHECK(found);
}

TEST_CASE("a missing schema module is an error")
{
    TempDir td;
    auto result = dpm::manifest::spike_evaluate("{}", td.path);
    CHECK(!result.ok);
    CHECK(!result.errors.empty());
}
