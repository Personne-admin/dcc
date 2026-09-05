import std;
import dcc.types;
import dcc.comptime;

#include "harness.hh"

using namespace std::literals;

namespace types = dcc::types;
namespace comptime = dcc::comptime;

namespace
{
    types::TypePtr i32(types::TypeContext& ctx)
    {
        return ctx.int_t(32, true);
    }

    types::TypePtr f64(types::TypeContext& ctx)
    {
        return ctx.float_t(64);
    }

} // namespace

SECTION("comptime: Value construction");

TEST_CASE("construct Int")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(42, i32(ctx));
    CHECK_EQ(v.kind(), comptime::Value::Kind::Int);
    CHECK_EQ(v.type, i32(ctx));
    CHECK_EQ(v.get_int(), 42);
}

TEST_CASE("construct Float")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_float(3.14, f64(ctx));
    CHECK_EQ(v.kind(), comptime::Value::Kind::Float);
    CHECK_EQ(v.get_float(), 3.14);
}

TEST_CASE("construct Bool")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_bool(true, ctx.m_boolt());
    CHECK_EQ(v.kind(), comptime::Value::Kind::Bool);
    CHECK_EQ(v.get_bool(), true);

    auto f = comptime::Value::make_bool(false, ctx.m_boolt());
    CHECK_EQ(f.get_bool(), false);
}

TEST_CASE("construct Char")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_char(65, ctx.m_chart());
    CHECK_EQ(v.kind(), comptime::Value::Kind::Char);
    CHECK_EQ(v.get_char(), 65u);
}

TEST_CASE("construct Null")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_null(ctx.m_nullt());
    CHECK_EQ(v.kind(), comptime::Value::Kind::Null);
    CHECK_EQ(v.type, ctx.m_nullt());

    auto v2 = comptime::Value::make_null(ctx.pointer_to(i32(ctx), types::Qual::None));
    CHECK_EQ(v2.kind(), comptime::Value::Kind::Null);
}

TEST_CASE("construct String")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_string("hello"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None));
    CHECK_EQ(v.kind(), comptime::Value::Kind::String);
    CHECK_EQ(v.get_string(), "hello");
}

TEST_CASE("construct Aggregate")
{
    types::TypeContext ctx;
    auto i32t = i32(ctx);
    auto t = ctx.array_t(i32t, 3);
    std::vector<comptime::Value> elems;
    elems.push_back(comptime::Value::make_int(10, i32t));
    elems.push_back(comptime::Value::make_int(20, i32t));
    elems.push_back(comptime::Value::make_int(30, i32t));
    auto v = comptime::Value::make_aggregate(std::move(elems), t);
    CHECK_EQ(v.kind(), comptime::Value::Kind::Aggregate);
    CHECK_EQ(v.size(), 3u);
    CHECK_EQ(v.at(0).get_int(), 10);
    CHECK_EQ(v.at(1).get_int(), 20);
    CHECK_EQ(v.at(2).get_int(), 30);
}

TEST_CASE("construct Slice")
{
    types::TypeContext ctx;
    auto i32t = i32(ctx);
    auto t = ctx.slice_t(i32t, types::Qual::None);
    std::vector<comptime::Value> elems;
    elems.push_back(comptime::Value::make_int(1, i32t));
    elems.push_back(comptime::Value::make_int(2, i32t));
    auto v = comptime::Value::make_slice(std::move(elems), t);
    CHECK_EQ(v.kind(), comptime::Value::Kind::Slice);
    CHECK_EQ(v.size(), 2u);
}

TEST_CASE("construct Pointer")
{
    types::TypeContext ctx;
    auto t = ctx.pointer_to(i32(ctx), types::Qual::None);

    auto null_ptr = comptime::Value::make_pointer(t);
    CHECK_EQ(null_ptr.kind(), comptime::Value::Kind::Pointer);
    CHECK(null_ptr.is_null_ptr());

    auto non_null = comptime::Value::make_pointer_to(comptime::ValuePtr{false, 7, {3}}, t);
    CHECK_EQ(non_null.kind(), comptime::Value::Kind::Pointer);
    CHECK(!non_null.is_null_ptr());
    CHECK_EQ(non_null.get_pointer().allocation, 7u);
    CHECK_EQ(non_null.get_pointer().path, (std::vector<std::uint32_t>{3}));
}

