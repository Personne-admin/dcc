import std;
import dcc.ast;
import dcc.comptime;
import dcc.ctfe;
import dcc.diag;
import dcc.ir.lower;
import dcc.lex;
import dcc.parser;
import dcc.sema;
import dcc.si;
import dcc.sm;
import dcc.types;

#include "harness.hh"

namespace ast = dcc::ast;
namespace comptime = dcc::comptime;
namespace ctfe = dcc::ctfe;
namespace diag = dcc::diag;
namespace lower = dcc::ir::lower;
namespace lex = dcc::lex;
namespace parser = dcc::parser;
namespace sema = dcc::sema;
namespace si = dcc::si;
namespace sm = dcc::sm;
namespace types = dcc::types;

namespace
{

struct Fixture
{
    sm::SourceManager sm;
    std::ostringstream diags;
    diag::DiagnosticEngine engine;
    ast::AstContext ast_ctx;
    si::string_interner interner;
    std::filesystem::path dir;
    std::unique_ptr<sema::SemaContext> sema;
    sema::ModuleInfo* mod = nullptr;

    explicit Fixture(std::string_view src) : engine(sm, diags)
    {
        static int counter = 1000;
        ++counter;
        dir = std::filesystem::temp_directory_path() / ("dcc-resid-" + std::to_string(counter));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out{dir / "test.dc"};
        out << "module test;\n" << src;
        out.close();

        auto parse_fn = [this](sm::FileId fid, ast::AstContext& ctx, diag::DiagnosticEngine& d) -> ast::TranslationUnit* {
            auto const* file = sm.get(fid);
            if (!file)
                return nullptr;
            lex::Lexer lexer{*file, interner};
            parser::Parser p{lexer, ctx, d, parser::ParseMode::Batch};
            return p.parse();
        };

        sema::SemaOptions opts;
        opts.import_roots.push_back(dir);
        opts.interner = &interner;
        sema = std::make_unique<sema::SemaContext>(sm, engine, ast_ctx, std::move(parse_fn), std::move(opts));
        mod = sema->analyze_entry(dir / "test.dc");
    }

    ~Fixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    bool ok() const
    {
        if (!mod)
            return false;
        for (auto const& d : engine.diagnostics())
            if (d.severity() == diag::Severity::Error)
                return false;
        return true;
    }

    ast::FuncDecl const* find(std::string_view name) const
    {
        if (!mod || !mod->tu)
            return nullptr;
        for (auto* d : mod->tu->decls)
        {
            auto* f = ast::node_cast<ast::FuncDecl>(d);
            if (f && f->name == name)
                return f;
        }
        return nullptr;
    }

    types::TypePtr param_type(ast::FuncDecl const* fn, std::size_t i) const
    {
        if (!fn || i >= fn->params.size() || !fn->params[i].type)
            return nullptr;
        return reinterpret_cast<types::TypePtr>(fn->params[i].type->sema.canonical);
    }
};

struct SpecResult
{
    ctfe::Result result;
    ctfe::Trace trace;
};

SpecResult run_specialize(Fixture& fx, ast::FuncDecl const* fn, std::vector<comptime::Value> args)
{
    SpecResult out;
    ctfe::Context ctx;
    ctx.types = &fx.sema->types();
    ctx.source_manager = &fx.sm;
    ctfe::Evaluator ev(std::move(ctx), ctfe::Mode::Specialize);
    out.result = ev.specialize(*fn, std::move(args));
    out.trace = ev.trace();
    return out;
}

constexpr std::string_view kTaintPrelude = R"dc(
enum FErr : u8 {
    Bad,
}

enum R {
    Ok(i32),
    @implicit_construction Err(FErr),
}

bool is_ok(const R* self) {
    return match *self {
        R::Ok(_) => true,
        _ => false,
    };
}

i32 unwrap(const R* self) {
    return match *self {
        R::Ok(v) => v,
        _ => 0,
    };
}

FErr unwrap_err(const R* self) {
    return match *self {
        R::Err(e) => e,
        _ => FErr::Bad,
    };
}

R wrap(R r) {
    i32 v = r?;
    return R::Ok(42);
}

struct Pair {
    i32 a;
    i32 b;
}

Pair mkpair(R r) {
    R w = wrap(r);
    return Pair { a = 1, b = 2 };
}

enum Align : u8 {
    Left,
    Right,
}

Align mkalign(R r) {
    R w = wrap(r);
    return Align::Left;
}
)dc";

std::string with_taint_prelude(std::string_view body)
{
    std::string out{kTaintPrelude};
    out += body;
    return out;
}

SECTION("residual: tainted observers abandon");

TEST_CASE("match on tainted result abandons")
{
    Fixture fx(with_taint_prelude(R"dc(
i32 use(R r) {
    R w = wrap(r);
    return match w {
        R::Ok(x) => x,
        _ => 0,
    };
}
)dc"));
    REQUIRE(fx.ok());
    auto const* fn = fx.find("use");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
    CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnsupportedEffect);
}

