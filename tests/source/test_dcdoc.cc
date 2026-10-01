import std;
import dcc.session;
import dcc.sema;
import dcdoc.builder;
import dcdoc.model;
import dcdoc.prose;
import dcdoc.typst.emit;
import dcdoc.html.emit;
import dcdoc.markdown.emit;

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

[[nodiscard]] bool have_tool(std::string_view name);
[[nodiscard]] std::string read_file_bytes(std::filesystem::path const& p);

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

SECTION("dcdoc signatures");

TEST_CASE("signatures reproduce real dc syntax")
{
    TempDir td;
    td.write_file("sigv.dc",
                  "module sigv;\n\npublic void parse_numberws(const u8** ptr, usize* n, usize* skiped_bytes) {}\n\npublic T identity(T)(T x) { return x; "
                  "}\n\npublic struct Holder {\n    void* ctx;\n    u8[16] buf;\n    []const u8 data;\n    void(*)(i32 code) on_event;\n}\n\npublic struct "
                  "Map(K, V) {\n    K key;\n}\n\npublic struct Box(T = i32) {\n    T value;\n}\n\npublic union U {\n    i32 i;\n    f32 f;\n}\n\npublic enum E "
                  ": u8 {\n    A,\n    B(i32),\n    C = 42,\n}\n\npublic using X = i32;\nusing usize MAX = 8;\n");
    td.write_file("main.dc", "module main;\npublic import sigv;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    REQUIRE(project.file_errors.empty());
    std::string dump = dcdoc::dump(project);
    CHECK(contains(dump, "public void parse_numberws(const u8** ptr, usize* n, usize* skiped_bytes)"));
    CHECK(contains(dump, "public T identity(T)(T x)"));
    CHECK(contains(dump, "const u8** ptr"));
    CHECK(contains(dump, "T x"));
    CHECK(contains(dump, "void* ctx;"));
    CHECK(contains(dump, "u8[16] buf;"));
    CHECK(contains(dump, "[]const u8 data;"));
    CHECK(contains(dump, "void(*)(i32 code) on_event;"));
    CHECK(contains(dump, "public struct Map(K, V)"));
    CHECK(contains(dump, "K key;"));
    CHECK(contains(dump, "public struct Box(T = i32)"));
    CHECK(contains(dump, "T value;"));
    CHECK(contains(dump, "T = i32"));
    CHECK(contains(dump, "public union U"));
    CHECK(contains(dump, "public enum E : u8"));
    CHECK(contains(dump, "B(i32)"));
    CHECK(contains(dump, "C = 42"));
    CHECK(contains(dump, "public using X = i32;"));
    CHECK(contains(dump, "using usize MAX = 8;"));
    CHECK(contains(dump, "target:\"sigv::identity::T#tparam\" resolved:true via:\"ParamType\""));
    CHECK(contains(dump, "target:\"sigv::Box::T#tparam\" resolved:true via:\"FieldType\""));
}

SECTION("dcdoc typst");

TEST_CASE("CommonMark prose keeps pointer stars and lazy list lines")
{
    std::string source = "reads *ptr and returns a u8* value\n\n(*p)++\n\na const u8** buffer\n\nx*y*z\n\n"
                         "- time O(h + d + m*b) where h is the count,\n"
                         "boss attacks continue here.\n"
                         "- space O(h + *m*) where *m* is mana.\n";
    auto blocks = dcdoc::prose::parse_doc(source, {});
    REQUIRE(blocks.size() == 5);
    CHECK(blocks[0].spans[1].kind == dcdoc::prose::InlineKind::Emph);
    CHECK(blocks[1].spans[0].text == "(*p)++");
    CHECK(blocks[2].spans[0].text == "a const u8** buffer");
    CHECK(blocks[3].spans[1].kind == dcdoc::prose::InlineKind::Emph);
    CHECK(blocks[4].kind == dcdoc::prose::BlockKind::BulletList);
    CHECK(blocks[4].children.size() == 2);
    CHECK(blocks[4].children[0].children.size() == 1);
    TempDir td;
    td.write_file("m.dc", "module m;\n//! Complexity\n//! - time O(h + d + m*b) where h is the count,\n"
                          "//! boss attacks continue here.\n//! - space O(h + *m*) where *m* is mana.\n"
                          "public void f() {}\n");
    dcdoc::Builder builder{td.path / "m.dc", {td.path}};
    auto project = builder.build();
    std::string typ = dcdoc::typst::render(project);
    std::string html_dir = (td.path / "html").string();
    REQUIRE(dcdoc::html::write_site(project, html_dir) == 0);
    std::string html = read_file_bytes(td.path / "html" / "m.html");
    CHECK(contains(html, "<ul>"));
    CHECK(contains(html, "boss attacks continue here"));
    CHECK(contains(html, "<em>m</em>"));
    std::string md = dcdoc::markdown::render_single(project);
    CHECK(contains(md, "- time O(h + d + m\\*b)"));
    CHECK(contains(md, "- space O(h + *m*)"));
    auto typ_path = td.path / "out.typ";
    auto pdf_path = td.path / "out.pdf";
    { std::ofstream out{typ_path}; REQUIRE(static_cast<bool>(out)); out << typ; }
    int rc = dcdoc::typst::compile_pdf(typ_path, pdf_path);
    if (rc == 2) return;
    REQUIRE(rc == 0);
    if (have_tool("pdftotext"))
    {
        auto txt_path = td.path / "out.txt";
        std::string cmd = "pdftotext -layout " + pdf_path.string() + " " + txt_path.string() + " 2>/dev/null";
        REQUIRE(std::system(cmd.c_str()) == 0);
        std::string txt = read_file_bytes(txt_path);
        CHECK(contains(txt, "O(h + d + m*b)"));
        CHECK(contains(txt, "boss attacks continue here"));
        CHECK(contains(txt, "O(h + m)"));
    }
}

TEST_CASE("section links resolve across modules and warn on dangling slugs")
{
    TempDir td;
    td.write_file("one.dc", "//!! See [#intro], [#intro-2], [`two::details#section`], and [`one::gone#section`].\n"
                             "module one;\n//! Intro\n//! First.\n\n//! Intro\n//! Second.\n"
                             "/// Refer to [`one::intro#section`](again).\npublic void f() {}\n");
    td.write_file("two.dc", "module two;\n//! Details\n//! Body.\npublic void g() {}\n");
    dcdoc::Builder builder{td.path / "one.dc", {td.path}};
    auto project = builder.build();
    REQUIRE(project.modules.size() == 2);
    REQUIRE(project.overview_refs.size() == 4);
    CHECK(project.overview_refs[0].target == "one::intro#section");
    CHECK(project.overview_refs[1].target == "one::intro-2#section");
    CHECK(project.overview_refs[2].target == "two::details#section");
    CHECK(!project.overview_refs[3].resolved);
    CHECK(contains(dcdoc::dump(project), "one::gone#section"));
    CHECK(project.warnings.size() == 1);
    std::string typ = dcdoc::typst::render(project);
    CHECK(contains(typ, "#(link(<"));
    auto typ_path = td.path / "sections.typ";
    auto pdf_path = td.path / "sections.pdf";
    { std::ofstream out{typ_path}; REQUIRE(static_cast<bool>(out)); out << typ; }
    int rc = dcdoc::typst::compile_pdf(typ_path, pdf_path);
    if (rc != 2) REQUIRE(rc == 0);
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    std::string page = read_file_bytes(site / "index.html");
    CHECK(contains(page, "one.html#one::intro%23section"));
    CHECK(contains(page, "two.html#two::details%23section"));
    std::string md = dcdoc::markdown::render_single(project);
    CHECK(contains(md, "[one::intro#section](#intro)"));
}

TEST_CASE("pdf listings keep source lines, comments, and reciprocal links")
{
    TempDir td;
    td.write_file("m.dc", "//!! Module overview.\nmodule m;\n//! API\n//! Narrative.\n"
                          "/// A value.\npublic struct Value {\n    /// Field docs.\n    i32 count; // inlay\n}\n"
                          "/// Uses a value.\npublic void use(Value v) {\n    /// Local docs.\n"
                          "    i32 local = 1; // this comment stays beside the statement\n    return;\n}\n");
    dcdoc::Builder builder{td.path / "m.dc", {td.path}};
    auto project = builder.build();
    REQUIRE(project.file_errors.empty());
    std::string typ = dcdoc::typst::render(project);
    std::string lean = dcdoc::typst::render(project, false);
    CHECK(contains(typ, "Source: m.dc"));
    CHECK(!contains(lean, "Source: m.dc"));
    CHECK(contains(typ, "<source-m-6>"));
    CHECK(contains(typ, "link(<source-m-6>"));
    CHECK(contains(typ, "// inlay"));
    CHECK(contains(typ, "/// Local docs."));
    auto typ_path = td.path / "listing.typ";
    auto pdf_path = td.path / "listing.pdf";
    { std::ofstream out{typ_path}; REQUIRE(static_cast<bool>(out)); out << typ; }
    int rc = dcdoc::typst::compile_pdf(typ_path, pdf_path);
    if (rc == 2) return;
    REQUIRE(rc == 0);
    if (have_tool("pdftotext"))
    {
        auto txt_path = td.path / "listing.txt";
        std::string cmd = "pdftotext -layout " + pdf_path.string() + " " + txt_path.string() + " 2>/dev/null";
        REQUIRE(std::system(cmd.c_str()) == 0);
        std::string txt = read_file_bytes(txt_path);
        CHECK(contains(txt, "Source: m.dc"));
        CHECK(contains(txt, "// inlay"));
        CHECK(contains(txt, "/// Local docs."));
        CHECK(contains(txt, "defined in m.dc:6"));
    }
}

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
    CHECK(contains(typ, "#(link(<p1--Thing-struct>, [p1::Thing]))"));
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

TEST_CASE("real pointer signatures compile")
{
    TempDir td;
    td.write_file("cb.dc", "module cb;\n\n/// Walker.\npublic struct Solution {\n    i32 v;\n}\n\n/// Runs callback.\npublic void for_each(bool(*)(Solution i) "
                           "cb, const u8** ptr, []u8 buf) {}\n");
    td.write_file("main.dc", "module main;\npublic import cb;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    REQUIRE(project.file_errors.empty());
    std::string typ = dcdoc::typst::render(project);
    CHECK(contains(typ, "text(fill: rgb(31, 111, 235), \"bool\")"));
    CHECK(contains(typ, "link(<cb--Solution-struct>,"));
    auto typ_path = td.path / "cb.typ";
    auto pdf_path = td.path / "cb.pdf";
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
    if (have_tool("pdftotext"))
    {
        auto txt_path = td.path / "cb.txt";
        std::string cmd = std::string{"pdftotext "} + pdf_path.string() + " " + txt_path.string() + " 2>/dev/null";
        REQUIRE(std::system(cmd.c_str()) == 0);
        std::string txt = read_file_bytes(txt_path);
        std::string flat;
        for (char c : txt)
            flat += (c == 10 || c == 13) ? char(32) : c;
        CHECK(contains(flat, "public void for_each(bool(*)(Solution i) cb, const u8** ptr, []u8 buf)"));
        CHECK(!contains(txt, "(public)"));
    }
}

TEST_CASE("short and wrapped signatures extract as dc syntax")
{
    TempDir td;
    td.write_file("sig.dc", "module sig;\npublic void short_fn(i32 x) {}\n"
                            "public void long_function_name(const u8** source_pointer, usize* remaining_bytes, "
                            "bool(*)(i32 value) callback) {}\n");
    dcdoc::Builder builder{td.path / "sig.dc", {td.path}};
    auto project = builder.build();
    REQUIRE(project.file_errors.empty());
    std::string typ = dcdoc::typst::render(project);
    CHECK(contains(typ, "linebreak() + h(2em)"));
    auto typ_path = td.path / "sig.typ";
    auto pdf_path = td.path / "sig.pdf";
    { std::ofstream out{typ_path}; REQUIRE(static_cast<bool>(out)); out << typ; }
    int rc = dcdoc::typst::compile_pdf(typ_path, pdf_path);
    if (rc == 2) return;
    REQUIRE(rc == 0);
    if (have_tool("pdftotext"))
    {
        auto txt_path = td.path / "sig.txt";
        std::string cmd = "pdftotext -layout " + pdf_path.string() + " " + txt_path.string() + " 2>/dev/null";
        REQUIRE(std::system(cmd.c_str()) == 0);
        std::string txt = read_file_bytes(txt_path);
        CHECK(contains(txt, "public void short_fn(i32 x)"));
        CHECK(contains(txt, "public void long_function_name(const u8** source_pointer,"));
        CHECK(contains(txt, "usize* remaining_bytes,"));
        CHECK(contains(txt, "bool(*)(i32 value) callback)"));
    }
}

SECTION("dcdoc html");

TEST_CASE("html site has expected files and cross-page links")
{
    TempDir td;
    td.write_file("shapes.dc", "//!! Shapes module.\nmodule shapes;\n\n//! Primitives\n\n/// A point.\npublic struct Point {\n    i32 x;\n    i32 y;\n}\n");
    td.write_file("drawing.dc",
                  "module drawing;\npublic import shapes;\n\n/// Draws with [`shapes::Point`] and [`Nope::Missing`].\npublic void draw(shapes::Point p) {}\n");
    td.write_file("main.dc", "module main;\npublic import drawing;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    for (std::string const& f : {"index.html", "shapes.html", "drawing.html", "main.html", "search-index.json", "style.css", "search.js"})
        CHECK(std::filesystem::exists(site / f));
    std::string drawing = read_file_bytes(site / "drawing.html");
    CHECK(contains(drawing, "<a class=\"ref\" href=\"shapes.html#shapes::Point%23struct\">shapes::Point</a>"));
    CHECK(contains(drawing, "<span class=\"ref-unres\">Nope::Missing</span>"));
    std::string index = read_file_bytes(site / "index.html");
    CHECK(contains(index, "href=\"shapes.html\""));
    CHECK(contains(index, "Shapes module"));
    std::string db = read_file_bytes(site / "search-index.json");
    CHECK(contains(db, "\"page\":\"drawing.html\""));
    CHECK(contains(db, "\"name\":\"draw\""));
}

TEST_CASE("html refs render three distinct forms")
{
    TempDir td;
    td.write_file("p1.dc", "module p1;\n\n/// Thing.\npublic struct Thing {\n    i32 v;\n}\n\n/// Runner one.\npublic void go() {}\n");
    td.write_file("p2.dc", "module p2;\n\n/// Runner two.\npublic void go() {}\n");
    td.write_file("doc_a.dc", "module doc_a;\npublic import p1;\n\n/// Uses [`p1::Thing`] and [`Nope::Missing`].\npublic void f() {}\n");
    td.write_file("doc_b.dc", "module doc_b;\n\n/// Calls [`go`].\npublic void g() {}\n");
    td.write_file("main.dc", "module main;\npublic import doc_a;\npublic import doc_b;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    std::string a = read_file_bytes(site / "doc_a.html");
    CHECK(contains(a, "<a class=\"ref\""));
    CHECK(contains(a, "<span class=\"ref-unres\">Nope::Missing</span>"));
    std::string b = read_file_bytes(site / "doc_b.html");
    CHECK(contains(b, "<span class=\"ref-ambig\">go</span>"));
    CHECK(!contains(b, "<a class=\"ref\""));
}

TEST_CASE("html pages are strictly well-formed")
{
    if (!have_tool("xmllint"))
        return;
    TempDir td;
    td.write_file("shapes.dc", "//!! Shapes module.\nmodule shapes;\n\n/// A point.\npublic struct Point {\n    i32 x;\n}\n");
    td.write_file("main.dc", "module main;\npublic import shapes;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    for (std::string const& f : {"index.html", "shapes.html", "main.html"})
    {
        std::string cmd = std::string{"xmllint --noout "} + (site / f).string() + " 2>&1";
        std::array<char, 256> buf{};
        std::string out;
        auto* pipe = ::popen(cmd.c_str(), "r");
        REQUIRE(pipe != nullptr);
        while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
            out += buf.data();
        CHECK(::pclose(pipe) == 0);
    }
}

TEST_CASE("html escapes special characters")
{
    TempDir td;
    td.write_file(
        "m.dc",
        "module m;\n\n/// Less <greater> &amp \"say\" \u0027quote\u0027 *star* _u_ `c`.\n/// <script>alert(1)</script> end.\npublic void f(u8[16] a) {}\n");
    td.write_file("main.dc", "module main;\npublic import m;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    std::string page = read_file_bytes(site / "m.html");
    CHECK(contains(page, "&lt;greater&gt;"));
    CHECK(contains(page, "&amp;amp"));
    CHECK(contains(page, "&quot;"));
    CHECK(contains(page, "&#39;"));
    CHECK(contains(page, "&lt;script&gt;alert(1)&lt;/script&gt;"));
    CHECK(!contains(page, "<script>alert"));
    CHECK(contains(page, "<em>star</em>"));
    CHECK(contains(page, "u8[16]"));
}

TEST_CASE("html fences become pre code blocks")
{
    TempDir td;
    td.write_file("m.dc", "module m;\n\n/// Example:\n/// ```\n/// let x = 1;\n/// ```\npublic void f() {}\n");
    td.write_file("main.dc", "module main;\npublic import m;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    std::string page = read_file_bytes(site / "m.html");
    CHECK(contains(page, "<pre><code class=\"language-dc\">"));
    CHECK(contains(page, "let x = 1;"));
    CHECK(!contains(page, "```"));
}

TEST_CASE("html zero-item site still generates")
{
    TempDir td;
    td.write_file("bare.dc", "module bare;\npublic void f() {}\n");
    td.write_file("main.dc", "module main;\npublic import bare;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    CHECK(std::filesystem::exists(site / "index.html"));
    CHECK(std::filesystem::exists(site / "bare.html"));
    CHECK(contains(read_file_bytes(site / "index.html"), "bare"));
}

TEST_CASE("html output preserves unrelated files")
{
    TempDir td;
    td.write_file("m.dc", "module m;\n\n/// Fine.\npublic void f() {}\n");
    td.write_file("main.dc", "module main;\npublic import m;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto site = td.path / "site";
    td.write_file("site/keep.txt", "do not touch");
    td.write_file("site/index.html", "OLD");
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    CHECK(read_file_bytes(site / "keep.txt") == "do not touch");
    CHECK(read_file_bytes(site / "index.html") != "OLD");
}

SECTION("dcdoc markdown");

[[nodiscard]] bool md_fences_balanced(std::string const& md)
{
    std::size_t open = 0;
    std::size_t i = 0;
    while (i < md.size())
    {
        std::size_t j = md.find(10, i);
        if (j == std::string::npos)
            j = md.size();
        std::size_t k = i;
        while (k < j && md[k] == 96)
            ++k;
        std::size_t run = k - i;
        if (run >= 3)
        {
            if (open == 0)
                open = run;
            else if (run >= open)
                open = 0;
        }
        if (j == md.size())
            break;
        i = j + 1;
    }
    return open == 0;
}

[[nodiscard]] bool md_blocks_separated(std::string const& md)
{
    if (md.find("\n\n\n") != std::string::npos)
        return false;
    bool in_fence = false;
    std::size_t frun = 0;
    bool prev_blank = true;
    bool prev_boundary = false;
    std::size_t i = 0;
    while (i < md.size())
    {
        std::size_t j = md.find(10, i);
        if (j == std::string::npos)
            j = md.size();
        bool blank = true;
        for (std::size_t t = i; t < j; ++t)
            if (md[t] != 32 && md[t] != 9 && md[t] != 13)
                blank = false;
        if (!blank && !in_fence)
        {
            std::size_t k = i;
            while (k < j && md[k] == 96)
                ++k;
            std::size_t run = k - i;
            bool current = false;
            if (run >= 3)
            {
                in_fence = true;
                frun = run;
                current = true;
            }
            else if (md[i] == 35)
                current = true;
            else if (j - i >= 11 && md.compare(i, 11, "Defined in ") == 0)
                current = true;
            if ((current || prev_boundary) && !prev_blank)
                return false;
            prev_boundary = current;
        }
        else if (!blank)
        {
            std::size_t k = i;
            while (k < j && md[k] == 96)
                ++k;
            std::size_t run = k - i;
            bool rest_blank = true;
            for (std::size_t t = k; t < j; ++t)
                if (md[t] != 32 && md[t] != 9 && md[t] != 13)
                    rest_blank = false;
            if (run >= 3 && run >= frun && rest_blank)
            {
                in_fence = false;
                prev_boundary = true;
            }
        }
        else
            prev_boundary = false;
        prev_blank = blank;
        if (j == md.size())
            break;
        i = j + 1;
    }
    return !in_fence;
}

TEST_CASE("markdown separation check rejects adjacent blocks")
{
    CHECK(!md_blocks_separated("## a\n## b\n"));
    CHECK(!md_blocks_separated("```dc\nx\n```\ntext\n"));
    CHECK(!md_blocks_separated("# a\n\n\n# b\n"));
    CHECK(md_blocks_separated("# a\n\ntext\n"));
}

TEST_CASE("markdown single file structure")
{
    TempDir td;
    td.write_file("shapes.dc", "module shapes;\n\n/// A point.\npublic struct Point {\n    i32 x;\n}\n");
    td.write_file("drawing.dc",
                  "module drawing;\npublic import shapes;\n\n/// Draws with [`shapes::Point`] and [`Nope::Missing`].\npublic void draw(shapes::Point p) {}\n");
    td.write_file("main.dc", "module main;\npublic import drawing;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string md = dcdoc::markdown::render_single(project);
    CHECK(contains(md, "# main"));
    CHECK(contains(md, "## drawing"));
    CHECK(contains(md, "#### draw"));
    CHECK(contains(md, "```dc"));
    CHECK(contains(md, "[shapes::Point](#point)"));
    CHECK(contains(md, "Nope::Missing"));
    CHECK(md_fences_balanced(md));
    CHECK(md_blocks_separated(md));
}

TEST_CASE("markdown multi-file structure")
{
    TempDir td;
    td.write_file("shapes.dc", "module shapes;\n\n/// A point.\npublic struct Point {\n    i32 x;\n}\n");
    td.write_file("drawing.dc", "module drawing;\npublic import shapes;\n\n/// Draws with [`shapes::Point`].\npublic void draw(shapes::Point p) {}\n");
    td.write_file("main.dc", "module main;\npublic import drawing;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto dir = td.path / "md";
    REQUIRE(dcdoc::markdown::write_markdown(project, dir, true) == 0);
    for (std::string const& f : {"index.md", "shapes.md", "drawing.md", "main.md"})
        CHECK(std::filesystem::exists(dir / f));
    std::string drawing = read_file_bytes(dir / "drawing.md");
    CHECK(contains(drawing, "[shapes::Point](shapes.md#point)"));
    CHECK(md_blocks_separated(drawing));
    CHECK(md_fences_balanced(drawing));
    std::string index = read_file_bytes(dir / "index.md");
    CHECK(contains(index, "[drawing](drawing.md)"));
    CHECK(md_blocks_separated(index));
}

TEST_CASE("markdown refs render three distinct forms")
{
    TempDir td;
    td.write_file("p1.dc", "module p1;\n\n/// Thing.\npublic struct Thing {\n    i32 v;\n}\n\n/// Runner one.\npublic void go() {}\n");
    td.write_file("p2.dc", "module p2;\n\n/// Runner two.\npublic void go() {}\n");
    td.write_file("doc_a.dc", "module doc_a;\npublic import p1;\n\n/// Uses [`p1::Thing`] and [`Nope::Missing`].\npublic void f() {}\n");
    td.write_file("doc_b.dc", "module doc_b;\npublic import p2;\n\n/// Calls [`go`].\npublic void g() {}\n");
    td.write_file("doc_c.dc", "module doc_c;\n\n/// Calls [`go`] and [`Nope::Missing`].\npublic void h() {}\n");
    td.write_file("main.dc", "module main;\npublic import doc_a;\npublic import doc_b;\npublic import doc_c;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string md = dcdoc::markdown::render_single(project);
    CHECK(contains(md, "[p1::Thing](#thing)"));
    CHECK(contains(md, "[go](#go-2)"));
    CHECK(contains(md, "*go*"));
    CHECK(contains(md, "Nope::Missing"));
    CHECK(!contains(md, "[Nope::Missing]("));
    CHECK(md_fences_balanced(md));
    CHECK(md_blocks_separated(md));
}

TEST_CASE("markdown overload headings disambiguate")
{
    TempDir td;
    td.write_file("shapes.dc", "module shapes;\n\n/// A point.\npublic struct Point {\n    i32 x;\n    i32 y;\n}\n\n/// A line.\npublic struct Line {\n    "
                               "Point a;\n    Point b;\n}\n");
    td.write_file("drawing.dc", "module drawing;\npublic import shapes;\n\n/// Draws a point.\npublic void draw(shapes::Point p) {}\n\n/// Draws a "
                                "line.\npublic void draw(shapes::Line l) {}\n");
    td.write_file("main.dc", "module main;\npublic import drawing;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string md = dcdoc::markdown::render_single(project);
    CHECK(contains(md, "#### draw\n"));
    CHECK(contains(md, "#### draw (2)\n"));
    CHECK(md == dcdoc::markdown::render_single(project));
    CHECK(md_fences_balanced(md));
    CHECK(md_blocks_separated(md));
}

TEST_CASE("markdown escapes special characters")
{
    TempDir td;
    td.write_file("m.dc", "module m;\n\n/// Doc with #hash *star* _under_ [bracket] `code` \\slash \"say\" \u0027q\u0027.\n/// # Heading-looking\n/// - "
                          "list-looking\n/// > quote-looking\n/// 1. ordered-looking\n///     indented line here\npublic void f(u8[16] a) {}\n");
    td.write_file("main.dc", "module main;\npublic import m;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string md = dcdoc::markdown::render_single(project);
    CHECK(contains(md, "#hash"));
    CHECK(contains(md, "*star*"));
    CHECK(contains(md, "*under*"));
    CHECK(contains(md, "\\[bracket\\]"));
    CHECK(contains(md, "`code`"));
    CHECK(contains(md, "\\\\slash"));
    CHECK(contains(md, "\"say\""));
    CHECK(contains(md, "\u0027q\u0027"));
    CHECK(contains(md, "### Heading-looking"));
    CHECK(contains(md, "- list-looking"));
    CHECK(contains(md, "> quote-looking"));
    CHECK(contains(md, "1. ordered-looking"));
    CHECK(contains(md, "indented line here"));
    CHECK(contains(md, "u8[16]"));
    CHECK(md_fences_balanced(md));
    CHECK(md_blocks_separated(md));
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    std::string page = read_file_bytes(site / "m.html");
    CHECK(contains(page, "<h3>Heading-looking</h3>"));
    CHECK(contains(page, "<ul>"));
}

TEST_CASE("markdown fences handle backtick content")
{
    TempDir td;
    td.write_file("m.dc", "module m;\n\n/// Example:\n/// ```dc\n/// let tick = `x`;\n/// op ```` y\n/// ```\n/// After.\npublic void f() {}\n");
    td.write_file("main.dc", "module main;\npublic import m;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    std::string md = dcdoc::markdown::render_single(project);
    CHECK(contains(md, "`````dc"));
    CHECK(contains(md, "let tick = `x`;"));
    CHECK(md_fences_balanced(md));
    CHECK(md_blocks_separated(md));
}

TEST_CASE("markdown zero-item output is non-empty")
{
    TempDir td;
    td.write_file("bare.dc", "module bare;\npublic void f() {}\n");
    td.write_file("main.dc", "module main;\npublic import bare;\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    auto out = td.path / "out.md";
    REQUIRE(dcdoc::markdown::write_markdown(project, out, false) == 0);
    std::string md = read_file_bytes(out);
    CHECK(!md.empty());
    CHECK(contains(md, "# main"));
    CHECK(md_fences_balanced(md));
    CHECK(md_blocks_separated(md));
}

namespace
{
    std::filesystem::path build_prefix()
    {
        const char* archive = ::getenv("DCC_TEST_LIBDCEXT_A");
        if (!archive)
            return {};
        return std::filesystem::path{archive}.parent_path().parent_path();
    }
}

TEST_CASE("stdlib resolves via default prefix root")
{
    const char* std_root = ::getenv("DCC_TEST_LIBDCEXT_SRC");
    if (!std_root)
        return;
    std::error_code ec;
    if (!std::filesystem::is_directory(std_root, ec) || ec)
        return;
    TempDir td;
    td.write_file("main.dc", "module main;\npublic import std::fmt;\n\n/// Entry with [`std::fmt::Writer`].\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}, {}, build_prefix()};
    dcdoc::Project project = builder.build();
    CHECK(project.file_errors.empty());
    CHECK(contains(dcdoc::dump(project), "target:\"std::fmt::Writer#struct\" resolved:true"));
}

TEST_CASE("stdlib modules render no chapters but keep resolved refs")
{
    const char* std_root = ::getenv("DCC_TEST_LIBDCEXT_SRC");
    if (!std_root)
        return;
    std::error_code ec;
    if (!std::filesystem::is_directory(std_root, ec) || ec)
        return;
    TempDir td;
    td.write_file("main.dc", "module main;\npublic import std::fmt;\n\n/// Formats via [`std::fmt::Writer`].\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}, {}, build_prefix()};
    dcdoc::Project project = builder.build();
    REQUIRE(project.file_errors.empty());
    std::string dump = dcdoc::dump(project);
    CHECK(!contains(dump, "- module \"std::"));
    CHECK(!contains(dump, "dcc-core:"));
    CHECK(contains(dump, "target:\"std::fmt::Writer#struct\" resolved:true"));
    std::string md = dcdoc::markdown::render_single(project);
    CHECK(!contains(md, "## std::fmt"));
    CHECK(contains(md, "[std::fmt::Writer]"));
    CHECK(!contains(md, "[std::fmt::Writer]("));
    std::string typ = dcdoc::typst::render(project);
    CHECK(contains(typ, "#text(style: \"italic\")[std::fmt::Writer]"));
    CHECK(!contains(typ, "link(<std--fmt--Writer"));
    CHECK(!contains(typ, "Source: std"));
    auto site = td.path / "site";
    REQUIRE(dcdoc::html::write_site(project, site) == 0);
    CHECK(!std::filesystem::exists(site / "std.fmt.html"));
    std::string page = read_file_bytes(site / "main.html");
    CHECK(contains(page, "<span class=\"ref-ext\">std::fmt::Writer</span>"));
    CHECK(!contains(page, "<a class=\"ref\""));
}

TEST_CASE("project core module gets a listing while prefix and virtual modules do not")
{
    const char* std_root = ::getenv("DCC_TEST_LIBDCEXT_SRC");
    if (!std_root) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(std_root, ec) || ec) return;

    TempDir td;
    td.write_file("core/util.dc", "module core::util;\n//! Utilities\n//! Project source.\n"
                                  "/// A project value.\npublic struct Value { i32 count; }\n");
    td.write_file("main.dc", "module main;\npublic import core::util;\n"
                             "public import core::atomic;\npublic import std::fmt;\n"
                             "/// Entry.\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}, {}, build_prefix()};
    auto project = builder.build();
    REQUIRE(project.file_errors.empty());
    std::string dump = dcdoc::dump(project);
    CHECK(contains(dump, "- module \"core::util\""));
    CHECK(!contains(dump, "- module \"std::"));
    CHECK(!contains(dump, "- module \"core::atomic\""));

    std::string typ = dcdoc::typst::render(project);
    CHECK(contains(typ, "Source: core/util.dc"));
    CHECK(!contains(typ, "Source: atomic.dc"));
    CHECK(!contains(typ, "Source: fmt.dc"));

    auto typ_path = td.path / "origin.typ";
    auto pdf_path = td.path / "origin.pdf";
    { std::ofstream out{typ_path}; REQUIRE(static_cast<bool>(out)); out << typ; }
    int rc = dcdoc::typst::compile_pdf(typ_path, pdf_path);
    if (rc == 2) return;
    REQUIRE(rc == 0);
    if (have_tool("pdftotext"))
    {
        auto txt_path = td.path / "origin.txt";
        std::string cmd = "pdftotext -layout " + pdf_path.string() + " " + txt_path.string() + " 2>/dev/null";
        REQUIRE(std::system(cmd.c_str()) == 0);
        std::string txt = read_file_bytes(txt_path);
        CHECK(contains(txt, "core::util"));
        CHECK(contains(txt, "Source: core/util.dc"));
        CHECK(!contains(txt, "Source: atomic.dc"));
        CHECK(!contains(txt, "Source: fmt.dc"));
    }
}
