import std;
import dcc.ast;
import dcc.comptime;
import dcc.ctfe;
import dcc.diag;
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
            static int counter = 0;
            ++counter;
            dir = std::filesystem::temp_directory_path() / ("dcc-spec-" + std::to_string(counter));
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

    SpecResult run_specialize(Fixture& fx, ast::FuncDecl const* fn, std::vector<comptime::Value> args, std::function<void(ctfe::Context&)> tweak = {})
    {
        SpecResult out;
        ctfe::Context ctx;
        ctx.types = &fx.sema->types();
        ctx.source_manager = &fx.sm;
        if (tweak)
            tweak(ctx);
        ctfe::Evaluator ev(std::move(ctx), ctfe::Mode::Specialize);
        out.result = ev.specialize(*fn, std::move(args));
        out.trace = ev.trace();
        return out;
    }

    std::size_t count_kind(ctfe::Trace const& trace, ctfe::Residual::Kind kind)
    {
        std::size_t n = 0;
        for (auto const& node : trace.nodes)
            if (node.kind == kind)
                ++n;
        return n;
    }

    std::vector<std::size_t> emit_indices(ctfe::Trace const& trace)
    {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < trace.nodes.size(); ++i)
            if (trace.nodes[i].kind == ctfe::Residual::Kind::Emit)
                out.push_back(i);
        return out;
    }

    std::vector<std::size_t> value_indices_with_int(ctfe::Trace const& trace, std::int64_t v)
    {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < trace.nodes.size(); ++i)
        {
            auto const& node = trace.nodes[i];
            if (node.kind == ctfe::Residual::Kind::Value && node.value.kind() == comptime::Value::Kind::Int && node.value.get_int() == v)
                out.push_back(i);
        }
        return out;
    }

    SECTION("specialize: modes and reads");

    TEST_CASE("fully known calls fold with value-only trace")
    {
        Fixture fx(R"dc(
i32 add1(i32 x) {
    return x + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("add1");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(41, t));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(!spec.result.value->is_unknown());
        CHECK_EQ(spec.result.value->get_int(), 42);
        REQUIRE(!spec.trace.nodes.empty());
        CHECK_EQ(spec.trace.nodes.front().kind, ctfe::Residual::Kind::Seq);
        CHECK_EQ(count_kind(spec.trace, ctfe::Residual::Kind::Emit), 0u);
    }

    TEST_CASE("reads of runtime storage introduce unknowns")
    {
        Fixture fx(R"dc(
i32 g;

i32 f() {
    return g + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        std::vector<comptime::Value> args;
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
        CHECK(!emit_indices(spec.trace).empty());
    }

    TEST_CASE("required mode still fails the same read")
    {
        Fixture fx(R"dc(
i32 g;

i32 f() {
    return g + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        REQUIRE(fn->body.has_value());
        REQUIRE(!fn->body->stmts.empty());
        auto const* ret = ast::node_cast<ast::ReturnStmt>(fn->body->stmts.front());
        REQUIRE(ret != nullptr);
        REQUIRE(ret->value != nullptr);
        ctfe::Context ctx;
        ctx.types = &fx.sema->types();
        ctx.source_manager = &fx.sm;
        ctfe::Evaluator ev(std::move(ctx), ctfe::Mode::Required);
        auto r = ev.evaluate(*ret->value);
        CHECK(r.failed());
        CHECK(r.message == "read of non-constant storage");
    }

    SECTION("specialize: unknown introduction");

    TEST_CASE("unknown parameter poisons arithmetic with origin")
    {
        Fixture fx(R"dc(
i32 add1(i32 x) {
    return x + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("add1");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
        auto emits = emit_indices(spec.trace);
        CHECK_EQ(emits.size(), 1u);
        REQUIRE(!emits.empty());
        CHECK_EQ(spec.result.value->unknown_origin(), emits.front());
        auto const& emit = spec.trace.nodes[emits.front()];
        REQUIRE(emit.node != nullptr);
        CHECK_EQ(emit.node->kind, ast::ExprKind::Binary);
        REQUIRE(emit.children.size() == 2u);
        CHECK_EQ(emit.children.front(), 0u);
        auto const& rhs = spec.trace.nodes[emit.children.back()];
        CHECK_EQ(rhs.kind, ctfe::Residual::Kind::Value);
        CHECK_EQ(rhs.value.get_int(), 1);
    }

    TEST_CASE("indirect calls residualize and poison locals")
    {
        Fixture fx(R"dc(
using Fn = i32(*)(i32 x);

i32 apply(Fn f, i32 x) {
    i32 k = 7;
    f(x);
    return k + 0;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("apply");
        REQUIRE(fn != nullptr);
        auto ft = fx.param_type(fn, 0);
        auto xt = fx.param_type(fn, 1);
        REQUIRE(ft != nullptr);
        REQUIRE(xt != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(ft, 0));
        args.push_back(comptime::Value::make_int(3, xt));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(!spec.result.value->is_unknown());
        CHECK_EQ(spec.result.value->get_int(), 7);
        auto emits = emit_indices(spec.trace);
        CHECK(!emits.empty());
        bool saw_call = false;
        for (auto i : emits)
            if (spec.trace.nodes[i].node && spec.trace.nodes[i].node->kind == ast::ExprKind::Call)
                saw_call = true;
        CHECK(saw_call);
    }

    TEST_CASE("extern calls residualize with nested emits")
    {
        Fixture fx(R"dc(
extern i32 ext_fn(i32 x);

i32 f(i32 x) {
    return ext_fn(x) + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(41, t));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
        auto emits = emit_indices(spec.trace);
        CHECK(emits.size() >= 2u);
        REQUIRE(emits.size() >= 2u);
        CHECK(emits.front() < emits.back());
        CHECK_EQ(spec.result.value->unknown_origin(), emits.back());
    }

    TEST_CASE("volatile reads introduce unknowns")
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
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
        CHECK(!emit_indices(spec.trace).empty());
    }

    TEST_CASE("volatile stores do not introduce")
    {
        Fixture fx(R"dc(
volatile i32 vg;

void set_vol(i32 x) {
    vg = x;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("set_vol");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(1, t));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::NotEvaluatable);
    }

    SECTION("specialize: question gate");

    TEST_CASE("question on unknown continues with unknown payload")
    {
        Fixture fx(R"dc(
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

R pass(R r) {
    i32 v = r?;
    return R::Ok(v);
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("pass");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK_EQ(spec.result.value->kind(), comptime::Value::Kind::Aggregate);
        REQUIRE(spec.result.value->size() == 2u);
        CHECK(spec.result.value->at(1).is_unknown());
        auto emits = emit_indices(spec.trace);
        CHECK_EQ(emits.size(), 1u);
        REQUIRE(!emits.empty());
        REQUIRE(spec.trace.nodes[emits.front()].node != nullptr);
        CHECK_EQ(spec.trace.nodes[emits.front()].node->kind, ast::ExprKind::Postfix);
    }

    TEST_CASE("folds after the check stay ordered")
    {
        Fixture fx(R"dc(
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

R after(R r) {
    i32 v = r?;
    i32 w = 40 + 2;
    return R::Ok(w + (v - v));
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("after");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK_EQ(spec.result.value->kind(), comptime::Value::Kind::Aggregate);
        REQUIRE(spec.result.value->size() == 2u);
        CHECK(spec.result.value->at(1).is_unknown());
        auto questions = emit_indices(spec.trace);
        auto folds = value_indices_with_int(spec.trace, 42);
        REQUIRE(!questions.empty());
        REQUIRE(!folds.empty());
        CHECK(questions.front() < folds.front());
    }

    SECTION("specialize: truth gates abandon");

    TEST_CASE("unknown if abandons")
    {
        Fixture fx(R"dc(
i32 f(bool c) {
    return if c { 1 } else { 2 };
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnknownCondition);
    }

    TEST_CASE("unknown match abandons")
    {
        Fixture fx(R"dc(
i32 f(i32 x) {
    return match x { 1 => 10, _ => 20 };
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnknownCondition);
    }

    TEST_CASE("unknown loop abandons")
    {
        Fixture fx(R"dc(
i32 f(i32 n) {
    while n > 0 {
        n = n - 1;
    }
    return n;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnknownCondition);
    }

    TEST_CASE("unknown and abandons")
    {
        Fixture fx(R"dc(
bool andb(bool a, bool b) {
    return a && b;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("andb");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        auto u = fx.param_type(fn, 1);
        REQUIRE(t != nullptr);
        REQUIRE(u != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        args.push_back(comptime::Value::make_bool(false, u));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnknownCondition);
    }

    TEST_CASE("unknown guard abandons")
    {
        Fixture fx(R"dc(
i32 f(i32 x, i32 z) {
    return match x { y if y > z => 1, _ => 2 };
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        auto u = fx.param_type(fn, 1);
        REQUIRE(t != nullptr);
        REQUIRE(u != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(5, t));
        args.push_back(comptime::Value::make_unknown(u, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnknownCondition);
    }

    TEST_CASE("unknown index abandons")
    {
        Fixture fx(R"dc(
u8 f([] const u8 s, i32 i) {
    return s[i];
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        auto u = fx.param_type(fn, 1);
        REQUIRE(t != nullptr);
        REQUIRE(u != nullptr);
        std::vector<comptime::Value> elems;
        auto et = fx.sema->types().int_t(8, false);
        elems.push_back(comptime::Value::make_int(10, et));
        elems.push_back(comptime::Value::make_int(20, et));
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_slice(std::move(elems), t));
        args.push_back(comptime::Value::make_unknown(u, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnknownIndex);
    }

    TEST_CASE("unknown base does not abandon")
    {
        Fixture fx(R"dc(
u8 f([] const u8 s, i32 i) {
    return s[i];
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        auto u = fx.param_type(fn, 1);
        REQUIRE(t != nullptr);
        REQUIRE(u != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        args.push_back(comptime::Value::make_int(1, u));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::NotEvaluatable);
    }

    SECTION("specialize: effects and recursion");

    TEST_CASE("static if follows its branch")
    {
        Fixture fx(R"dc(
i32 f(i32 x) {
    static if true {
        return x + 1;
    } else {
        return x + 2;
    }
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
    }

    TEST_CASE("defer abandons")
    {
        Fixture fx(R"dc(
void f(i32 x) {
    defer x = x;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(1, t));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::UnsupportedEffect);
    }

    TEST_CASE("in-progress recursion residualizes")
    {
        Fixture fx(R"dc(
i32 down(i32 n) {
    return down(n);
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("down");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(5, t));
        std::unordered_set<ast::FuncDecl const*> active;
        auto spec = run_specialize(fx, fn, std::move(args), [&](ctfe::Context& ctx) { ctx.specializing = &active; });
        CHECK(active.empty());
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
        bool saw_call = false;
        for (auto i : emit_indices(spec.trace))
            if (spec.trace.nodes[i].node && spec.trace.nodes[i].node->kind == ast::ExprKind::Call)
                saw_call = true;
        CHECK(saw_call);
    }

    TEST_CASE("deep chains abandon on recursion")
    {
        std::string src;
        for (int i = 0; i < 12; ++i)
            src += "i32 f" + std::to_string(i) + "() { return f" + std::to_string(i + 1) + "(); }\n";
        src += "i32 f12() { return 41; }\n";
        Fixture fx(src);
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f0");
        REQUIRE(fn != nullptr);
        std::vector<comptime::Value> args;
        auto spec = run_specialize(fx, fn, std::move(args), [](ctfe::Context& ctx) { ctx.recursion_limit = 6; });
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::RecursionExhausted);
    }

    TEST_CASE("tiny trace limit abandons")
    {
        Fixture fx(R"dc(
i32 add1(i32 x) {
    return x + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("add1");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(41, t));
        auto spec = run_specialize(fx, fn, std::move(args), [](ctfe::Context& ctx) { ctx.trace_limit = 2; });
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::TraceExhausted);
    }

    TEST_CASE("tiny step limit abandons")
    {
        Fixture fx(R"dc(
i32 f(i32 n) {
    while n > 0 {
        n = n - 1;
    }
    return n;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(100, t));
        auto spec = run_specialize(fx, fn, std::move(args), [](ctfe::Context& ctx) { ctx.step_limit = 10; });
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::StepExhausted);
    }

    TEST_CASE("zero memory limit abandons")
    {
        Fixture fx(R"dc(
i32 add1(i32 x) {
    return x + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("add1");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(41, t));
        auto spec = run_specialize(fx, fn, std::move(args), [](ctfe::Context& ctx) { ctx.memory_limit = 0; });
        CHECK_EQ(spec.result.flow, ctfe::Flow::Abandoned);
        CHECK_EQ(spec.result.abandon_reason, ctfe::AbandonReason::CellExhausted);
    }

    SECTION("specialize: trace shape invariants");

    TEST_CASE("unknown slice length residualizes")
    {
        Fixture fx(R"dc(
usize slen([] const u8 s) {
    return s.len;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("slen");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
        CHECK(!emit_indices(spec.trace).empty());
    }

    TEST_CASE("unknown struct fields re-originate")
    {
        Fixture fx(R"dc(
struct Pair {
    i32 a;
    i32 b;
}

i32 f(Pair p) {
    return p.a;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
        CHECK_NE(spec.result.value->unknown_origin(), 0u);
        auto emits = emit_indices(spec.trace);
        CHECK(!emits.empty());
        bool saw_field = false;
        for (auto i : emits)
            if (spec.trace.nodes[i].node && spec.trace.nodes[i].node->kind == ast::ExprKind::FieldAccess)
                saw_field = true;
        CHECK(saw_field);
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

    TEST_CASE("cast on unknown residualizes")
    {
        Fixture fx(R"dc(
i64 f(i32 x) {
    return x as i64;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_unknown(t, 0));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(spec.result.value->is_unknown());
        CHECK(!emit_indices(spec.trace).empty());
    }

    TEST_CASE("trace references point backward")
    {
        Fixture fx(R"dc(
extern i32 ext_fn(i32 x);

i32 f(i32 x) {
    return ext_fn(x) + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> args;
        args.push_back(comptime::Value::make_int(41, t));
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(!spec.trace.nodes.empty());
        CHECK_EQ(spec.trace.nodes.front().kind, ctfe::Residual::Kind::Seq);
        for (std::size_t i = 0; i < spec.trace.nodes.size(); ++i)
        {
            auto const& node = spec.trace.nodes[i];
            if (node.kind == ctfe::Residual::Kind::Value)
                CHECK_NE(node.value.kind(), comptime::Value::Kind::Unknown);
            if (node.kind == ctfe::Residual::Kind::Emit)
                for (auto child : node.children)
                    CHECK_LT(child, spec.trace.nodes.size());
            if (node.kind == ctfe::Residual::Kind::Seq)
                for (auto child : node.children)
                    CHECK_LT(child, spec.trace.nodes.size());
        }
    }

    TEST_CASE("repeated runs agree")
    {
        Fixture fx(R"dc(
i32 add1(i32 x) {
    return x + 1;
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("add1");
        REQUIRE(fn != nullptr);
        auto t = fx.param_type(fn, 0);
        REQUIRE(t != nullptr);
        std::vector<comptime::Value> first_args;
        first_args.push_back(comptime::Value::make_unknown(t, 0));
        auto first = run_specialize(fx, fn, std::move(first_args));
        std::vector<comptime::Value> second_args;
        second_args.push_back(comptime::Value::make_unknown(t, 0));
        auto second = run_specialize(fx, fn, std::move(second_args));
        CHECK_EQ(first.result.flow, second.result.flow);
        CHECK_EQ(first.trace.nodes.size(), second.trace.nodes.size());
        REQUIRE(first.result.value.has_value());
        REQUIRE(second.result.value.has_value());
        CHECK(first.result.value == second.result.value);
    }

    TEST_CASE("compiles blocks still fold")
    {
        Fixture fx(R"dc(
bool f() {
    return compiles { 1 + 1 };
}
)dc");
        REQUIRE(fx.ok());
        auto const* fn = fx.find("f");
        REQUIRE(fn != nullptr);
        std::vector<comptime::Value> args;
        auto spec = run_specialize(fx, fn, std::move(args));
        CHECK_EQ(spec.result.flow, ctfe::Flow::Normal);
        REQUIRE(spec.result.value.has_value());
        CHECK(!spec.result.value->is_unknown());
        CHECK(spec.result.value->get_bool());
    }

    TEST_CASE("abandon reasons are distinct and messaged")
    {
        ctfe::AbandonReason reasons[] = {
            ctfe::AbandonReason::None,
            ctfe::AbandonReason::UnknownCondition,
            ctfe::AbandonReason::UnknownIndex,
            ctfe::AbandonReason::UnsupportedEffect,
            ctfe::AbandonReason::StepExhausted,
            ctfe::AbandonReason::CellExhausted,
            ctfe::AbandonReason::TraceExhausted,
            ctfe::AbandonReason::RecursionExhausted,
            ctfe::AbandonReason::CompilesBlocked,
        };
        for (auto reason : reasons)
        {
            auto text = ctfe::abandon_message(reason);
            CHECK(!text.empty());
            for (auto other : reasons)
            {
                if (other != reason)
                    CHECK(ctfe::abandon_message(other) != text);
            }
        }
    }

} // namespace