SECTION("comptime: copy and move");

TEST_CASE("copy construct Int")
{
    types::TypeContext ctx;
    auto a = comptime::Value::make_int(99, i32(ctx));
    auto b = a;
    CHECK_EQ(b.kind(), comptime::Value::Kind::Int);
    CHECK_EQ(b.get_int(), 99);
    CHECK_EQ(b.type, a.type);
    CHECK_EQ(b, a);
}

TEST_CASE("move construct Int")
{
    types::TypeContext ctx;
    auto a = comptime::Value::make_int(55, i32(ctx));
    auto b = std::move(a);
    CHECK_EQ(b.kind(), comptime::Value::Kind::Int);
    CHECK_EQ(b.get_int(), 55);
}

TEST_CASE("copy assign Aggregate")
{
    types::TypeContext ctx;
    auto i32t = i32(ctx);
    auto t = ctx.array_t(i32t, 2);
    std::vector<comptime::Value> elems;
    elems.push_back(comptime::Value::make_int(1, i32t));
    elems.push_back(comptime::Value::make_int(2, i32t));
    auto a = comptime::Value::make_aggregate(std::move(elems), t);

    auto b = comptime::Value::make_int(0, i32t);
    b = a;
    CHECK_EQ(b.kind(), comptime::Value::Kind::Aggregate);
    CHECK_EQ(b.size(), 2u);
    CHECK_EQ(b.at(0).get_int(), 1);
    CHECK_EQ(b.at(1).get_int(), 2);
    CHECK_EQ(b, a);
}

TEST_CASE("move assign Aggregate")
{
    types::TypeContext ctx;
    auto i32t = i32(ctx);
    auto t = ctx.array_t(i32t, 2);
    std::vector<comptime::Value> elems;
    elems.push_back(comptime::Value::make_int(7, i32t));
    elems.push_back(comptime::Value::make_int(8, i32t));
    auto a = comptime::Value::make_aggregate(std::move(elems), t);
    auto expected = a;

    auto b = comptime::Value::make_int(0, i32t);
    b = std::move(a);
    CHECK_EQ(b.kind(), comptime::Value::Kind::Aggregate);
    CHECK_EQ(b.size(), 2u);
    CHECK_EQ(b, expected);
}

TEST_CASE("copy String round-trip")
{
    types::TypeContext ctx;
    auto a = comptime::Value::make_string("round-trip"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None));
    auto b = a;
    CHECK_EQ(b.get_string(), "round-trip");
    b = a;
    CHECK_EQ(b.get_string(), "round-trip");
}

SECTION("comptime: equality");

TEST_CASE("Int equality")
{
    types::TypeContext ctx;
    auto t = i32(ctx);
    auto a = comptime::Value::make_int(42, t);
    auto b = comptime::Value::make_int(42, t);
    auto c = comptime::Value::make_int(43, t);
    CHECK_EQ(a, b);
    CHECK_NE(a, c);
}

TEST_CASE("Float equality")
{
    types::TypeContext ctx;
    auto t = f64(ctx);
    auto a = comptime::Value::make_float(1.5, t);
    auto b = comptime::Value::make_float(1.5, t);
    auto c = comptime::Value::make_float(2.5, t);
    CHECK_EQ(a, b);
    CHECK_NE(a, c);
}

TEST_CASE("Bool equality")
{
    types::TypeContext ctx;
    auto t = ctx.m_boolt();
    CHECK_EQ(comptime::Value::make_bool(true, t), comptime::Value::make_bool(true, t));
    CHECK_NE(comptime::Value::make_bool(true, t), comptime::Value::make_bool(false, t));
}

TEST_CASE("Char equality")
{
    types::TypeContext ctx;
    auto t = ctx.m_chart();
    CHECK_EQ(comptime::Value::make_char(97, t), comptime::Value::make_char(97, t));
    CHECK_NE(comptime::Value::make_char(97, t), comptime::Value::make_char(98, t));
}

