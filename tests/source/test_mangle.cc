import std;
import dcc.types;
import dcc.comptime;
import dcc.ast;
import dcc.sm;
import dcc.ir.mangle;
import dcc.sema.type_helpers;
import dcc.target;

#include "harness.hh"

using namespace std::literals;

namespace types = dcc::types;
namespace comptime = dcc::comptime;
namespace mangle = dcc::ir::mangle;
namespace ast = dcc::ast;

namespace
{
    bool demangle_check(std::string_view s)
    {
        mangle::DemangledName d;
        return mangle::demangle(d, s);
    }

    template <typename F> bool demangle_with(std::string_view s, F&& f)
    {
        mangle::DemangledName d;
        if (mangle::demangle(d, s))
        {
            std::forward<F>(f)(d);
            return true;
        }

        return false;
    }

} // namespace

namespace
{
    types::TypePtr i32(types::TypeContext& ctx)
    {
        return ctx.int_t(32, true);
    }
    types::TypePtr u8(types::TypeContext& ctx)
    {
        return ctx.int_t(8, false);
    }
    types::TypePtr i8(types::TypeContext& ctx)
    {
        return ctx.int_t(8, true);
    }
    types::TypePtr f64(types::TypeContext& ctx)
    {
        return ctx.float_t(64);
    }
    types::TypePtr f32(types::TypeContext& ctx)
    {
        return ctx.float_t(32);
    }

    auto make_resolver([[maybe_unused]] std::vector<std::string_view> path, [[maybe_unused]] std::string_view name)
    {
        return mangle::NominalResolver{
            [path_vec = std::vector<std::string>(path.begin(), path.end()), name_str = std::string{name}](void const*) -> std::optional<mangle::NominalInfo> {
                return mangle::NominalInfo{
                    .module_path = std::vector<std::string_view>(path_vec.begin(), path_vec.end()),
                    .name = name_str,
                };
            }};
    }

    ast::FuncDecl* make_func(ast::AstContext& ctx, std::string_view name, bool nomangle = false, std::string_view cc = {})
    {
        auto* fd = ctx.make<ast::FuncDecl>(dcc::sm::SourceRange{}, name, dcc::sm::SourceRange{});
        fd->sema.is_nomangle = nomangle;
        if (!cc.empty())
            fd->sema.calling_conv = cc;

        return fd;
    }

    ast::VarDecl* make_var(ast::AstContext& ctx, std::string_view name, bool nomangle = false)
    {
        auto* vd = ctx.make<ast::VarDecl>(dcc::sm::SourceRange{}, name, dcc::sm::SourceRange{});
        vd->sema.is_nomangle = nomangle;
        return vd;
    }

} // namespace

SECTION("mangle_type: primitives");

TEST_CASE("mangle_type Void")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_type(ctx.m_voidt());
    CHECK_EQ(s, "_DC0Tv");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.tag, mangle::DemangledType::Tag::Void);
}

TEST_CASE("mangle_type Bool")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.m_boolt()), "_DC0Tb");
}

TEST_CASE("mangle_type Int i32")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_type(i32(ctx));
    CHECK_EQ(s, "_DC0Ti32s");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.bits, 32u);
    CHECK(d.type_only.is_signed);
}

TEST_CASE("mangle_type Int u8")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(u8(ctx)), "_DC0Ti8u");
}

TEST_CASE("mangle_type usize default 64-bit")
{
    types::TypeContext ctx;
    auto usz = ctx.usize_t();
    auto u64t = ctx.int_t(64, false);

    CHECK_NE(usz, u64t);

    CHECK(static_cast<types::IntType const*>(usz)->is_pointer_sized);
    CHECK(!static_cast<types::IntType const*>(u64t)->is_pointer_sized);

    CHECK_EQ(mangle::mangle_type(usz), "_DC0TIu");
    CHECK_EQ(mangle::mangle_type(u64t), "_DC0Ti64u");

    {
        mangle::DemangledName d;
        REQUIRE(mangle::demangle(d, "_DC0TIu"));
        CHECK_EQ(d.type_only.tag, mangle::DemangledType::Tag::Int);
        CHECK(d.type_only.is_pointer_sized);
        CHECK(!d.type_only.is_signed);
    }
    {
        mangle::DemangledName d;
        REQUIRE(mangle::demangle(d, "_DC0Ti64u"));
        CHECK_EQ(d.type_only.tag, mangle::DemangledType::Tag::Int);
        CHECK(!d.type_only.is_pointer_sized);
        CHECK(!d.type_only.is_signed);
        CHECK_EQ(d.type_only.bits, 64u);
    }
}

TEST_CASE("mangle_type isize default 64-bit")
{
    types::TypeContext ctx;
    auto isz = ctx.isize_t();
    auto i64t = ctx.int_t(64, true);
    CHECK_NE(isz, i64t);
    CHECK(static_cast<types::IntType const*>(isz)->is_pointer_sized);
    CHECK(!static_cast<types::IntType const*>(i64t)->is_pointer_sized);
    CHECK_EQ(mangle::mangle_type(isz), "_DC0TIs");
    CHECK_EQ(mangle::mangle_type(i64t), "_DC0Ti64s");

    {
        mangle::DemangledName d;
        REQUIRE(mangle::demangle(d, "_DC0TIs"));
        CHECK_EQ(d.type_only.tag, mangle::DemangledType::Tag::Int);
        CHECK(d.type_only.is_pointer_sized);
        CHECK(d.type_only.is_signed);
    }
}

TEST_CASE("mangle_type usize x86 32-bit target")
{
    auto target_opt = dcc::target::TargetConfig::parse_triple("x86-elf");
    REQUIRE(target_opt.has_value());
    types::TypeContext ctx{32 * 1024, &*target_opt};
    CHECK_EQ(ctx.pointer_bits(), 32u);
    auto usz = ctx.usize_t();
    auto* it = static_cast<types::IntType const*>(usz);
    CHECK_EQ(it->bits, 32u);
    CHECK(!it->is_signed);
    CHECK(it->is_pointer_sized);

    auto u32t = ctx.int_t(32, false);
    auto* u32it = static_cast<types::IntType const*>(u32t);
    CHECK_EQ(u32it->bits, 32u);
    CHECK(!u32it->is_signed);
    CHECK(!u32it->is_pointer_sized);
    CHECK_NE(usz, u32t);

    CHECK_EQ(mangle::mangle_type(usz), "_DC0TIu");
    CHECK_EQ(mangle::mangle_type(u32t), "_DC0Ti32u");
}

