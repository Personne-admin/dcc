import std;
import dcc.session;
import dcc.sm;
import dcc.diag;
import dcc.ast;
import dcc.lex;
import dcc.si;
import dcc.ir;
import dcc.ir.lower;

#include "harness.hh"

namespace
{
    [[nodiscard]] dcc::diag::Diagnostic const* find_diag(dcc::diag::DiagnosticEngine const& engine, std::string_view sub)
    {
        for (auto const& d : engine.diagnostics())
            if (d.message().find(sub) != std::string::npos)
                return &d;
        return nullptr;
    }
    [[nodiscard]] bool has_diag(dcc::diag::DiagnosticEngine const& engine, std::string_view sub)
    {
        return find_diag(engine, sub) != nullptr;
    }
    [[nodiscard]] dcc::ast::TranslationUnit* parse_with_docs(dcc::session::CompilerSession& session, std::string_view source, std::string uri)
    {
        auto fid = session.open_in_memory(std::move(uri), std::string{source});
        if (fid == dcc::sm::FileId::Invalid)
            return nullptr;
        return session.parse_file(fid);
    }
    [[nodiscard]] dcc::ast::FuncDecl const* find_func(dcc::ast::TranslationUnit const& tu, std::string_view name)
    {
        for (auto const* d : tu.decls)
        {
            if (!d || d->kind != dcc::ast::DeclKind::Func)
                continue;
            auto const* f = static_cast<dcc::ast::FuncDecl const*>(d);
            if (f->name == name)
                return f;
        }
        return nullptr;
    }
} // namespace

SECTION("doc: lexing kinds");

TEST_CASE("each of the four kinds lexes to the correct token kind")
{
    dcc::sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///lex.dc", std::string{"//!! overview\n//! section\n/// doc\n// inlay\nvoid\n"}, 1);
    auto const* file = mgr.get(fid);
    dcc::si::string_interner interner;
    dcc::lex::Lexer lexer{*file, interner, true};
    auto t0 = lexer.next();
    CHECK(t0.kind == dcc::lex::TokenKind::DocOverview);
    auto t1 = lexer.next();
    CHECK(t1.kind == dcc::lex::TokenKind::DocSection);
    auto t2 = lexer.next();
    CHECK(t2.kind == dcc::lex::TokenKind::DocComment);
    auto t3 = lexer.next();
    CHECK(t3.kind == dcc::lex::TokenKind::InlayComment);
    auto t4 = lexer.next();
    CHECK(t4.kind == dcc::lex::TokenKind::KwVoid);
}

TEST_CASE("disambiguation overview section doc inlay plain")
{
    auto lex_first = [](std::string src) {
        dcc::sm::SourceManager mgr;
        auto fid = mgr.open_in_memory("file:///x.dc", std::move(src), 1);
        auto const* file = mgr.get(fid);
        dcc::si::string_interner interner;
        dcc::lex::Lexer lexer{*file, interner, true};
        return lexer.next().kind;
    };
    CHECK(lex_first("//!! hi\n") == dcc::lex::TokenKind::DocOverview);
    CHECK(lex_first("//! hi\n") == dcc::lex::TokenKind::DocSection);
    CHECK(lex_first("/// hi\n") == dcc::lex::TokenKind::DocComment);
    CHECK(lex_first("// hi\n") == dcc::lex::TokenKind::InlayComment);
    CHECK(lex_first("//!!! extra\n") == dcc::lex::TokenKind::DocOverview);
    {
        dcc::sm::SourceManager mgr;
        auto fid = mgr.open_in_memory("file:///p.dc", std::string{"//// banner\nvoid\n"}, 1);
        auto const* file = mgr.get(fid);
        dcc::si::string_interner interner;
        dcc::lex::Lexer lexer{*file, interner, true};
        CHECK(lexer.next().kind == dcc::lex::TokenKind::KwVoid);
    }
}

TEST_CASE("doc marker inside string is not a comment")
{
    dcc::sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///s.dc", std::string{"\"/// not comment\"\n"}, 1);
    auto const* file = mgr.get(fid);
    dcc::si::string_interner interner;
    dcc::lex::Lexer lexer{*file, interner, true};
    CHECK(lexer.next().kind == dcc::lex::TokenKind::StringLiteral);
}

