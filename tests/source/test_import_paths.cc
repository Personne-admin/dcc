import std;
import dcc.session;
import dcc.sm;
import dcc.diag;
import dcc.ast;
import dcc.lex;
import dcc.parser;
import dcc.si;
import dcc.sema;
import dcc.ir;
import dcc.ir.lower;
import dcc.target;

#include "harness.hh"

#include <unistd.h>

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        static inline std::atomic<int> s_counter{0};

        TempDir()
        {
            auto tmp = std::filesystem::temp_directory_path();
            auto dir = tmp / ("dcc_test_import_" + std::to_string(::getpid()) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(dir);
            path = std::move(dir);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write_file(std::filesystem::path const& relative, const std::string& content) const
        {
            auto full = path / relative;
            auto parent = full.parent_path();
            if (!parent.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
            }

            std::ofstream f(full);
            f << content;
        }

        [[nodiscard]] std::filesystem::path file(std::filesystem::path const& relative) const { return path / relative; }
    };

    void write_module(TempDir const& td, std::filesystem::path const& relative, std::string_view module_name, const std::vector<std::string>& imports = {},
                      std::string_view extra = "")
    {
        std::string content;
        content += std::format("module {};\n", module_name);
        for (auto const& imp : imports)
            content += "import " + imp + ";\n";

        content += extra;
        td.write_file(relative, content);
    }

    bool compile_entry(std::filesystem::path const& entry, std::vector<std::filesystem::path> import_roots = {}, bool expect_success = true,
                       std::string* out_errors = nullptr)
    {
        dcc::session::SessionOptions sopts;
        sopts.silent_diagnostics = true;

        std::stringstream diag_stream;
        sopts.diagnostic_stream = &diag_stream;

        dcc::session::CompilerSession session{sopts};
        dcc::session::CompileOptions opts;
        opts.import_roots = std::move(import_roots);

        auto result = session.analyze_entry(entry, opts);

        if (out_errors)
            *out_errors = diag_stream.str();

        bool ok = !result.has_errors && result.module != nullptr;
        return ok == expect_success;
    }

    [[nodiscard]] std::string test_expand_workspace_variables(std::string path, std::filesystem::path const& workspace_root)
    {
        constexpr std::string_view kWorkspaceFolder = "${workspaceFolder}";
        constexpr std::string_view kWorkspaceFolderBasename = "${workspaceFolderBasename}";

        auto replace = [](std::string& s, std::string_view var, std::string const& replacement) {
            std::size_t pos = 0;
            while ((pos = s.find(var, pos)) != std::string::npos)
            {
                s.replace(pos, var.size(), replacement);
                pos += replacement.size();
            }
        };

        if (workspace_root.empty())
            return path;

        replace(path, kWorkspaceFolder, workspace_root.string());

        auto stem = workspace_root.filename().string();
        if (!stem.empty())
            replace(path, kWorkspaceFolderBasename, stem);

        return path;
    }

} // anonymous namespace

SECTION("Import-roots: basic resolution");

TEST_CASE("module resolved via single import root")
{
    TempDir td;
    write_module(td, "lib/foo.dc", "foo");
    write_module(td, "app.dc", "app", {"foo"});

    CHECK(compile_entry(td.file("app.dc"), {td.file("lib")}));
}

TEST_CASE("module resolves through first root when multiple roots")
{
    TempDir td;
    write_module(td, "first/bar.dc", "bar");
    write_module(td, "app.dc", "app", {"bar"});

    CHECK(compile_entry(td.file("app.dc"), {td.file("first"), td.file("nonexistent")}));
}

TEST_CASE("module resolves through second root when first lacks it")
{
    TempDir td;
    write_module(td, "second/baz.dc", "baz");
    write_module(td, "app.dc", "app", {"baz"});

    CHECK(compile_entry(td.file("app.dc"), {td.file("nonexistent1"), td.file("second")}));
}

TEST_CASE("module not resolved without matching import root")
{
    TempDir td;
    write_module(td, "hidden/hid.dc", "hid");
    write_module(td, "app.dc", "app", {"hid"});

    CHECK(compile_entry(td.file("app.dc"), {}, false));
}

SECTION("Import-roots: parent-path fallback works");

TEST_CASE("module in same directory resolves without extra roots")
{
    TempDir td;
    write_module(td, "neighbor.dc", "neighbor");
    write_module(td, "app.dc", "app", {"neighbor"});

    CHECK(compile_entry(td.file("app.dc"), {}));
}