TEST_CASE("mangle_type Float f64")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(f64(ctx)), "_DC0Tf64.");
}

TEST_CASE("mangle_type Float f32")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(f32(ctx)), "_DC0Tf32.");
}

TEST_CASE("mangle_type Char")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.m_chart()), "_DC0Tc");
}

TEST_CASE("mangle_type NullT")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.m_nullt()), "_DC0Tn");
}

TEST_CASE("mangle_type Error")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.m_errort()), "_DC0TE");
}

SECTION("mangle_type: compound");

TEST_CASE("mangle_type Pointer to i32")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.pointer_to(i32(ctx), types::Qual::None)), "_DC0TPqi32s");
}

TEST_CASE("mangle_type Pointer to const i32")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.pointer_to(i32(ctx), types::Qual::Const)), "_DC0TPCqi32s");
}

TEST_CASE("mangle_type Pointer to volatile restrict i32")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.pointer_to(i32(ctx), types::Qual::Volatile | types::Qual::Restrict)), "_DC0TPVRqi32s");
}

TEST_CASE("mangle_type Array 3 x i32")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_type(ctx.array_t(i32(ctx), 3));
    CHECK_EQ(s, "_DC0TA3i32s");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.tag, mangle::DemangledType::Tag::Array);
    CHECK_EQ(d.type_only.count, 3u);
}

TEST_CASE("mangle_type Slice of const char")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.slice_t(ctx.m_chart(), types::Qual::Const)), "_DC0TSCqc");
}

TEST_CASE("mangle_type Slice of mutable i64")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.slice_t(ctx.int_t(64, true), types::Qual::None)), "_DC0TSqi64s");
}

TEST_CASE("mangle_type Fam of i32")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.fam_t(i32(ctx))), "_DC0TFi32s");
}

TEST_CASE("mangle_type FuncPtr (i32, bool) -> void")
{
    types::TypeContext ctx;
    std::vector<types::TypePtr> params = {i32(ctx), ctx.m_boolt()};
    auto s = mangle::mangle_type(ctx.funcptr_t(ctx.m_voidt(), params));
    CHECK_EQ(s, "_DC0Tpv2i32sb");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));

    CHECK_EQ(d.type_only.tag, mangle::DemangledType::Tag::FuncPtr);
    CHECK_EQ(d.type_only.template_args.size(), 2u);
    CHECK_EQ(d.type_only.template_args[0].tag, mangle::DemangledType::Tag::Int);
    CHECK_EQ(d.type_only.template_args[1].tag, mangle::DemangledType::Tag::Bool);
}

TEST_CASE("mangle_type Range of i32")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.range_t(i32(ctx))), "_DC0Tri32s");
}

TEST_CASE("mangle_type RangeInclusive of f64")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_type(ctx.range_inclusive_t(f64(ctx))), "_DC0TRf64.");
}

TEST_CASE("mangle_type TemplateParam index=0 name=T")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_type(ctx.template_param_t(nullptr, "T", 0));
    CHECK_EQ(s, "_DC0TZ1.T0");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.tag, mangle::DemangledType::Tag::TemplateParam);
    CHECK_EQ(d.type_only.param_index, 0u);
    CHECK_EQ(d.type_only.name, "T");
}

SECTION("mangle_type: nominal");

TEST_CASE("mangle_type Struct with module path")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "Point", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Struct, sd);
    auto s = mangle::mangle_type(t, make_resolver({"geom"}, "Point"));
    CHECK_EQ(s, "_DC0TD1.4.geom5.Point0.");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.module_path.size(), 1u);
    CHECK_EQ(d.type_only.module_path[0], "geom");
    CHECK_EQ(d.type_only.name, "Point");
}

TEST_CASE("mangle_type Struct without resolver encodes empty")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "MyStruct", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Struct, sd);
    auto s = mangle::mangle_type(t);
    CHECK_EQ(s, "_DC0TD0.0.0.");
}

TEST_CASE("mangle_type Union")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* ud = actx.make<ast::UnionDecl>(dcc::sm::SourceRange{}, "Data", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Union, ud);
    auto s = mangle::mangle_type(t, make_resolver({"core"}, "Data"));
    CHECK_EQ(s, "_DC0TD1.4.core4.Data0.");
}

TEST_CASE("mangle_type Enum")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* ed = actx.make<ast::EnumDecl>(dcc::sm::SourceRange{}, "Color", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Enum, ed);
    auto s = mangle::mangle_type(t, make_resolver({"gfx"}, "Color"));
    CHECK_EQ(s, "_DC0TD1.3.gfx5.Color0.");
}

SECTION("mangle_value");

TEST_CASE("mangle_value Null")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_null(ctx.m_nullt()));
    CHECK_EQ(s, "_DC0VNn");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.tag, mangle::DemangledValue::Tag::Null);
}

TEST_CASE("mangle_value Int 42")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(42, i32(ctx));
    auto s = mangle::mangle_value(v);
    CHECK_EQ(s, "_DC0VIi32s42");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.int_val, 42);
}

TEST_CASE("mangle_value Int negative")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_int(-1, i32(ctx)));
    CHECK_EQ(s, "_DC0VIi32sm1");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.int_val, -1);
}

TEST_CASE("mangle_value Int zero")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_int(0, i32(ctx)));
    CHECK_EQ(s, "_DC0VIi32s0");
}

TEST_CASE("mangle_value Int -128")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_int(-128, i8(ctx)));
    CHECK_EQ(s, "_DC0VIi8sm128");
}

TEST_CASE("mangle_value Float 3.14")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_float(3.14, f64(ctx));
    auto s = mangle::mangle_value(v);
    CHECK(s.starts_with("_DC0VFf64"));
    CHECK_EQ(s.size(), 26u);
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.tag, mangle::DemangledValue::Tag::Float);
    CHECK(std::abs(d.value_only.float_val - 3.14) < 1e-10);
}

TEST_CASE("mangle_value Bool true")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_value(comptime::Value::make_bool(true, ctx.m_boolt())), "_DC0VBb1");
}

TEST_CASE("mangle_value Bool false")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_value(comptime::Value::make_bool(false, ctx.m_boolt())), "_DC0VBb0");
}

