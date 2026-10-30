import std;
import dpm.cache;
import dpm.hash;

#include "harness.hh"

#include <stdlib.h>
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
            path = std::filesystem::temp_directory_path() / ("dpm_cache_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            dpm::cache::detail::remove_tree(path);
        }
    };

    void write(std::filesystem::path const& full, std::string const& content)
    {
        std::filesystem::create_directories(full.parent_path());
        std::ofstream f{full, std::ios::binary};
        f << content;
    }

    std::filesystem::path stage(dpm::cache::Cache const& cache, std::string const& main_content = "main")
    {
        std::string error;
        auto staging = cache.new_staging(error);
        CHECK(staging.has_value());
        if (!staging)
            return {};
        write(*staging / "project.dcon", main_content);
        write(*staging / "src/a.dc", "module a;");
        return *staging;
    }

    bool writable(std::filesystem::path const& p)
    {
        return ::access(p.c_str(), W_OK) == 0;
    }
} // namespace

SECTION("dpm cache");

TEST_CASE("default root follows the environment")
{
    auto saved = [](char const* name) -> std::optional<std::string>
    {
        char const* v = ::getenv(name);
        return v ? std::optional<std::string>{v} : std::nullopt;
    };
    auto home = saved("DPM_HOME");
    auto xdg = saved("XDG_CACHE_HOME");
    auto user = saved("HOME");
    ::setenv("DPM_HOME", "/x/dpm-home", 1);
    CHECK_EQ(dpm::cache::default_root().string(), std::string{"/x/dpm-home"});
    ::unsetenv("DPM_HOME");
    ::setenv("XDG_CACHE_HOME", "/x/xdg", 1);
    CHECK_EQ(dpm::cache::default_root().string(), std::string{"/x/xdg/dpm"});
    ::unsetenv("XDG_CACHE_HOME");
    ::setenv("HOME", "/x/user", 1);
    CHECK_EQ(dpm::cache::default_root().string(), std::string{"/x/user/.cache/dpm"});
    auto restore = [](char const* name, std::optional<std::string> const& v)
    {
        if (v)
            ::setenv(name, v->c_str(), 1);
        else
            ::unsetenv(name);
    };
    restore("DPM_HOME", home);
    restore("XDG_CACHE_HOME", xdg);
    restore("HOME", user);
}

TEST_CASE("layout paths")
{
    dpm::cache::Cache cache{"/c"};
    CHECK_EQ(cache.src_path("sha256:abc").string(), std::string{"/c/src/abc"});
    CHECK_EQ(cache.src_path("abc").string(), std::string{"/c/src/abc"});
    CHECK_EQ(cache.registry_dir().string(), std::string{"/c/registry"});
    CHECK_EQ(cache.git_dir().string(), std::string{"/c/git"});
    CHECK_EQ(cache.download_dir().string(), std::string{"/c/dl"});
    CHECK_EQ(cache.tmp_dir().string(), std::string{"/c/tmp"});
    CHECK_EQ(cache.lock_path().string(), std::string{"/c/lock"});
}

TEST_CASE("install moves a verified staging tree to its hash and makes it read-only")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    auto staging = stage(cache);
    auto expected = dpm::hash::tree_hash(staging).hash;
    auto r = cache.install(staging, expected);
    REQUIRE(r.ok);
    CHECK(!r.existed);
    CHECK_EQ(r.hash, expected);
    CHECK_EQ(r.path, cache.src_path(expected));
    CHECK(std::filesystem::exists(r.path / "project.dcon"));
    CHECK(!std::filesystem::exists(staging));
    CHECK(!writable(r.path));
    CHECK(!writable(r.path / "src"));
    CHECK(!writable(r.path / "src/a.dc"));
    CHECK_EQ(dpm::hash::tree_hash(r.path).hash, expected);
}

TEST_CASE("install without an expected hash records the computed one")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    auto r = cache.install(stage(cache));
    REQUIRE(r.ok);
    CHECK(dpm::hash::is_hash_string(r.hash));
}

TEST_CASE("a hash mismatch is fatal and leaves nothing behind")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    auto staging = stage(cache);
    auto wrong = "sha256:" + std::string(64, '0');
    auto r = cache.install(staging, wrong);
    CHECK(!r.ok);
    CHECK(r.error.find("hash mismatch") != std::string::npos);
    CHECK(!std::filesystem::exists(staging));
    CHECK(std::filesystem::is_empty(cache.src_dir()));
}

