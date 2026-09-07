import std;

#include "harness.hh"
#include "stdlib_os.hh"

SECTION("libdcext OS pure logic");

TEST_CASE("error mappings, lexical paths, encoding boundaries and ABI arithmetic")
{
    CHECK_EQ(os_test::run(os_test::fixture("logic.dc"), false).status, 0);
}
