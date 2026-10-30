import std;
import dpm.semver;

#include "harness.hh"

namespace
{
    using dpm::semver::compare;
    using dpm::semver::parse;

    [[nodiscard]] dpm::semver::Version v(std::string_view text)
    {
        auto parsed = parse(text);
        return parsed.value_or(dpm::semver::Version{});
    }

    [[nodiscard]] bool less(std::string_view a, std::string_view b)
    {
        return compare(v(a), v(b)) == std::strong_ordering::less;
    }
} // namespace

SECTION("dpm semver");

TEST_CASE("valid versions parse into their parts")
{
    auto a = parse("1.2.3");
    REQUIRE(a.has_value());
    CHECK(a->major == 1);
    CHECK(a->minor == 2);
    CHECK(a->patch == 3);
    CHECK(!a->is_prerelease());
    auto b = parse("1.0.0-rc.1+build.5");
    REQUIRE(b.has_value());
    CHECK(b->pre.size() == 2);
    CHECK(b->pre[0] == "rc");
    CHECK(b->build == "build.5");
    CHECK(dpm::semver::to_string(*b) == "1.0.0-rc.1+build.5");
}

TEST_CASE("malformed versions are rejected")
{
    for (std::string_view bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "v1.2.3", "1.2.3-", "1.2.3+", "1.2.3-01", "1.2.3-a..b", "1.2.3-a_b",
                                 "a.b.c", "1.2.-3", "99999999999999999999.0.0", " 1.2.3"})
        CHECK(!parse(bad).has_value());
}

TEST_CASE("precedence follows the semver rules")
{
    CHECK(less("1.0.0", "2.0.0"));
    CHECK(less("2.0.0", "2.1.0"));
    CHECK(less("2.1.0", "2.1.1"));
    CHECK(less("1.0.0-alpha", "1.0.0"));
    CHECK(less("1.0.0-alpha", "1.0.0-alpha.1"));
    CHECK(less("1.0.0-alpha.1", "1.0.0-alpha.beta"));
    CHECK(less("1.0.0-alpha.beta", "1.0.0-beta"));
    CHECK(less("1.0.0-beta", "1.0.0-beta.2"));
    CHECK(less("1.0.0-beta.2", "1.0.0-beta.11"));
    CHECK(less("1.0.0-beta.11", "1.0.0-rc.1"));
    CHECK(less("1.0.0-rc.1", "1.0.0"));
    CHECK(less("1.9.0", "1.10.0"));
}

TEST_CASE("build metadata is ignored in comparison")
{
    CHECK(compare(v("1.0.0+a"), v("1.0.0+b")) == std::strong_ordering::equal);
    CHECK(compare(v("1.0.0+a"), v("1.0.0")) == std::strong_ordering::equal);
}

TEST_CASE("series follow the major version, or the minor below 1.0")
{
    CHECK(dpm::semver::same_series(v("1.2.0"), v("1.9.3")));
    CHECK(!dpm::semver::same_series(v("1.2.0"), v("2.0.0")));
    CHECK(dpm::semver::same_series(v("0.3.1"), v("0.3.9")));
    CHECK(!dpm::semver::same_series(v("0.3.1"), v("0.4.0")));
    CHECK(dpm::semver::series_of(v("1.2.0")).to_string() == "1.x");
    CHECK(dpm::semver::series_of(v("0.3.1")).to_string() == "0.3.x");
}

TEST_CASE("a candidate satisfies a minimum in the same series only")
{
    CHECK(dpm::semver::satisfies_minimum(v("1.4.0"), v("1.2.0")));
    CHECK(dpm::semver::satisfies_minimum(v("1.2.0"), v("1.2.0")));
    CHECK(!dpm::semver::satisfies_minimum(v("1.1.9"), v("1.2.0")));
    CHECK(!dpm::semver::satisfies_minimum(v("2.0.0"), v("1.2.0")));
    CHECK(!dpm::semver::satisfies_minimum(v("0.4.0"), v("0.3.0")));
}

TEST_CASE("pre-releases are chosen only when the minimum is one")
{
    CHECK(!dpm::semver::satisfies_minimum(v("1.3.0-rc.1"), v("1.2.0")));
    CHECK(dpm::semver::satisfies_minimum(v("1.3.0-rc.1"), v("1.3.0-beta.1")));
    CHECK(dpm::semver::satisfies_minimum(v("1.3.0"), v("1.3.0-beta.1")));
    CHECK(!dpm::semver::satisfies_minimum(v("1.3.0-alpha"), v("1.3.0-beta")));
}

TEST_CASE("tags accept an optional v and a subdirectory prefix")
{
    CHECK(dpm::semver::parse_tag("v1.2.3").has_value());
    CHECK(dpm::semver::parse_tag("1.2.3").has_value());
    CHECK(!dpm::semver::parse_tag("vv1.2.3").has_value());
    CHECK(!dpm::semver::parse_tag("release-1").has_value());
    CHECK(dpm::semver::parse_tag("sub/v1.2.3", "sub").has_value());
    CHECK(dpm::semver::parse_tag("sub/1.2.3", "sub").has_value());
    CHECK(!dpm::semver::parse_tag("other/v1.2.3", "sub").has_value());
    CHECK(!dpm::semver::parse_tag("v1.2.3", "sub").has_value());
}
