import std;
import dpm.hash;
import dpm.lock;
import dpm.manifest;
import dpm.resolve;
import dpm.semver;

#include "harness.hh"

#include <stdlib.h>

namespace
{
    using dpm::resolve::PackageInfo;
    using dpm::resolve::Requirement;

    struct TempDir
    {
        std::filesystem::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("dpm_resolve_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write(std::filesystem::path const& relative, std::string const& content) const
        {
            auto full = path / relative;
            std::filesystem::create_directories(full.parent_path());
            std::ofstream f{full, std::ios::binary};
            f << content;
        }
    };

    [[nodiscard]] std::filesystem::path env_path(char const* name)
    {
        char const* value = ::getenv(name);
        return value ? std::filesystem::path{value} : std::filesystem::path{};
    }

    struct Fake : dpm::resolve::RequirementSource
    {
        std::map<std::pair<std::string, std::string>, PackageInfo> packages;
        std::map<std::string, std::string> alias_map;
        std::map<std::string, std::vector<std::string>> version_lists;
        int info_calls{0};

        void add(std::string const& location, std::string const& version, std::vector<Requirement> deps = {}, std::string name = {})
        {
            PackageInfo info;
            info.name = name.empty() ? location.substr(location.rfind('/') + 1) : std::move(name);
            info.version = version;
            info.requirements = std::move(deps);
            info.source = "git+https://" + location + ".git";
            info.commit = std::string(40, 'a');
            info.hash = "sha256:" + std::string(64, 'b');
            packages[{location, version}] = std::move(info);
            version_lists[location].push_back(version);
        }

        std::optional<std::string> canonical(std::string_view alias, std::string& error) override
        {
            auto it = alias_map.find(std::string{alias});
            if (it == alias_map.end())
            {
                error = "unknown alias";
                return std::nullopt;
            }
            return it->second;
        }

        std::optional<PackageInfo> info(std::string_view location, std::string_view version, std::string& error) override
        {
            ++info_calls;
            auto it = packages.find({std::string{location}, std::string{version}});
            if (it == packages.end())
            {
                error = "no such version";
                return std::nullopt;
            }
            return it->second;
        }

        std::vector<std::string> versions(std::string_view location) override
        {
            auto it = version_lists.find(std::string{location});
            return it == version_lists.end() ? std::vector<std::string>{} : it->second;
        }
    };

    dpm::manifest::Dependency dep(std::string const& location, std::string const& version)
    {
        dpm::manifest::Dependency d;
        d.location = location;
        d.version = version;
        return d;
    }

    dpm::manifest::Project project(std::vector<dpm::manifest::Dependency> deps, std::vector<dpm::manifest::Dependency> dev = {})
    {
        dpm::manifest::Project p;
        p.name = "app";
        p.version = "1.0.0";
        p.dependencies = std::move(deps);
        p.dev_dependencies = std::move(dev);
        return p;
    }

    std::map<std::string, std::string> versions_of(dpm::resolve::Resolution const& r)
    {
        std::map<std::string, std::string> out;
        for (auto const& [location, p] : r.packages)
            out[location] = p.version;
        return out;
    }

    using VersionMap = std::map<std::string, std::string>;

    std::string joined(std::vector<std::string> const& v)
    {
        std::string out;
        for (auto const& s : v)
            out += (out.empty() ? "" : "\n") + s;
        return out;
    }