TEST_CASE("mangle_value Char 65")
{
    types::TypeContext ctx;
    CHECK_EQ(mangle::mangle_value(comptime::Value::make_char(65, ctx.m_chart())), "_DC0VCc65");
}

TEST_CASE("mangle_value String hello")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_string("hello"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None)));
    CHECK_EQ(s, "_DC0VSPqc5.hello");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.string_val, "hello");
}

TEST_CASE("mangle_value String empty")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_string(""s, ctx.pointer_to(ctx.m_chart(), types::Qual::None)));
    CHECK_EQ(s, "_DC0VSPqc0.");
}

TEST_CASE("mangle_value Aggregate [1,2,3] as array")
{
    types::TypeContext ctx;
    auto t = ctx.array_t(i32(ctx), 3);
    std::vector<comptime::Value> elems;
    elems.push_back(comptime::Value::make_int(1, i32(ctx)));
    elems.push_back(comptime::Value::make_int(2, i32(ctx)));
    elems.push_back(comptime::Value::make_int(3, i32(ctx)));
    auto v = comptime::Value::make_aggregate(std::move(elems), t);
    auto s = mangle::mangle_value(v);
    CHECK_EQ(s, "_DC0VAA3i32s3Ii32s1Ii32s2Ii32s3");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.elements.size(), 3u);
    CHECK_EQ(d.value_only.elements[0].int_val, 1);
    CHECK_EQ(d.value_only.elements[1].int_val, 2);
    CHECK_EQ(d.value_only.elements[2].int_val, 3);
}

TEST_CASE("mangle_value Slice [10,20]")
{
    types::TypeContext ctx;
    auto t = ctx.slice_t(i32(ctx), types::Qual::None);
    std::vector<comptime::Value> elems;
    elems.push_back(comptime::Value::make_int(10, i32(ctx)));
    elems.push_back(comptime::Value::make_int(20, i32(ctx)));
    auto v = comptime::Value::make_slice(std::move(elems), t);
    auto s = mangle::mangle_value(v);
    CHECK_EQ(s, "_DC0VsSqi32s2Ii32s10Ii32s20");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.tag, mangle::DemangledValue::Tag::Slice);
    CHECK_EQ(d.value_only.elements.size(), 2u);
}

TEST_CASE("mangle_value Pointer null")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_pointer(ctx.pointer_to(i32(ctx), types::Qual::None)));
    CHECK_EQ(s, "_DC0VPPqi32s0");
}

TEST_CASE("mangle_value Pointer to index 5")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_pointer_to(5, ctx.pointer_to(i32(ctx), types::Qual::None)));
    CHECK_EQ(s, "_DC0VPPqi32s15");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK(!d.value_only.is_null_ptr);
    CHECK_EQ(d.value_only.pointer_index, 5u);
}

TEST_CASE("mangle_value Pointer const volatile null")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_value(comptime::Value::make_pointer(ctx.pointer_to(i32(ctx), types::Qual::Const | types::Qual::Volatile)));
    CHECK_EQ(s, "_DC0VPPCVqi32s0");
}

SECTION("mangle_function");

TEST_CASE("mangle_function void fn root::main")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "main");
    auto s = mangle::mangle_function({"root"}, *fd, {}, ctx.m_voidt());
    CHECK_EQ(s, "_DC0F1.4.root4.main0v");
}

TEST_CASE("mangle_function add(i32,i32)->i32 in math")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "add");
    std::vector<types::TypePtr> params = {i32(ctx), i32(ctx)};
    auto s = mangle::mangle_function({"math"}, *fd, params, i32(ctx));
    CHECK_EQ(s, "_DC0F1.4.math3.add2i32si32si32s");
}

TEST_CASE("mangle_function with stdcall")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "foo", false, "stdcall");
    auto s = mangle::mangle_function({"win32"}, *fd, {}, ctx.m_voidt());
    CHECK_EQ(s, "_DC0F1.5.win323.foo0vc1");
}

TEST_CASE("mangle_function with template type arg")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "foo");
    std::vector<types::TypePtr> params = {i32(ctx)};
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    auto s = mangle::mangle_function({"lib"}, *fd, params, ctx.m_voidt(), targs);
    CHECK_EQ(s, "_DC0F1.3.lib3.foo1i32svX1ti32s");
}

TEST_CASE("mangle_function with 2 template type args")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "convert");
    std::vector<types::TypePtr> params = {i32(ctx)};
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, f64(ctx), nullptr});
    auto s = mangle::mangle_function({"lib"}, *fd, params, ctx.m_voidt(), targs);
    CHECK_EQ(s, "_DC0F1.3.lib7.convert1i32svX2ti32stf64.");
}

TEST_CASE("mangle_function @nomangle returns raw name")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "my_raw_fn", true);
    auto s = mangle::mangle_function({"ignored"}, *fd, {}, ctx.m_voidt());
    CHECK_EQ(s, "my_raw_fn");
}

SECTION("mangle_specialization");

TEST_CASE("mangle_specialization foo<i32>(i32)->void")
{
    types::TypeContext ctx;
    std::vector<types::TypePtr> params = {i32(ctx)};
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    auto s = mangle::mangle_specialization({"lib"}, "foo", params, ctx.m_voidt(), targs);
    CHECK_EQ(s, "_DC0S1.3.lib3.foo1i32svX1ti32s");
    auto d = mangle::demangle(s);
    REQUIRE(d.has_value());
    CHECK_EQ(d->kind, mangle::DemangledName::Kind::Specialization);
    CHECK_EQ(d->name, "foo");
    CHECK_EQ(d->param_types.size(), 1u);
    CHECK_EQ(d->template_args.size(), 1u);
}

TEST_CASE("mangle_specialization empty params and targs")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_specialization({"ns"}, "empty", {}, ctx.m_voidt());
    CHECK_EQ(s, "_DC0S1.2.ns5.empty0v");
    auto d = mangle::demangle(s);
    REQUIRE(d.has_value());
    CHECK_EQ(d->kind, mangle::DemangledName::Kind::Specialization);
}