TEST_CASE("module in subdirectory resolves with that subdir as root")
{
    TempDir td;
    write_module(td, "sub/helper.dc", "helper");
    write_module(td, "main.dc", "main", {"helper"});

    CHECK(compile_entry(td.file("main.dc"), {td.file("sub")}));
}

SECTION("Import-roots: transitive imports");

TEST_CASE("transitive import resolves through configured roots")
{
    TempDir td;
    write_module(td, "dep_a.dc", "dep_a");
    write_module(td, "dep_b.dc", "dep_b", {"dep_a"});
    write_module(td, "app.dc", "app", {"dep_b"});

    CHECK(compile_entry(td.file("app.dc"), {td.path}));
}

SECTION("Import-roots: deduplication and ordering");

TEST_CASE("duplicate roots do not cause errors")
{
    TempDir td;
    write_module(td, "mod.dc", "mod");
    write_module(td, "app.dc", "app", {"mod"});

    CHECK(compile_entry(td.file("app.dc"), {td.path, td.path, td.file("nonexistent")}));
}

TEST_CASE("first root takes precedence for module resolution")
{
    TempDir td1;
    TempDir td2;
    write_module(td1, "shadow.dc", "shadow");
    write_module(td2, "shadow.dc", "wrong_name");
    write_module(td1, "app.dc", "app", {"shadow"});

    CHECK(compile_entry(td1.file("app.dc"), {td1.path, td2.path}));
    CHECK(compile_entry(td1.file("app.dc"), {td2.path, td1.path}, false));
}

SECTION("Import-roots: nonexistent paths handled gracefully");

TEST_CASE("nonexistent import roots do not crash")
{
    TempDir td;
    write_module(td, "app.dc", "app");
    CHECK(compile_entry(td.file("app.dc"), {std::filesystem::path{"/nonexistent/path/12345"}, td.path}));
}

TEST_CASE("empty import roots works (document parent is always a root)")
{
    TempDir td;
    write_module(td, "app.dc", "app");
    CHECK(compile_entry(td.file("app.dc"), {}));
}

SECTION("Reconfigure robustness: repeated recompilation with changing roots");

TEST_CASE("repeated analyze_entry with changing import roots is safe")
{
    TempDir td;
    write_module(td, "lib/foo.dc", "foo");
    write_module(td, "app.dc", "app", {"foo"});

    dcc::session::SessionOptions sopts;
    sopts.silent_diagnostics = true;
    std::stringstream diag_stream;
    sopts.diagnostic_stream = &diag_stream;

    dcc::session::CompilerSession session{sopts};

    {
        dcc::session::CompileOptions opts;
        opts.import_roots = {td.file("lib")};
        auto result = session.analyze_entry(td.file("app.dc"), opts);
        CHECK(result.module != nullptr);
    }

    {
        dcc::session::CompileOptions opts;
        opts.import_roots = {td.path};
        auto result = session.analyze_entry(td.file("app.dc"), opts);
        CHECK(result.module != nullptr);
    }

    {
        dcc::session::CompileOptions opts;
        opts.import_roots = {td.file("lib")};
        auto result = session.analyze_entry(td.file("app.dc"), opts);
        CHECK(result.module != nullptr);
    }
}

TEST_CASE("recompile with nonexistent root in changed config does not crash")
{
    TempDir td;
    write_module(td, "app.dc", "app");

    dcc::session::SessionOptions sopts;
    sopts.silent_diagnostics = true;
    std::stringstream diag_stream;
    sopts.diagnostic_stream = &diag_stream;

    dcc::session::CompilerSession session{sopts};

    {
        dcc::session::CompileOptions opts;
        opts.import_roots = {td.path};
        auto result = session.analyze_entry(td.file("app.dc"), opts);
        CHECK(result.module != nullptr);
    }

    {
        dcc::session::CompileOptions opts;
        opts.import_roots = {std::filesystem::path{"/nonexistent/path/config_change"}, td.path};
        auto result = session.analyze_entry(td.file("app.dc"), opts);
        CHECK(result.module != nullptr);
    }

    {
        dcc::session::CompileOptions opts;
        opts.import_roots = {td.path};
        auto result = session.analyze_entry(td.file("app.dc"), opts);
        CHECK(result.module != nullptr);
    }
}

SECTION("Import-roots: workspace variable expansion equivalent");

TEST_CASE("module resolved when import root matches workspace folder expansion")
{
    TempDir td;

    write_module(td, "lib/foo.dc", "foo");
    write_module(td, "app.dc", "app", {"foo"});

    auto expanded_root = td.file("lib");
    CHECK(compile_entry(td.file("app.dc"), {expanded_root}));
}