    constexpr auto a = "github.com/u/a";
    constexpr auto b = "github.com/u/b";
    constexpr auto c = "github.com/u/c";
    constexpr auto d = "github.com/u/d";
    constexpr auto e = "github.com/u/e";
} // namespace

SECTION("dpm resolve");

TEST_CASE("a chain resolves to the named minimums")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(b, "1.0.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK(versions_of(r) == (VersionMap{{a, "1.0.0"}, {b, "1.0.0"}}));
    REQUIRE(r.lock.packages.size() == 2);
    CHECK_EQ(r.lock.packages[0].location, std::string{a});
    CHECK(r.lock.packages[0].dependencies == std::vector<std::string>{b});
    CHECK(r.lock.packages[1].dependencies.empty());
    CHECK_EQ(r.lock.packages[0].hash, "sha256:" + std::string(64, 'b'));
    CHECK(r.roots == std::vector<std::string>{a});
}

TEST_CASE("a diamond takes the maximum of the minimums and follows the selected requirements")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.1.0"}});
    f.add(c, "1.0.0", {{b, "1.2.0"}});
    f.add(b, "1.1.0", {{e, "1.0.0"}});
    f.add(b, "1.2.0", {{d, "1.0.0"}});
    f.add(d, "1.0.0");
    f.add(e, "1.0.0");
    auto r = dpm::resolve::resolve(project({dep(c, "1.0.0"), dep(a, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK_EQ(r.packages.at(b).version, std::string{"1.2.0"});
    CHECK(r.packages.at(b).dependencies == std::vector<std::string>{d});
    CHECK(r.packages.contains(e));
}

TEST_CASE("a requirement raised by a higher version is picked up")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(a, "1.3.0", {{b, "1.4.0"}});
    f.add(b, "1.0.0");
    f.add(b, "1.4.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.3.0"), dep(b, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK(versions_of(r) == (VersionMap{{a, "1.3.0"}, {b, "1.4.0"}}));
}

TEST_CASE("a root minimum above a transitive one wins")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(b, "1.0.0");
    f.add(b, "1.5.0");
    auto r = dpm::resolve::resolve(project({dep(b, "1.5.0"), dep(a, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK_EQ(r.packages.at(b).version, std::string{"1.5.0"});
}

TEST_CASE("two series of one package are an error that names both paths")
{
    Fake f;
    f.add(a, "1.0.0", {{c, "1.2.0"}});
    f.add(b, "1.0.0", {{c, "2.0.0"}});
    f.add(c, "1.2.0");
    f.add(c, "2.0.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0"), dep(b, "1.0.0")}), f, {});
    CHECK(!r.ok);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].starts_with("github.com/u/c needed as 1.x by "));
    CHECK(r.errors[0].find(" and as 2.x by ") != std::string::npos);
}

TEST_CASE("the series conflict message shows requirer and route")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(b, "1.0.0", {{c, "1.0.0"}});
    f.add(c, "1.0.0");
    f.add(c, "2.0.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0"), dep(c, "2.0.0")}), f, {});
    CHECK(!r.ok);
    REQUIRE(r.errors.size() == 1);
    CHECK_EQ(r.errors[0], std::string{"github.com/u/c needed as 1.x by github.com/u/b@1.0.0 (via github.com/u/a@1.0.0) and as 2.x by app"});
}

TEST_CASE("zero-major series are per minor")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "0.1.0"}});
    f.add(c, "1.0.0", {{b, "0.1.5"}});
    f.add(d, "1.0.0", {{b, "0.2.0"}});
    f.add(b, "0.1.0");
    f.add(b, "0.1.5");
    f.add(b, "0.2.0");
    auto ok = dpm::resolve::resolve(project({dep(a, "1.0.0"), dep(c, "1.0.0")}), f, {});
    REQUIRE(ok.ok);
    CHECK_EQ(ok.packages.at(b).version, std::string{"0.1.5"});
    auto bad = dpm::resolve::resolve(project({dep(a, "1.0.0"), dep(d, "1.0.0")}), f, {});
    CHECK(!bad.ok);
    CHECK(bad.errors[0].find("0.1.x") != std::string::npos);
    CHECK(bad.errors[0].find("0.2.x") != std::string::npos);
}

TEST_CASE("dev dependencies are marked and shared packages are not")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(b, "1.0.0");
    f.add(c, "1.0.0", {{d, "1.0.0"}, {b, "1.0.0"}});
    f.add(d, "1.0.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}, {dep(c, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK(!r.packages.at(a).dev);
    CHECK(!r.packages.at(b).dev);
    CHECK(r.packages.at(c).dev);
    CHECK(r.packages.at(d).dev);
    CHECK(r.lock.packages[2].dev);
}

