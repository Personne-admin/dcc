import std;
import dcc.types;
import dcc.ast;
import dcc.sema.infer;

#include "harness.hh"

namespace types = dcc::types;
namespace infer = dcc::infer;

namespace
{
    types::TypePtr tparam(types::TypeContext& ctx, int& tag, std::string_view name, std::uint32_t index)
    {
        return ctx.template_param_t(&tag, name, index);
    }

    types::TypePtr ptr(types::TypeContext& ctx, types::TypePtr inner, types::Qual q = types::Qual::None)
    {
        return ctx.pointer_to(inner, q);
    }

    types::TypePtr arr(types::TypeContext& ctx, types::TypePtr inner, std::uint64_t n)
    {
        return ctx.array_t(inner, n);
    }

    types::TypePtr slice(types::TypeContext& ctx, types::TypePtr inner, types::Qual q = types::Qual::None)
    {
        return ctx.slice_t(inner, q);
    }

    types::TypePtr fam(types::TypeContext& ctx, types::TypePtr inner)
    {
        return ctx.fam_t(inner);
    }

    types::TypePtr fn(types::TypeContext& ctx, types::TypePtr ret, std::span<types::TypePtr const> params)
    {
        return ctx.funcptr_t(ret, params);
    }

    types::TypePtr nominal(types::TypeContext& ctx, types::TypeKind kind, int& decl, std::span<types::TypePtr const> args = {})
    {
        return ctx.nominal_t(kind, &decl, args);
    }

} // namespace

SECTION("infer: function templates");

TEST_CASE("deduces repeated function parameter")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};
    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    std::array<types::TypePtr, 2> params{T, T};
    std::array<types::TypePtr, 2> args{i32, i32};

    REQUIRE(ib.deduce_function(params, args));
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(T)), i32);
    CHECK_EQ(ib.substitute(T), i32);
}

TEST_CASE("conflicting function bindings fail")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};
    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);
    auto boolean = ctx.m_boolt();

    std::array<types::TypePtr, 2> params{T, T};
    std::array<types::TypePtr, 2> args{i32, boolean};

    auto r = ib.deduce_function(params, args);
    CHECK(!r);
    CHECK_EQ(r.error, infer::DeductionError::Conflict);
}

TEST_CASE("function arity mismatch fails")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};
    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    std::array<types::TypePtr, 1> params{T};
    std::array<types::TypePtr, 2> args{i32, i32};

    auto r = ib.deduce_function(params, args);
    CHECK(!r);
    CHECK_EQ(r.error, infer::DeductionError::ArityMismatch);
}

SECTION("infer: nominal templates");

TEST_CASE("deduces nominal template arguments")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int t_tag{};
    int vector_decl{};

    auto T = tparam(ctx, t_tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    std::array<types::TypePtr, 1> pat_args{T};
    std::array<types::TypePtr, 1> actual_args{i32};

    auto pattern = nominal(ctx, types::TypeKind::Struct, vector_decl, pat_args);
    auto actual = nominal(ctx, types::TypeKind::Struct, vector_decl, actual_args);

    REQUIRE(ib.deduce(pattern, actual));
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(T)), i32);
    CHECK_EQ(ib.substitute(pattern), actual);
}

TEST_CASE("nominal template arity mismatch fails")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int t_tag{};
    int vector_decl{};

    auto T = tparam(ctx, t_tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    std::array<types::TypePtr, 1> pat_args{T};
    std::array<types::TypePtr, 2> actual_args{i32, i32};

    auto pattern = nominal(ctx, types::TypeKind::Struct, vector_decl, pat_args);
    auto actual = nominal(ctx, types::TypeKind::Struct, vector_decl, actual_args);

    auto r = ib.deduce(pattern, actual);
    CHECK(!r);
    CHECK_EQ(r.error, infer::DeductionError::ArityMismatch);
}

SECTION("infer: record shapes");