TEST_CASE("markers with no space and several spaces")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\n///text\nvoid f() {}\n", "file:///a.dc");
    REQUIRE(tu != nullptr);
    auto const* f = find_func(*tu, "f");
    REQUIRE(f != nullptr);
    REQUIRE(tu->doc_for(f) != nullptr);
    CHECK_EQ(tu->doc_for(f)->text, "text");
    dcc::session::CompilerSession s2{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu2 = parse_with_docs(s2, "module m;\n///   spaced\nvoid g() {}\n", "file:///b.dc");
    REQUIRE(tu2 != nullptr);
    auto const* g = find_func(*tu2, "g");
    REQUIRE(g != nullptr);
    CHECK_EQ(tu2->doc_for(g)->text, "spaced");
}

TEST_CASE("crlf endings and eof mid-comment")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\r\n/// hello\r\nvoid f() {}\r\n", "file:///c.dc");
    REQUIRE(tu != nullptr);
    CHECK_EQ(tu->doc_for(find_func(*tu, "f"))->text, "hello");
    dcc::sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///e.dc", std::string{"/// doc without newline"}, 1);
    auto const* file = mgr.get(fid);
    dcc::si::string_interner interner;
    dcc::lex::Lexer lexer{*file, interner, true};
    CHECK(lexer.next().kind == dcc::lex::TokenKind::DocComment);
    CHECK(lexer.next().kind == dcc::lex::TokenKind::Eof);
}

SECTION("doc: attachment");

TEST_CASE("attaches to func struct enum union alias valuealias concept")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session,
                               "module m;\n/// f doc\nvoid f() {}\n/// s doc\nstruct S { bool x; }\n/// e doc\nenum E { A, }\n/// u doc\nunion U { bool x; "
                               "}\n/// a doc\nusing A = bool;\n",
                               "file:///k.dc");
    REQUIRE(tu != nullptr);
    CHECK(!has_diag(session.diagnostics(), "doc-orphan"));
    REQUIRE(tu->doc_for(find_func(*tu, "f")) != nullptr);
    CHECK_EQ(tu->doc_for(find_func(*tu, "f"))->text, "f doc");
}

TEST_CASE("blank line breaks attachment and warns")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\n/// doc\n\nvoid f() {}\n", "file:///bl.dc");
    REQUIRE(tu != nullptr);
    CHECK(tu->doc_for(find_func(*tu, "f")) == nullptr);
    CHECK(has_diag(session.diagnostics(), "doc-orphan"));
}

TEST_CASE("attributes between doc and decl still attach")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(
        session, "module m;\n/// d1\n@[deprecated(\"x\")]\nvoid f() {}\n/// d2\n@inline @noinline\nvoid g() {}\n/// d3\n@[inline, noinline]\nvoid h() {}\n",
        "file:///at.dc");
    REQUIRE(tu != nullptr);
    CHECK(!has_diag(session.diagnostics(), "doc-orphan"));
    CHECK_EQ(tu->doc_for(find_func(*tu, "f"))->text, "d1");
    CHECK_EQ(tu->doc_for(find_func(*tu, "g"))->text, "d2");
    CHECK_EQ(tu->doc_for(find_func(*tu, "h"))->text, "d3");
}

TEST_CASE("consecutive merge and empty content is content")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\n/// one\n///\n/// three\nvoid f() {}\n", "file:///mg.dc");
    REQUIRE(tu != nullptr);
    CHECK_EQ(tu->doc_for(find_func(*tu, "f"))->text, "one\n\nthree");
}

TEST_CASE("strict blank ends run discriminating pair")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\n/// a\n\n/// b\nvoid f() {}\n", "file:///st.dc");
    REQUIRE(tu != nullptr);
    CHECK_EQ(tu->doc_for(find_func(*tu, "f"))->text, "b");
    CHECK(has_diag(session.diagnostics(), "doc-orphan"));
}

TEST_CASE("inlay between docs ends run")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\n/// a\n// note\n/// b\nvoid f() {}\n", "file:///ib.dc");
    REQUIRE(tu != nullptr);
    CHECK_EQ(tu->doc_for(find_func(*tu, "f"))->text, "b");
    CHECK(has_diag(session.diagnostics(), "doc-orphan"));
    CHECK(!tu->inlays.empty());
}

