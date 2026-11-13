import std;

#include "harness.hh"
#include "stdlib_os.hh"

TEST_CASE("automatic hosted names work without imports")
{
    auto source = os_test::fixture("beginner.dc");
    REQUIRE(!source.empty());
    for (auto backend : {"llvm", "custom"})
    {
#if !DCC_ENABLE_LLVM
        if (backend == std::string_view{"llvm"}) continue;
#endif
        for (auto opt : {"-O0", "-O2"})
            for (auto mode : {"", "-fwhole-program"})
            {
                auto result = os_test::run(source, false, std::format("-fbackend {} {} {}", backend, opt, mode));
                CHECK_EQ(result.status, 0);
                CHECK_EQ(result.out, "Hello, world!\nValue: 42\nValue: 100\n");
                CHECK(result.err.empty());
            }
    }
}

TEST_CASE("ordinary package functions and generics own their prelude bindings")
{
    auto helper = R"dc(module helper;
public Result(i32,Error) answer() {
    Array(i32) values={};
    defer values.deinit();
    values.push(42)?;
    println("helper {}",values.items()[0])?;
    return Result::Ok(values.items()[0]);
}
public Status(Error) generic(T)(T value) {
    Array(T) values={};
    defer values.deinit();
    values.push(value)?;
    println("generic {}",values.items()[0])?;
    return Status::Ok;
}
)dc";
    auto entry = R"dc(module main;
import helper;
Status(Error) work() {
    i32 value=helper::answer()?;
    helper::generic(value)?;
    return Status::Ok;
}
public i32 main() { return if work().is_ok() {0}else{1}; }
)dc";
    for (auto backend : {"llvm", "custom"})
    {
#if !DCC_ENABLE_LLVM
        if (backend == std::string_view{"llvm"}) continue;
#endif
        for (auto opt : {"-O0", "-O2"})
            for (bool whole : {false, true})
            {
                auto result = os_test::run_modules({{"helper.dc",helper,"",opt},{"main.dc",entry,"",opt}},false,backend,whole);
                CHECK_EQ(result.status,0);
                CHECK_EQ(result.out,"helper 42\ngeneric 42\n");
                CHECK(result.err.empty());
            }
    }
}

TEST_CASE("explicit legacy printing imports override hosted defaults")
{
    auto source = R"dc(module main;
import std::os::print;
using std::os::print::{println,PrintError};
Status(PrintError) work() { println("legacy")?; return Status::Ok; }
public i32 main() { return if work().is_ok() {0}else{1}; }
)dc";
    auto result=os_test::run(source,false,"-fwhole-program");
    CHECK_EQ(result.status,0);
    CHECK(result.out.empty());
    CHECK_EQ(result.err,"legacy\n");
}

TEST_CASE("local function names shadow hosted defaults")
{
    auto source=R"dc(module main;
i32 println(i32 value) { return value; }
public i32 main() {
    i32 print=7;
    return println(print)-7;
}
)dc";
    auto result=os_test::run(source,false,"-fwhole-program");
    CHECK_EQ(result.status,0);
    CHECK(result.out.empty());
    CHECK(result.err.empty());
}

TEST_CASE("hosted PrintError is an automatic type alias")
{
    auto result=os_test::run(R"dc(module main;
Status(PrintError) greeting(){return println("ok");}
Status(Error) work(){greeting()?;return Status::Ok;}
public i32 main(){return if work().is_ok(){0}else{1};}
)dc",false,"-fwhole-program");
    CHECK_EQ(result.status,0);
    CHECK_EQ(result.out,"ok\n");
    CHECK(result.err.empty());
}
