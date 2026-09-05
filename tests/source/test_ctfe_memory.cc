import std;
import dcc.types;
import dcc.comptime;
import dcc.ctfe.memory;

#include "harness.hh"

using namespace std::literals;

namespace types = dcc::types;
namespace comptime = dcc::comptime;
namespace ctfe = dcc::ctfe;

namespace
{
    types::TypePtr i32(types::TypeContext& ctx)
    {
        return ctx.int_t(32, true);
    }

    comptime::Value array_of(types::TypeContext& ctx, std::span<std::int64_t const> values)
    {
        std::vector<comptime::Value> elements;
        elements.reserve(values.size());
        for (auto v : values)
            elements.push_back(comptime::Value::make_int(v, i32(ctx)));
        return comptime::Value::make_aggregate(std::move(elements), ctx.array_t(i32(ctx), values.size()));
    }

    comptime::ValuePtr first_element(ctfe::Heap& heap, comptime::ValuePtr const& object)
    {
        return heap.field(object, 0).value_or(comptime::ValuePtr{});
    }

} // namespace

SECTION("ctfe memory: allocation");

TEST_CASE("an allocation designates its object")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    auto object = heap.allocate(comptime::Value::make_int(7, i32(ctx)), i32(ctx), true);
    CHECK_EQ(heap.size(), 1u);
    CHECK(!object.is_null);
    CHECK_EQ(object.path.size(), 1u);

    auto const* value = heap.read(object);
    REQUIRE(value != nullptr);
    CHECK_EQ(value->get_int(), 7);
}

TEST_CASE("a null pointer reads nothing")
{
    ctfe::Heap heap;
    CHECK(heap.read(comptime::ValuePtr{}) == nullptr);
    CHECK(!heap.is_live(comptime::ValuePtr{}));
}

TEST_CASE("writes are rejected on read-only allocations")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    auto object = heap.allocate(comptime::Value::make_int(1, i32(ctx)), i32(ctx), false);
    CHECK(heap.write_target(object) == nullptr);
    CHECK(!heap.is_mutable(object));
}

TEST_CASE("ending a lifetime invalidates every pointer into the allocation")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    std::int64_t values[] = {1, 2, 3};
    auto object = heap.allocate(array_of(ctx, values), ctx.array_t(i32(ctx), 3), true);
    auto element = first_element(heap, object);
    REQUIRE(heap.read(element) != nullptr);

    heap.end_lifetime(object.allocation);
    CHECK(!heap.is_live(element));
    CHECK(heap.read(element) == nullptr);
    CHECK(heap.write_target(element) == nullptr);
}

TEST_CASE("identical literals share one allocation")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    std::int64_t values[] = {1, 2};
    auto type = ctx.array_t(i32(ctx), 2);
    auto a = heap.intern("ab", array_of(ctx, values), type);
    auto b = heap.intern("ab", array_of(ctx, values), type);
    auto c = heap.intern("cd", array_of(ctx, values), type);

    CHECK_EQ(a.allocation, b.allocation);
    CHECK_NE(a.allocation, c.allocation);
    CHECK(!heap.is_mutable(a));
}

SECTION("ctfe memory: subobjects");

TEST_CASE("a path walks into nested aggregates")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    std::int64_t inner[] = {10, 20};
    std::vector<comptime::Value> rows;
    rows.push_back(array_of(ctx, inner));
    rows.push_back(array_of(ctx, inner));
    auto type = ctx.array_t(ctx.array_t(i32(ctx), 2), 2);
    auto object = heap.allocate(comptime::Value::make_aggregate(std::move(rows), type), type, true);

    auto row = heap.field(object, 1);
    REQUIRE(row.has_value());
    auto cell = heap.field(*row, 1);
    REQUIRE(cell.has_value());

    auto const* value = heap.read(*cell);
    REQUIRE(value != nullptr);
    CHECK_EQ(value->get_int(), 20);

    *heap.write_target(*cell) = comptime::Value::make_int(99, i32(ctx));
    CHECK_EQ(heap.read(*cell)->get_int(), 99);
    CHECK_EQ(heap.read(object)->at(0).at(1).get_int(), 20);
}

TEST_CASE("out-of-range subobjects have no address")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    std::int64_t values[] = {1, 2};
    auto object = heap.allocate(array_of(ctx, values), ctx.array_t(i32(ctx), 2), true);
    CHECK(!heap.field(object, 2).has_value());
    CHECK(!heap.field(first_element(heap, object), 0).has_value());
}

SECTION("ctfe memory: pointer arithmetic");

TEST_CASE("offsets stay within the containing array")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    std::int64_t values[] = {1, 2, 3};
    auto object = heap.allocate(array_of(ctx, values), ctx.array_t(i32(ctx), 3), true);
    auto base = first_element(heap, object);

    CHECK_EQ(heap.container_length(base).value(), 3u);

    auto third = heap.offset(base, 2);
    REQUIRE(third.has_value());
    CHECK_EQ(heap.read(*third)->get_int(), 3);

    auto back = heap.offset(*third, -2);
    REQUIRE(back.has_value());
    CHECK_EQ(heap.read(*back)->get_int(), 1);

    CHECK(!heap.offset(base, -1).has_value());
    CHECK(!heap.offset(base, 4).has_value());
}

TEST_CASE("one past the end is addressable but not readable")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    std::int64_t values[] = {1, 2};
    auto object = heap.allocate(array_of(ctx, values), ctx.array_t(i32(ctx), 2), true);
    auto end = heap.offset(first_element(heap, object), 2);
    REQUIRE(end.has_value());
    CHECK(heap.read(*end) == nullptr);
}

TEST_CASE("a scalar allocation behaves as a single element")
{
    types::TypeContext ctx;
    ctfe::Heap heap;

    auto object = heap.allocate(comptime::Value::make_int(5, i32(ctx)), i32(ctx), true);
    CHECK_EQ(heap.container_length(object).value(), 1u);

    auto end = heap.offset(object, 1);
    REQUIRE(end.has_value());
    CHECK(heap.read(*end) == nullptr);
    CHECK(!heap.offset(object, 2).has_value());
}