TEST_CASE("mangle_specialization with value template arg")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Value, i32(ctx), actx.own_value(comptime::Value::make_int(42, i32(ctx)))});
    auto s = mangle::mangle_specialization({"lib"}, "val", {}, ctx.m_voidt(), targs);
    CHECK_EQ(s, "_DC0S1.3.lib3.val0vX1vIi32s42");
    auto d = mangle::demangle(s);
    REQUIRE(d.has_value());
    CHECK_EQ(d->kind, mangle::DemangledName::Kind::Specialization);
    CHECK_EQ(d->template_args.size(), 1u);
}

TEST_CASE("mangle_function multi-segment module path")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "startup");
    auto s = mangle::mangle_function({"com", "example", "app"}, *fd, {}, ctx.m_voidt());
    CHECK_EQ(s, "_DC0F3.3.com7.example3.app7.startup0v");
}

TEST_CASE("mangle_function same name different modules differ")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "run");
    auto s1 = mangle::mangle_function({"mod_a"}, *fd, {}, ctx.m_voidt());
    auto s2 = mangle::mangle_function({"mod_b"}, *fd, {}, ctx.m_voidt());
    CHECK_NE(s1, s2);
}

SECTION("mangle_global");

TEST_CASE("mangle_global i32 x in app")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* vd = make_var(actx, "x");
    auto s = mangle::mangle_global({"app"}, *vd, i32(ctx));
    CHECK_EQ(s, "_DC0G1.3.app1.xi32s");
}

TEST_CASE("mangle_global @nomangle returns raw name")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* vd = make_var(actx, "my_global", true);
    auto s = mangle::mangle_global({"ignored"}, *vd, i32(ctx));
    CHECK_EQ(s, "my_global");
}

SECTION("demangle round-trip");

TEST_CASE("demangle all primitive types")
{
    CHECK(demangle_check("_DC0Tv"));
    CHECK(demangle_check("_DC0Tb"));
    CHECK(demangle_check("_DC0Ti32s"));
    CHECK(demangle_check("_DC0Ti8u"));
    CHECK(demangle_check("_DC0Tf64."));
    CHECK(demangle_check("_DC0Tf32."));
    CHECK(demangle_check("_DC0Tc"));
    CHECK(demangle_check("_DC0Tn"));
    CHECK(demangle_check("_DC0TE"));
}

TEST_CASE("demangle all compound types")
{
    CHECK(demangle_check("_DC0TPqi32s"));
    CHECK(demangle_check("_DC0TPCqi32s"));
    CHECK(demangle_check("_DC0TA3i32s"));
    CHECK(demangle_check("_DC0TSCqc"));
    CHECK(demangle_check("_DC0TFi32s"));
    CHECK(demangle_check("_DC0Tpv2i32sb"));
    CHECK(demangle_check("_DC0Tri32s"));
    CHECK(demangle_check("_DC0TRf64."));
    CHECK(demangle_check("_DC0TZ1.T0"));
    CHECK(demangle_check("_DC0TD1.4.geom5.Point0."));
}

TEST_CASE("demangle functions")
{
    CHECK(demangle_check("_DC0F1.4.root4.main0v"));
    CHECK(demangle_check("_DC0F1.4.math3.add2i32si32si32s"));
    CHECK(demangle_check("_DC0F1.5.win323.foo0vc1"));
    CHECK(demangle_check("_DC0F1.3.lib3.foo1i32svX1ti32s"));
}

TEST_CASE("demangle globals")
{
    CHECK(demangle_check("_DC0G1.3.app1.xi32s"));
}

TEST_CASE("demangle convenience overload")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_type(i32(ctx));
    mangle::DemangledName d1;
    CHECK(mangle::demangle(d1, s));
    auto d2 = mangle::demangle(s);
    REQUIRE(d2.has_value());
    CHECK_EQ(d2->type_only.tag, mangle::DemangledType::Tag::Int);
    CHECK_EQ(d2->type_only.bits, 32u);
    auto d3 = mangle::demangle("garbage");
    CHECK(!d3.has_value());
}

TEST_CASE("demangle rejects invalid strings")
{
    CHECK(!demangle_check(""));
    CHECK(!demangle_check("_DC0"));
    CHECK(!demangle_check("_DC0X"));
    CHECK(!demangle_check("_DC0T"));
    CHECK(!demangle_check("_DC0F"));
    CHECK(!demangle_check("not_mangled"));
    CHECK(!demangle_check("_DC0Tv_extra"));
    CHECK(!demangle_check("_DC0T$"));
    CHECK(!demangle_check("_DC0Ti"));
}

SECTION("uniqueness");

TEST_CASE("same function name different modules")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "frobnicate");
    auto s1 = mangle::mangle_function({"a", "b"}, *fd, {}, ctx.m_voidt());
    auto s2 = mangle::mangle_function({"c", "d"}, *fd, {}, ctx.m_voidt());
    CHECK_NE(s1, s2);
}

TEST_CASE("same template different scalar args")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "tpl");
    std::vector<mangle::TemplateArg> targs1, targs2;
    targs1.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Value, i32(ctx), actx.own_value(comptime::Value::make_int(42, i32(ctx)))});
    targs2.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Value, i32(ctx), actx.own_value(comptime::Value::make_int(99, i32(ctx)))});
    auto s1 = mangle::mangle_function({"lib"}, *fd, {}, ctx.m_voidt(), targs1);
    auto s2 = mangle::mangle_function({"lib"}, *fd, {}, ctx.m_voidt(), targs2);
    CHECK_NE(s1, s2);
}

TEST_CASE("aggregate NTTP Point{1,2} vs Point{1,3}")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd = make_func(actx, "make");
    auto i32t = i32(ctx);
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "Point", dcc::sm::SourceRange{});
    auto point_t = ctx.nominal_t(types::TypeKind::Struct, sd);

    auto make_pt = [&](int a, int b) {
        std::vector<comptime::Value> elems;
        elems.push_back(comptime::Value::make_int(a, i32t));
        elems.push_back(comptime::Value::make_int(b, i32t));
        return comptime::Value::make_aggregate(std::move(elems), point_t);
    };

    std::vector<mangle::TemplateArg> ta1, ta2;
    ta1.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Value, point_t, actx.own_value(make_pt(1, 2))});
    ta2.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Value, point_t, actx.own_value(make_pt(1, 3))});
    auto s1 = mangle::mangle_function({"lib"}, *fd, {}, ctx.m_voidt(), ta1);
    auto s2 = mangle::mangle_function({"lib"}, *fd, {}, ctx.m_voidt(), ta2);
    CHECK_NE(s1, s2);
}