TEST_CASE("aliases resolve to canonical locations and unknown aliases fail")
{
    Fake f;
    f.alias_map["json5"] = a;
    f.add(a, "1.0.0");
    auto r = dpm::resolve::resolve(project({dep("json5", "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK_EQ(r.lock.packages[0].location, std::string{a});
    auto bad = dpm::resolve::resolve(project({dep("nope", "1.0.0")}), f, {});
    CHECK(!bad.ok);
    CHECK(bad.errors[0].find("nope") != std::string::npos);
}

TEST_CASE("an alias and its location are one package")
{
    Fake f;
    f.alias_map["alpha"] = a;
    f.add(a, "1.0.0", {{"alpha", "1.2.0"}});
    f.add(a, "1.2.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK_EQ(r.packages.size(), std::size_t{1});
    CHECK_EQ(r.packages.at(a).version, std::string{"1.2.0"});
}

TEST_CASE("cycles terminate")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(b, "1.0.0", {{a, "1.0.0"}});
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK_EQ(r.packages.size(), std::size_t{2});
}

TEST_CASE("an unavailable version is an error naming it")
{
    Fake f;
    f.add(a, "1.0.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.9.0")}), f, {});
    CHECK(!r.ok);
    CHECK(r.errors[0].find("github.com/u/a@1.9.0") != std::string::npos);
}

TEST_CASE("a yanked minimum moves to the next unyanked version in its series")
{
    Fake f;
    f.add(a, "1.0.0");
    f.add(a, "1.0.1");
    f.add(a, "1.1.0");
    f.add(a, "2.0.0");
    f.packages[{a, "1.0.0"}].yanked = true;
    f.packages[{a, "1.0.1"}].yanked = true;
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK_EQ(r.packages.at(a).version, std::string{"1.1.0"});
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("yanked") != std::string::npos);
}

TEST_CASE("a yanked version already in the lock is kept with a warning")
{
    Fake f;
    f.add(a, "1.0.0");
    f.add(a, "1.1.0");
    f.packages[{a, "1.0.0"}].yanked = true;
    dpm::manifest::Lock previous;
    dpm::manifest::LockedPackage lp;
    lp.location = a;
    lp.version = "1.0.0";
    previous.packages.push_back(lp);
    dpm::resolve::Options options;
    options.previous = &previous;
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, options);
    REQUIRE(r.ok);
    CHECK_EQ(r.packages.at(a).version, std::string{"1.0.0"});
    CHECK_EQ(r.warnings.size(), std::size_t{1});
}

TEST_CASE("a yanked version with no replacement is an error")
{
    Fake f;
    f.add(a, "1.0.0");
    f.add(a, "2.0.0");
    f.packages[{a, "1.0.0"}].yanked = true;
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, {});
    CHECK(!r.ok);
    CHECK(r.errors[0].find("yanked") != std::string::npos);
}

TEST_CASE("requires_dcc is checked against the given compiler version")
{
    Fake f;
    f.add(a, "1.0.0");
    f.packages[{a, "1.0.0"}].requires_dcc = "0.7.0";
    dpm::resolve::Options options;
    options.dcc_version = "0.6.0";
    auto bad = dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, options);
    CHECK(!bad.ok);
    CHECK(bad.errors[0].find("requires dcc 0.7.0") != std::string::npos);
    options.dcc_version = "0.7.1";
    CHECK(dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, options).ok);
    CHECK(dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, {}).ok);
}

TEST_CASE("two packages with one namespace are an error")
{
    Fake f;
    f.add(a, "1.0.0", {}, "shared");
    f.add(b, "1.0.0", {}, "shared");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0"), dep(b, "1.0.0")}), f, {});
    CHECK(!r.ok);
    CHECK(r.errors[0].find("both provide the namespace 'shared'") != std::string::npos);
    Fake g;
    g.add(a, "1.0.0", {}, "app");
    CHECK(!dpm::resolve::resolve(project({dep(a, "1.0.0")}), g, {}).ok);
}

