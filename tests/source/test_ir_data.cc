import std;
import dcc.ir;
import dcc.ir.data;
import dcc.ir.pass;
import dcc.ir.pipeline;
import dcc.target;

#include "harness.hh"

using namespace dcc::ir;

SECTION("relocation-aware constant data");

TEST_CASE("pointer widths and addends survive serialization")
{
    for (auto bits : {16, 32, 64})
    {
        auto target = dcc::target::TargetConfig::host_default();
        target.pointer_bits = static_cast<std::uint8_t>(bits);
        target.pointer_align = static_cast<std::uint8_t>(bits / 8);
        IrContext context(4096, &target);
        auto* pointer = context.pointer_to(context.int_t(8, false));
        auto* reference = context.symbol_ref("target", pointer, 3);
        std::string error;
        auto* data = materialize_constant_data(context, pointer, reference, error);
        REQUIRE(data != nullptr);
        CHECK_EQ(data->bytes.size(), static_cast<std::size_t>(bits / 8));
        CHECK_EQ(data->relocations.size(), 1u);
        CHECK_EQ(data->relocations[0].width, static_cast<std::uint64_t>(bits / 8));
        CHECK(data->relocations[0].reference == reference);
        CHECK_EQ(data->relocations[0].reference->addend, 3);
    }
}

TEST_CASE("embedding arrays rebases every relocation and preserves padding")
{
    IrContext context;
    auto* byte = context.int_t(8, false);
    auto* pointer = context.pointer_to(byte);
    std::array<IrType const*, 2> members{byte, pointer};
    std::array<std::uint64_t, 2> offsets{0, 8};
    auto* type = context.aggregate_t(members, offsets, 16, 8);
    auto* aggregate = context.aggregate(type);
    aggregate->values = {context.int_const(byte, 42), context.symbol_ref("text", pointer, 2)};
    std::string error;
    auto* data = materialize_constant_data(context, type, aggregate, error);
    REQUIRE(data != nullptr);
    auto* array = context.aggregate(context.array_t(type, 2));
    array->values = {data, data};
    auto* embedded = materialize_constant_data(context, array->type, array, error);
    REQUIRE(embedded != nullptr);
    CHECK_EQ(embedded->relocations.size(), 2u);
    CHECK_EQ(embedded->relocations[0].offset, 8u);
    CHECK_EQ(embedded->relocations[1].offset, 24u);
    CHECK_EQ(embedded->bytes[0], 42u);
    CHECK_EQ(embedded->bytes[16], 42u);
    CHECK_EQ(embedded->bytes[7], 0u);
    CHECK_EQ(embedded->bytes.size(), 32u);
}

TEST_CASE("invalid relocation ranges and overlap are rejected")
{
    IrContext context;
    auto* type = context.pointer_to(context.int_t(8, false));
    auto* data = context.raw_constant(type);
    auto* reference = context.symbol_ref("symbol", type);
    data->relocations.push_back({0, 8, reference});
    CHECK(constant_data_error(*data).empty());
    data->relocations.push_back({0, 8, reference});
    CHECK(!constant_data_error(*data).empty());
    data->relocations.pop_back();
    data->relocations[0].offset = 1;
    CHECK(!constant_data_error(*data).empty());
    data->relocations[0] = {0, 3, reference};
    CHECK(!constant_data_error(*data).empty());
    data->relocations[0] = {0, 8, nullptr};
    CHECK(!constant_data_error(*data).empty());
    data->relocations.clear();
    data->bytes.pop_back();
    CHECK(!constant_data_error(*data).empty());
}

TEST_CASE("constant data follows target byte order")
{
    auto target = dcc::target::TargetConfig::host_default();
    target.little_endian = false;
    IrContext context(4096, &target);
    auto* type = context.int_t(32, false);
    std::string error;
    auto* data = materialize_constant_data(context, type, context.int_const(type, 0x12345678), error);
    REQUIRE(data != nullptr);
    CHECK_EQ(data->bytes[0], 0x12u);
    CHECK_EQ(data->bytes[3], 0x78u);
}

TEST_CASE("module cloning remaps symbolic references in constant data")
{
    IrContext context;
    auto* module = context.module("data");
    auto* byte = context.int_t(8, false);
    auto* target = context.global("target", byte, context.int_const(byte, 9), false);
    auto* pointer = context.pointer_to(byte);
    std::string error;
    auto* data = materialize_constant_data(context, pointer, context.global_ref(target, pointer, 1), error);
    auto* source = context.global("source", pointer, data, false);
    module->globals = {target, source};
    IrContext destination;
    auto* copy = dcc::ir::pass::global_pass_manager().run(*module, destination, dcc::ir::pass::OptLevel::O2);
    auto* cloned = ir_cast<IrRawConstant>(copy->globals[1]->init);
    REQUIRE(cloned != nullptr);
    CHECK(cloned != data);
    CHECK(cloned->relocations[0].reference->global == copy->globals[0]);
    CHECK_EQ(cloned->relocations[0].reference->addend, 1);
}

TEST_CASE("segmented pointers retain independent offset and segment fixups")
{
    auto target = *dcc::target::TargetConfig::parse_triple("i8086-binary");
    IrContext context(4096, &target);
    auto* pointer = context.pointer_to(context.int_t(8, false), Segment::None, PointerFlavor::Far);
    auto* reference = context.symbol_ref("data", pointer, 5);
    std::string error;
    auto* data = materialize_constant_data(context, pointer, reference, error);
    REQUIRE(data != nullptr);
    REQUIRE(data->relocations.size() == 2);
    CHECK_EQ(data->relocations[0].width, 2u);
    CHECK_EQ(data->relocations[1].offset, 2u);
    CHECK(data->relocations[1].kind == IrDataRelocationKind::Segment);
    CHECK(data->relocations[1].reference == reference);
}

TEST_CASE("function references retain symbol identity")
{
    IrContext context;
    auto* function = context.function("function", ir_type_cast<IrFuncType>(context.func_t(context.void_t(), {})));
    auto* reference = context.func_ref(function, -3);
    std::string error;
    auto* data = materialize_constant_data(context, reference->type, reference, error);
    REQUIRE(data != nullptr);
    CHECK(data->relocations[0].reference->function == function);
    CHECK_EQ(data->relocations[0].reference->addend, -3);
}

TEST_CASE("nonconstant and oversized values cannot become constant data")
{
    IrContext context;
    auto* byte = context.int_t(8, false);
    auto* integer = context.int_t(32, false);
    std::string error;
    CHECK(materialize_constant_data(context, byte, context.int_const(integer, 42), error) == nullptr);
    CHECK(!error.empty());
    CHECK(materialize_constant_data(context, integer, context.local("value", 0, integer), error) == nullptr);
    CHECK(!error.empty());
    CHECK(materialize_constant_data(context, nullptr, context.int_const(integer, 0), error) == nullptr);
}

TEST_CASE("based relocation widths follow the segmented offset model")
{
    auto target = *dcc::target::TargetConfig::parse_triple("i8086-binary");
    REQUIRE(!target.configure(dcc::target::CodeModel::Unreal, "i386"));
    IrContext context(4096, &target);
    auto* pointer = context.pointer_to(context.int_t(8, false), Segment::Ds, PointerFlavor::Based);
    std::string error;
    auto* data = materialize_constant_data(context, pointer, context.symbol_ref("data", pointer), error);
    REQUIRE(data != nullptr);
    CHECK_EQ(data->relocations[0].width, 4u);
    CHECK(constant_data_tree_error(data, pointer, 2, 4, true).empty());
    CHECK(!constant_data_tree_error(data, pointer, 4, 0, true).empty());
}
