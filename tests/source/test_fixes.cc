import std;
import dcc.session;
import dcc.sm;
import dcc.diag;
import dcc.ast;
import dcc.lex;

#include "harness.hh"

namespace
{
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            auto base = std::filesystem::temp_directory_path();
            auto tag = std::format("dcc-fixes-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
            path = base / tag;
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write_file(std::string_view name, std::string_view content) const
        {
            std::ofstream out{path / name};
            out << content;
        }
    };

    [[nodiscard]] dcc::diag::Diagnostic const* find_diagnostic(dcc::diag::DiagnosticEngine const& engine, std::string_view message_substring)
    {
        for (auto const& d : engine.diagnostics())
            if (d.message().find(message_substring) != std::string::npos)
                return &d;
        return nullptr;
    }

    [[nodiscard]] dcc::ast::PostfixExpr const* find_question_postfix(dcc::ast::TranslationUnit const& tu)
    {
        for (auto const* d : tu.decls)
        {
            if (!d || d->kind != dcc::ast::DeclKind::Var)
                continue;

            auto const& vd = static_cast<dcc::ast::VarDecl const&>(*d);
            if (!vd.init || vd.init->kind != dcc::ast::ExprKind::Postfix)
                continue;

            auto const& p = static_cast<dcc::ast::PostfixExpr const&>(*vd.init);
            if (p.op == dcc::lex::TokenKind::Question)
                return &p;
        }
        return nullptr;
    }

} // namespace

SECTION("parser: missing-semicolon insertion fix");

TEST_CASE("parser attaches a zero-width ';' insertion fix at the structural location")
{
    std::string_view source = "module m;\nvoid f() {\n    i32 x = 0;\n    i32 y = 3;\n    defer x = 1 y = 2;\n}\n";

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto fid = session.open_in_memory("file:///fix_semi.dc", std::string{source});
    auto* tu = session.parse_file(fid);
    REQUIRE(tu != nullptr);

    auto const* diag = find_diagnostic(session.diagnostics(), "expected ';' after expression statement");
    REQUIRE(diag != nullptr);
    CHECK_EQ(diag->origin(), dcc::diag::DiagnosticOrigin::Parser);

    auto fixes = diag->fixes();
    REQUIRE(fixes.size() == 1);
    auto const& fix = fixes[0];

    CHECK_EQ(fix.message, "insert ';' after the expression statement");
    CHECK_EQ(fix.replacement, ";");

    CHECK_EQ(fix.range.begin.fileId, fid);
    CHECK_EQ(fix.range.end.fileId, fid);
    CHECK_EQ(fix.range.begin.offset, 66u);
    CHECK_EQ(fix.range.end.offset, 66u);

    REQUIRE(!diag->labels().empty());
    CHECK_EQ(diag->labels()[0].range.begin.offset, 67u);
}

SECTION("parser: structural postfix operator range");

TEST_CASE("parser records the exact '?' operator range on PostfixExpr")
{
    std::string_view source = "module m;\ni32 g = 42?;\n";

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto fid = session.open_in_memory("file:///fix_q.dc", std::string{source});
    auto* tu = session.parse_file(fid);
    REQUIRE(tu != nullptr);

    auto const* postfix = find_question_postfix(*tu);
    REQUIRE(postfix != nullptr);

    CHECK_EQ(postfix->op_range.begin.fileId, fid);
    CHECK_EQ(postfix->op_range.end.fileId, fid);
    CHECK_EQ(postfix->op_range.begin.offset, 20u);
    CHECK_EQ(postfix->op_range.end.offset, 21u);

    CHECK_EQ(postfix->range.begin.offset, 18u);
    CHECK_EQ(postfix->range.end.offset, 21u);
}

SECTION("sema: '?' outside a function deletion fix");

TEST_CASE("sema attaches a deletion fix for exactly the '?' token when used outside a function")
{
    TempDir td;
    td.write_file("main.dc", "module main;\ni32 g = 42?;\n");

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto result = session.analyze_entry(td.path / "main.dc", {});
    REQUIRE(result.has_errors);

    auto const* diag = find_diagnostic(session.diagnostics(), "cannot use `?` outside a function");
    REQUIRE(diag != nullptr);

    auto fixes = diag->fixes();
    REQUIRE(fixes.size() == 1);
    auto const& fix = fixes[0];

    CHECK_EQ(fix.message, "remove the `?` operator");
    CHECK_EQ(fix.replacement, "");

    CHECK_EQ(fix.range.begin.offset, 23u);
    CHECK_EQ(fix.range.end.offset, 24u);
    CHECK_EQ(fix.range.end.offset - fix.range.begin.offset, 1u);
}