TEST_CASE("Null equality")
{
    types::TypeContext ctx;
    auto a = comptime::Value::make_null(ctx.m_nullt());
    auto b = comptime::Value::make_null(ctx.m_nullt());
    CHECK_EQ(a, b);
    auto c = comptime::Value::make_null(ctx.pointer_to(i32(ctx), types::Qual::None));
    CHECK_NE(a, c);
}

TEST_CASE("String equality")
{
    types::TypeContext ctx;
    auto t = ctx.pointer_to(ctx.m_chart(), types::Qual::None);
    auto a = comptime::Value::make_string("abc"s, t);
    auto b = comptime::Value::make_string("abc"s, t);
    auto c = comptime::Value::make_string("xyz"s, t);
    CHECK_EQ(a, b);
    CHECK_NE(a, c);
}

TEST_CASE("Aggregate equality")
{
    types::TypeContext ctx;
    auto t = ctx.array_t(i32(ctx), 3);
    auto mk = [&](int a, int b, int c) {
        std::vector<comptime::Value> elems;
        elems.push_back(comptime::Value::make_int(a, i32(ctx)));
        elems.push_back(comptime::Value::make_int(b, i32(ctx)));
        elems.push_back(comptime::Value::make_int(c, i32(ctx)));
        return comptime::Value::make_aggregate(std::move(elems), t);
    };

    auto x = mk(1, 2, 3);
    auto y = mk(1, 2, 3);
    auto z = mk(1, 2, 4);
    CHECK_EQ(x, y);
    CHECK_NE(x, z);
}

TEST_CASE("Slice equality")
{
    types::TypeContext ctx;
    auto t = ctx.slice_t(i32(ctx), types::Qual::None);
    auto mk = [&](int a, int b) {
        std::vector<comptime::Value> elems;
        elems.push_back(comptime::Value::make_int(a, i32(ctx)));
        elems.push_back(comptime::Value::make_int(b, i32(ctx)));
        return comptime::Value::make_slice(std::move(elems), t);
    };
    CHECK_EQ(mk(1, 2), mk(1, 2));
    CHECK_NE(mk(1, 2), mk(1, 3));
}

TEST_CASE("Pointer equality")
{
    types::TypeContext ctx;
    auto t = ctx.pointer_to(i32(ctx), types::Qual::None);

    auto a = comptime::Value::make_pointer(t);
    auto b = comptime::Value::make_pointer(t);
    CHECK_EQ(a, b);

    auto c = comptime::Value::make_pointer_to(comptime::ValuePtr{false, 1, {0}}, t);
    auto d = comptime::Value::make_pointer_to(comptime::ValuePtr{false, 1, {0}}, t);
    CHECK_EQ(c, d);

    auto e = comptime::Value::make_pointer_to(comptime::ValuePtr{false, 1, {1}}, t);
    CHECK_NE(c, e);

    auto f = comptime::Value::make_pointer_to(comptime::ValuePtr{false, 2, {0}}, t);
    CHECK_NE(c, f);
    CHECK_NE(a, c);
}

TEST_CASE("different kinds not equal")
{
    types::TypeContext ctx;
    auto t = i32(ctx);
    CHECK_NE(comptime::Value::make_int(0, t), comptime::Value::make_bool(false, ctx.m_boolt()));
}

SECTION("comptime: const_to_int");

TEST_CASE("const_to_int success")
{
    types::TypeContext ctx;
    CHECK_EQ(comptime::Value::make_int(42, i32(ctx)).const_to_int(), std::optional<std::int64_t>(42));
    CHECK_EQ(comptime::Value::make_bool(true, ctx.m_boolt()).const_to_int(), std::optional<std::int64_t>(1));
    CHECK_EQ(comptime::Value::make_bool(false, ctx.m_boolt()).const_to_int(), std::optional<std::int64_t>(0));
    CHECK_EQ(comptime::Value::make_char(65, ctx.m_chart()).const_to_int(), std::optional<std::int64_t>(65));
    CHECK_EQ(comptime::Value::make_float(3.0, f64(ctx)).const_to_int(), std::optional<std::int64_t>(3));
}

