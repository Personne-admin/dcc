import std;
import dpm.cache;
import dpm.fetch;
import dpm.hash;
import dpm.manifest;
import dpm.registry;
import dpm.resolve;
import dpm.semver;

#include "harness.hh"

#include <stdlib.h>

namespace
{
    namespace reg = dpm::registry;

    struct TempDir
    {
        std::filesystem::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("dpm_registry_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(path);
        }

        ~TempDir() { dpm::cache::detail::remove_tree(path); }

        void write(std::string const& rel, std::string const& content) const
        {
            auto full = path / rel;
            std::filesystem::create_directories(full.parent_path());
            std::ofstream f{full, std::ios::binary};
            f << content;
        }
    };

    std::filesystem::path schema_root()
    {
        char const* value = ::getenv("DCC_TEST_DPM_SCHEMA");
        return value ? std::filesystem::path{value} : std::filesystem::path{};
    }

    std::string const k_commit(40, 'c');
    std::string const k_hash = "sha256:" + std::string(64, 'a');
    std::string const k_other_hash = "sha256:" + std::string(64, 'b');

    std::string version_text(std::string const& version, std::string const& source, std::string const& hash = k_hash, std::string const& extra = {})
    {
        bool const git = source.starts_with("git+");
        return std::format("{{ version = \"{}\", source = \"{}\",{} hash = \"{}\"{} }}", version, source, git ? std::format(" commit = \"{}\",", k_commit) : std::string{}, hash, extra);
    }

    std::string alias_text(std::string const& alias, std::string const& location, std::vector<std::string> const& versions)
    {
        std::string joined;
        for (auto const& v : versions)
            joined += (joined.empty() ? "" : ", ") + v;
        return std::format("dpm::v1::RegistryAlias {{ alias = \"{}\", location = \"{}\", versions = {{ {} }} }}\n", alias, location, joined);
    }

    constexpr std::string_view k_config = "dpm::v1::RegistryConfig { name = \"test\" }\n";

    void put_alias(TempDir const& td, std::string const& alias, std::string const& location, std::vector<std::string> const& versions)
    {
        td.write(reg::alias_path(alias), alias_text(alias, location, versions));
    }

    reg::Config make_config(TempDir const& registry, TempDir const& cache_dir)
    {
        (void)cache_dir;
        reg::Config config;
        config.schema_root = schema_root();
        config.allow_file = true;
        config.urls = {"file://" + registry.path.string()};
        return config;
    }

    std::string const k_git = "git+https://github.com/u/json5.git";

    struct Inner : dpm::resolve::RequirementSource
    {
        int calls{0};

        std::optional<dpm::resolve::PackageInfo> info(std::string_view location, std::string_view version, std::string& error) override
        {
            ++calls;
            if (location == "github.com/u/plain")
            {
                dpm::resolve::PackageInfo info;
                info.version = std::string{version};
                info.name = "plain";
                return info;
            }
            error = "unknown package";
            return std::nullopt;
        }
    };
} // namespace

SECTION("dpm registry");

TEST_CASE("registry URLs are https, or file and loopback http only when enabled")
{
    CHECK(!reg::url_error("https://example.com/reg", false, false));
    CHECK(!reg::url_error("https://example.com:8443/reg/", false, false));
    CHECK(reg::url_error("http://example.com/reg", false, false).has_value());
    CHECK(reg::url_error("http://127.0.0.1:8000/reg", false, false).has_value());
    CHECK(!reg::url_error("http://127.0.0.1:8000/reg", false, true));
    CHECK(!reg::url_error("http://localhost/reg", false, true));
    CHECK(!reg::url_error("http://[::1]:80/reg", false, true));
    CHECK(reg::url_error("http://example.com/reg", false, true).has_value());
    CHECK(reg::url_error("http://127.0.0.1.evil.com/reg", false, true).has_value());
    CHECK(reg::url_error("http://127.0.0.1@evil.com/reg", false, true).has_value());
    CHECK(reg::url_error("http://localhost:abc/reg", false, true).has_value());
    CHECK(reg::url_error("file:///tmp/reg", false, false).has_value());
    CHECK(!reg::url_error("file:///tmp/reg", true, false));
    CHECK(reg::url_error("file://relative", true, false).has_value());
    CHECK(reg::url_error("ftp://example.com/reg", true, true).has_value());
    CHECK(reg::url_error("https://user:pw@example.com/reg", false, false).has_value());
    CHECK(reg::url_error("https://example.com/reg?x=1", false, false).has_value());
    CHECK(reg::url_error("https://example.com/re g", false, false).has_value());
    CHECK(reg::url_error("https://", false, false).has_value());
    CHECK_EQ(reg::normalize_url("https://example.com/reg//"), std::string{"https://example.com/reg"});
}