TEST_CASE("sema keeps the existing void-return deletion fix")
{
    TempDir td;
    td.write_file("main.dc", "module main;\nvoid f() {\n    return 1;\n}\n");

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto result = session.analyze_entry(td.path / "main.dc", {});
    REQUIRE(result.has_errors);

    auto const* diag = find_diagnostic(session.diagnostics(), "void function cannot return a value");
    REQUIRE(diag != nullptr);

    auto fixes = diag->fixes();
    REQUIRE(fixes.size() == 1);
    auto const& fix = fixes[0];

    CHECK_EQ(fix.message, "remove the value from the return statement");
    CHECK_EQ(fix.replacement, "");

    CHECK_EQ(fix.range.begin.offset, 35u);
    CHECK_EQ(fix.range.end.offset, 36u);
}

SECTION("parser: asm placeholder structural raw-source ranges");

namespace
{
    [[nodiscard]] dcc::ast::AsmStmt const* find_asm_stmt(dcc::ast::TranslationUnit const& tu)
    {
        for (auto const* d : tu.decls)
        {
            if (!d || d->kind != dcc::ast::DeclKind::Func)
                continue;

            auto const& fd = static_cast<dcc::ast::FuncDecl const&>(*d);
            if (!fd.body.has_value())
                continue;

            for (auto* s : fd.body->stmts)
                if (s && s->kind == dcc::ast::StmtKind::Asm)
                    return static_cast<dcc::ast::AsmStmt const*>(s);
        }
        return nullptr;
    }

    [[nodiscard]] dcc::ast::TranslationUnit* parse_asm_source(dcc::session::CompilerSession& session, std::string_view source)
    {
        auto fid = session.open_in_memory("file:///asm_raw.dc", std::string{source});
        if (fid == dcc::sm::FileId::Invalid)
            return nullptr;

        return session.parse_file(fid);
    }

} // namespace

TEST_CASE("asm placeholder raw ranges shift past an escape before the placeholder")
{
    std::string_view source = "module main;\nvoid f() {\n    asm { \"nop \\n %[x]; mov %%eax, %[x]\" };\n}\n";

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto* tu = parse_asm_source(session, source);
    REQUIRE(tu != nullptr);

    auto const* stmt = find_asm_stmt(*tu);
    REQUIRE(stmt != nullptr);
    CHECK_EQ(stmt->template_range.begin.offset, 34u);
    CHECK_EQ(stmt->template_range.end.offset, 64u);
    REQUIRE(stmt->placeholder_spans.size() == 3u);

    auto const& op1 = stmt->placeholder_spans[0];
    CHECK_EQ(op1.kind, dcc::ast::AsmPlaceholderSpan::Kind::OperandRef);
    CHECK_EQ(op1.name, "x");

    CHECK_EQ(op1.byte_offset, 6u);
    CHECK_EQ(op1.byte_length, 4u);

    REQUIRE(op1.raw_range.valid());
    CHECK_EQ(op1.raw_range.begin.offset, 42u);
    CHECK_EQ(op1.raw_range.end.offset, 46u);

    auto const& reg = stmt->placeholder_spans[1];
    CHECK_EQ(reg.kind, dcc::ast::AsmPlaceholderSpan::Kind::RegLiteral);
    CHECK_EQ(reg.name, "eax");
    CHECK_EQ(reg.byte_offset, 16u);
    CHECK_EQ(reg.byte_length, 5u);

    REQUIRE(reg.raw_range.valid());
    CHECK_EQ(reg.raw_range.begin.offset, 52u);
    CHECK_EQ(reg.raw_range.end.offset, 57u);

    auto const& op2 = stmt->placeholder_spans[2];
    CHECK_EQ(op2.kind, dcc::ast::AsmPlaceholderSpan::Kind::OperandRef);
    CHECK_EQ(op2.name, "x");
    CHECK_EQ(op2.byte_offset, 23u);
    CHECK_EQ(op2.byte_length, 4u);

    REQUIRE(op2.raw_range.valid());
    CHECK_EQ(op2.raw_range.begin.offset, 59u);
    CHECK_EQ(op2.raw_range.end.offset, 63u);

    CHECK(op1.raw_range.end.offset <= op2.raw_range.begin.offset);
    CHECK(reg.raw_range.end.offset <= op2.raw_range.begin.offset);
    auto content_begin = stmt->template_range.begin.offset + 1u;
    auto content_end = stmt->template_range.end.offset - 1u;
    for (auto const& span : stmt->placeholder_spans)
    {
        CHECK(span.raw_range.begin.offset >= content_begin);
        CHECK(span.raw_range.end.offset <= content_end);
        CHECK(span.raw_range.begin.offset < span.raw_range.end.offset);
    }
}