TEST_CASE("const_to_int failure")
{
    types::TypeContext ctx;
    CHECK(!comptime::Value::make_null(ctx.m_nullt()).const_to_int().has_value());
    CHECK(!comptime::Value::make_string("x"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None)).const_to_int().has_value());

    auto inf_val = comptime::Value::make_float(std::numeric_limits<double>::infinity(), f64(ctx));
    CHECK(!inf_val.const_to_int().has_value());

    auto nan_val = comptime::Value::make_float(std::numeric_limits<double>::quiet_NaN(), f64(ctx));
    CHECK(!nan_val.const_to_int().has_value());
}

SECTION("comptime: const_to_float");

TEST_CASE("const_to_float success")
{
    types::TypeContext ctx;
    auto val = comptime::Value::make_float(2.5, f64(ctx));
    CHECK_EQ(val.const_to_float(), std::optional<double>(2.5));
    CHECK_EQ(comptime::Value::make_int(7, i32(ctx)).const_to_float(), std::optional<double>(7.0));
    CHECK_EQ(comptime::Value::make_bool(true, ctx.m_boolt()).const_to_float(), std::optional<double>(1.0));
    CHECK_EQ(comptime::Value::make_char(65, ctx.m_chart()).const_to_float(), std::optional<double>(65.0));
}

TEST_CASE("const_to_float failure")
{
    types::TypeContext ctx;
    CHECK(!comptime::Value::make_null(ctx.m_nullt()).const_to_float().has_value());
    CHECK(!comptime::Value::make_string("x"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None)).const_to_float().has_value());
}

SECTION("comptime: const_to_bool");

TEST_CASE("const_to_bool success")
{
    types::TypeContext ctx;
    CHECK_EQ(comptime::Value::make_int(0, i32(ctx)).const_to_bool(), std::optional<bool>(false));
    CHECK_EQ(comptime::Value::make_int(1, i32(ctx)).const_to_bool(), std::optional<bool>(true));
    CHECK_EQ(comptime::Value::make_bool(true, ctx.m_boolt()).const_to_bool(), std::optional<bool>(true));
    CHECK_EQ(comptime::Value::make_bool(false, ctx.m_boolt()).const_to_bool(), std::optional<bool>(false));
    CHECK_EQ(comptime::Value::make_char(0, ctx.m_chart()).const_to_bool(), std::optional<bool>(false));
    CHECK_EQ(comptime::Value::make_char(1, ctx.m_chart()).const_to_bool(), std::optional<bool>(true));
    CHECK_EQ(comptime::Value::make_float(0.0, f64(ctx)).const_to_bool(), std::optional<bool>(false));
    CHECK_EQ(comptime::Value::make_float(3.14, f64(ctx)).const_to_bool(), std::optional<bool>(true));
}

TEST_CASE("const_to_bool failure")
{
    types::TypeContext ctx;
    CHECK(!comptime::Value::make_null(ctx.m_nullt()).const_to_bool().has_value());
    CHECK(!comptime::Value::make_string("x"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None)).const_to_bool().has_value());
}

SECTION("comptime: const_to_bits");

TEST_CASE("const_to_bits")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(-1, i32(ctx));
    auto bits = v.const_to_bits();
    REQUIRE(bits.has_value());
    CHECK_EQ(*bits, static_cast<std::uint64_t>(-1));
}

TEST_CASE("const_to_bits fails on Null")
{
    types::TypeContext ctx;
    CHECK(!comptime::Value::make_null(ctx.m_nullt()).const_to_bits().has_value());
}

SECTION("comptime: fold_int_binary");

TEST_CASE("fold_int_binary add")
{
    types::TypeContext ctx;
    auto r = comptime::Value::fold_int_binary(comptime::BinaryOp::Add, 10, 20, i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_int(), 30);
}

TEST_CASE("fold_int_binary sub")
{
    types::TypeContext ctx;
    auto r = comptime::Value::fold_int_binary(comptime::BinaryOp::Sub, 100, 30, i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_int(), 70);
}

TEST_CASE("fold_int_binary mul")
{
    types::TypeContext ctx;
    auto r = comptime::Value::fold_int_binary(comptime::BinaryOp::Mul, 6, 7, i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_int(), 42);
}

TEST_CASE("fold_int_binary div")
{
    types::TypeContext ctx;
    auto r = comptime::Value::fold_int_binary(comptime::BinaryOp::Div, 42, 7, i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_int(), 6);
}