TEST_CASE("module resolved when import root matches workspace folder basename expansion")
{
    TempDir td;
    auto basename = td.path.filename().string();

    write_module(td, "sub/pkg.dc", "pkg");
    write_module(td, "app.dc", "app", {"pkg"});

    auto expanded_root = td.file("sub");
    CHECK(compile_entry(td.file("app.dc"), {expanded_root}));
}

TEST_CASE("import roots with spaces in workspace path work after expansion")
{
    TempDir td;

    auto spaced_dir = td.path / "with spaces" / "lib";
    std::filesystem::create_directories(spaced_dir);
    write_module(td, "with spaces/lib/m.dc", "m");
    write_module(td, "app.dc", "app", {"m"});

    CHECK(compile_entry(td.file("app.dc"), {spaced_dir}));
}

SECTION("Workspace variable expansion logic");

TEST_CASE("expand_workspaceFolder replaces variable in path")
{
    auto result = test_expand_workspace_variables("${workspaceFolder}/libdcext/std", "/home/user/project");
    CHECK_EQ(result, "/home/user/project/libdcext/std");
}

TEST_CASE("expand_workspaceFolderBasename replaces variable in path")
{
    auto result = test_expand_workspace_variables("lib/${workspaceFolderBasename}", "/home/user/myproject");
    CHECK_EQ(result, "lib/myproject");
}

TEST_CASE("expand both workspaceFolder and workspaceFolderBasename in same path")
{
    auto result = test_expand_workspace_variables("${workspaceFolder}/build/${workspaceFolderBasename}", "/home/user/proj");
    CHECK_EQ(result, "/home/user/proj/build/proj");
}

TEST_CASE("expand_workspace_variables handles empty workspace root gracefully")
{
    auto result = test_expand_workspace_variables("${workspaceFolder}/lib", std::filesystem::path{});
    CHECK_EQ(result, "${workspaceFolder}/lib");
}

TEST_CASE("expand_workspace_variables handles path without variables")
{
    auto result = test_expand_workspace_variables("/absolute/path/to/lib", "/home/user/proj");
    CHECK_EQ(result, "/absolute/path/to/lib");
}

TEST_CASE("expand_workspace_variables handles multiple occurrences")
{
    auto result = test_expand_workspace_variables("${workspaceFolder}/a/${workspaceFolder}/b", "/root");
    CHECK_EQ(result, "/root/a//root/b");
}

TEST_CASE("expand_workspaceFolder when root has trailing slash")
{
    auto result = test_expand_workspace_variables("${workspaceFolder}/include", "/home/user/project/");
    CHECK_EQ(result, "/home/user/project//include");
}

TEST_CASE("expand_workspaceFolderBasename with root ending in dot")
{
    auto result = test_expand_workspace_variables("${workspaceFolderBasename}.cfg", "/path/to/my.app");
    CHECK_EQ(result, "my.app.cfg");
}

SECTION("Import-roots: in-memory file resolution (LSP simulation)");

TEST_CASE("in-memory import target resolves without disk file")
{
    TempDir td;
    write_module(td, "app.dc", "app", {"dep"});

    dcc::session::SessionOptions sopts;
    sopts.silent_diagnostics = true;
    std::stringstream diag_stream;
    sopts.diagnostic_stream = &diag_stream;

    dcc::session::CompilerSession session{sopts};

    auto entry_path = td.file("app.dc");

    auto dep_path = td.file("dep.dc");
    auto dep_uri = dcc::sm::SourceManager::to_file_uri(dep_path);
    std::ignore = session.open_in_memory(dep_uri, "module dep;\npublic void helper() {}\n");

    dcc::session::CompileOptions opts;
    opts.import_roots = {td.path};

    auto result = session.analyze_entry(entry_path, opts);

    std::string errors = diag_stream.str();
    CHECK(!result.has_errors);
    CHECK(result.module != nullptr);
}