TEST_CASE("registry selection prefers the environment, then the project, then the user, then the default")
{
    auto user = std::vector<std::string>{"https://user.example/reg"};
    auto a = reg::choose_registries("https://env.example/reg", "https://project.example/reg", user, false, false);
    CHECK(a.error.empty());
    CHECK_EQ(a.urls, std::vector<std::string>{"https://env.example/reg"});
    auto b = reg::choose_registries(std::nullopt, "https://project.example/reg/", user, false, false);
    CHECK_EQ(b.urls, std::vector<std::string>{"https://project.example/reg"});
    auto c = reg::choose_registries(std::nullopt, "", user, false, false);
    CHECK_EQ(c.urls, user);
    auto d = reg::choose_registries(std::nullopt, "", {}, false, false);
    CHECK_EQ(d.urls, std::vector<std::string>{std::string{reg::k_default_url}});
    auto off = reg::choose_registries(std::string{}, "https://project.example/reg", user, false, false);
    CHECK(off.error.empty());
    CHECK(off.urls.empty());
    auto bad = reg::choose_registries("http://example.com/reg", "", {}, false, false);
    CHECK(!bad.error.empty());
    CHECK(bad.urls.empty());
    auto file = reg::choose_registries("file:///tmp/reg", "", {}, false, false);
    CHECK(!file.error.empty());
}

TEST_CASE("registry file paths are derived from validated names")
{
    CHECK_EQ(reg::alias_path("json5"), std::string{"aliases/js/json5.dcon"});
    CHECK_EQ(reg::alias_path("x"), std::string{"aliases/_x/x.dcon"});
    CHECK_EQ(reg::location_path("github.com/u/p").value_or(""), std::string{"locations/github.com/u/p.dcon"});
    CHECK_EQ(reg::location_path("github.com/u/p/sub/dir").value_or(""), std::string{"locations/github.com/u/p/sub/dir.dcon"});
    CHECK(!reg::location_path("git+https://example.com/p.git"));
    CHECK(!reg::location_path("https://example.com/p.tar.gz"));
    CHECK(!reg::location_path("example.com/u/p"));
    CHECK(!reg::location_path("github.com/u/p q"));
    CHECK(!reg::location_path("github.com/u/../p"));
    CHECK(!reg::location_path("github.com/u/p%2f"));
    CHECK(!reg::location_path("github.com/u"));
}

TEST_CASE("latest version skips yanked and pre-release versions and honours the series")
{
    if (schema_root().empty())
        return;
    dpm::manifest::RegistryAlias record;
    auto add = [&](std::string v, bool yanked)
    {
        dpm::manifest::RegistryVersion e;
        e.version = std::move(v);
        e.yanked = yanked;
        record.versions.push_back(std::move(e));
    };
    add("1.0.0", false);
    add("1.2.0", false);
    add("1.3.0", true);
    add("2.0.0-rc.1", false);
    add("2.1.0", false);
    add("2.2.0", true);
    CHECK_EQ(reg::latest_version(record, std::nullopt).value_or(""), std::string{"2.1.0"});
    CHECK_EQ(reg::latest_version(record, dpm::semver::series_of(*dpm::semver::parse("1.0.0"))).value_or(""), std::string{"1.2.0"});
    CHECK(!reg::latest_version(record, dpm::semver::series_of(*dpm::semver::parse("3.0.0"))));
    dpm::manifest::RegistryAlias empty;
    CHECK(!reg::latest_version(empty, std::nullopt));
}