TEST_CASE("fold_int_binary div by zero fails")
{
    types::TypeContext ctx;
    auto r = comptime::Value::fold_int_binary(comptime::BinaryOp::Div, 42, 0, i32(ctx));
    CHECK(!r.has_value());
}

TEST_CASE("fold_int_binary overflow add fails")
{
    types::TypeContext ctx;
    auto r = comptime::Value::fold_int_binary(comptime::BinaryOp::Add, std::numeric_limits<std::int64_t>::max(), 1, i32(ctx));
    CHECK(!r.has_value());
}

TEST_CASE("fold_int_binary bitwise")
{
    types::TypeContext ctx;
    auto r_and = comptime::Value::fold_int_binary(comptime::BinaryOp::BitAnd, 0xFF, 0x0F, i32(ctx));
    REQUIRE(r_and.has_value());
    CHECK_EQ(r_and->get_int(), 0x0F);

    auto r_or = comptime::Value::fold_int_binary(comptime::BinaryOp::BitOr, 0xF0, 0x0F, i32(ctx));
    REQUIRE(r_or.has_value());
    CHECK_EQ(r_or->get_int(), 0xFF);

    auto r_xor = comptime::Value::fold_int_binary(comptime::BinaryOp::BitXor, 0xFF, 0x0F, i32(ctx));
    REQUIRE(r_xor.has_value());
    CHECK_EQ(r_xor->get_int(), 0xF0);
}

TEST_CASE("fold_int_binary shift")
{
    types::TypeContext ctx;
    auto r_shl = comptime::Value::fold_int_binary(comptime::BinaryOp::Shl, 1, 8, i32(ctx));
    REQUIRE(r_shl.has_value());
    CHECK_EQ(r_shl->get_int(), 256);

    auto r_shr = comptime::Value::fold_int_binary(comptime::BinaryOp::Shr, 256, 8, i32(ctx));
    REQUIRE(r_shr.has_value());
    CHECK_EQ(r_shr->get_int(), 1);
}

TEST_CASE("fold_int_binary shift too large fails")
{
    types::TypeContext ctx;
    auto r = comptime::Value::fold_int_binary(comptime::BinaryOp::Shl, 1, 64, i32(ctx));
    CHECK(!r.has_value());
}

SECTION("comptime: fold_int_cmp");

TEST_CASE("fold_int_cmp")
{
    types::TypeContext ctx;
    auto t = ctx.m_boolt();

    auto eq = comptime::Value::fold_int_cmp(comptime::BinaryOp::Eq, 5, 5, t);
    REQUIRE(eq.has_value());
    CHECK(eq->get_bool());

    auto ne = comptime::Value::fold_int_cmp(comptime::BinaryOp::Ne, 5, 6, t);
    REQUIRE(ne.has_value());
    CHECK(ne->get_bool());

    auto lt = comptime::Value::fold_int_cmp(comptime::BinaryOp::Lt, 3, 5, t);
    REQUIRE(lt.has_value());
    CHECK(lt->get_bool());

    auto gt = comptime::Value::fold_int_cmp(comptime::BinaryOp::Gt, 5, 3, t);
    REQUIRE(gt.has_value());
    CHECK(gt->get_bool());
}

SECTION("comptime: fold_unary");

TEST_CASE("fold_unary plus int")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(42, i32(ctx));
    auto r = v.fold_unary(comptime::UnaryOp::Plus, i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_int(), 42);
}

TEST_CASE("fold_unary minus int")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(42, i32(ctx));
    auto r = v.fold_unary(comptime::UnaryOp::Minus, i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_int(), -42);
}

TEST_CASE("fold_unary not bool")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_bool(true, ctx.m_boolt());
    auto r = v.fold_unary(comptime::UnaryOp::Not, ctx.m_boolt());
    REQUIRE(r.has_value());
    CHECK(!r->get_bool());
}

TEST_CASE("fold_unary bitnot int")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(0, i32(ctx));
    auto r = v.fold_unary(comptime::UnaryOp::BitNot, i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_int(), ~0);
}

TEST_CASE("fold_unary minus INT64_MIN fails")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(std::numeric_limits<std::int64_t>::min(), i32(ctx));
    auto r = v.fold_unary(comptime::UnaryOp::Minus, i32(ctx));
    CHECK(!r.has_value());
}