TEST_CASE("deduces record fields by name")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int t_tag{};
    int u_tag{};

    auto T = tparam(ctx, t_tag, "T", 0);
    auto U = tparam(ctx, u_tag, "U", 1);
    auto i32 = ctx.int_t(32, true);
    auto boolean = ctx.m_boolt();

    std::array pattern{
        infer::RecordField{"lhs", T},
        infer::RecordField{"rhs", U},
    };
    std::array actual{
        infer::RecordField{"rhs", boolean},
        infer::RecordField{"lhs", i32},
    };

    REQUIRE(ib.deduce_record(pattern, actual));
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(T)), i32);
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(U)), boolean);

    auto instantiated = ib.substitute_record(pattern);
    CHECK_EQ(instantiated[0].type, i32);
    CHECK_EQ(instantiated[1].type, boolean);
}

TEST_CASE("record field mismatch fails")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int t_tag{};

    auto T = tparam(ctx, t_tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    std::array pattern{infer::RecordField{"lhs", T}};
    std::array actual{infer::RecordField{"rhs", i32}};

    auto r = ib.deduce_record(pattern, actual);
    CHECK(!r);
    CHECK_EQ(r.error, infer::DeductionError::MissingField);
}

SECTION("infer: nested composite instantiation");

TEST_CASE("substitutes through ptr slice array fnptr")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int t_tag{};
    int u_tag{};

    auto T = tparam(ctx, t_tag, "T", 0);
    auto U = tparam(ctx, u_tag, "U", 1);
    auto i32 = ctx.int_t(32, true);
    auto boolean = ctx.m_boolt();

    std::array<types::TypePtr, 1> fn_params{ptr(ctx, U, types::Qual::Const)};
    std::array<types::TypePtr, 1> fn_actual_params{ptr(ctx, boolean, types::Qual::Const)};

    auto pattern = ptr(ctx, slice(ctx, arr(ctx, fn(ctx, T, fn_params), 4), types::Qual::Volatile), types::Qual::Restrict);
    auto actual = ptr(ctx, slice(ctx, arr(ctx, fn(ctx, i32, fn_actual_params), 4), types::Qual::Volatile), types::Qual::Restrict);

    REQUIRE(ib.deduce(pattern, actual));
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(T)), i32);
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(U)), boolean);
    CHECK_EQ(ib.substitute(pattern), actual);
}

TEST_CASE("deduces fam element type")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int t_tag{};

    auto T = tparam(ctx, t_tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    auto pattern = fam(ctx, T);
    auto actual = fam(ctx, i32);

    REQUIRE(ib.deduce(pattern, actual));
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(T)), i32);
    CHECK_EQ(ib.substitute(pattern), actual);
}

SECTION("infer: negative cases");

TEST_CASE("occurs check rejects recursive binding")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int t_tag{};

    auto T = tparam(ctx, t_tag, "T", 0);

    auto recursive = ptr(ctx, T);
    auto r = ib.deduce(T, recursive);
    CHECK(!r);
    CHECK_EQ(r.error, infer::DeductionError::OccursCheck);
}

SECTION("infer: pointer pointee qualifiers");

TEST_CASE("deduces volatile ptr pointee qualifier match")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};

    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    auto pattern = ptr(ctx, T, types::Qual::Volatile);
    auto actual = ptr(ctx, i32, types::Qual::Volatile);

    REQUIRE(ib.deduce(pattern, actual));
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(T)), i32);
    CHECK_EQ(ib.substitute(pattern), actual);
}

TEST_CASE("deduces const ptr pointee qualifier match")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};

    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    auto pattern = ptr(ctx, T, types::Qual::Const);
    auto actual = ptr(ctx, i32, types::Qual::Const);

    REQUIRE(ib.deduce(pattern, actual));
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(T)), i32);
}

