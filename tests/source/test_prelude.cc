import std;
import dcc.session;
import dcc.target;

#include "harness.hh"
#include <unistd.h>

namespace
{
    struct Project
    {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     std::format("dcc-prelude-{}-{}", getpid(), std::chrono::steady_clock::now().time_since_epoch().count());
        Project()
        {
            write("std/prelude.dc", "module std::prelude; public enum R(T,E) { Ok(T), Err(E) } using public Result = R;");
            write("libdcext.manifest", "dcc-libdcext-manifest 1\nsource all std::prelude std/prelude.dc\nsource all implementation implementation.dc\nsource hosted std::prelude::hosted std/prelude/hosted.dc\n");
            write("std/prelude/hosted.dc", "module std::prelude::hosted;");
            write("implementation.dc", "module implementation; public struct Result { i32 code; }");
        }
        ~Project() { std::filesystem::remove_all(root); }
        void write(std::string_view path, std::string_view text)
        {
            auto file = root / path;
            std::filesystem::create_directories(file.parent_path());
            std::ofstream out{file};
            out << text;
        }
        bool analyze(std::string_view entry, bool enabled = true, bool resolve_only = false)
        {
            dcc::session::CompilerSession session{{.silent_diagnostics = true}};
            dcc::session::CompileOptions options;
            options.import_roots = {root};
            options.inject_libdcext_prelude = enabled;
            if (resolve_only)
            {
                std::array paths{root / entry};
                return !session.analyze_resolve_only_files(paths, options).has_errors;
            }
            return !session.analyze_entry(root / entry, options).has_errors;
        }
    };
}

TEST_CASE("prelude applies equally to imported and separately analyzed user modules")
{
    Project p;
    p.write("pkg/helper.dc", "module pkg::helper; public Result(i32,u8) work() { return Result::Ok(42); } public T identity(T)(T x) { Result(T,u8) r=Result::Ok(x); return x; }");
    p.write("main.dc", "module main; import pkg::helper; public i32 main() { pkg::helper::work(); return pkg::helper::identity(42)-42; }");
    CHECK(p.analyze("main.dc"));
    CHECK(p.analyze("pkg/helper.dc"));
    CHECK(p.analyze("main.dc", true, true));
    CHECK(!p.analyze("main.dc", false));
    p.write("main.dc", "module main @no_prelude; import pkg::helper; public i32 main(){pkg::helper::work();return pkg::helper::identity(42)-42;}");
    CHECK(p.analyze("main.dc"));
}

TEST_CASE("manifest provenance excludes library sources without namespace heuristics")
{
    Project p;
    p.write("pkg/std/prelude_like.dc", "module pkg::std::prelude_like; public Result(i32,u8) work() { return Result::Ok(7); }");
    p.write("main.dc", "module main; import implementation; import pkg::std::prelude_like; public i32 main() { pkg::std::prelude_like::work(); return 0; }");
    CHECK(p.analyze("main.dc"));
    CHECK(p.analyze("implementation.dc"));
}

TEST_CASE("module prelude opt-out remains stable when imported or analyzed separately")
{
    Project p;
    p.write("helper.dc", "module helper @no_prelude; public struct Result { i32 code; } public i32 work() { Result r={code=7}; return r.code; }");
    p.write("main.dc", "module main; import helper; public i32 main() { return helper::work()-7; }");
    CHECK(p.analyze("main.dc"));
    CHECK(p.analyze("helper.dc", false));
    p.write("helper.dc", "module helper @no_prelude; public Result(i32,u8) work() { return Result::Ok(7); }");
    CHECK(!p.analyze("main.dc"));
    CHECK(!p.analyze("helper.dc"));
}

TEST_CASE("opted-out generic bodies cannot borrow missing prelude types from callers")
{
    Project p;
    p.write("helper.dc", "module helper @no_prelude; public T identity(T)(T x) { Result(T,u8) r=Result::Ok(x); return x; }");
    p.write("main.dc", "module main; import helper; public i32 main() { return helper::identity(42)-42; }");
    CHECK(!p.analyze("main.dc"));
}

TEST_CASE("automatic exports include only direct prelude bindings")
{
    Project p;
    p.write("extra.dc", "module extra; public enum Position { start } using public start=Position::start;");
    p.write("std/prelude.dc", "module std::prelude; public import extra; public enum R(T,E) { Ok(T), Err(E) } using public Result=R;");
    p.write("main.dc", "module main; public i32 main() { return start as i32; }");
    CHECK(!p.analyze("main.dc"));
    p.write("main.dc", "module main; Result(i32,u8) work() { return Result::Ok(42); }");
    CHECK(p.analyze("main.dc"));
}

TEST_CASE("explicit functions and imports override automatic value defaults")
{
    Project p;
    p.write("std/prelude.dc", "module std::prelude; public i32 default_print(i32 value) { return value; } using public println=default_print;");
    p.write("main.dc", "module main; i32 println(i32 value) {return value+1;} public i32 main(){return println(41)-42;}");
    CHECK(p.analyze("main.dc"));
    p.write("other.dc", "module other; public i32 println(i32 value){return value+1;}");
    p.write("main.dc", "module main; import other; using other::{println}; public i32 main(){return println(41)-42;}");
    CHECK(p.analyze("main.dc"));
}

TEST_CASE("generic UFCS retains direct instantiation declarations without caller type lookup")
{
    Project p;
    p.write("helper.dc", "module helper @no_prelude; public i32 invoke(T)(T* value) { return value.read(); }");
    p.write("main.dc", "module main; import helper; struct Local { i32 value; } i32 read(Local* self){return self.value;} public i32 main(){Local x={value=42};return helper::invoke(&x)-42;}");
    CHECK(p.analyze("main.dc"));
}
