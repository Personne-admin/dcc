import std;
import dcc.session;
import dcc.target;

#include "harness.hh"
#include "stdlib_os.hh"

namespace
{
    struct Analysis
    {
        bool failed;
        std::string diagnostics;
        bool hosted_dependencies{};
    };

    Analysis analyze(std::string_view source, bool prelude = true, dcc::target::Os os = dcc::target::Os::Linux,
                     std::string_view helper = {})
    {
        auto root = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path().parent_path();
        auto dir = std::filesystem::temp_directory_path() /
                   std::format("dcc-standard-error-{}-{}", getpid(), std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(dir);
        struct Cleanup
        {
            std::filesystem::path path;
            ~Cleanup() { std::filesystem::remove_all(path); }
        } cleanup{dir};
        {
            std::ofstream out{dir / "main.dc"};
            out << source;
        }
        if (!helper.empty())
        {
            std::ofstream out{dir / "helper.dc"};
            out << helper;
        }
        std::ostringstream diagnostics;
        dcc::session::CompilerSession session{{.diagnostic_stream = &diagnostics}};
        session.diagnostics().set_color(false);
        dcc::session::CompileOptions options;
        options.import_roots = {root / "include"};
        options.inject_libdcext_prelude = prelude;
        options.target = dcc::target::TargetConfig::for_os(os);
        auto result = session.analyze_entry(dir / "main.dc", options);
        bool hosted_dependencies = false;
        if (auto* context = session.sema_context())
            for (auto const& module : context->graph().all())
                for (auto forbidden : {"std::io", "std::prelude::hosted", "std::os::heap", "std::os::file", "std::os::time",
                                       "std::sys::linux::syscall", "std::sys::win::imports"})
                    hosted_dependencies |= module->canonical_path.str() == forbidden;
        return {result.has_errors, diagnostics.str(), hosted_dependencies};
    }
}

TEST_CASE("standard errors preserve categories and payloads through propagation")
{
    auto source = os_test::fixture("standard-error.dc");
    REQUIRE(!source.empty());
    for (auto backend : {"custom", "llvm"})
    {
#if !DCC_ENABLE_LLVM
        if (backend == std::string_view{"llvm"})
            continue;
#endif
        for (auto optimization : {"-O0", "-O2"})
        {
            auto result = os_test::run(source, false, std::format("-fwhole-program -fbackend {} {}", backend, optimization));
            CHECK_EQ(result.status, 0);
            CHECK(result.out.empty());
            CHECK(result.err.empty());
        }
    }
}

TEST_CASE("standard error prelude works in generic functions and explicit helper imports")
{
    auto result = analyze(R"dc(module main;
import helper;
Status(Error) generic(E)(E value) { helper::failed(value)?; return Status::Ok; }
Status(Error) work() { helper::work()?; generic(std::alloc::AllocError::OutOfMemory)?; return Status::Ok; }
import std::alloc;
)dc", true, dcc::target::Os::Linux, R"dc(module helper;
public import std::prelude;
import std::alloc;
public Status(E) failed(E)(E value) { return Status::Err(value); }
public Status(Error) work() { failed(std::alloc::AllocError::OutOfMemory)?; return Status::Ok; }
)dc");
    CHECK(!result.failed);
}

TEST_CASE("standard error has the same variants on hosted and freestanding targets")
{
    for (auto os : {dcc::target::Os::Linux, dcc::target::Os::Windows, dcc::target::Os::Freestanding})
    {
        auto result = analyze(R"dc(module main;
import std::error;
import std::os::print;
import std::os::error;
Status(E) failed(E)(E value) { return Status::Err(value); }
Status(Error) work() { failed(std::os::print::PrintError::TooLong)?; return Status::Ok; }
std::error::Error io() { return std::os::error::Error::AccessDenied; }
)dc", true, os);
        CHECK(!result.failed);
    }
}

TEST_CASE("standard error explicit imports and prelude opt-out remain independent")
{
    CHECK(analyze("module main; Status(Error) f() { return Status::Ok; }", false).failed);
    CHECK(!analyze("module main; import std::prelude; Status(Error) f() { return Status::Ok; }", false).failed);
    CHECK(!analyze("module main; import std::error; std::error::Error f() { return std::error::Error::Allocation(std::alloc::AllocError::OutOfMemory); } import std::alloc;", false).failed);
    CHECK(!analyze("module main; import std::os::error; using std::os::error::Error; Error f() { return Error::NotFound; }").failed);
    CHECK(!analyze("module main; struct Error { i32 value; } i32 f() { Error e = {value = 7}; return e.value; }").failed);
    auto run = os_test::run("module main; import std::error; public i32 main() { std::error::Error e = std::error::Error::Io(std::os::error::Error::NotFound); return 0; } import std::os::error;", false, "-fno-prelude -fwhole-program");
    CHECK_EQ(run.status, 0);
}

TEST_CASE("standard error rejects arbitrary user values and unsupported writer errors in sema")
{
    auto user = analyze(R"dc(module main;
enum UserError : u8 { X }
Status(UserError) failed() { return UserError::X; }
Status(Error) work() { failed()?; return Status::Ok; }
)dc");
    CHECK(user.failed);
    CHECK(user.diagnostics.find("error type `UserError` cannot be propagated") != std::string::npos);
    CHECK(analyze("module main; Error f() { return 7; }").failed);
    auto writer = analyze(R"dc(module main;
import std::fmt;
enum UserError : u8 { X }
Status(std::fmt::FormatError(UserError)) failed() { return Status::Err(std::fmt::FormatError::Write(UserError::X)); }
Status(Error) work() { failed()?; return Status::Ok; }
)dc");
    CHECK(writer.failed);
    CHECK(writer.diagnostics.find("cannot be propagated") != std::string::npos);
}

TEST_CASE("unified errors retain ambiguity and nesting diagnostics")
{
    auto ambiguous = analyze(R"dc(module main;
import std::alloc;
enum Ambiguous { @implicit_construction First(Error), @implicit_construction Second(Error) }
Status(Error) failed() { return Status::Err(Error::Allocation(std::alloc::AllocError::OutOfMemory)); }
Status(Ambiguous) work() { failed()?; return Status::Ok; }
)dc");
    CHECK(ambiguous.failed);
    CHECK(ambiguous.diagnostics.find("matches more than one implicit construction") != std::string::npos);
    CHECK(ambiguous.diagnostics.find("Ambiguous::First, Ambiguous::Second") != std::string::npos);
    auto nested = analyze(R"dc(module main;
import std::alloc;
enum Application { @implicit_construction Library(Error) }
Status(std::alloc::AllocError) failed() { return std::alloc::AllocError::OutOfMemory; }
Status(Application) work() { failed()?; return Status::Ok; }
)dc");
    CHECK(nested.failed);
    CHECK(nested.diagnostics.find("cannot be propagated") != std::string::npos);
}

TEST_CASE("shared error prelude does not load hosted IO implementations")
{
    auto result = analyze("module main; Status(Error) work() { return Status::Ok; }", true, dcc::target::Os::Freestanding);
    CHECK(!result.failed);
    CHECK(!result.hosted_dependencies);
}