TEST_CASE("deduces const-volatile ptr pointee qualifier match")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};

    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    auto pattern = ptr(ctx, T, types::Qual::Const | types::Qual::Volatile);
    auto actual = ptr(ctx, i32, types::Qual::Const | types::Qual::Volatile);

    REQUIRE(ib.deduce(pattern, actual));
    CHECK_EQ(ib.lookup(static_cast<types::TemplateParamType const*>(T)), i32);
}

TEST_CASE("deduces ptr actual extra qualifiers ok")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};

    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    auto pattern = ptr(ctx, T, types::Qual::None);
    auto actual = ptr(ctx, i32, types::Qual::Volatile);

    REQUIRE(ib.deduce(pattern, actual));

    auto bound = ib.lookup(static_cast<types::TemplateParamType const*>(T));
    REQUIRE(bound);
    CHECK_EQ(bound->kind, types::TypeKind::Int);
    auto const* int_t = static_cast<types::IntType const*>(bound);
    CHECK_EQ(int_t->bits, 32);
    CHECK(int_t->is_signed);
}

TEST_CASE("volatile ptr pattern vs non-volatile actual fails")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};

    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    auto pattern = ptr(ctx, T, types::Qual::Volatile);
    auto actual = ptr(ctx, i32, types::Qual::None);

    auto r = ib.deduce(pattern, actual);
    CHECK(!r);
    CHECK_EQ(r.error, infer::DeductionError::Conflict);
}

TEST_CASE("const-volatile ptr pattern vs const-only actual fails")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};

    auto T = tparam(ctx, tag, "T", 0);
    auto i32 = ctx.int_t(32, true);

    auto pattern = ptr(ctx, T, types::Qual::Const | types::Qual::Volatile);
    auto actual = ptr(ctx, i32, types::Qual::Const);

    auto r = ib.deduce(pattern, actual);
    CHECK(!r);
    CHECK_EQ(r.error, infer::DeductionError::Conflict);
}

TEST_CASE("ptr qualifier same, pointee deduces template param")
{
    types::TypeContext ctx;
    infer::TemplateBindings ib{ctx};
    int tag{};

    auto T = tparam(ctx, tag, "T", 0);
    auto boolean = ctx.m_boolt();

    auto pattern = ptr(ctx, T, types::Qual::Volatile);
    auto actual = ptr(ctx, boolean, types::Qual::Volatile);

    REQUIRE(ib.deduce(pattern, actual));
    auto bound = ib.lookup(static_cast<types::TemplateParamType const*>(T));
    CHECK(bound == boolean);
}

TEST_CASE("function pointer splice deduction binds either direction and preserves identity")
{
    types::TypeContext ctx;
    dcc::ast::TemplateParam pack;
    pack.name = "T";
    pack.is_pack = true;
    auto* t = static_cast<types::TemplateParamType const*>(ctx.template_param_t(&pack, "T", 0));
    auto i32 = ctx.int_t(32, true);
    auto f64 = ctx.float_t(64);
    for (auto const& suffix : std::vector<std::vector<types::TypePtr>>{{}, {f64}, {f64, ctx.m_boolt()}})
        for (bool prefix : {false, true})
            for (bool reverse : {false, true})
            {
                infer::TemplateBindings bindings{ctx};
                std::vector<types::TypePtr> pattern_params;
                std::vector<types::TypePtr> actual_params;
                if (prefix)
                {
                    pattern_params.push_back(ctx.int_t(8, false));
                    actual_params.push_back(ctx.int_t(8, false));
                }
                pattern_params.push_back(t);
                actual_params.insert(actual_params.end(), suffix.begin(), suffix.end());
                auto pattern = ctx.funcptr_t(i32, pattern_params);
                auto actual = ctx.funcptr_t(i32, actual_params);
                REQUIRE(reverse ? bindings.deduce(actual, pattern) : bindings.deduce(pattern, actual));
                REQUIRE(bindings.lookup_pack(t) != nullptr);
                CHECK(*bindings.lookup_pack(t) == suffix);
                CHECK(bindings.substitute(pattern) == actual);
            }
}