TEST_CASE("an alias is read from a file registry and the first registry with it wins")
{
    if (schema_root().empty())
        return;
    TempDir first;
    TempDir second;
    TempDir home;
    first.write("registry.dcon", std::string{k_config});
    second.write("registry.dcon", std::string{k_config});
    put_alias(second, "json5", "github.com/u/json5", {version_text("1.0.0", k_git)});
    put_alias(second, "other", "github.com/u/other", {version_text("1.0.0", "git+https://github.com/u/other.git")});
    auto config = make_config(first, home);
    config.urls.push_back("file://" + second.path.string());
    reg::Client client{dpm::cache::Cache{home.path}, config};
    std::string error;
    auto hit = client.alias("json5", error);
    REQUIRE(hit.has_value());
    CHECK_EQ(hit->record.location, std::string{"github.com/u/json5"});
    CHECK_EQ(hit->registry, "file://" + second.path.string());
    put_alias(first, "json5", "github.com/v/json5", {version_text("1.0.0", "git+https://github.com/v/json5.git")});
    reg::Client again{dpm::cache::Cache{home.path}, config};
    auto winner = again.alias("json5", error);
    REQUIRE(winner.has_value());
    CHECK_EQ(winner->record.location, std::string{"github.com/v/json5"});
    CHECK_EQ(winner->registry, "file://" + first.path.string());
    CHECK(!again.alias("nothing", error).has_value());
    CHECK(error.find("not in any registry") != std::string::npos);
}

TEST_CASE("a registry without a descriptor, with a newer format, or an invalid file is refused")
{
    if (schema_root().empty())
        return;
    TempDir registry;
    TempDir home;
    put_alias(registry, "json5", "github.com/u/json5", {version_text("1.0.0", k_git)});
    {
        reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
        std::string error;
        CHECK(!client.alias("json5", error).has_value());
        CHECK(error.find("registry.dcon") != std::string::npos);
    }
    registry.write("registry.dcon", "dpm::v1::RegistryConfig { format = 2, name = \"x\" }\n");
    {
        reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
        std::string error;
        CHECK(!client.alias("json5", error).has_value());
        CHECK(error.find("unsupported registry format 2") != std::string::npos);
    }
    registry.write("registry.dcon", std::string{k_config});
    registry.write(reg::alias_path("broken"), "dpm::v1::RegistryAlias { alias = \"broken\", location = ");
    {
        reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
        std::string error;
        CHECK(!client.alias("broken", error).has_value());
        CHECK(!error.empty());
    }
}

TEST_CASE("an alias record must describe the alias that was asked for and its sources must belong to its location")
{
    if (schema_root().empty())
        return;
    TempDir registry;
    TempDir home;
    registry.write("registry.dcon", std::string{k_config});
    registry.write(reg::alias_path("json5"), alias_text("other", "github.com/u/json5", {version_text("1.0.0", k_git)}));
    registry.write(reg::alias_path("moved"), alias_text("moved", "github.com/u/moved", {version_text("1.0.0", "git+https://evil.example/u/moved.git")}));
    registry.write(reg::alias_path("mirror"), alias_text("mirror", "github.com/u/mirror", {version_text("1.0.0", "https://example.com/mirror-1.0.0.tar.gz")}));
    registry.write(reg::alias_path("tarloc"), alias_text("tarloc", "https://example.com/tarloc.tar.gz", {version_text("1.0.0", k_git)}));
    reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
    std::string error;
    CHECK(!client.alias("json5", error).has_value());
    CHECK(error.find("describes alias 'other'") != std::string::npos);
    CHECK(!client.alias("moved", error).has_value());
    CHECK(error.find("does not belong to github.com/u/moved") != std::string::npos);
    CHECK(client.alias("mirror", error).has_value());
    CHECK(!client.alias("tarloc", error).has_value());
}