TEST_CASE("default vs stdcall produce different manglings")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd1 = make_func(actx, "proc");
    auto* fd2 = make_func(actx, "proc", false, "stdcall");
    auto s1 = mangle::mangle_function({"win"}, *fd1, {}, ctx.m_voidt());
    auto s2 = mangle::mangle_function({"win"}, *fd2, {}, ctx.m_voidt());
    CHECK_NE(s1, s2);
}

SECTION("equality");

TEST_CASE("structurally equal aggregate NTTPs mangle same")
{
    types::TypeContext ctx;
    auto i32t = i32(ctx);
    auto t = ctx.array_t(i32t, 2);

    auto mk = [&](int a, int b) {
        std::vector<comptime::Value> e;
        e.push_back(comptime::Value::make_int(a, i32t));
        e.push_back(comptime::Value::make_int(b, i32t));
        return comptime::Value::make_aggregate(std::move(e), t);
    };

    CHECK_EQ(mangle::mangle_value(mk(5, 6)), mangle::mangle_value(mk(5, 6)));
}

TEST_CASE("alias transparency: same resolved type gives same mangling")
{
    types::TypeContext ctx;
    auto t1 = ctx.array_t(i32(ctx), 5);
    auto t2 = ctx.array_t(i32(ctx), 5);
    CHECK_EQ(t1, t2);
    CHECK_EQ(mangle::mangle_type(t1), mangle::mangle_type(t2));
}

SECTION("mangle_type: nominal alias");

TEST_CASE("mangle_type nominal alias Fd differs from i32")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* ud = actx.make<ast::UsingDecl>(dcc::sm::SourceRange{});
    ud->alias_path = ast::Path{actx.allocator()};
    ud->alias_path.segments.push_back({"Fd", {}});
    ud->sema.is_nominal = true;
    auto fd_t = ctx.nominal_alias_t(i32(ctx), ud);
    auto fd_str = mangle::mangle_type(fd_t, make_resolver({"m"}, "Fd"));
    auto i32_str = mangle::mangle_type(i32(ctx));
    CHECK_NE(fd_str, i32_str);
    CHECK_EQ(fd_str, "_DC0TNi32s1.1.m2.Fd");
}

TEST_CASE("mangle_type nominal alias Fd and Handle differ")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* fd_ud = actx.make<ast::UsingDecl>(dcc::sm::SourceRange{});
    fd_ud->alias_path = ast::Path{actx.allocator()};
    fd_ud->alias_path.segments.push_back({"Fd", {}});
    fd_ud->sema.is_nominal = true;
    auto* h_ud = actx.make<ast::UsingDecl>(dcc::sm::SourceRange{});
    h_ud->alias_path = ast::Path{actx.allocator()};
    h_ud->alias_path.segments.push_back({"Handle", {}});
    h_ud->sema.is_nominal = true;
    auto fd_t = ctx.nominal_alias_t(i32(ctx), fd_ud);
    auto h_t = ctx.nominal_alias_t(i32(ctx), h_ud);
    auto fd_str = mangle::mangle_type(fd_t, make_resolver({"m"}, "Fd"));
    auto h_str = mangle::mangle_type(h_t, make_resolver({"m"}, "Handle"));
    auto i32_str = mangle::mangle_type(i32(ctx));
    CHECK_NE(fd_str, h_str);
    CHECK_NE(fd_str, i32_str);
    CHECK_NE(h_str, i32_str);
}

TEST_CASE("mangle_type transparent using alias resolves to same mangling as underlying")
{
    types::TypeContext ctx;
    auto i32_str = mangle::mangle_type(i32(ctx));
    CHECK_EQ(i32_str, "_DC0Ti32s");
}

TEST_CASE("demangle nominal alias Fd round-trip")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* ud = actx.make<ast::UsingDecl>(dcc::sm::SourceRange{});
    ud->alias_path = ast::Path{actx.allocator()};
    ud->alias_path.segments.push_back({"Fd", {}});
    ud->sema.is_nominal = true;
    auto fd_t = ctx.nominal_alias_t(i32(ctx), ud);
    auto s = mangle::mangle_type(fd_t, make_resolver({"m"}, "Fd"));
    CHECK(mangle::demangle(s).has_value());
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.name, "Fd");
    CHECK_EQ(d.type_only.module_path.size(), 1u);
    CHECK_EQ(d.type_only.module_path[0], "m");
}

SECTION("mangle_type: restricted");

TEST_CASE("restricted types normalize, print, and intern")
{
    types::TypeContext ctx;
    auto* u8t = static_cast<types::IntType const*>(ctx.int_t(8, false));
    std::array a{types::RestrictionInterval{64, 64}, types::RestrictionInterval{32, 32}, types::RestrictionInterval{32, 32}};
    std::array b{types::RestrictionInterval{32, 32}, types::RestrictionInterval{64, 64}};
    auto t1 = ctx.restricted_t(u8t, a);
    auto t2 = ctx.restricted_t(u8t, b);
    CHECK_EQ(t1, t2);
    CHECK_EQ(dcc::sema::format_dcc_type(t1), "u8{32, 64}");
}

TEST_CASE("restricted normalization merges overlap but not adjacency")
{
    types::TypeContext ctx;
    auto* u8t = static_cast<types::IntType const*>(ctx.int_t(8, false));
    std::array intervals{types::RestrictionInterval{40, 50}, types::RestrictionInterval{32, 45}, types::RestrictionInterval{51, 51}};
    auto t = ctx.restricted_t(u8t, intervals);
    auto const* rt = types::type_cast<types::RestrictedType>(t);
    REQUIRE(rt != nullptr);
    CHECK_EQ(rt->intervals.size(), 2u);
    CHECK_EQ(dcc::sema::format_dcc_type(t), "u8{32..50, 51}");
}

TEST_CASE("restricted signed printing uses underlying values")
{
    types::TypeContext ctx;
    auto* i8t = static_cast<types::IntType const*>(ctx.int_t(8, true));
    std::array intervals{types::RestrictionInterval{127, 127}, types::RestrictionInterval{128, 128}, types::RestrictionInterval{129, 129}};
    auto t = ctx.restricted_t(i8t, intervals);
    CHECK_EQ(dcc::sema::format_dcc_type(t), "i8{-1, 0, 1}");
}