TEST_CASE("equality on tainted value abandons")
{
    Fixture fx(with_taint_prelude(R"dc(
bool cmp(R r) {
    Pair p = mkpair(r);
    return p.a == 1;
}
)dc"));
    REQUIRE(fx.ok());
    auto const* fn = fx.find("cmp");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
    CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnsupportedEffect);
}

TEST_CASE("discard of tainted result abandons")
{
    Fixture fx(with_taint_prelude(R"dc(
void drop(R r) {
    wrap(r);
}
)dc"));
    REQUIRE(fx.ok());
    auto const* fn = fx.find("drop");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
    CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnsupportedEffect);
}

TEST_CASE("tainted argument to residual call abandons")
{
    Fixture fx(with_taint_prelude(R"dc(
extern void sink(R r);

void feed(R r) {
    R w = wrap(r);
    sink(w);
}
)dc"));
    REQUIRE(fx.ok());
    auto const* fn = fx.find("feed");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
    CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnsupportedEffect);
}

TEST_CASE("adjust on unknown abandons")
{
    Fixture fx(R"dc(
void bump(i32 x) {
    x++;
}
)dc");
    REQUIRE(fx.ok());
    auto const* fn = fx.find("bump");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
    CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnsupportedEffect);
}

SECTION("residual: money shapes proceed");

TEST_CASE("branch on tainted bool proceeds")
{
    Fixture fx(with_taint_prelude(R"dc(
i32 pick(R r) {
    Pair p = mkpair(r);
    if p.a < 10 {
        return 1;
    }
    return 2;
}
)dc"));
    REQUIRE(fx.ok());
    auto const* fn = fx.find("pick");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
    REQUIRE(spec.result.value.has_value());
    CHECK_EQ(spec.result.value->get_int(), 1);
}

TEST_CASE("match on tainted plain enum proceeds")
{
    Fixture fx(with_taint_prelude(R"dc(
i32 usealign(R r) {
    Align a = mkalign(r);
    return match a {
        Align::Left => 1,
        _ => 2,
    };
}
)dc"));
    REQUIRE(fx.ok());
    auto const* fn = fx.find("usealign");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
    REQUIRE(spec.result.value.has_value());
    CHECK_EQ(spec.result.value->get_int(), 1);
}

SECTION("residual: trace validation");

constexpr std::string_view kExternSnippet = R"dc(
extern i32 ext(i32 x);

i32 f(i32 x) {
    return ext(x) + 1;
}
)dc";

ctfe::Trace extern_trace(int arg)
{
    Fixture fx(kExternSnippet);
    auto const* fn = fx.find("f");
    auto t = fx.param_type(fn, 0);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_int(arg, t));
    return run_specialize(fx, fn, std::move(args)).trace;
}

TEST_CASE("residual call trace validates")
{
    auto trace = extern_trace(41);
    auto always = [](ast::Decl const*) { return true; };
    auto check = lower::Lowerer::validate_residual_trace(trace, always, true, 1);
    CHECK(check.ok);
}

TEST_CASE("truncated children fail validation")
{
    auto trace = extern_trace(41);
    for (auto& node : trace.nodes)
    {
        if (node.kind == ctfe::Residual::Kind::Emit && node.node && node.node->kind == ast::ExprKind::Binary && node.children.size() == 2u)
        {
            node.children.pop_back();
            break;
        }
    }
    auto always = [](ast::Decl const*) { return true; };
    auto check = lower::Lowerer::validate_residual_trace(trace, always, true, 1);
    CHECK(!check.ok);
}

TEST_CASE("forward references fail validation")
{
    auto trace = extern_trace(41);
    for (auto& node : trace.nodes)
    {
        if (node.kind == ctfe::Residual::Kind::Emit && node.children.size() >= 2u)
        {
            node.children.front() = trace.nodes.size() - 1;
            break;
        }
    }
    auto always = [](ast::Decl const*) { return true; };
    auto check = lower::Lowerer::validate_residual_trace(trace, always, true, 1);
    CHECK(!check.ok);
}

TEST_CASE("pointer values fail validation")
{
    Fixture fx(R"dc(
extern void sink(const i32* p);

void feed() {
    i32 x = 5;
    sink(&x);
}
)dc");
    REQUIRE(fx.ok());
    auto const* fn = fx.find("feed");
    REQUIRE(fn != nullptr);
    std::vector<comptime::Value> args;
    auto spec = run_specialize(fx, fn, std::move(args));
    REQUIRE(spec.result.flow == ctfe::Flow::Normal);
    auto always = [](ast::Decl const*) { return true; };
    auto check = lower::Lowerer::validate_residual_trace(spec.trace, always, true, 1);
    CHECK(!check.ok);
}