namespace
{
    std::string template_text(std::string const& name, std::string const& location, std::vector<std::string> const& versions)
    {
        std::string joined;
        for (auto const& v : versions)
            joined += (joined.empty() ? "" : ", ") + v;
        return std::format("dpm::v1::RegistryTemplate {{ name = \"{}\", location = \"{}\", versions = {{ {} }} }}\n", name, location, joined);
    }
} // namespace

TEST_CASE("template record paths are derived from the template name and live apart from aliases")
{
    CHECK_EQ(reg::template_path("limine-x86-64"), std::string{"templates/li/limine-x86-64.dcon"});
    CHECK_EQ(reg::template_path("ab"), std::string{"templates/ab/ab.dcon"});
    CHECK_EQ(reg::template_path("x"), std::string{"templates/_x/x.dcon"});
    CHECK(reg::template_path("json5") != reg::alias_path("json5"));
}

TEST_CASE("a template is read from the templates directory, not from the aliases")
{
    if (schema_root().empty())
        return;
    TempDir registry;
    TempDir home;
    registry.write("registry.dcon", std::string{k_config});
    std::string const git = "git+https://github.com/u/tpl.git";
    registry.write(reg::template_path("kernel"), template_text("kernel", "github.com/u/tpl", {version_text("1.0.0", git), version_text("1.1.0", git, k_hash, ", yanked = true")}));
    put_alias(registry, "kernel", "github.com/u/some-package", {version_text("2.0.0", "git+https://github.com/u/some-package.git")});
    reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
    std::string error;
    auto hit = client.tmpl("kernel", error);
    REQUIRE(hit.has_value());
    CHECK_EQ(hit->record.location, std::string{"github.com/u/tpl"});
    CHECK_EQ(hit->record.versions.size(), std::size_t{2});
    CHECK_EQ(hit->registry, "file://" + registry.path.string());
    auto package = client.alias("kernel", error);
    REQUIRE(package.has_value());
    CHECK_EQ(package->record.location, std::string{"github.com/u/some-package"});
    CHECK(!client.tmpl("absent", error).has_value());
    CHECK(error.find("template 'absent' is not in any registry") != std::string::npos);
    CHECK(!client.alias("absent", error).has_value());
    CHECK(error.find("alias 'absent' is not in any registry") != std::string::npos);
    CHECK_EQ(reg::latest_version(hit->record.versions, std::nullopt).value_or(""), std::string{"1.0.0"});
}

TEST_CASE("a template record must describe the template that was asked for and its sources must belong to its location")
{
    if (schema_root().empty())
        return;
    TempDir registry;
    TempDir home;
    registry.write("registry.dcon", std::string{k_config});
    registry.write(reg::template_path("liar"), template_text("other-name", "github.com/u/tpl", {version_text("1.0.0", "git+https://github.com/u/tpl.git")}));
    registry.write(reg::template_path("moved"), template_text("moved", "github.com/u/moved", {version_text("1.0.0", "git+https://evil.example/u/moved.git")}));
    registry.write(reg::template_path("broken"), "dpm::v1::RegistryTemplate { name = \"broken\" }\n");
    registry.write(reg::template_path("alias-file"), alias_text("alias-file", "github.com/u/p", {}));
    reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
    std::string error;
    CHECK(!client.tmpl("liar", error).has_value());
    CHECK(error.find("describes template 'other-name'") != std::string::npos);
    CHECK(!client.tmpl("moved", error).has_value());
    CHECK(error.find("does not belong to github.com/u/moved") != std::string::npos);
    CHECK(!client.tmpl("broken", error).has_value());
    CHECK(!client.tmpl("alias-file", error).has_value());
}