TEST_CASE("asm placeholder whose percent comes from a unicode escape maps to the raw escape")
{
    std::string_view source = "module main;\nvoid f() {\n    asm { \"\\u{25}[y]\" };\n}\n";

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto* tu = parse_asm_source(session, source);
    REQUIRE(tu != nullptr);

    auto const* stmt = find_asm_stmt(*tu);
    REQUIRE(stmt != nullptr);
    REQUIRE(stmt->placeholder_spans.size() == 1u);

    auto const& span = stmt->placeholder_spans[0];
    CHECK_EQ(span.kind, dcc::ast::AsmPlaceholderSpan::Kind::OperandRef);
    CHECK_EQ(span.name, "y");
    CHECK_EQ(span.byte_offset, 0u);
    CHECK_EQ(span.byte_length, 4u);

    REQUIRE(span.raw_range.valid());
    CHECK_EQ(span.raw_range.begin.offset, 35u);
    CHECK_EQ(span.raw_range.end.offset, 44u);
}

TEST_CASE("asm placeholder raw range survives escaped quotes and backslashes before it")
{
    std::string_view source = "module main;\nvoid f() {\n    asm { \"\\\"\\\\ %[q]\" };\n}\n";

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto* tu = parse_asm_source(session, source);
    REQUIRE(tu != nullptr);

    auto const* stmt = find_asm_stmt(*tu);
    REQUIRE(stmt != nullptr);
    REQUIRE(stmt->placeholder_spans.size() == 1u);

    auto const& span = stmt->placeholder_spans[0];
    CHECK_EQ(span.name, "q");
    CHECK_EQ(span.byte_offset, 3u);
    CHECK_EQ(span.byte_length, 4u);

    REQUIRE(span.raw_range.valid());
    CHECK_EQ(span.raw_range.begin.offset, 40u);
    CHECK_EQ(span.raw_range.end.offset, 44u);
}

TEST_CASE("asm placeholder raw range maps past a multibyte unicode escape")
{
    std::string_view source = "module main;\nvoid f() {\n    asm { \"a\\u{2192}b %[w]\" };\n}\n";

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto* tu = parse_asm_source(session, source);
    REQUIRE(tu != nullptr);

    auto const* stmt = find_asm_stmt(*tu);
    REQUIRE(stmt != nullptr);
    REQUIRE(stmt->placeholder_spans.size() == 1u);

    auto const& span = stmt->placeholder_spans[0];
    CHECK_EQ(span.name, "w");
    CHECK_EQ(span.byte_offset, 6u);
    CHECK_EQ(span.byte_length, 4u);

    REQUIRE(span.raw_range.valid());
    CHECK_EQ(span.raw_range.begin.offset, 46u);
    CHECK_EQ(span.raw_range.end.offset, 50u);
}

TEST_CASE("sema places the undefined asm operand diagnostic on the raw placeholder range")
{
    TempDir td;
    td.write_file("main.dc", "module main;\nvoid f() {\n    asm { \"\\u{25}[missing]\" };\n}\n");

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    auto result = session.analyze_entry(td.path / "main.dc", {});
    REQUIRE(result.has_errors);

    auto const* diag = find_diagnostic(session.diagnostics(), "undefined asm operand `missing`");
    REQUIRE(diag != nullptr);

    REQUIRE(!diag->labels().empty());
    auto const& label = diag->labels()[0];
    CHECK_EQ(label.range.begin.offset, 35u);
    CHECK_EQ(label.range.end.offset, 50u);
    CHECK_EQ(label.range.end.offset - label.range.begin.offset, 15u);
}