SECTION("comptime: fold_binary");

TEST_CASE("fold_binary add int")
{
    types::TypeContext ctx;
    auto lhs = comptime::Value::make_int(10, i32(ctx));
    auto rhs = comptime::Value::make_int(20, i32(ctx));
    auto r = lhs.fold_binary(comptime::BinaryOp::Add, rhs, i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_int(), 30);
}

TEST_CASE("fold_binary add float")
{
    types::TypeContext ctx;
    auto lhs = comptime::Value::make_float(1.5, f64(ctx));
    auto rhs = comptime::Value::make_float(2.5, f64(ctx));
    auto r = lhs.fold_binary(comptime::BinaryOp::Add, rhs, f64(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_float(), 4.0);
}

TEST_CASE("fold_binary compare float")
{
    types::TypeContext ctx;
    auto lhs = comptime::Value::make_float(3.0, f64(ctx));
    auto rhs = comptime::Value::make_float(5.0, f64(ctx));
    auto r = lhs.fold_binary(comptime::BinaryOp::Lt, rhs, ctx.m_boolt());
    REQUIRE(r.has_value());
    CHECK(r->get_bool());
}

TEST_CASE("fold_binary compare mixed int char bool")
{
    types::TypeContext ctx;
    auto lhs = comptime::Value::make_char(65, ctx.m_chart());
    auto rhs = comptime::Value::make_int(65, i32(ctx));
    auto r = lhs.fold_binary(comptime::BinaryOp::Eq, rhs, ctx.m_boolt());
    REQUIRE(r.has_value());
    CHECK(r->get_bool());
}

TEST_CASE("fold_binary null eq null")
{
    types::TypeContext ctx;
    auto lhs = comptime::Value::make_null(ctx.m_nullt());
    auto rhs = comptime::Value::make_null(ctx.m_nullt());
    auto r = lhs.fold_binary(comptime::BinaryOp::Eq, rhs, ctx.m_boolt());
    REQUIRE(r.has_value());
    CHECK(r->get_bool());
}

TEST_CASE("fold_binary null ne null")
{
    types::TypeContext ctx;
    auto lhs = comptime::Value::make_null(ctx.m_nullt());
    auto rhs = comptime::Value::make_null(ctx.m_nullt());
    auto r = lhs.fold_binary(comptime::BinaryOp::Ne, rhs, ctx.m_boolt());
    REQUIRE(r.has_value());
    CHECK(!r->get_bool());
}

TEST_CASE("fold_binary non-null no result")
{
    types::TypeContext ctx;
    auto lhs = comptime::Value::make_int(1, i32(ctx));
    auto rhs = comptime::Value::make_string("x"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None));
    auto r = lhs.fold_binary(comptime::BinaryOp::Add, rhs, i32(ctx));
    CHECK(!r.has_value());
}

SECTION("comptime: fold_cast");

TEST_CASE("fold_cast int to bool")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(42, i32(ctx));
    auto r = v.fold_cast(ctx.m_boolt());
    REQUIRE(r.has_value());
    CHECK_EQ(r->kind(), comptime::Value::Kind::Bool);
    CHECK(r->get_bool());
}

TEST_CASE("fold_cast float to int")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_float(3.99, f64(ctx));
    auto r = v.fold_cast(i32(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->kind(), comptime::Value::Kind::Int);
    CHECK_EQ(r->get_int(), 3);
}

TEST_CASE("fold_cast int to float")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(42, i32(ctx));
    auto r = v.fold_cast(f64(ctx));
    REQUIRE(r.has_value());
    CHECK_EQ(r->kind(), comptime::Value::Kind::Float);
    CHECK_EQ(r->get_float(), 42.0);
}

TEST_CASE("fold_cast null to pointer")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_null(ctx.m_nullt());
    auto ptr_t = ctx.pointer_to(i32(ctx), types::Qual::None);
    auto r = v.fold_cast(ptr_t);
    REQUIRE(r.has_value());
    CHECK_EQ(r->kind(), comptime::Value::Kind::Null);
    CHECK_EQ(r->type, ptr_t);
}