TEST_CASE("a template lookup is remembered, and a registry without templates simply has none")
{
    if (schema_root().empty())
        return;
    TempDir registry;
    TempDir home;
    registry.write("registry.dcon", std::string{k_config});
    reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
    std::string error;
    CHECK(!client.tmpl("kernel", error).has_value());
    registry.write(reg::template_path("kernel"), template_text("kernel", "github.com/u/tpl", {}));
    CHECK(!client.tmpl("kernel", error).has_value());
    reg::Client fresh{dpm::cache::Cache{home.path}, make_config(registry, home)};
    CHECK(fresh.tmpl("kernel", error).has_value());
}

TEST_CASE("registry files larger than the limit are refused")
{
    if (schema_root().empty())
        return;
    TempDir registry;
    TempDir home;
    registry.write("registry.dcon", std::string{k_config});
    registry.write(reg::alias_path("big"),
                   alias_text("big", "github.com/u/big", {version_text("1.0.0", "git+https://github.com/u/big.git")}) + std::string(reg::k_max_file + 1, ' '));
    reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
    std::string error;
    CHECK(!client.alias("big", error).has_value());
    CHECK(error.find("larger than") != std::string::npos);
}

TEST_CASE("a location pointer finds the alias record when it agrees about the location")
{
    if (schema_root().empty())
        return;
    TempDir registry;
    TempDir home;
    registry.write("registry.dcon", std::string{k_config});
    put_alias(registry, "json5", "github.com/u/json5", {version_text("1.0.0", k_git)});
    put_alias(registry, "liar", "github.com/u/liar", {version_text("1.0.0", "git+https://github.com/u/liar.git")});
    registry.write("locations/github.com/u/json5.dcon", "dpm::v1::RegistryLocation { alias = \"json5\" }\n");
    registry.write("locations/github.com/x/imposter.dcon", "dpm::v1::RegistryLocation { alias = \"liar\" }\n");
    registry.write("locations/github.com/x/dangling.dcon", "dpm::v1::RegistryLocation { alias = \"nowhere\" }\n");
    reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
    auto hit = client.by_location("github.com/u/json5");
    REQUIRE(hit.has_value());
    CHECK_EQ(hit->record.alias, std::string{"json5"});
    CHECK(!client.by_location("github.com/u/unlisted").has_value());
    CHECK(!client.by_location("github.com/x/imposter").has_value());
    CHECK(!client.by_location("github.com/x/dangling").has_value());
    CHECK(!client.by_location("git+https://example.com/p.git").has_value());
    auto warnings = client.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{2});
}