TEST_CASE("nested unknowns fail validation")
{
    Fixture fx(with_taint_prelude(R"dc(
extern void sinkp(Pair p);

R feedp(R r) {
    i32 v = r?;
    sinkp(Pair { a = v, b = 2 });
    return R::Ok(0);
}
)dc"));
    REQUIRE(fx.ok());
    auto const* fn = fx.find("feedp");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    REQUIRE(spec.result.flow == ctfe::Flow::Normal);
    auto always = [](ast::Decl const*) { return true; };
    auto check = lower::Lowerer::validate_residual_trace(spec.trace, always, true, 1);
    CHECK(!check.ok);
}

TEST_CASE("adjust nodes fail validation")
{
    Fixture fx(R"dc(
void bump(i32 x) {
    x++;
}
)dc");
    REQUIRE(fx.ok());
    auto const* fn = fx.find("bump");
    REQUIRE(fn != nullptr);
    REQUIRE(fn->body.has_value());
    REQUIRE(!fn->body->stmts.empty());
    auto const* stmt = ast::node_cast<ast::ExprStmt>(fn->body->stmts.front());
    REQUIRE(stmt != nullptr);
    auto const* post = ast::node_cast<ast::PostfixExpr>(stmt->expr);
    REQUIRE(post != nullptr);
    ctfe::Trace trace;
    ctfe::Residual seq;
    seq.kind = ctfe::Residual::Kind::Seq;
    trace.nodes.push_back(std::move(seq));
    ctfe::Residual emit;
    emit.kind = ctfe::Residual::Kind::Emit;
    emit.node = post;
    trace.nodes.push_back(std::move(emit));
    auto always = [](ast::Decl const*) { return true; };
    auto check = lower::Lowerer::validate_residual_trace(trace, always, true, 1);
    CHECK(!check.ok);
}

TEST_CASE("unresolvable reads fail validation")
{
    Fixture fx(R"dc(
volatile i32 vg;

i32 f() {
    return vg + 1;
}
)dc");
    REQUIRE(fx.ok());
    auto const* fn = fx.find("f");
    REQUIRE(fn != nullptr);
    std::vector<comptime::Value> args;
    auto spec = run_specialize(fx, fn, std::move(args));
    REQUIRE(spec.result.flow == ctfe::Flow::Normal);
    auto never = [](ast::Decl const*) { return false; };
    auto denied = lower::Lowerer::validate_residual_trace(spec.trace, never, true, 1);
    CHECK(!denied.ok);
    auto always = [](ast::Decl const*) { return true; };
    auto allowed = lower::Lowerer::validate_residual_trace(spec.trace, always, true, 1);
    CHECK(allowed.ok);
}

TEST_CASE("question outside functions fails validation")
{
    Fixture fx(with_taint_prelude(R"dc(
R pass(R r) {
    i32 v = r?;
    return R::Ok(v);
}
)dc"));
    REQUIRE(fx.ok());
    auto const* fn = fx.find("pass");
    REQUIRE(fn != nullptr);
    auto t = fx.param_type(fn, 0);
    REQUIRE(t != nullptr);
    std::vector<comptime::Value> args;
    args.push_back(comptime::Value::make_unknown(t, 0));
    auto spec = run_specialize(fx, fn, std::move(args));
    REQUIRE(spec.result.flow == ctfe::Flow::Normal);
    auto always = [](ast::Decl const*) { return true; };
    auto check = lower::Lowerer::validate_residual_trace(spec.trace, always, false, 1);
    CHECK(!check.ok);
}

SECTION("residual: call mapping");

TEST_CASE("direct call maps positionally")
{
    auto mapping = lower::Lowerer::resolve_call_emit_mapping(false, false, 0, 2, 2);
    REQUIRE(mapping.has_value());
    CHECK(!mapping->has_leading);
    CHECK_EQ(mapping->arg_offset, 0u);
    CHECK_EQ(mapping->arg_count, 2u);
}

TEST_CASE("indirect call maps callee first")
{
    auto mapping = lower::Lowerer::resolve_call_emit_mapping(false, true, 0, 2, 3);
    REQUIRE(mapping.has_value());
    CHECK(mapping->has_leading);
    CHECK_EQ(mapping->arg_offset, 0u);
    CHECK_EQ(mapping->arg_count, 2u);
}

TEST_CASE("ufcs call maps receiver first")
{
    auto mapping = lower::Lowerer::resolve_call_emit_mapping(true, false, 0, 2, 3);
    REQUIRE(mapping.has_value());
    CHECK(mapping->has_leading);
    CHECK_EQ(mapping->arg_offset, 0u);
    CHECK_EQ(mapping->arg_count, 2u);
}

TEST_CASE("count mismatch maps to nothing")
{
    auto mapping = lower::Lowerer::resolve_call_emit_mapping(false, false, 0, 2, 3);
    CHECK(!mapping.has_value());
}

TEST_CASE("offset calls map from offset")
{
    auto mapping = lower::Lowerer::resolve_call_emit_mapping(false, false, 1, 3, 2);
    REQUIRE(mapping.has_value());
    CHECK(!mapping->has_leading);
    CHECK_EQ(mapping->arg_offset, 1u);
    CHECK_EQ(mapping->arg_count, 2u);
}

} // namespace