TEST_CASE("restricted types mangle distinctly and round-trip")
{
    types::TypeContext ctx;
    auto* u64t = static_cast<types::IntType const*>(ctx.int_t(64, false));
    std::array first{types::RestrictionInterval{1, 5}, types::RestrictionInterval{10, 20}};
    std::array second{types::RestrictionInterval{1, 6}, types::RestrictionInterval{10, 20}};
    auto t1 = ctx.restricted_t(u64t, first);
    auto t2 = ctx.restricted_t(u64t, second);
    auto s1 = mangle::mangle_type(t1);
    auto s2 = mangle::mangle_type(t2);
    CHECK_EQ(s1, "_DC0TGi64u2.1-5.10-20");
    CHECK_NE(s1, s2);

    auto d = mangle::demangle(s1);
    REQUIRE(d.has_value());
    CHECK_EQ(d->type_only.tag, mangle::DemangledType::Tag::Restricted);
    REQUIRE(d->type_only.underlying != nullptr);
    CHECK_EQ(d->type_only.underlying->tag, mangle::DemangledType::Tag::Int);
    CHECK_EQ(d->type_only.restriction_intervals.size(), 2u);
    CHECK_EQ(d->type_only.restriction_intervals[0], (types::RestrictionInterval{1, 5}));
    CHECK_EQ(d->type_only.restriction_intervals[1], (types::RestrictionInterval{10, 20}));
}

TEST_CASE("restricted signed ordinals have stable mangling")
{
    types::TypeContext ctx;
    auto* i8t = static_cast<types::IntType const*>(ctx.int_t(8, true));
    std::array intervals{types::RestrictionInterval{118, 138}};
    auto t = ctx.restricted_t(i8t, intervals);
    CHECK_EQ(mangle::mangle_type(t), "_DC0TGi8s1.118-138");
    CHECK(demangle_check("_DC0TGi8s1.118-138"));
}

TEST_CASE("demangle rejects malformed restricted types")
{
    CHECK(!demangle_check("_DC0TG"));
    CHECK(!demangle_check("_DC0TGi64u0."));
    CHECK(!demangle_check("_DC0TGi64u1.1"));
    CHECK(!demangle_check("_DC0TGi64u1.1-"));
    CHECK(!demangle_check("_DC0TGi64u1.5-1"));
    CHECK(!demangle_check("_DC0TGi64u2.1-5.4-8"));
}

SECTION("UTF-8 encoding");

TEST_CASE("UTF-8 struct name")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "föo", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Struct, sd);
    auto s = mangle::mangle_type(t, make_resolver({"utf"}, "föo"));
    CHECK_EQ(s, "_DC0TD1.3.utf8.f$C3$B6o0.");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.name, "föo");
}

TEST_CASE("UTF-8 string value")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_string("héllo"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None));
    auto s = mangle::mangle_value(v);
    CHECK_EQ(s, "_DC0VSPqc10.h$C3$A9llo");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.string_val, "héllo");
}

TEST_CASE("identifier with dollar sign")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "a$b", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Struct, sd);
    auto s = mangle::mangle_type(t, make_resolver({}, "a$b"));
    CHECK_EQ(s, "_DC0TD0.4.a$$b0.");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.name, "a$b");
}

TEST_CASE("identifier with dot")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "a.b", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Struct, sd);
    auto s = mangle::mangle_type(t, make_resolver({}, "a.b"));
    CHECK_EQ(s, "_DC0TD0.5.a$2Eb0.");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.type_only.name, "a.b");
}

SECTION("aggregate NTTP edge cases");

TEST_CASE("empty aggregate")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_aggregate({}, ctx.array_t(i32(ctx), 0));
    auto s = mangle::mangle_value(v);
    CHECK_EQ(s, "_DC0VAA0i32s0");
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK(d.value_only.elements.empty());
}

TEST_CASE("nested aggregate")
{
    types::TypeContext ctx;
    auto i32t = i32(ctx);
    auto inner_t = ctx.array_t(i32t, 2);
    auto outer_t = ctx.array_t(inner_t, 2);

    auto mk_inner = [&](int a, int b) {
        std::vector<comptime::Value> e;
        e.push_back(comptime::Value::make_int(a, i32t));
        e.push_back(comptime::Value::make_int(b, i32t));
        return comptime::Value::make_aggregate(std::move(e), inner_t);
    };

    std::vector<comptime::Value> outer_elems;
    outer_elems.push_back(mk_inner(1, 2));
    outer_elems.push_back(mk_inner(3, 4));
    auto v = comptime::Value::make_aggregate(std::move(outer_elems), outer_t);
    auto s = mangle::mangle_value(v);
    CHECK(s.starts_with("_DC0VA"));
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));
    CHECK_EQ(d.value_only.elements.size(), 2u);
    CHECK_EQ(d.value_only.elements[0].elements.size(), 2u);
    CHECK_EQ(d.value_only.elements[0].elements[0].int_val, 1);
    CHECK_EQ(d.value_only.elements[0].elements[1].int_val, 2);
    CHECK_EQ(d.value_only.elements[1].elements[0].int_val, 3);
    CHECK_EQ(d.value_only.elements[1].elements[1].int_val, 4);
}

TEST_CASE("mixed primitive/aggregate/slice NTTP")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto i32t = i32(ctx);
    auto f64t = f64(ctx);
    auto sl_t = ctx.slice_t(i32t, types::Qual::None);

    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "Mixed", dcc::sm::SourceRange{});
    auto mixed_t = ctx.nominal_t(types::TypeKind::Struct, sd);

    std::vector<comptime::Value> sl_elems;
    sl_elems.push_back(comptime::Value::make_int(10, i32t));
    sl_elems.push_back(comptime::Value::make_int(20, i32t));

    std::vector<comptime::Value> outer;
    outer.push_back(comptime::Value::make_int(42, i32t));
    outer.push_back(comptime::Value::make_float(3.14, f64t));
    outer.push_back(comptime::Value::make_slice(std::move(sl_elems), sl_t));

    auto v = comptime::Value::make_aggregate(std::move(outer), mixed_t);
    auto s = mangle::mangle_value(v);
    CHECK(s.starts_with("_DC0VA"));
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, s));

    CHECK_EQ(d.value_only.elements.size(), 3u);
    CHECK_EQ(d.value_only.elements[0].int_val, 42);
    CHECK(std::abs(d.value_only.elements[1].float_val - 3.14) < 1e-10);
    CHECK_EQ(d.value_only.elements[2].elements.size(), 2u);
}

