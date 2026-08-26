import std;
import dcc.sm;
import dcc.session;
import dcc.query;
import dccd.protocol;
import dccd.workspace_index;

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
            auto dir = tmp / ("dcc_test_wsidx_" + std::to_string(::getpid()) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(dir);
            path = std::move(dir);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write_file(std::string_view relative, std::string_view content) const
        {
            auto full = path / std::filesystem::path{relative};
            auto parent = full.parent_path();
            if (!parent.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
            }

            std::ofstream f(full);
            f << content;
        }

        [[nodiscard]] std::filesystem::path file(std::string_view relative) const { return path / std::filesystem::path{relative}; }
    };

    struct Workspace
    {
        TempDir td;
        dcc::session::SessionOptions sopts;
        dcc::session::CompileOptions copts;
        dcc::session::CompilerSession session;
        dccd::workspace_index::WorkspaceIndex index;

        Workspace()
            : sopts{[] {
                  dcc::session::SessionOptions o;
                  o.silent_diagnostics = true;
                  return o;
              }()},
              copts{}, session{sopts}
        {
            copts.import_roots.push_back(td.path);
        }

        void write(std::string_view rel, std::string_view content) { td.write_file(rel, content); }

        [[nodiscard]] bool refresh(std::string_view rel) { return session.source_manager().refresh_disk_file(td.file(rel)); }

        [[nodiscard]] bool analyze(std::string_view entry_rel)
        {
            auto result = session.analyze_entry(td.file(entry_rel), copts);
            return !result.has_errors && result.module != nullptr;
        }

        void sync() { index.sync(session); }
    };

    constexpr std::string_view kA = "module a;\npublic i32 from_a() { return 1; }\n";
    constexpr std::string_view kB = "module b;\nimport a;\npublic i32 from_b() { return a::from_a(); }\n";
    constexpr std::string_view kMain = "module main;\nimport b;\nvoid entry() { b::from_b(); }\n";

    constexpr std::string_view kAModified = "module a;\npublic i32 from_a() { return 1; }\npublic i32 from_a2() { return 2; }\n";

    constexpr std::string_view kAShifted = "module a;\npublic i32 helper() { return 0; }\npublic i32 from_a() { return 1; }\n";

    constexpr std::string_view kMainNoImports = "module main;\nvoid entry() {}\n";

    constexpr std::string_view kBReexport = "module b;\npublic import a;\npublic i32 from_b() { return a::from_a(); }\n";
    constexpr std::string_view kMainReexport = "module main;\nimport b;\nvoid entry() { i32 x = b::from_a(); b::from_b(); }\n";

    constexpr std::string_view kC = "module c;\npublic i32 from_c() { return 3; }\n";
    constexpr std::string_view kMainWithC = "module main;\nimport b;\nimport c;\nvoid entry() { b::from_b(); c::from_c(); }\n";

    constexpr std::string_view kExtraOriginal = "module extra;\npublic i32 old_name() { return 1; }\n";
    constexpr std::string_view kExtraRenamed = "module extra;\npublic i32 new_name_x() { return 2; }\n";
    constexpr std::string_view kMainImportsExtra = "module main;\nimport b;\nimport extra;\nvoid entry() { b::from_b(); extra::new_name_x(); }\n";

    [[nodiscard]] bool setup_initial(Workspace& w)
    {
        w.write("a.dc", kA);
        w.write("b.dc", kB);
        w.write("main.dc", kMain);
        return w.analyze("main.dc");
    }

    [[nodiscard]] bool setup_reexport(Workspace& w)
    {
        w.write("a.dc", kA);
        w.write("b.dc", kBReexport);
        w.write("main.dc", kMainReexport);
        return w.analyze("main.dc");
    }

    [[nodiscard]] bool setup_with_unrelated(Workspace& w)
    {
        w.write("a.dc", kA);
        w.write("b.dc", kB);
        w.write("c.dc", kC);
        w.write("main.dc", kMainWithC);
        return w.analyze("main.dc");
    }

    [[nodiscard]] bool same_range(dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) noexcept
    {
        return a.begin == b.begin && a.end == b.end;
    }

    [[nodiscard]] bool same_ranges(std::vector<dcc::sm::SourceRange> const& a, std::vector<dcc::sm::SourceRange> const& b) noexcept
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!same_range(a[i], b[i]))
                return false;
        return true;
    }

    [[nodiscard]] bool has_symbol(std::vector<dccd::protocol::SymbolInformation> const& infos, std::string_view name)
    {
        for (auto const& s : infos)
            if (s.name == name)
                return true;
        return false;
    }

} // namespace

SECTION("workspace index: sync and retention");