TEST_CASE("function pointer splice rejection rolls back return prefix and pack deductions")
{
    types::TypeContext ctx;
    dcc::ast::TemplateParam pack;
    pack.is_pack = true;
    dcc::ast::TemplateParam ret;
    dcc::ast::TemplateParam prefix;
    auto* t = static_cast<types::TemplateParamType const*>(ctx.template_param_t(&pack, "T", 0));
    auto* r = static_cast<types::TemplateParamType const*>(ctx.template_param_t(&ret, "R", 1));
    auto* p = static_cast<types::TemplateParamType const*>(ctx.template_param_t(&prefix, "P", 2));
    auto i32 = ctx.int_t(32, true);
    auto f64 = ctx.float_t(64);
    std::array<types::TypePtr, 3> pattern_params{p, ctx.m_boolt(), t};
    std::array<types::TypePtr, 3> actual_params{i32, f64, i32};
    auto pattern = ctx.funcptr_t(r, pattern_params);
    auto actual = ctx.funcptr_t(i32, actual_params);
    for (bool reverse : {false, true})
    {
        infer::TemplateBindings bindings{ctx};
        auto result = reverse ? bindings.deduce(actual, pattern) : bindings.deduce(pattern, actual);
        CHECK(!result);
        CHECK_EQ(result.detail, "function pointer fixed parameter prefix mismatch");
        CHECK(bindings.empty());
        std::array<types::TypePtr, 1> splice{t};
        std::array<types::TypePtr, 1> single{i32};
        auto a = ctx.funcptr_t(ctx.m_voidt(), splice);
        auto b = ctx.funcptr_t(ctx.m_voidt(), single);
        REQUIRE(bindings.deduce(a, b));
        auto rejected = bindings.deduce(a, ctx.funcptr_t(ctx.m_voidt(), {}));
        CHECK(!rejected);
        CHECK_EQ(rejected.detail, "conflicting pack binding");
        REQUIRE(bindings.lookup_pack(t) != nullptr);
        CHECK(*bindings.lookup_pack(t) == std::vector<types::TypePtr>{i32});
    }
}

SECTION("infer: contextual return qualification");

TEST_CASE("deduces mutable slice and pointer elements from const return context")
{
    types::TypeContext ctx;
    int tag{};
    auto T = tparam(ctx, tag, "T", 0);
    auto u8 = ctx.int_t(8, false);

    infer::TemplateBindings slices{ctx};
    REQUIRE(slices.deduce_return(slice(ctx, T), slice(ctx, u8, types::Qual::Const)));
    CHECK_EQ(slices.substitute(T), u8);
    CHECK_EQ(slices.substitute(slice(ctx, T)), slice(ctx, u8));

    infer::TemplateBindings pointers{ctx};
    REQUIRE(pointers.deduce_return(ptr(ctx, T), ptr(ctx, u8, types::Qual::Const)));
    CHECK_EQ(pointers.substitute(T), u8);
    CHECK_EQ(pointers.substitute(ptr(ctx, T)), ptr(ctx, u8));
}

TEST_CASE("return context cannot remove const or add volatile qualification")
{
    types::TypeContext ctx;
    int tag{};
    auto T = tparam(ctx, tag, "T", 0);
    auto u8 = ctx.int_t(8, false);
    infer::TemplateBindings bindings{ctx};

    CHECK(!bindings.deduce_return(slice(ctx, T, types::Qual::Const), slice(ctx, u8)));
    CHECK(!bindings.deduce_return(ptr(ctx, T, types::Qual::Const), ptr(ctx, u8)));
    CHECK(!bindings.deduce_return(slice(ctx, T), slice(ctx, u8, types::Qual::Volatile)));
    CHECK_EQ(bindings.substitute(T), T);
}