SECTION("demangler structural");

TEST_CASE("demangle function details")
{
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, "_DC0F1.4.math3.add2i32si32si32s"));
    CHECK_EQ(d.kind, mangle::DemangledName::Kind::Function);
    CHECK_EQ(d.module_path.size(), 1u);
    CHECK_EQ(d.module_path[0], "math");
    CHECK_EQ(d.name, "add");
    CHECK_EQ(d.param_types.size(), 2u);
    CHECK(d.calling_conv.empty());
    CHECK(d.template_args.empty());
}

TEST_CASE("demangle function with calling convention")
{
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, "_DC0F1.5.win323.foo0vc1"));
    CHECK_EQ(d.calling_conv, "stdcall");
}

TEST_CASE("demangle function with template args")
{
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, "_DC0F1.3.lib3.foo1i32svX1ti32s"));
    CHECK_EQ(d.template_args.size(), 1u);
    CHECK_EQ(d.template_args[0].kind, mangle::DemangledTemplateArg::Kind::Type);
}

TEST_CASE("demangle global details")
{
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, "_DC0G1.3.app1.xi32s"));
    CHECK_EQ(d.kind, mangle::DemangledName::Kind::Global);
    CHECK_EQ(d.name, "x");
}

TEST_CASE("demangle type-only")
{
    mangle::DemangledName d;
    REQUIRE(mangle::demangle(d, "_DC0Ti32s"));
    CHECK_EQ(d.kind, mangle::DemangledName::Kind::Type);
}

TEST_CASE("demangle rejects unterminated quals")
{
    CHECK(!demangle_check("_DC0TPC"));
    CHECK(!demangle_check("_DC0TPCV"));
}

TEST_CASE("demangle rejects bad decimal")
{
    CHECK(!demangle_check("_DC0Tia"));
}

TEST_CASE("demangle rejects too-short float hex")
{
    CHECK(!demangle_check("_DC0VFf64.ABCDEF"));
}

TEST_CASE("demangle rejects incomplete after quals")
{
    CHECK(!demangle_check("_DC0TPq"));
}

TEST_CASE("demangle rejects trailing after end of function")
{
    CHECK(!demangle_check("_DC0F1.4.root4.main0v_trailing"));
}

SECTION("nominal resolver");

TEST_CASE("mangle_type struct without resolver uses empty name")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "MyStruct", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Struct, sd);
    auto s = mangle::mangle_type(t);
    CHECK_EQ(s, "_DC0TD0.0.0.");
}

TEST_CASE("mangle_type struct resolver with empty module path")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "Local", dcc::sm::SourceRange{});
    auto t = ctx.nominal_t(types::TypeKind::Struct, sd);
    auto s = mangle::mangle_type(t, make_resolver({}, "Local"));
    CHECK_EQ(s, "_DC0TD0.5.Local0.");
}

SECTION("mangle_type_specialization");

TEST_CASE("mangle_type_specialization Optional<i32>")
{
    types::TypeContext ctx;
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    auto s = mangle::mangle_type_specialization({"core"}, "Optional", targs);
    CHECK_EQ(s, "_DC0Y1.4.core8.OptionalX1ti32s");
}

TEST_CASE("mangle_type_specialization Result<i32, f64>")
{
    types::TypeContext ctx;
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, f64(ctx), nullptr});
    auto s = mangle::mangle_type_specialization({"core"}, "Result", targs);
    CHECK_EQ(s, "_DC0Y1.4.core6.ResultX2ti32stf64.");
}

TEST_CASE("mangle_type_specialization with value template arg Array<i32, 42>")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Value, i32(ctx), actx.own_value(comptime::Value::make_int(42, i32(ctx)))});
    auto s = mangle::mangle_type_specialization({"std"}, "Array", targs);
    CHECK_EQ(s, "_DC0Y1.3.std5.ArrayX2ti32svIi32s42");
}

TEST_CASE("mangle_type_specialization empty template args")
{
    types::TypeContext ctx;
    auto s = mangle::mangle_type_specialization({"lib"}, "NonTemplate");
    CHECK_EQ(s, "_DC0Y1.3.lib11.NonTemplate");
    auto d = mangle::demangle(s);
    REQUIRE(d.has_value());
    CHECK_EQ(d->kind, mangle::DemangledName::Kind::TypeSpec);
    CHECK_EQ(d->module_path.size(), 1u);
    CHECK_EQ(d->module_path[0], "lib");
    CHECK_EQ(d->name, "NonTemplate");
    CHECK(d->template_args.empty());
}

TEST_CASE("mangle_type_specialization round-trip demangle")
{
    types::TypeContext ctx;
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    auto s = mangle::mangle_type_specialization({"core"}, "Optional", targs);
    CHECK_EQ(s, "_DC0Y1.4.core8.OptionalX1ti32s");
    auto d = mangle::demangle(s);
    REQUIRE(d.has_value());
    CHECK_EQ(d->kind, mangle::DemangledName::Kind::TypeSpec);
    CHECK_EQ(d->module_path.size(), 1u);
    CHECK_EQ(d->module_path[0], "core");
    CHECK_EQ(d->name, "Optional");
    CHECK_EQ(d->template_args.size(), 1u);
    CHECK_EQ(d->template_args[0].kind, mangle::DemangledTemplateArg::Kind::Type);
    CHECK_EQ(d->template_args[0].type.tag, mangle::DemangledType::Tag::Int);
    CHECK_EQ(d->template_args[0].type.bits, 32u);
    CHECK(d->template_args[0].type.is_signed);
}

TEST_CASE("demangle rejects invalid _DC0Y strings")
{
    CHECK(!demangle_check("_DC0Y"));
    CHECK(!demangle_check("_DC0Y1.4.core"));
    CHECK(!demangle_check("_DC0Y1.4.core8.OptionalX"));
    CHECK(!demangle_check("_DC0Y1.4.core8.OptionalX1"));
    CHECK(!demangle_check("_DC0Y1.4.core8.OptionalX1ti32s_extra"));
}

TEST_CASE("mangle_type_specialization multi-segment module path")
{
    types::TypeContext ctx;
    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    auto s = mangle::mangle_type_specialization({"std", "collections"}, "Vec", targs);
    CHECK_EQ(s, "_DC0Y2.3.std11.collections3.VecX1ti32s");
}