TEST_CASE("input order does not change the result")
{
    std::mt19937 rng{2024};
    for (int round = 0; round < 25; ++round)
    {
        Fake f;
        std::vector<std::string> names;
        for (int i = 0; i < 12; ++i)
            names.push_back(std::format("github.com/u/p{:02}", i));
        for (int i = 0; i < 12; ++i)
            for (int minor = 0; minor < 4; ++minor)
            {
                std::vector<Requirement> deps;
                for (int j = i + 1; j < 12; ++j)
                    if (rng() % 3 == 0)
                        deps.push_back({names[static_cast<std::size_t>(j)], std::format("1.{}.0", rng() % 4)});
                f.add(names[static_cast<std::size_t>(i)], std::format("1.{}.0", minor), std::move(deps));
            }
        std::vector<dpm::manifest::Dependency> roots;
        for (int i = 0; i < 12; i += 2)
            roots.push_back(dep(names[static_cast<std::size_t>(i)], std::format("1.{}.0", rng() % 4)));
        auto first = dpm::resolve::resolve(project(roots), f, {});
        REQUIRE(first.ok);
        auto expected = dpm::lock::print(first.lock);
        for (int shuffle = 0; shuffle < 4; ++shuffle)
        {
            std::ranges::shuffle(roots, rng);
            for (auto& [key, info] : f.packages)
                std::ranges::shuffle(info.requirements, rng);
            auto again = dpm::resolve::resolve(project(roots), f, {});
            REQUIRE(again.ok);
            CHECK_EQ(dpm::lock::print(again.lock), expected);
        }
        for (auto const& [location, p] : first.packages)
        {
            auto const& info = f.packages.at({location, p.version});
            for (auto const& req : info.requirements)
            {
                auto it = first.packages.find(req.location);
                REQUIRE(it != first.packages.end());
                auto have = dpm::semver::parse(it->second.version);
                auto need = dpm::semver::parse(req.version);
                CHECK(dpm::semver::compare(*have, *need) >= 0);
            }
        }
        for (auto const& root : roots)
            CHECK(dpm::semver::compare(*dpm::semver::parse(first.packages.at(root.location).version), *dpm::semver::parse(root.version)) >= 0);
    }
}

TEST_CASE("a large graph resolves quickly")
{
    Fake f;
    for (int i = 0; i < 1000; ++i)
    {
        std::vector<Requirement> deps;
        for (int j : {i + 1, i + 7, i + 31})
            if (j < 1000)
                deps.push_back({std::format("github.com/u/p{:04}", j), "1.0.0"});
        f.add(std::format("github.com/u/p{:04}", i), "1.0.0", std::move(deps));
    }
    auto start = std::chrono::steady_clock::now();
    auto r = dpm::resolve::resolve(project({dep("github.com/u/p0000", "1.0.0")}), f, {});
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(r.ok);
    CHECK_EQ(r.packages.size(), std::size_t{1000});
    CHECK(elapsed < std::chrono::seconds(5));
}

TEST_CASE("tree and why render the selection")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(c, "1.0.0", {{b, "1.2.0"}});
    f.add(b, "1.0.0");
    f.add(b, "1.2.0");
    f.add(d, "1.0.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0"), dep(c, "1.0.0")}, {dep(d, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK_EQ(dpm::resolve::render_tree(r, "1.0.0"),
             std::string{"app@1.0.0\n"
                         "|-- github.com/u/a@1.0.0\n"
                         "|   `-- github.com/u/b@1.2.0\n"
                         "|-- github.com/u/c@1.0.0\n"
                         "|   `-- github.com/u/b@1.2.0\n"
                         "`-- github.com/u/d@1.0.0 [dev]\n"});
    auto text = dpm::resolve::why(r, b);
    REQUIRE(text.has_value());
    CHECK_EQ(*text,
             std::string{"github.com/u/b 1.2.0\n"
                         "  github.com/u/a@1.0.0 needs >= 1.0.0\n"
                         "  github.com/u/c@1.0.0 needs >= 1.2.0  <- selected\n"});
    CHECK(!dpm::resolve::why(r, "github.com/u/zzz").has_value());
}

TEST_CASE("a repeated package with dependencies is expanded once")
{
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(c, "1.0.0", {{b, "1.0.0"}});
    f.add(b, "1.0.0", {{d, "1.0.0"}});
    f.add(d, "1.0.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0"), dep(c, "1.0.0")}), f, {});
    REQUIRE(r.ok);
    CHECK_EQ(dpm::resolve::render_tree(r, "1.0.0"),
             std::string{"app@1.0.0\n"
                         "|-- github.com/u/a@1.0.0\n"
                         "|   `-- github.com/u/b@1.0.0\n"
                         "|       `-- github.com/u/d@1.0.0\n"
                         "`-- github.com/u/c@1.0.0\n"
                         "    `-- github.com/u/b@1.0.0 (*)\n"});
}

