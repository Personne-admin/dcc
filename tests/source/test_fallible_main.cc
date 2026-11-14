import std;

#include "harness.hh"
#include "stdlib_os.hh"

TEST_CASE("hosted integer and fallible mains keep their process exit contracts")
{
    struct Program { std::string source; int exit; std::string output; };
    std::vector<Program> programs{
        {"module main; public i32 main(){return 0;}",0,""},
        {"module main; public i32 main(){return 17;}",17,""},
        {"module main; Status(Error) main(){println(\"Hello, world!\")?;return Status::Ok;}",0,"Hello, world!\n"},
        {"module main; Status(Error) main(){return Status::Err(Error::Allocation(AllocError::OutOfMemory));}",1,""},
        {"module main; enum Custom{Bad} Status(Custom) main(){return Status::Ok;}",0,""},
        {"module main; enum Custom{Bad} Status(Custom) main(){return Custom::Bad;}",1,""},
        {"module main; Status(AllocError) helper(){return AllocError::OutOfMemory;} Status(Error) main(){helper()?;return Status::Ok;}",1,""},
        {"module main; Status(Error) main(){Array(i32) a={};defer a.deinit();a.push(42)?;return Status::Ok;}",0,""},
        {"module main; Status(Error) main(){Array(u64) a={};defer a.deinit();a.reserve(~(0 as usize))?;return Status::Ok;}",1,""},
        {"module main; import std::sys::linux::syscall; Status(Error) main(){std::sys::linux::syscall::close(1);println(\"closed\")?;return Status::Ok;}",1,""},
    };
    for(auto backend:{"llvm","custom"})
    {
#if !DCC_ENABLE_LLVM
        if(backend==std::string_view{"llvm"}) continue;
#endif
        for(auto opt:{"-O0","-O2"})
            for(auto mode:{"","-fwhole-program"})
                for(auto const& program:programs)
                {
                    auto result=os_test::run(program.source,false,std::format("-fbackend {} {} {}",backend,opt,mode));
                    CHECK_EQ(result.status,program.exit);
                    CHECK_EQ(result.out,program.output);
                    CHECK(result.err.empty());
                }
    }
}

TEST_CASE("fallible main remains an ordinary addressable aggregate returning function")
{
    auto source=R"dc(module main;
public struct Failure{u64[64] bytes;i32 code;}
i32 calls;
public Status(Failure) main(){
    calls+=1;
    if calls==1 {
        Status(Failure)(*)() original=main;
        Status(Failure) result=original();
        match result {
            Status::Err(error)=>{
                if error.code==42 && error.bytes[63]==123456789 {return Status::Ok;}
            },
            _=>{},
        }
    }
    Failure error={};
    error.bytes[63]=123456789;
    error.code=42;
    return Status::Err(error);
}
)dc";
    for(auto backend:{"llvm","custom"})
    {
#if !DCC_ENABLE_LLVM
        if(backend==std::string_view{"llvm"}) continue;
#endif
        for(auto opt:{"-O0","-O2"})
            for(auto mode:{"","-fwhole-program"})
                CHECK_EQ(os_test::run(source,false,std::format("-fbackend {} {} {}",backend,opt,mode)).status,0);
    }
}

TEST_CASE("fallible main works with explicit imports and prelude disabled")
{
    auto source=R"dc(module main;
public import std::result;
public import std::error;
public std::result::Status(std::error::Error) main(){return std::result::Status::Ok;}
)dc";
    CHECK_EQ(os_test::run(source,false,"-fno-prelude").status,0);
    CHECK_EQ(os_test::run(source,false,"-fno-prelude -fwhole-program -fbackend custom").status,0);
}