TEST_CASE("unsafe contents are refused")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    auto staging = stage(cache);
    std::filesystem::create_symlink("/etc/passwd", staging / "link");
    auto r = cache.install(staging);
    CHECK(!r.ok);
    CHECK(r.error.find("unsafe") != std::string::npos);
    CHECK(!r.problems.empty());
    CHECK(!std::filesystem::exists(staging));
    CHECK(std::filesystem::is_empty(cache.src_dir()));
}

TEST_CASE("identical content installs once and shares the directory")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    auto first = cache.install(stage(cache));
    auto second_staging = stage(cache);
    auto second = cache.install(second_staging);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    CHECK(second.existed);
    CHECK_EQ(first.path, second.path);
    CHECK(!std::filesystem::exists(second_staging));
    CHECK_EQ(std::distance(std::filesystem::directory_iterator{cache.src_dir()}, std::filesystem::directory_iterator{}), std::ptrdiff_t{1});
}

TEST_CASE("verify accepts intact trees and reports every kind of damage")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    auto good = cache.install(stage(cache, "one"));
    auto tampered = cache.install(stage(cache, "two"));
    auto added = cache.install(stage(cache, "three"));
    auto removed = cache.install(stage(cache, "four"));
    REQUIRE(good.ok);
    REQUIRE(tampered.ok);
    REQUIRE(added.ok);
    REQUIRE(removed.ok);

    auto all = cache.verify();
    REQUIRE(all.size() == std::size_t{4});
    for (auto const& e : all)
        CHECK(e.ok);

    std::filesystem::permissions(tampered.path / "project.dcon", std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    write(tampered.path / "project.dcon", "evil");
    std::filesystem::permissions(added.path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    write(added.path / "extra.dc", "module extra;");
    std::filesystem::permissions(removed.path / "src", std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    std::filesystem::remove(removed.path / "src/a.dc");
    std::filesystem::create_directories(cache.src_dir() / "not-a-digest");

    auto results = cache.verify();
    REQUIRE(results.size() == std::size_t{5});
    std::map<std::string, dpm::cache::VerifyEntry> by_name;
    for (auto const& e : results)
        by_name[e.name] = e;
    CHECK(by_name[good.hash.substr(7)].ok);
    CHECK(!by_name[tampered.hash.substr(7)].ok);
    CHECK(!by_name[added.hash.substr(7)].ok);
    CHECK(!by_name[removed.hash.substr(7)].ok);
    CHECK(!by_name["not-a-digest"].ok);
    CHECK(by_name[tampered.hash.substr(7)].message.find("hash to") != std::string::npos);
}

TEST_CASE("clean keeps installed trees unless asked for everything")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    auto r = cache.install(stage(cache));
    REQUIRE(r.ok);
    write(cache.download_dir() / "abc/file.tar.gz", "x");
    write(cache.git_dir() / "h/o/r.git/HEAD", "x");
    write(cache.registry_dir() / "id/aliases/a.dcon", "x");
    write(cache.tmp_dir() / "partial/f", "x");

    auto soft = cache.clean(false);
    CHECK_EQ(soft.removed.size(), std::size_t{3});
    CHECK(!std::filesystem::exists(cache.download_dir()));
    CHECK(!std::filesystem::exists(cache.git_dir()));
    CHECK(!std::filesystem::exists(cache.tmp_dir()));
    CHECK(std::filesystem::exists(r.path));
    CHECK(std::filesystem::exists(cache.registry_dir()));

    auto hard = cache.clean(true);
    CHECK(!std::filesystem::exists(cache.src_dir()));
    CHECK(!std::filesystem::exists(cache.registry_dir()));
    CHECK(hard.removed.size() == 2);
}

TEST_CASE("the lock can be taken and retaken")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    {
        auto lock = cache.lock();
        REQUIRE(lock.has_value());
    }
    auto again = cache.lock();
    CHECK(again.has_value());
    CHECK(std::filesystem::exists(cache.lock_path()));
}

TEST_CASE("staging directories are unique and inside tmp")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "home"};
    std::string error;
    auto a = cache.new_staging(error);
    auto b = cache.new_staging(error);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK_NE(*a, *b);
    CHECK_EQ(a->parent_path(), cache.tmp_dir());
}
