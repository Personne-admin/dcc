import std;
import dcc.session;
import dcc.sema;
import dcdoc.builder;
import dcdoc.model;
import dcdoc.typst.emit;

#include "harness.hh"

#include <stdio.h>
#include <stdlib.h>

struct TempDir
{
    std::filesystem::path path;
    static inline int s_counter{0};

    TempDir()
    {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto dir = std::filesystem::temp_directory_path() / ("dcdoc_test_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
        std::filesystem::create_directories(dir);
        path = std::move(dir);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    void write_file(std::filesystem::path const& relative, std::string const& content) const
    {
        auto full = path / relative;
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream f{full};
        f << content;
    }
};

[[nodiscard]] bool contains(std::string const& haystack, std::string_view needle)
{
    return haystack.contains(needle);
}

SECTION("dcdoc model");

TEST_CASE("multi-file project resolves cross-file refs and records misses")
{
    TempDir td;
    td.write_file("shapes.dc", "//!! Shapes module.\nmodule shapes;\n\n//! Primitives\n\n/// A point.\npublic struct Point {\n    i32 x;\n    i32 y;\n}\n\n/// "
                               "A line.\npublic struct Line {\n    Point a;\n    Point b;\n}\n");
    td.write_file("drawing.dc",
                  "module drawing;\npublic import shapes;\n\n/// Draws with [`shapes::Point`] and [`Nope::Missing`].\npublic void draw(shapes::Point p) {}\n");
    td.write_file("generic.dc", "module generic;\n\n/// A map.\npublic struct Map(K, V) {\n    K key;\n}\n");
    td.write_file("main.dc", "module main;\npublic import drawing;\npublic import generic;\n\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string dump = dcdoc::dump(project);
    CHECK_EQ(project.modules.size(), 4u);
    CHECK(contains(dump, "shapes::Point#struct"));
    CHECK(contains(dump, "drawing::draw#fn(shapes::Point)"));
    CHECK(contains(dump, "generic::Map::key#field"));
    CHECK(contains(dump, "generic::Map::K#tparam"));
    CHECK(contains(dump, "Primitives"));
    CHECK(contains(dump, "Shapes module."));
    bool miss_found = false;
    for (auto const& m : project.misses)
        if (m == "Nope::Missing")
            miss_found = true;
    CHECK(miss_found);
    CHECK(project.file_errors.empty());
}

TEST_CASE("parse error does not abort the rest of the project")
{
    TempDir td;
    td.write_file("good_a.dc", "module good_a;\n\n/// Fine.\npublic void a() {}\n");
    td.write_file("good_b.dc", "module good_b;\n\n/// Also fine.\npublic void b() {}\n");
    td.write_file("bad.dc", "module bad;\nthis is not valid dc (((\n");
    dcdoc::Builder builder{td.path / "good_a.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string dump = dcdoc::dump(project);
    CHECK(contains(dump, "good_a::a#fn()"));
    CHECK(contains(dump, "good_b::b#fn()"));
    CHECK(!project.file_errors.empty());
}

TEST_CASE("resolve-only instantiates nothing")
{
    TempDir td;
    td.write_file("generic.dc", "module generic;\n\n/// Identity.\npublic T identity(T)(T x) { return x; }\n");
    td.write_file("main.dc", "module main;\nimport generic;\npublic i32 run() { return generic::identity(42); }\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    CHECK_EQ(builder.instantiation_count(), 0u);
    CHECK(contains(dcdoc::dump(project), "generic::identity#fn(T)"));
    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    dcc::session::CompileOptions opts;
    opts.import_roots.push_back(td.path);
    auto result = session.analyze_entry(td.path / "main.dc", opts);
    REQUIRE(result.module != nullptr);
    CHECK(session.sema_context()->spec_registry().instantiation_count() > 0u);
}

SECTION("dcdoc typst");

TEST_CASE("resolved ambiguous and unresolved refs render distinctly")
{
    TempDir td;
    td.write_file("p1.dc", "module p1;\n\n/// Thing.\npublic struct Thing {\n    i32 v;\n}\n\n/// Runner one.\npublic void go() {}\n");
    td.write_file("p2.dc", "module p2;\n\n/// Runner two.\npublic void go() {}\n");
    td.write_file("doc_a.dc", "module doc_a;\npublic import p1;\n\n/// Uses [`p1::Thing`] and [`Nope::Missing`].\npublic void f() {}\n");
    td.write_file("doc_b.dc", "module doc_b;\n\n/// Calls [`go`].\npublic void g() {}\n");
    td.write_file("main.dc", "module main;\npublic import doc_a;\npublic import doc_b;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string typ = dcdoc::typst::render(project);
    CHECK(typ == dcdoc::typst::render(project));
    CHECK(contains(typ, "#link(<p1--Thing-struct>)[p1::Thing]"));
    CHECK(contains(typ, "#text(fill: luma(130))[go]"));
    CHECK(contains(typ, "Nope::Missing"));
    CHECK(!contains(typ, "#link(<go"));
}

TEST_CASE("fenced code block renders as raw block")
{
    TempDir td;
    td.write_file("m.dc", "module m;\n\n/// Example:\n/// ```\n/// let x = 1;\n///\n/// let y = 2;\n/// ```\npublic void f() {}\n");
    td.write_file("main.dc", "module main;\npublic import m;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string typ = dcdoc::typst::render(project);
    CHECK(contains(typ, "lang: \"dc\""));
    CHECK(contains(typ, "let x = 1;"));
}

TEST_CASE("zero documented items still render")
{
    TempDir td;
    td.write_file("bare.dc", "module bare;\npublic void f() {}\n");
    td.write_file("main.dc", "module main;\npublic import bare;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string typ = dcdoc::typst::render(project);
    CHECK(!typ.empty());
    CHECK(contains(typ, "#heading(level: 1)[bare]"));
}

TEST_CASE("missing typst binary is a clean error")
{
    const char* old_path = ::getenv("PATH");
    std::string saved = old_path ? old_path : "";
    ::setenv("PATH", "/nonexistent-dir-for-dcdoc-test", 1);
    int rc = dcdoc::typst::compile_pdf("/tmp/dcdoc-missing.typ", "/tmp/dcdoc-missing.pdf");
    if (!saved.empty())
        ::setenv("PATH", saved.c_str(), 1);
    CHECK_EQ(rc, 2);
}

[[nodiscard]] bool have_tool(std::string_view name)
{
    std::string cmd = std::string{"command -v "} + std::string{name} + " 2>/dev/null";
    std::array<char, 128> buf{};
    auto* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        return false;
    bool found = ::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr;
    ::pclose(pipe);
    return found;
}

[[nodiscard]] std::string read_file_bytes(std::filesystem::path const& p)
{
    std::ifstream in{p, std::ios::binary};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TEST_CASE("pdf end to end validates")
{
    TempDir td;
    td.write_file("shapes.dc", "//!! Shapes module.\nmodule shapes;\n\n//! Primitives\n\n/// A point.\npublic struct Point {\n    i32 x;\n    i32 y;\n}\n\n/// "
                               "A line.\npublic struct Line {\n    Point a;\n    Point b;\n}\n");
    td.write_file("drawing.dc",
                  "module drawing;\npublic import shapes;\n\n/// Draws with [`shapes::Point`] and [`Nope::Missing`].\npublic void draw(shapes::Point p) {}\n");
    td.write_file("generic.dc", "module generic;\n\n/// A map.\npublic struct Map(K, V) {\n    K key;\n}\n");
    td.write_file("main.dc", "module main;\npublic import drawing;\npublic import generic;\n\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    REQUIRE(project.file_errors.empty());
    std::string typ = dcdoc::typst::render(project);
    auto typ_path = td.path / "out.typ";
    auto pdf_path = td.path / "out.pdf";
    {
        std::ofstream out{typ_path};
        REQUIRE(static_cast<bool>(out));
        out << typ;
    }
    int rc = dcdoc::typst::compile_pdf(typ_path, pdf_path);
    if (rc == 2)
        return;
    REQUIRE(rc == 0);
    std::string pdf = read_file_bytes(pdf_path);
    REQUIRE(pdf.size() > 1024);
    CHECK(pdf.starts_with("%PDF-"));
    std::string tail = pdf.substr(pdf.size() > 2048 ? pdf.size() - 2048 : 0);
    CHECK(tail.find("%%EOF") != std::string::npos);
    if (have_tool("pdfinfo"))
    {
        std::string cmd = std::string{"pdfinfo "} + pdf_path.string();
        std::array<char, 256> buf{};
        std::string out;
        auto* pipe = ::popen(cmd.c_str(), "r");
        REQUIRE(pipe != nullptr);
        while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
            out += buf.data();
        ::pclose(pipe);
        auto pos = out.find("Pages:");
        REQUIRE(pos != std::string::npos);
        CHECK(std::stoi(out.substr(pos + 6)) > 0);
    }
    if (have_tool("pdftotext"))
    {
        auto txt_path = td.path / "out.txt";
        std::string cmd = std::string{"pdftotext "} + pdf_path.string() + " " + txt_path.string() + " 2>/dev/null";
        REQUIRE(std::system(cmd.c_str()) == 0);
        std::string txt = read_file_bytes(txt_path);
        CHECK(contains(txt, "Shapes module"));
        CHECK(contains(txt, "Point"));
        CHECK(contains(txt, "A point"));
    }
}

TEST_CASE("stress special characters compile")
{
    TempDir td;
    td.write_file("stress.dc",
                  "//!! Stress module <overview> with #hash $dollar @at *star _under_ [brackets] and `ticks`.\nmodule stress;\n\n//! Sec <tion> #1 $x @y *em* "
                  "_it_ `c` [b]\n//! Body with <angle> #hash $math @ref *star* _under_ `code` [text].\n/// Doc: <a> #b $c @d *e* _f_ `g` [h] and math "
                  "$x^2$.\n///\n/// ```\n/// #raw #link <tag> $100 `tick` *star* _u_ [b]\n/// ```\n/// Tail with `inline <code> $x` end.\npublic struct "
                  "Holder(T) {\n    T value;\n}\npublic u8[64] buffer;\npublic void use_it(Holder(u8) h, u8[16] arr) {}\n");
    td.write_file("main.dc", "module main;\npublic import stress;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string typ = dcdoc::typst::render(project);
    CHECK(contains(typ, "\\<a>"));
    CHECK(contains(typ, "\\#b"));
    CHECK(contains(typ, "`g`"));
    CHECK(contains(typ, "lang: \"dc\""));
    auto typ_path = td.path / "stress.typ";
    auto pdf_path = td.path / "stress.pdf";
    {
        std::ofstream out{typ_path};
        REQUIRE(static_cast<bool>(out));
        out << typ;
    }
    int rc = dcdoc::typst::compile_pdf(typ_path, pdf_path);
    if (rc == 2)
        return;
    REQUIRE(rc == 0);
    std::string pdf = read_file_bytes(pdf_path);
    REQUIRE(pdf.size() > 1024);
    CHECK(pdf.starts_with("%PDF-"));
}