TEST_CASE("initial sync extracts every module record with fresh data")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.sync();

    auto const& st = w.index.stats();
    CHECK_EQ(st.sync_count, 1u);
    CHECK_EQ(st.modules_re_extracted, 3u);
    CHECK_EQ(st.modules_retained, 0u);
    CHECK_EQ(st.modules_dropped, 0u);
    CHECK_EQ(w.index.module_count(), 3u);

    auto const* a = w.index.module_record("a");
    REQUIRE(a != nullptr);
    CHECK_EQ(a->canonical_path, "a");
    CHECK_EQ(a->file_path, std::filesystem::weakly_canonical(w.td.file("a.dc")).string());
    CHECK_NE(a->file_id, dcc::sm::FileId::Invalid);
    CHECK(a->content_revision > 0u);
    CHECK_EQ(a->imports.size(), 0u);

    auto const* b = w.index.module_record("b");
    REQUIRE(b != nullptr);
    CHECK_EQ(b->imports.size(), 1u);
    CHECK_EQ(b->imports[0], "a");

    REQUIRE(w.index.module_record("main") != nullptr);
    CHECK_EQ(w.index.module_record("main")->imports.size(), 1u);
}

TEST_CASE("unchanged modules are retained across a second sync")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.sync();

    w.sync();

    auto const& st = w.index.stats();
    CHECK_EQ(st.sync_count, 2u);
    CHECK_EQ(st.modules_re_extracted, 3u);
    CHECK_EQ(st.modules_retained, 3u);
    CHECK_EQ(st.modules_dropped, 0u);
    CHECK_EQ(w.index.module_count(), 3u);

    auto const* a = w.index.module_record("a");
    REQUIRE(a != nullptr);
    CHECK_EQ(a->symbols.size(), 1u);
}

SECTION("workspace index: dependent invalidation and unrelated modules");

TEST_CASE("changing a module re-extracts its transitive importer closure")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.sync();

    w.sync();
    CHECK_EQ(w.index.stats().modules_retained, 3u);

    w.write("a.dc", kAModified);
    REQUIRE(w.refresh("a.dc"));
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    auto const& st = w.index.stats();
    CHECK_EQ(st.modules_re_extracted, 6u);
    CHECK_EQ(st.modules_retained, 3u);
    CHECK_EQ(st.modules_dropped, 0u);
    CHECK_EQ(w.index.module_count(), 3u);

    auto const* a = w.index.module_record("a");
    REQUIRE(a != nullptr);
    CHECK_EQ(a->symbols.size(), 2u);

    auto const* b = w.index.module_record("b");
    REQUIRE(b != nullptr);
    CHECK_EQ(b->symbols.size(), 1u);

    auto const* main = w.index.module_record("main");
    REQUIRE(main != nullptr);
    CHECK_EQ(main->symbols.size(), 1u);
    CHECK_EQ(main->occurrences.size(), 1u);
}

TEST_CASE("removed modules are dropped and the entry re-extracted")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.sync();

    w.write("main.dc", kMainNoImports);
    REQUIRE(w.refresh("main.dc"));
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    auto const& st = w.index.stats();
    CHECK_EQ(st.modules_dropped, 2u);
    CHECK_EQ(st.modules_re_extracted, 4u);
    CHECK_EQ(w.index.module_count(), 1u);

    REQUIRE(w.index.module_record("a") == nullptr);
    REQUIRE(w.index.module_record("b") == nullptr);
    REQUIRE(w.index.module_record("main") != nullptr);

    w.sync();
    CHECK_EQ(w.index.stats().modules_retained, 1u);
    CHECK_EQ(w.index.stats().modules_re_extracted, 4u);
    CHECK_EQ(w.index.stats().modules_dropped, 2u);
}

SECTION("workspace index: stable ids and lookups");