TEST_CASE("docs on members params locals and orphans")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session,
                               "module m;\nstruct S {\n    /// fdoc\n    bool x;\n}\nunion U {\n    /// udoc\n    bool y;\n}\nenum E {\n    /// vdoc\n    "
                               "A,\n}\nvoid f(\n    /// pdoc\n    bool p) {\n    /// ldoc\n    bool q = true;\n}\n",
                               "file:///mem.dc");
    REQUIRE(tu != nullptr);
    for (auto const* d : tu->decls)
    {
        if (d->kind == dcc::ast::DeclKind::Struct)
        {
            auto const* s = static_cast<dcc::ast::StructDecl const*>(d);
            REQUIRE(!s->fields.empty());
            REQUIRE(s->fields[0].doc != nullptr);
            CHECK_EQ(s->fields[0].doc->text, "fdoc");
        }
        if (d->kind == dcc::ast::DeclKind::Func && static_cast<dcc::ast::FuncDecl const*>(d)->name == "f")
        {
            auto const* f = static_cast<dcc::ast::FuncDecl const*>(d);
            REQUIRE(!f->params.empty());
            CHECK_EQ(f->params[0].doc->text, "pdoc");
            bool found = false;
            for (auto const* st : f->body->stmts)
            {
                if (st->kind != dcc::ast::StmtKind::DeclStmt)
                    continue;
                auto const* ds = static_cast<dcc::ast::DeclStmt const*>(st);
                if (tu->doc_for(ds->decl) && tu->doc_for(ds->decl)->text == "ldoc")
                    found = true;
            }
            CHECK(found);
        }
    }
    {
        dcc::session::CompilerSession s2{{.silent_diagnostics = true, .enable_doc_comments = true}};
        auto* t2 = parse_with_docs(s2, "module m;\nvoid f() {}\n/// orphan\n", "file:///eof2.dc");
        REQUIRE(t2 != nullptr);
        CHECK(has_diag(s2.diagnostics(), "doc-orphan"));
    }
    {
        dcc::session::CompilerSession s3{{.silent_diagnostics = true, .enable_doc_comments = true}};
        auto* t3 = parse_with_docs(s3, "module m;\nstruct S {\n    /// orphan\n}\n", "file:///br.dc");
        REQUIRE(t3 != nullptr);
        CHECK(has_diag(s3.diagnostics(), "doc-orphan"));
    }
}

SECTION("doc: sections");

TEST_CASE("partition order and implicit unnamed")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\nvoid early() {}\n//! Alpha\nvoid a() {}\nvoid b() {}\n//! Beta\nvoid c() {}\n", "file:///sec.dc");
    REQUIRE(tu != nullptr);
    REQUIRE(tu->sections.size() >= 3);
    CHECK(tu->sections[0]->title.empty());
    CHECK_EQ(tu->sections[1]->title, "Alpha");
    CHECK_EQ(tu->sections[2]->title, "Beta");
    CHECK(tu->section_for(find_func(*tu, "early")) == tu->sections[0]);
    CHECK(tu->section_for(find_func(*tu, "a")) == tu->sections[1]);
    CHECK(tu->section_for(find_func(*tu, "c")) == tu->sections[2]);
}

TEST_CASE("title body split and blank-separated sections")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\n//! Title\n//! body one\n//! body two\nvoid f() {}\n", "file:///tb.dc");
    REQUIRE(tu != nullptr);
    CHECK_EQ(tu->sections[1]->title, "Title");
    CHECK(tu->sections[1]->body.find("body one") != std::string_view::npos);
    dcc::session::CompilerSession s2{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* t2 = parse_with_docs(s2, "module m;\n//! First\n\n//! Second\nvoid f() {}\n", "file:///2s.dc");
    REQUIRE(t2 != nullptr);
    CHECK_EQ(t2->sections[1]->title, "First");
    CHECK_EQ(t2->sections[2]->title, "Second");
    CHECK(t2->sections[1]->decls.empty());
}

