import std;
#include "harness.hh"
#include "stdlib_os.hh"

TEST_CASE("hosted heap and default arrays preserve allocation ownership")
{
    auto source = os_test::fixture("heap-policy.dc");
    REQUIRE(!source.empty());
    for (auto backend : {"llvm", "custom"})
    {
#if !DCC_ENABLE_LLVM
        if (backend == std::string_view{"llvm"}) continue;
#endif
        for (auto opt : {"-O0", "-O2"})
            for (auto mode : {"", "-fwhole-program"})
                CHECK_EQ(os_test::run(source, false, std::format("-fbackend {} {} {}", backend, opt, mode)).status, 0);
    }
}

TEST_CASE("prelude opt-out does not disable the hosted default allocator")
{
    auto source = R"dc(module main;
import std::array;
public i32 main() {
    std::array::Array(i32) values = {};
    defer values.deinit();
    if !values.push(42).is_ok() { return 1; }
    return values.items()[0]-42;
}
)dc";
    CHECK_EQ(os_test::run(source, false, "-fno-prelude -fwhole-program").status, 0);
}

TEST_CASE("explicit allocator selection overrides the hosted heap")
{
    auto policy = R"dc(module policy;
public import std::alloc;
public import std::result;
public struct Local {}
public std::result::Result([]u8,std::alloc::AllocError) allocate(const Local* self, usize len, usize align) { return std::alloc::AllocError::InvalidAlignment; }
public bool resize_in_place(const Local* self, []u8 block, usize len, usize align) { return false; }
public void free(const Local* self, []u8 block, usize align) {}
)dc";
    auto main = R"dc(module main;
import std::array;
import std::alloc;
public i32 main() {
    std::array::Array(i32) values={};
    return if values.push(42).unwrap_err() == std::alloc::AllocError::InvalidAlignment { 0 } else { 1 };
}
)dc";
    CHECK_EQ(os_test::run_modules({{"policy.dc", policy, "", "-fdefault-allocator=policy::Local"},
                                  {"main.dc", main, "", "-fdefault-allocator=policy::Local"}}, false, "llvm").status, 0);
}

TEST_CASE("injected declarations do not inject preludes into allocator modules")
{
    auto source=R"dc(module main;
public i32 main(){
    Array(i32) values={};
    defer values.deinit();
    if !values.push(42).is_ok(){return 1;}
    return values.items()[0]-42+GRAPH_OPTION;
}
)dc";
    for(auto backend:{"llvm","custom"})
    {
#if !DCC_ENABLE_LLVM
        if(backend==std::string_view{"llvm"}) continue;
#endif
        for(auto opt:{"-O0","-O2"})
            for(auto mode:{"","-fwhole-program"})
                CHECK_EQ(os_test::run(source,false,std::format("-fbackend {} {} {} -J 'public const i32 GRAPH_OPTION=0;'",backend,opt,mode)).status,0);
    }
}