SECTION("dpm path sources");

TEST_CASE("path packages resolve through live manifests and normalize nested paths")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    TempDir td;
    td.write("util/project.dcon", R"(dpm::v1::Project { name = "util", version = "0.3.0", dependencies = { { location = "path:../shared" } } })");
    td.write("shared/project.dcon", R"(dpm::v1::Project { name = "shared", version = "1.4.0" })");
    td.write("libs/extra/project.dcon", R"(dpm::v1::Project { name = "extra", version = "2.0.0", dependencies = { { location = "path:../../shared" } } })");
    dpm::resolve::PathSource source{td.path, schema_root};
    auto p = project({dep("path:util", ""), dep("path:./libs/extra/", "")});
    auto r = dpm::resolve::resolve(p, source, {});
    if (!r.ok)
        std::println(std::cerr, "{}", joined(r.errors));
    REQUIRE(r.ok);
    REQUIRE(r.lock.packages.size() == 3);
    CHECK_EQ(r.lock.packages[0].location, std::string{"path:libs/extra"});
    CHECK_EQ(r.lock.packages[1].location, std::string{"path:shared"});
    CHECK_EQ(r.lock.packages[2].location, std::string{"path:util"});
    CHECK_EQ(r.lock.packages[1].version, std::string{"1.4.0"});
    CHECK_EQ(r.lock.packages[1].source, std::string{"path:shared"});
    CHECK(r.lock.packages[1].hash.empty());
    CHECK(r.lock.packages[2].dependencies == std::vector<std::string>{"path:shared"});
}

TEST_CASE("a missing or invalid path package is an error")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    TempDir td;
    td.write("bad/project.dcon", "{ nope }");
    dpm::resolve::PathSource source{td.path, schema_root};
    CHECK(!dpm::resolve::resolve(project({dep("path:missing", "")}), source, {}).ok);
    CHECK(!dpm::resolve::resolve(project({dep("path:bad", "")}), source, {}).ok);
    CHECK(!dpm::resolve::resolve(project({dep(a, "1.0.0")}), source, {}).ok);
}

TEST_CASE("only path packages may require path dependencies")
{
    Fake f;
    f.add(a, "1.0.0", {{"path:../x", ""}});
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}), f, {});
    CHECK(!r.ok);
    CHECK(r.errors[0].find("only path packages") != std::string::npos);
}

SECTION("dpm lock model");

TEST_CASE("the fingerprint ignores order and formatting but not requirements")
{
    auto base = project({dep(a, "1.0.0"), dep(b, "1.0.0")}, {dep(c, "1.0.0")});
    auto reordered = project({dep(b, "1.0.0"), dep(a, "1.0.0")}, {dep(c, "1.0.0")});
    auto fp = dpm::lock::fingerprint(base);
    CHECK_EQ(fp, dpm::lock::fingerprint(reordered));
    CHECK(fp.starts_with("sha256:"));
    CHECK_NE(fp, dpm::lock::fingerprint(project({dep(a, "1.0.1"), dep(b, "1.0.0")}, {dep(c, "1.0.0")})));
    CHECK_NE(fp, dpm::lock::fingerprint(project({dep(a, "1.0.0"), dep(b, "1.0.0"), dep(c, "1.0.0")})));
    CHECK_NE(fp, dpm::lock::fingerprint(project({dep(a, "1.0.0")}, {dep(c, "1.0.0")})));
    auto renamed = base;
    renamed.name = "other";
    renamed.description = "x";
    CHECK_EQ(fp, dpm::lock::fingerprint(renamed));
}