TEST_CASE("symbol ids and lookups stay stable across re-extraction")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.sync();

    auto id_a1 = w.index.symbol_id_for("a", "from_a");
    REQUIRE(id_a1.has_value());
    CHECK(id_a1->valid());

    auto id_b1 = w.index.symbol_id_for("b", "from_b");
    REQUIRE(id_b1.has_value());

    w.sync();
    CHECK_EQ(w.index.symbol_id_for("a", "from_a"), id_a1);
    CHECK_EQ(w.index.symbol_id_for("b", "from_b"), id_b1);

    auto decl_a = w.index.declaration_for(*id_a1);
    REQUIRE(decl_a.has_value());
    CHECK(decl_a->valid());
    CHECK_EQ(decl_a->begin.fileId, w.index.module_record("a")->file_id);
    CHECK(decl_a->begin.offset < decl_a->end.offset);

    auto occ_a = w.index.occurrences_for(*id_a1);
    CHECK_EQ(occ_a.size(), 1u);
    if (!occ_a.empty())
        CHECK_EQ(occ_a[0].begin.fileId, w.index.module_record("b")->file_id);

    auto occ_b = w.index.occurrences_for(*id_b1);
    CHECK_EQ(occ_b.size(), 1u);
    if (!occ_b.empty())
        CHECK_EQ(occ_b[0].begin.fileId, w.index.module_record("main")->file_id);

    w.write("a.dc", kAModified);
    REQUIRE(w.refresh("a.dc"));
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    auto id_a2 = w.index.symbol_id_for("a", "from_a");
    REQUIRE(id_a2.has_value());
    CHECK_EQ(*id_a2, *id_a1);
    CHECK_EQ(w.index.symbol_id_for("b", "from_b"), id_b1);

    auto occ_a2 = w.index.occurrences_for(*id_a2);
    CHECK_EQ(occ_a2.size(), 1u);
    if (!occ_a2.empty())
        CHECK_EQ(occ_a2[0].begin.fileId, w.index.module_record("b")->file_id);

    REQUIRE(w.index.symbol_id_for("a", "from_a2").has_value());
    CHECK(!w.index.symbol_id_for("a", "does_not_exist").has_value());

    auto decl_a2 = w.index.declaration_for(*id_a2);
    REQUIRE(decl_a2.has_value());
    CHECK(same_range(*decl_a2, *decl_a));
}

TEST_CASE("occurrence and declaration data survive a full arena reset")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.sync();

    auto id_a = w.index.symbol_id_for("a", "from_a");
    REQUIRE(id_a.has_value());
    auto decl_before = w.index.declaration_for(*id_a);
    REQUIRE(decl_before.has_value());
    auto occ_before = w.index.occurrences_for(*id_a);

    REQUIRE(w.analyze("main.dc"));
    w.sync();

    CHECK_EQ(w.index.module_count(), 3u);
    CHECK(decl_before.has_value());
    auto decl_after = w.index.declaration_for(*id_a);
    REQUIRE(decl_after.has_value());
    CHECK(same_range(*decl_after, *decl_before));

    auto occ_after = w.index.occurrences_for(*id_a);
    CHECK(same_ranges(occ_after, occ_before));
    CHECK_EQ(w.index.symbol_id_for("a", "from_a"), id_a);
}

TEST_CASE("modules unrelated to the change are retained")
{
    Workspace w;
    REQUIRE(setup_with_unrelated(w));
    w.sync();

    w.write("a.dc", kAModified);
    REQUIRE(w.refresh("a.dc"));
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    auto const& st = w.index.stats();
    CHECK_EQ(st.modules_re_extracted, 7u);
    CHECK_EQ(st.modules_retained, 1u);
    CHECK_EQ(st.modules_dropped, 0u);
    CHECK_EQ(w.index.module_count(), 4u);

    auto const* c = w.index.module_record("c");
    REQUIRE(c != nullptr);
    CHECK_EQ(c->symbols.size(), 1u);
    CHECK_EQ(c->occurrences.size(), 0u);
}

TEST_CASE("public re-export chain re-extracts all importers when a symbol offset shifts")
{
    Workspace w;
    REQUIRE(setup_reexport(w));
    w.sync();

    auto id_a1 = w.index.symbol_id_for("a", "from_a");
    REQUIRE(id_a1.has_value());

    auto occ1 = w.index.occurrences_for(*id_a1);
    CHECK_EQ(occ1.size(), 2u);

    w.write("a.dc", kAShifted);
    REQUIRE(w.refresh("a.dc"));
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    auto const& st = w.index.stats();
    CHECK_EQ(st.modules_re_extracted, 6u);
    CHECK_EQ(st.modules_retained, 0u);
    CHECK_EQ(w.index.module_count(), 3u);

    auto id_a2 = w.index.symbol_id_for("a", "from_a");
    REQUIRE(id_a2.has_value());
    CHECK_NE(*id_a2, *id_a1);

    CHECK_EQ(w.index.occurrences_for(*id_a1).size(), 0u);
    auto occ2 = w.index.occurrences_for(*id_a2);
    CHECK_EQ(occ2.size(), 2u);
}

SECTION("workspace index: unlinked projection lifecycle");

