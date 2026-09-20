import std;
import dcc.session;
import dcc.sema;
import dcdoc.builder;
import dcdoc.model;
import dcdoc.typst.emit;
import dcdoc.markdown.emit;
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
    CHECK(contains(md, "\\*star\\*"));
    CHECK(contains(md, "\\_under\\_"));
    CHECK(contains(md, "\\[bracket\\]"));
    CHECK(contains(md, "`code`"));
    CHECK(contains(md, "\\\\slash"));
    CHECK(contains(md, "\"say\""));
    CHECK(contains(md, "\u0027q\u0027"));
    CHECK(contains(md, "\\# Heading-looking"));
    CHECK(contains(md, "\\- list-looking"));
    CHECK(contains(md, "\\> quote-looking"));
    CHECK(contains(md, "1\\. ordered-looking"));
    CHECK(contains(md, "&#32;&#32;&#32;&#32;indented"));
    CHECK(contains(md, "u8[16]"));
    CHECK(md_fences_balanced(md));
    CHECK(md_blocks_separated(md));
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

TEST_CASE("stdlib resolves via default prefix root")
{
    const char* std_root = ::getenv("DCC_TEST_LIBDCEXT_SRC");
    if (!std_root)
        return;
    std::error_code ec;
    if (!std::filesystem::is_directory(std_root, ec) || ec)
        return;
    TempDir td;
    td.write_file("main.dc", "module main;\npublic import std::fmt;\n\n/// Entry.\npublic void run() {}\n");
    dcdoc::Builder builder{td.path / "main.dc", {td.path}};
    dcdoc::Project project = builder.build();
    CHECK(project.file_errors.empty());
    CHECK(contains(dcdoc::dump(project), "std::fmt"));
}
