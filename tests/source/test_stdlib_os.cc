import std;

#include "harness.hh"
#include "stdlib_os.hh"

SECTION("libdcext OS pure logic");

TEST_CASE("error mappings, lexical paths, encoding boundaries and ABI arithmetic")
{
    CHECK_EQ(os_test::run(os_test::fixture("logic.dc"), false).status, 0);
}

TEST_CASE("nested field addresses agree across functions and backends")
{
    auto source = os_test::fixture("nested-field-address.dc");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}
TEST_CASE("repeated imported nested records retain their ABI layout")
{
    auto source = os_test::fixture("nested-filetime.dc");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}