TEST_CASE("nested section errors and no-section file")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\nvoid f() {\n    //! nested\n}\n", "file:///ne.dc");
    REQUIRE(tu != nullptr);
    CHECK(has_diag(session.diagnostics(), "doc-section-nested"));
    dcc::session::CompilerSession s2{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* t2 = parse_with_docs(s2, "module m;\nvoid f() {}\nvoid g() {}\n", "file:///ns.dc");
    REQUIRE(t2 != nullptr);
    CHECK(t2->sections.size() == 1);
}

SECTION("doc: overview");

TEST_CASE("overview before module captured after errors duplicate errors")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto fid = session.open_in_memory("file:///ov.dc", std::string{"//!! Over line\nmodule m;\nvoid f() {}\n"});
    auto* tu = session.parse_file(fid);
    REQUIRE(tu != nullptr);
    REQUIRE(tu->overview_of() != nullptr);
    CHECK(!has_diag(session.diagnostics(), "doc-overview-misplaced"));
    dcc::session::CompilerSession s2{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* t2 = parse_with_docs(s2, "module m;\n//!! late\nvoid f() {}\n", "file:///lo.dc");
    REQUIRE(t2 != nullptr);
    CHECK(has_diag(s2.diagnostics(), "doc-overview-misplaced"));
    dcc::session::CompilerSession s3{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto fid3 = s3.open_in_memory("file:///du.dc", std::string{"//!! a\n\n//!! b\nmodule m;\n"});
    auto* t3 = s3.parse_file(fid3);
    REQUIRE(t3 != nullptr);
    CHECK(has_diag(s3.diagnostics(), "doc-overview-duplicate"));
}

TEST_CASE("overview no-sections and vice versa")
{
    dcc::session::CompilerSession s1{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto fid = s1.open_in_memory("file:///o.dc", std::string{"//!! over\nmodule m;\nvoid f() {}\n"});
    auto* t1 = s1.parse_file(fid);
    REQUIRE(t1 != nullptr);
    CHECK(t1->overview_of() != nullptr);
    CHECK(t1->sections.size() == 1);
    dcc::session::CompilerSession s2{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* t2 = parse_with_docs(s2, "module m;\n//! Sec\nvoid f() {}\n", "file:///sv.dc");
    REQUIRE(t2 != nullptr);
    CHECK(t2->overview_of() == nullptr);
    CHECK(t2->sections.size() >= 2);
}

SECTION("doc: inlay");

TEST_CASE("trailing standalone first-line and both")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\nvoid f() {} // trailing\n", "file:///tr.dc");
    REQUIRE(tu != nullptr);
    REQUIRE(!tu->inlays.empty());
    REQUIRE(tu->inlay_for(find_func(*tu, "f")) != nullptr);
    dcc::session::CompilerSession s2{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* t2 = parse_with_docs(s2, "// first\nmodule m;\nvoid f() {}\n", "file:///fi.dc");
    REQUIRE(t2 != nullptr);
    REQUIRE(!t2->inlays.empty());
    CHECK(!t2->inlays[0]->has_owner);
    dcc::session::CompilerSession s3{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* t3 = parse_with_docs(s3, "module m;\n/// doc\nvoid f() {} // trail\n", "file:///bo.dc");
    REQUIRE(t3 != nullptr);
    REQUIRE(t3->doc_for(find_func(*t3, "f")) != nullptr);
    REQUIRE(t3->inlay_for(find_func(*t3, "f")) != nullptr);
}

SECTION("doc: normalization");

TEST_CASE("indent stripped and fence survives")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\n///     indented\n///       more\n///     back\nvoid f() {}\n", "file:///in.dc");
    REQUIRE(tu != nullptr);
    CHECK_EQ(tu->doc_for(find_func(*tu, "f"))->text, "indented\n  more\nback");
}

TEST_CASE("fence survives")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
    auto* tu = parse_with_docs(session, "module m;\n/// Ex:\n/// ```\n/// code one\n///\n/// code two\n/// ```\nvoid f() {}\n", "file:///fe.dc");
    REQUIRE(tu != nullptr);
    auto const* doc = tu->doc_for(find_func(*tu, "f"));
    REQUIRE(doc != nullptr);
    CHECK(doc->text.find("code one\n\ncode two") != std::string_view::npos);
}

SECTION("doc: gating and regression");

TEST_CASE("silent by default and IR identical")
{
    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto* tu = parse_with_docs(session, "module m;\n/// orphan\n\nvoid f() {}\n", "file:///si.dc");
    REQUIRE(tu != nullptr);
    CHECK(!has_diag(session.diagnostics(), "doc-orphan"));
    std::string with_docs = "module main;\n/// Doc\npublic void f() {}\n//! Sec\n/// G\npublic void g() { f(); }\n";
    std::string stripped = "module main;\npublic void f() {}\npublic void g() { f(); }\n";
    dcc::session::CompilerSession s1{{.silent_diagnostics = true}};
    auto fid1 = s1.open_in_memory("file:///a.dc", with_docs);
    auto* t1 = s1.parse_file(fid1);
    REQUIRE(t1 != nullptr);
    CHECK(!s1.diagnostics().has_errors());
    dcc::session::CompilerSession s2{{.silent_diagnostics = true}};
    auto fid2 = s2.open_in_memory("file:///b.dc", stripped);
    auto* t2 = s2.parse_file(fid2);
    REQUIRE(t2 != nullptr);
    CHECK_EQ(t1->decls.size(), t2->decls.size());
}
