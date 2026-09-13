import std;
import dcc.config;

#include "harness.hh"

namespace
{
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            auto base = std::filesystem::temp_directory_path();
            auto tag = std::format("dcc-config-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
            path = base / tag;
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    [[nodiscard]] std::filesystem::path canonicalize(std::filesystem::path const& p)
    {
        std::error_code ec;
        auto out = std::filesystem::weakly_canonical(p, ec);
        if (ec)
            return p.lexically_normal();

        return out;
    }

} // namespace

SECTION("config: prefix resolution");

TEST_CASE("exe-relative branch wins when its include directory exists")
{
    TempDir td;
    std::filesystem::create_directories(td.path / "include");
    auto exe = td.path / "bin" / "tool";
    auto baked = td.path / "does-not-exist-baked";

    auto resolved = dcc::config::resolve_prefix(exe, baked);

    REQUIRE(resolved.source == dcc::config::PrefixSource::ExeRelative);
    CHECK_EQ(resolved.path.string(), canonicalize(td.path).string());
}

TEST_CASE("missing include directory falls back to the baked prefix")
{
    TempDir td;
    auto baked = td.path / "custom-prefix";
    std::filesystem::create_directories(baked);
    auto exe = td.path / "bin" / "tool";

    auto resolved = dcc::config::resolve_prefix(exe, baked);

    REQUIRE(resolved.source == dcc::config::PrefixSource::Baked);
    CHECK_EQ(resolved.path.string(), canonicalize(baked).string());
}

TEST_CASE("missing baked prefix still resolves absolutely")
{
    TempDir td;
    auto baked = td.path / "missing-prefix";
    auto exe = td.path / "bin" / "tool";

    auto resolved = dcc::config::resolve_prefix(exe, baked);

    REQUIRE(resolved.source == dcc::config::PrefixSource::Baked);
    CHECK(resolved.path.is_absolute());
    CHECK_EQ(resolved.path.string(), canonicalize(baked).string());
}

TEST_CASE("empty exe path falls back to the baked prefix")
{
    TempDir td;
    auto baked = td.path / "custom-prefix";
    std::filesystem::create_directories(baked);

    auto resolved = dcc::config::resolve_prefix(std::filesystem::path{}, baked);

    REQUIRE(resolved.source == dcc::config::PrefixSource::Baked);
    CHECK_EQ(resolved.path.string(), canonicalize(baked).string());
}

TEST_CASE("relative baked prefix resolves absolutely")
{
    auto resolved = dcc::config::resolve_prefix(std::filesystem::path{}, std::filesystem::path{"rel-prefix-stub"});

    REQUIRE(resolved.source == dcc::config::PrefixSource::Baked);
    CHECK(resolved.path.is_absolute());
}

TEST_CASE("baked prefix is non-empty and source names are stable")
{
    CHECK(!dcc::config::baked_prefix().empty());
    CHECK_EQ(dcc::config::to_string(dcc::config::PrefixSource::ExeRelative), "exe-relative");
    CHECK_EQ(dcc::config::to_string(dcc::config::PrefixSource::Baked), "baked");
}