TEST_CASE("fold_cast string fails")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_string("hi"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None));
    auto r = v.fold_cast(ctx.m_boolt());
    CHECK(!r.has_value());
}

SECTION("comptime: scalar mutation");

TEST_CASE("set_int mutates value")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(0, i32(ctx));
    v.set_int(77);
    CHECK_EQ(v.get_int(), 77);
}

TEST_CASE("set_float mutates value")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_float(0.0, f64(ctx));
    v.set_float(2.718);
    CHECK_EQ(v.get_float(), 2.718);
}

TEST_CASE("set_bool mutates value")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_bool(false, ctx.m_boolt());
    v.set_bool(true);
    CHECK(v.get_bool());
}

TEST_CASE("set_char mutates value")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_char(0, ctx.m_chart());
    v.set_char(255);
    CHECK_EQ(v.get_char(), 255u);
}

SECTION("comptime: aggregate/slice mutation");

TEST_CASE("aggregate push_back and at")
{
    types::TypeContext ctx;
    auto t = ctx.array_t(i32(ctx), 0);
    std::vector<comptime::Value> empty_vec;
    auto v = comptime::Value::make_aggregate(std::move(empty_vec), t);
    CHECK(v.empty());

    v.push_back(comptime::Value::make_int(1, i32(ctx)));
    v.push_back(comptime::Value::make_int(2, i32(ctx)));
    CHECK_EQ(v.size(), 2u);
    CHECK_EQ(v.at(0).get_int(), 1);
    CHECK_EQ(v.at(1).get_int(), 2);
    CHECK(!v.empty());
}

TEST_CASE("aggregate pop_back")
{
    types::TypeContext ctx;
    auto t = ctx.array_t(i32(ctx), 0);
    std::vector<comptime::Value> empty_vec;
    auto v = comptime::Value::make_aggregate(std::move(empty_vec), t);
    v.push_back(comptime::Value::make_int(10, i32(ctx)));
    v.push_back(comptime::Value::make_int(20, i32(ctx)));
    v.pop_back();
    CHECK_EQ(v.size(), 1u);
    CHECK_EQ(v.at(0).get_int(), 10);
}

TEST_CASE("slice push_back")
{
    types::TypeContext ctx;
    auto t = ctx.slice_t(i32(ctx), types::Qual::None);
    std::vector<comptime::Value> empty_vec2;
    auto v = comptime::Value::make_slice(std::move(empty_vec2), t);
    CHECK(v.empty());

    v.push_back(comptime::Value::make_int(100, i32(ctx)));
    CHECK_EQ(v.size(), 1u);
    CHECK_EQ(v.at(0).get_int(), 100);
}

TEST_CASE("aggregate at mutates in-place")
{
    types::TypeContext ctx;
    auto t = ctx.array_t(i32(ctx), 0);
    std::vector<comptime::Value> empty_vec3;
    auto v = comptime::Value::make_aggregate(std::move(empty_vec3), t);
    v.push_back(comptime::Value::make_int(0, i32(ctx)));
    v.at(0).set_int(999);
    CHECK_EQ(v.at(0).get_int(), 999);
}

SECTION("comptime: hash");

TEST_CASE("equal values have equal hashes")
{
    types::TypeContext ctx;
    auto a = comptime::Value::make_int(42, i32(ctx));
    auto b = comptime::Value::make_int(42, i32(ctx));
    CHECK_EQ(a.hash(), b.hash());

    auto s1 = comptime::Value::make_string("same"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None));
    auto s2 = comptime::Value::make_string("same"s, ctx.pointer_to(ctx.m_chart(), types::Qual::None));
    CHECK_EQ(s1.hash(), s2.hash());
}

TEST_CASE("different values have different hashes (likely)")
{
    types::TypeContext ctx;
    auto a = comptime::Value::make_int(1, i32(ctx));
    auto b = comptime::Value::make_int(2, i32(ctx));
    CHECK_NE(a.hash(), b.hash());
}

TEST_CASE("std::hash works")
{
    types::TypeContext ctx;
    auto v = comptime::Value::make_int(99, i32(ctx));
    std::hash<comptime::Value> h;
    CHECK_EQ(h(v), v.hash());
}
