import std;

#include "harness.hh"
#include "stdlib_os.hh"

SECTION("libdcext OS pure logic");

TEST_CASE("error mappings, lexical paths, encoding boundaries and ABI arithmetic")
{
    CHECK_EQ(os_test::run(os_test::fixture("logic.dc"), false).status, 0);
}

TEST_CASE("os::time civil calendar round-trips, anchors and validators")
{
    auto source = os_test::fixture("time-civil.dc");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}

TEST_CASE("os::path normalize, join, relative, roots and stems")
{
    auto source = os_test::fixture("path-lexical.dc");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}

TEST_CASE("os::path to_wide and from_wide transcoding")
{
    auto source = os_test::fixture("path-wide.dc");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}

TEST_CASE("os::path consecutive dot-dot pops (llvm-only; em64t miscompiles, see docs/known-issues.md)")
{
#if DCC_ENABLE_LLVM
    auto source = os_test::fixture("path-double-dot.dc");
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
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
TEST_CASE("conditional returns leave loop fall-through reachable")
{
    auto source = os_test::fixture("conditional-return-loop.dc");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}
TEST_CASE("breaks escape loops while retry loops retain returns")
{
    auto source = os_test::fixture("loop-break-return.dc");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}
TEST_CASE("index lvalue through pointer dereference")
{
    static constexpr std::string_view source = (R"DCC(module main;
void f(i32[4]* buf, usize* n, i32 v) {
    (*buf)[*n] = v;
}
public i32 main() {
    i32[4] arr;
    usize n = 2;
    f(&arr, &n, 42);
    if arr[2] != 42 { return 1; }
    return 0;
}
)DCC");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}
TEST_CASE("mutable auto-ref receiver on temporary rvalue")
{
    static constexpr std::string_view source = (R"DCC(module main;
struct S {
    i32 val;
}
S make_s(i32 v) {
    S s;
    s.val = v;
    return s;
}
S* add(S* self, i32 delta) {
    self.val = self.val + delta;
    return self;
}
i32 get(S* self) {
    return self.val;
}
public i32 main() {
    if make_s(10).add(5).get() != 15 { return 1; }
    return 0;
}
)DCC");
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend em64t -O2").status, 0);
#if DCC_ENABLE_LLVM
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O0").status, 0);
    CHECK_EQ(os_test::run(source, false, "-fbackend llvm -O2").status, 0);
#endif
}