TEST_CASE("a file joining the live graph evicts its stale unlinked projection")
{
    Workspace w;
    REQUIRE(setup_initial(w));

    w.write("extra.dc", kExtraOriginal);
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    std::vector<std::filesystem::path> roots{w.td.path};
    CHECK(has_symbol(w.index.search_symbols(w.session, roots, "old_name"), "old_name"));
    CHECK_EQ(w.index.unlinked_count(), 1u);

    w.write("extra.dc", kExtraRenamed);
    w.write("main.dc", kMainImportsExtra);
    REQUIRE(w.refresh("extra.dc"));
    REQUIRE(w.refresh("main.dc"));
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    CHECK_EQ(w.index.unlinked_count(), 0u);
    REQUIRE(w.index.module_record("extra") != nullptr);

    auto res = w.index.search_symbols(w.session, roots, "old_name");
    CHECK(!has_symbol(res, "old_name"));
    CHECK(has_symbol(w.index.search_symbols(w.session, roots, "new_name_x"), "new_name_x"));
}

TEST_CASE("unlinked projections re-stat disk when the watcher missed a change")
{
    Workspace w;
    REQUIRE(setup_initial(w));

    w.write("extra.dc", kExtraOriginal);
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    std::vector<std::filesystem::path> roots{w.td.path};
    CHECK(has_symbol(w.index.search_symbols(w.session, roots, "old_name"), "old_name"));
    CHECK_EQ(w.index.stats().unlinked_parsed, 1u);

    w.write("extra.dc", kExtraRenamed);

    auto res = w.index.search_symbols(w.session, roots, "old_name");
    CHECK(!has_symbol(res, "old_name"));
    CHECK_EQ(w.index.stats().unlinked_parsed, 2u);
    CHECK(has_symbol(w.index.search_symbols(w.session, roots, "new_name_x"), "new_name_x"));
    CHECK_EQ(w.index.stats().unlinked_parsed, 2u);
    CHECK_EQ(w.index.stats().unlinked_reused, 1u);
}

SECTION("workspace index: invalidation, search and server fallback");

TEST_CASE("invalidate_module marks the index dirty so the next query re-syncs")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.sync();

    CHECK(w.index.module_record("a") != nullptr);
    w.index.invalidate_module("a");
    CHECK(w.index.module_record("a") == nullptr);

    auto res = w.index.search_symbols(w.session, {}, "from_a");
    CHECK(has_symbol(res, "from_a"));
    CHECK(w.index.module_record("a") != nullptr);
    CHECK(w.index.stats().sync_count >= 2u);
}

TEST_CASE("search_symbols serves module and unlinked symbols; repeated queries avoid re-parse")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.write("extra.dc", kExtraOriginal);
    REQUIRE(w.analyze("main.dc"));
    w.sync();

    std::vector<std::filesystem::path> roots{w.td.path};

    auto res1 = w.index.search_symbols(w.session, roots, "from_a");
    CHECK(has_symbol(res1, "from_a"));
    CHECK_EQ(w.index.stats().unlinked_parsed, 1u);
    CHECK_EQ(w.index.stats().workspace_queries_served_without_parse, 0u);

    auto res2 = w.index.search_symbols(w.session, roots, "from_a");
    CHECK(has_symbol(res2, "from_a"));
    CHECK_EQ(w.index.stats().unlinked_reused, 1u);
    CHECK_EQ(w.index.stats().workspace_queries_served_without_parse, 1u);

    auto res3 = w.index.search_symbols(w.session, roots, "");
    CHECK(has_symbol(res3, "from_a"));
    CHECK(has_symbol(res3, "from_b"));
    CHECK(has_symbol(res3, "old_name"));
    for (auto const& s : res3)
    {
        if (s.name == "from_a")
        {
            CHECK_EQ(s.kind, dccd::protocol::SymbolKind::Function);
            CHECK(s.location.uri.ends_with("a.dc"));
        }
    }
}

TEST_CASE("module_fresh drives the server occurrence fallback decision")
{
    Workspace w;
    REQUIRE(setup_initial(w));
    w.sync();

    auto const* a = w.index.module_record("a");
    REQUIRE(a != nullptr);
    auto fid = a->file_id;
    auto rev = a->content_revision;

    CHECK(w.index.module_fresh(fid, rev));
    CHECK(!w.index.module_fresh(fid, rev + 1));
    CHECK(!w.index.module_fresh(dcc::sm::FileId::Invalid, rev));

    w.write("a.dc", kAShifted);
    REQUIRE(w.refresh("a.dc"));
    CHECK(!w.index.module_fresh(fid, w.session.source_manager().content_revision(fid)));

    REQUIRE(w.analyze("main.dc"));
    w.sync();
    auto const* a2 = w.index.module_record("a");
    REQUIRE(a2 != nullptr);
    CHECK(w.index.module_fresh(a2->file_id, a2->content_revision));
}