TEST_CASE("the registry source answers from entries, keeps the lock's pins and falls back to the inner source")
{
    if (schema_root().empty())
        return;
    TempDir registry;
    TempDir home;
    registry.write("registry.dcon", std::string{k_config});
    put_alias(registry, "json5", "github.com/u/json5",
              {version_text("1.0.0", k_git), version_text("1.1.0", k_git, k_hash, ", yanked = true, requires_dcc = \"0.6.0\", dependencies = { { location = \"github.com/u/dep\", version = \"2.0.0\" }, { location = \"util\", version = \"1.0.0\" } }")});
    registry.write("locations/github.com/u/json5.dcon", "dpm::v1::RegistryLocation { alias = \"json5\" }\n");
    reg::Client client{dpm::cache::Cache{home.path}, make_config(registry, home)};
    Inner inner;
    dpm::manifest::Lock lock;
    dpm::manifest::LockedPackage pinned;
    pinned.location = "github.com/u/json5";
    pinned.version = "1.0.0";
    pinned.source = k_git;
    pinned.commit = std::string(40, 'd');
    pinned.hash = k_other_hash;
    lock.packages.push_back(pinned);
    reg::RegistrySource source{client, inner, &lock};
    std::string error;
    auto canonical = source.canonical("json5", error);
    REQUIRE(canonical.has_value());
    CHECK_EQ(*canonical, std::string{"github.com/u/json5"});
    auto locked = source.info("github.com/u/json5", "1.0.0", error);
    REQUIRE(locked.has_value());
    CHECK_EQ(locked->commit, std::string(40, 'd'));
    CHECK_EQ(locked->hash, k_other_hash);
    CHECK_EQ(source.warnings.size(), std::size_t{1});
    CHECK(!locked->yanked);
    auto fresh = source.info("github.com/u/json5", "1.1.0", error);
    REQUIRE(fresh.has_value());
    CHECK(fresh->yanked);
    CHECK_EQ(fresh->commit, k_commit);
    CHECK_EQ(fresh->hash, k_hash);
    CHECK_EQ(fresh->requires_dcc, std::string{"0.6.0"});
    REQUIRE(fresh->requirements.size() == std::size_t{2});
    CHECK_EQ(fresh->requirements[0].location, std::string{"github.com/u/dep"});
    CHECK_EQ(fresh->requirements[1].location, std::string{"util"});
    CHECK_EQ(inner.calls, 0);
    auto versions = source.versions("github.com/u/json5");
    CHECK_EQ(versions, (std::vector<std::string>{"1.0.0", "1.1.0"}));
    auto unlisted = source.info("github.com/u/json5", "1.5.0", error);
    CHECK(!unlisted.has_value());
    CHECK_EQ(inner.calls, 1);
    auto plain = source.info("github.com/u/plain", "1.0.0", error);
    REQUIRE(plain.has_value());
    CHECK_EQ(inner.calls, 2);
    CHECK(source.info("path:../x", "", error).has_value() == false);
}

TEST_CASE("cached registry files are served offline and when fresh, and stale copies survive an unreachable registry")
{
    if (schema_root().empty())
        return;
    TempDir home;
    dpm::cache::Cache cache{home.path};
    std::string base = "https://127.0.0.1:1/reg";
    reg::Config config;
    config.schema_root = schema_root();
    config.urls = {base};
    config.connect_timeout = std::chrono::seconds{2};
    config.total_timeout = std::chrono::seconds{5};
    auto cached_path = [&](std::string const& rel) { return cache.registry_dir() / dpm::hash::sha256_hex(base).substr(0, 16) / rel; };
    auto write_cached = [&](std::string const& rel, std::string const& text, std::chrono::hours age)
    {
        auto path = cached_path(rel);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream{path, std::ios::binary} << text;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - age);
    };
    write_cached("registry.dcon", std::string{k_config}, std::chrono::hours{1});
    write_cached(reg::alias_path("json5"), alias_text("json5", "github.com/u/json5", {version_text("1.0.0", k_git)}), std::chrono::hours{1});
    {
        config.offline = true;
        reg::Client client{cache, config};
        std::string error;
        auto hit = client.alias("json5", error);
        REQUIRE(hit.has_value());
        CHECK(!client.alias("absent", error).has_value());
        CHECK(error.find("not in cache") != std::string::npos);
    }
    {
        config.offline = false;
        reg::Client client{cache, config};
        std::string error;
        CHECK(client.alias("json5", error).has_value());
        CHECK(client.take_warnings().empty());
    }
    write_cached(reg::alias_path("json5"), alias_text("json5", "github.com/u/json5", {version_text("1.0.0", k_git)}), std::chrono::hours{72});
    {
        reg::Client client{cache, config};
        std::string error;
        auto hit = client.alias("json5", error);
        REQUIRE(hit.has_value());
        auto warnings = client.take_warnings();
        REQUIRE(warnings.size() == std::size_t{1});
        CHECK(warnings[0].find("using the cached copy") != std::string::npos);
    }
    {
        config.refresh = true;
        reg::Client client{cache, config};
        std::string error;
        CHECK(client.alias("json5", error).has_value());
        CHECK(!client.alias("absent", error).has_value());
        CHECK(error.find("cannot fetch") != std::string::npos);
    }
}