TEST_CASE("in-memory entry with in-memory import target both resolve")
{
    TempDir td;

    dcc::session::SessionOptions sopts;
    sopts.silent_diagnostics = true;
    std::stringstream diag_stream;
    sopts.diagnostic_stream = &diag_stream;

    dcc::session::CompilerSession session{sopts};

    auto dep_path = td.file("dep.dc");
    auto dep_uri = dcc::sm::SourceManager::to_file_uri(dep_path);
    std::ignore = session.open_in_memory(dep_uri, "module dep;\npublic i32 value = 42;\n");

    auto entry_path = td.file("app.dc");
    auto entry_uri = dcc::sm::SourceManager::to_file_uri(entry_path);
    std::ignore = session.open_in_memory(entry_uri, "module app;\nimport dep;\npublic i32 get() { return dep::value; }\n");

    write_module(td, "app.dc", "app", {"dep"}, "public i32 get() { return dep::value; }\n");

    dcc::session::CompileOptions opts;
    opts.import_roots = {td.path};

    auto result = session.analyze_entry(entry_path, opts);

    std::string errors = diag_stream.str();
    CHECK(!result.has_errors);
    CHECK(result.module != nullptr);
}

TEST_CASE("in-memory files survive repeated recompilation")
{
    TempDir td;

    dcc::session::SessionOptions sopts;
    sopts.silent_diagnostics = true;
    std::stringstream diag_stream;
    sopts.diagnostic_stream = &diag_stream;

    dcc::session::CompilerSession session{sopts};

    auto dep_path2 = td.file("dep.dc");
    auto dep_uri2 = dcc::sm::SourceManager::to_file_uri(dep_path2);
    std::ignore = session.open_in_memory(dep_uri2, "module dep;\npublic void helper() {}\n");
    write_module(td, "app.dc", "app", {"dep"});

    for (int i = 0; i < 3; ++i)
    {
        dcc::session::CompileOptions opts;
        opts.import_roots = {td.path};

        auto result = session.analyze_entry(td.file("app.dc"), opts);
        CHECK(result.module != nullptr);
        CHECK(!result.has_errors);
    }
}

TEST_CASE("direct semantic analysis provides i8086 division dependencies without source imports")
{
    struct Probe
    {
        std::string_view source;
        bool needs_helper;
    };
    Probe const probes[] = {
        {"@runtime u64 f(u64 a, u64 b) { return a / b + a % b; }", true},
        {"@runtime i64 f(i64 a, i64 b) { return a / b + a % b; }", true},
        {"@runtime u64 f(u64 a, u64 b) { a /= b; a %= b; return a; }", true},
        {"T divide(T)(T a, T b) { return a / b; } @runtime u64 f(u64 a, u64 b) { return divide(a, b); }", true},
        {"@runtime u64 f(u64 a, u64 b) { return a + b; }", false},
        {"@runtime u32 f(u32 a, u32 b) { return a / b; }", false},
        {"public u64 value = 12 as u64 / 3;", false},
        {"import core::i8086_arith; @runtime u64 f(u64 a, u64 b) { return a / b; }", true},
    };
    for (auto model : {dcc::target::CodeModel::Small, dcc::target::CodeModel::Unreal, dcc::target::CodeModel::Unreal32})
        for (auto const& probe : probes)
        {
            TempDir td;
            td.write_file("main.dc", std::string{"module main;\n"} + std::string{probe.source});
            dcc::sm::SourceManager sm;
            std::ostringstream errors;
            dcc::diag::DiagnosticEngine diag{sm, errors};
            dcc::ast::AstContext ast;
            dcc::si::string_interner interner;
            auto parse = [&](dcc::sm::FileId fid, dcc::ast::AstContext& ctx, dcc::diag::DiagnosticEngine& d) {
                dcc::lex::Lexer lexer{*sm.get(fid), interner};
                return dcc::parser::Parser{lexer, ctx, d}.parse();
            };
            auto target = *dcc::target::TargetConfig::parse_triple("i8086-binary");
            REQUIRE(!target.configure(model, "i386"));
            dcc::sema::SemaOptions opts;
            opts.target = target;
            opts.interner = &interner;
            dcc::sema::SemaContext sema{sm, diag, ast, parse, std::move(opts)};
            auto* entry = sema.analyze_entry(td.file("main.dc"));
            REQUIRE(entry != nullptr);
            CHECK(!diag.has_errors());
            auto helpers = std::ranges::count_if(sema.graph().all(), [](auto const& m) { return m->canonical_path.str() == "core::i8086_arith"; });
            CHECK_EQ(helpers, probe.needs_helper ? 1 : 0);
            dcc::ir::IrContext ir{256 * 1024, &target};
            dcc::ir::lower::Lowerer lowerer{ir, &sema.spec_registry(), &sema.graph(), false, &sm, &sema.types()};
            auto* module = lowerer.lower_module(*entry);
            REQUIRE(module != nullptr);
            CHECK(lowerer.lower_errors().empty());
        }
}