TEST_CASE("assets change the fingerprint only when declared, independent of their order; tasks never do")
{
    auto base = project({dep(a, "1.0.0"), dep(b, "1.0.0")}, {dep(c, "1.0.0")});
    std::vector<std::string> sorted{std::format("dep\t{}\t1.0.0\n", a), std::format("dep\t{}\t1.0.0\n", b), std::format("dev\t{}\t1.0.0\n", c)};
    std::ranges::sort(sorted);
    std::string expected = "dpm::v1\n";
    for (auto const& line : sorted)
        expected += line;
    CHECK_EQ(dpm::lock::fingerprint(base), "sha256:" + dpm::hash::sha256_hex(expected));
    auto with = base;
    dpm::manifest::Asset x{"limine", "https://example.com/l.tar.gz", "v1", 0, {}};
    dpm::manifest::Asset y{"ovmf", "path:../ovmf", "", 0, {}};
    with.assets = {x, y};
    auto fp = dpm::lock::fingerprint(with);
    CHECK_NE(fp, dpm::lock::fingerprint(base));
    auto swapped = base;
    swapped.assets = {y, x};
    CHECK_EQ(fp, dpm::lock::fingerprint(swapped));
    auto version = with;
    version.assets[0].version = "v2";
    CHECK_NE(fp, dpm::lock::fingerprint(version));
    auto location = with;
    location.assets[1].location = "path:../ovmf2";
    CHECK_NE(fp, dpm::lock::fingerprint(location));
    auto renamed = with;
    renamed.assets[1].name = "firmware";
    CHECK_NE(fp, dpm::lock::fingerprint(renamed));
    auto tasks = with;
    dpm::manifest::Task t;
    t.name = "iso";
    t.command = {"sh", "make-iso.sh"};
    tasks.tasks.push_back(t);
    CHECK_EQ(fp, dpm::lock::fingerprint(tasks));
}

TEST_CASE("a printed lock loads back to the same lock and prints identically")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    Fake f;
    f.add(a, "1.0.0", {{b, "1.0.0"}});
    f.add(b, "1.0.0");
    f.add(c, "0.2.0");
    auto r = dpm::resolve::resolve(project({dep(a, "1.0.0")}, {dep(c, "0.2.0")}), f, {});
    REQUIRE(r.ok);
    r.lock.dcc = "0.6.0";
    auto text = dpm::lock::print(r.lock);
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root;
    options.kind = dpm::manifest::FileKind::Lock;
    options.file_name = "project.lock.dcon";
    auto loaded = dpm::manifest::load(text, options);
    if (!loaded.ok)
        for (auto const& diagnostic : loaded.diagnostics)
            std::println(std::cerr, "{}", diagnostic.format());
    REQUIRE(loaded.ok);
    REQUIRE(loaded.lock.has_value());
    CHECK_EQ(dpm::lock::print(*loaded.lock), text);
    CHECK_EQ(loaded.lock->packages.size(), std::size_t{3});
    CHECK(loaded.lock->packages[2].dev);
    CHECK(dpm::lock::is_current(project({dep(a, "1.0.0")}, {dep(c, "0.2.0")}), *loaded.lock));
    CHECK(!dpm::lock::is_current(project({dep(a, "1.1.0")}, {dep(c, "0.2.0")}), *loaded.lock));
    CHECK(text.find("dev = Tri::On") != std::string::npos);
}

TEST_CASE("an empty lock prints and loads")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    dpm::manifest::Lock lock;
    lock.manifest = "sha256:" + std::string(64, '0');
    auto text = dpm::lock::print(lock);
    CHECK_EQ(text, "dpm::v1::Lock {\n    manifest = \"sha256:" + std::string(64, '0') + "\",\n    packages = {},\n}\n");
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root;
    options.kind = dpm::manifest::FileKind::Lock;
    CHECK(dpm::manifest::load(text, options).ok);
}

TEST_CASE("path packages print with an empty hash and load")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    dpm::manifest::Lock lock;
    lock.manifest = "sha256:" + std::string(64, '0');
    dpm::manifest::LockedPackage p;
    p.location = "path:../x";
    p.version = "1.0.0";
    p.source = "path:../x";
    lock.packages.push_back(p);
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root;
    options.kind = dpm::manifest::FileKind::Lock;
    auto loaded = dpm::manifest::load(dpm::lock::print(lock), options);
    REQUIRE(loaded.ok);
    CHECK(loaded.lock->packages[0].hash.empty());
}