TEST_CASE("mangle_type_specialization with user type resolution")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* sd = actx.make<ast::StructDecl>(dcc::sm::SourceRange{}, "String", dcc::sm::SourceRange{});
    auto string_t = ctx.nominal_t(types::TypeKind::Struct, sd);
    auto resolver = make_resolver({"core"}, "String");

    std::vector<mangle::TemplateArg> targs;
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, i32(ctx), nullptr});
    targs.push_back(mangle::TemplateArg{mangle::TemplateArg::Kind::Type, string_t, nullptr});
    auto s = mangle::mangle_type_specialization({"core"}, "Result", targs, resolver);
    CHECK(s.starts_with("_DC0Y1.4.core6.ResultX2ti32stD"));
    auto d = mangle::demangle(s);
    REQUIRE(d.has_value());
    CHECK_EQ(d->kind, mangle::DemangledName::Kind::TypeSpec);
    CHECK_EQ(d->name, "Result");
    CHECK_EQ(d->template_args.size(), 2u);
    CHECK_EQ(d->template_args[0].kind, mangle::DemangledTemplateArg::Kind::Type);
    CHECK_EQ(d->template_args[1].kind, mangle::DemangledTemplateArg::Kind::Type);
    CHECK_EQ(d->template_args[1].type.name, "String");
}

SECTION("mangle_lambda_function");

TEST_CASE("mangle_lambda_function uses distinct _DC0L path and source identity")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* l = actx.make<ast::LambdaExpr>(dcc::sm::SourceRange{dcc::sm::Location{dcc::sm::FileId{3}, 512}, dcc::sm::Location{dcc::sm::FileId{3}, 520}},
                                         actx.allocator());
    auto* fd = actx.make<ast::FuncDecl>(l->range, "__lambda_0", l->range);
    fd->synthesized_lambda = true;
    fd->lambda_source = l;
    std::vector<types::TypePtr> params = {i32(ctx)};
    auto s = mangle::mangle_function({"main"}, *fd, params, i32(ctx));
    CHECK_EQ(s, "_DC0L1.4.main1.33.5121i32si32s");
    auto d = mangle::demangle(s);
    REQUIRE(d.has_value());
    CHECK_EQ(d->kind, mangle::DemangledName::Kind::Lambda);
    CHECK_EQ(d->module_path.size(), 1u);
    CHECK_EQ(d->module_path[0], "main");
    CHECK_EQ(d->param_types.size(), 1u);
}

TEST_CASE("mangle_lambda_function distinct from same-named source function")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* l = actx.make<ast::LambdaExpr>(dcc::sm::SourceRange{dcc::sm::Location{dcc::sm::FileId{3}, 512}, dcc::sm::Location{dcc::sm::FileId{3}, 520}},
                                         actx.allocator());
    auto* fd = actx.make<ast::FuncDecl>(l->range, "__lambda_0", l->range);
    fd->synthesized_lambda = true;
    fd->lambda_source = l;
    std::vector<types::TypePtr> params = {i32(ctx)};
    auto lambda_s = mangle::mangle_function({"main"}, *fd, params, i32(ctx));

    auto* src = actx.make<ast::FuncDecl>(dcc::sm::SourceRange{}, "__lambda_0", dcc::sm::SourceRange{});
    auto src_s = mangle::mangle_function({"main"}, *src, params, i32(ctx));

    CHECK_NE(lambda_s, src_s);
    CHECK(lambda_s.starts_with("_DC0L"));
    CHECK(src_s.starts_with("_DC0F"));
}

TEST_CASE("mangle_lambda_function same-signature lambdas differ")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* l1 = actx.make<ast::LambdaExpr>(dcc::sm::SourceRange{dcc::sm::Location{dcc::sm::FileId{3}, 100}, dcc::sm::Location{dcc::sm::FileId{3}, 108}},
                                          actx.allocator());
    auto* fd1 = actx.make<ast::FuncDecl>(l1->range, "__lambda_0", l1->range);
    fd1->synthesized_lambda = true;
    fd1->lambda_source = l1;
    auto* l2 = actx.make<ast::LambdaExpr>(dcc::sm::SourceRange{dcc::sm::Location{dcc::sm::FileId{3}, 300}, dcc::sm::Location{dcc::sm::FileId{3}, 308}},
                                          actx.allocator());
    auto* fd2 = actx.make<ast::FuncDecl>(l2->range, "__lambda_1", l2->range);
    fd2->synthesized_lambda = true;
    fd2->lambda_source = l2;
    std::vector<types::TypePtr> params = {i32(ctx)};
    auto s1 = mangle::mangle_function({"main"}, *fd1, params, i32(ctx));
    auto s2 = mangle::mangle_function({"main"}, *fd2, params, i32(ctx));
    CHECK_NE(s1, s2);
    CHECK_EQ(s1, "_DC0L1.4.main1.33.1001i32si32s");
    CHECK_EQ(s2, "_DC0L1.4.main1.33.3001i32si32s");
}

SECTION("mangle_type Lambda");

TEST_CASE("mangle_type Lambda encodes stable source identity")
{
    types::TypeContext ctx;
    ast::AstContext actx;
    auto* l = actx.make<ast::LambdaExpr>(dcc::sm::SourceRange{dcc::sm::Location{dcc::sm::FileId{2}, 77}, dcc::sm::Location{dcc::sm::FileId{2}, 85}},
                                         actx.allocator());
    auto lt = ctx.lambda_t(l);
    auto s = mangle::mangle_type(lt);
    CHECK_EQ(s, "_DC0TL1.22.77");
    auto d = mangle::demangle(s);
    REQUIRE(d.has_value());
    CHECK_EQ(d->kind, mangle::DemangledName::Kind::Type);
    CHECK_EQ(d->type_only.tag, mangle::DemangledType::Tag::Lambda);
}

TEST_CASE("demangle rejects invalid _DC0L strings")
{
    CHECK(!demangle_check("_DC0L"));
    CHECK(!demangle_check("_DC0L1.4.main"));
    CHECK(!demangle_check("_DC0L1.4.main1.33.512"));
    CHECK(!demangle_check("_DC0L1.4.main1.33.5121i32si32s_extra"));
}
