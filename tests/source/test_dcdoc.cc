import std;
import dcc.session;
import dcc.sema;
import dcdoc.builder;
import dcdoc.model;

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
