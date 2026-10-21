import std;
import dcc.ir;
import dcc.target;
import dcc.backend.object.image;
import dcc.backend.x86.encode;
import dcc.backend.x86.modasm;
import dcc.backend.i8086.place;

#include "harness.hh"

using namespace dcc::backend::object;
using dcc::backend::i8086::FlatImageLayout;
using dcc::backend::i8086::FlatLayoutOptions;

namespace
{
    struct Builder
    {
        ImageDescription image;

        FragmentId fragment(std::string name, std::string section, ImageRole role, std::vector<std::uint8_t> bytes, std::uint64_t memory = 0,
                            std::uint64_t alignment = 1, bool nobits = false, bool alloc = true)
        {
            ImageFragment part;
            part.id = image.fragments.size();
            part.name = std::move(name);
            part.section = std::move(section);
            part.role = role;
            part.alignment = alignment;
            part.bytes = std::move(bytes);
            part.memory_size = memory == 0 ? part.bytes.size() : memory;
            part.nobits = nobits;
            part.alloc = alloc;
            part.exec = role == ImageRole::Code || role == ImageRole::Startup;
            image.fragments.push_back(std::move(part));
            return image.fragments.back().id;
        }

        SymbolId symbol(std::string name, std::optional<FragmentId> part, std::uint64_t offset, ImageSymbolKind kind)
        {
            ImageSymbol value;
            value.id = image.symbols.size();
            value.name = std::move(name);
            value.fragment = part;
            value.offset = offset;
            value.kind = kind;
            image.symbols.push_back(std::move(value));
            return image.symbols.back().id;
        }

        void fixup(FragmentId owner, std::uint64_t offset, SymbolId target, std::uint64_t width, ImageExpression expression, std::int64_t addend = 0,
                   ImageUnits units = ImageUnits::Bytes, ImageSpace space = ImageSpace::Unknown)
        {
            ImageFixup value;
            value.offset = offset;
            value.width = width;
            value.target = target;
            value.addend = addend;
            value.expression = expression;
            value.units = units;
            value.space = space;
            image.fragments[owner].fixups.push_back(value);
        }
    };

    FlatLayoutOptions options(dcc::target::CodeModel model, std::uint16_t offset = 0x100, std::optional<std::uint16_t> segment = 0x1000,
                              std::uint32_t reserve = 4096)
    {
        return FlatLayoutOptions{.segment = segment, .offset = offset, .stack_reserve = reserve, .model = model};
    }

    std::uint64_t u16(std::uint8_t low, std::uint8_t high)
    {
        return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 8);
    }

} // namespace

TEST_CASE("i8086 flat layout mirrors the linker script section order")
{
    for (auto model : {dcc::target::CodeModel::Small, dcc::target::CodeModel::Unreal, dcc::target::CodeModel::Unreal32})
    {
        Builder builder;
        auto text = builder.fragment("main", ".text", ImageRole::Code, {0xC3});
        auto rodata = builder.fragment("string", ".rodata", ImageRole::Rodata, {0x41, 0x00}, 0, 2);
        auto relro = builder.fragment("reloc", ".data.rel.ro", ImageRole::Relro, {0x11, 0x00}, 0, 2);
        auto data = builder.fragment("value", ".data", ImageRole::Data, {0x22, 0x00}, 0, 4);
        auto bss = builder.fragment("scratch", ".bss", ImageRole::Bss, {}, 8, 8, true);
        builder.symbol("main", text, 0, ImageSymbolKind::Code);
        builder.symbol("string", rodata, 0, ImageSymbolKind::Data);
        builder.symbol("reloc", relro, 0, ImageSymbolKind::Data);
        builder.symbol("value", data, 0, ImageSymbolKind::Data);
        builder.symbol("scratch", bss, 0, ImageSymbolKind::Data);
        auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(model));
        REQUIRE(placed.has_value());
        CHECK(placed->sections.size() == 4);
        CHECK(placed->sections[0].name == ".text");
        CHECK_EQ(placed->sections[0].linear, 0x100u);
        CHECK_EQ(placed->sections[1].name, std::string{".rodata"});
        CHECK_EQ(placed->sections[1].linear, 0x102u);
        CHECK_EQ(placed->sections[2].name, std::string{".data"});
        CHECK_EQ(placed->sections[2].linear, 0x104u);
        CHECK_EQ(placed->sections[3].name, std::string{".bss"});
        CHECK_EQ(placed->sections[3].linear, 0x110u);
        CHECK_EQ(placed->code_end, 0x101u);
        CHECK_EQ(placed->bss_start, 0x110u);
        CHECK_EQ(placed->bss_end, 0x118u);
        CHECK_EQ(placed->memory_end, 0x118u);
        CHECK_EQ(placed->image_start, 0x100u);
        CHECK_EQ(placed->bytes.size(), 0xAu);
        CHECK_EQ(placed->bytes[0], 0xC3);
        CHECK_EQ(placed->bytes[2], 0x41);
        CHECK_EQ(placed->bytes[4], 0x11);
        CHECK_EQ(placed->bytes[8], 0x22);
        auto const* value = placed->find_symbol("value");
        REQUIRE(value != nullptr);
        CHECK(value->defined);
        CHECK_EQ(value->space, ImageSpace::Dgroup);
        CHECK_EQ(value->value, 0x108u);
        auto const* scratch = placed->find_symbol("scratch");
        REQUIRE(scratch != nullptr);
        CHECK_EQ(scratch->value, 0x110u);
        auto const* code_end = placed->find_symbol("__dcc_code_end");
        REQUIRE(code_end != nullptr);
        CHECK_EQ(code_end->value, 0x101u);
        auto const* image_end = placed->find_symbol("__image_end");
        REQUIRE(image_end != nullptr);
        CHECK_EQ(image_end->value, 0x118u);
        CHECK(placed->has_entry);
        CHECK_EQ(placed->entry, 0x100u);
        CHECK_EQ(placed->stack_bottom, 0xF000u);
        CHECK_EQ(placed->stack_top, 0x10000u);
        REQUIRE(placed->segments.size() == 2);
        CHECK_EQ(placed->segments[0].space, ImageSpace::Code);
        CHECK_EQ(placed->segments[1].space, ImageSpace::Dgroup);
        CHECK_EQ(placed->segments[0].paragraph, 0x1000u);
        CHECK_EQ(placed->segments[1].paragraph, 0x1000u);
        CHECK_EQ(placed->segments[0].linear_base, 0u);
        CHECK(placed->segments[0].known);
        CHECK_EQ(placed->segments[0].memory_end, placed->code_end);
        CHECK_EQ(placed->segments[1].memory_end, placed->memory_end);
    }
}

TEST_CASE("i8086 flat layout resolves typed code data and segment references")
{
    Builder builder;
    auto start = builder.fragment("_start", ".start", ImageRole::Startup, {0xE9, 0x00, 0x00, 0xB8, 0x00, 0x00}, 0, 1);
    auto text = builder.fragment("entry", ".text", ImageRole::Code, {0xC3});
    auto table = builder.fragment("table", ".rodata.table", ImageRole::Rodata, {0x00, 0x00}, 0, 2);
    auto value = builder.fragment("value", ".data", ImageRole::Data, {0x2A, 0x00}, 0, 2);
    auto entry_symbol = builder.symbol("entry", text, 0, ImageSymbolKind::Code);
    auto value_symbol = builder.symbol("value", value, 0, ImageSymbolKind::Data);
    builder.symbol("table", table, 0, ImageSymbolKind::Data);
    builder.fixup(start, 1, entry_symbol, 2, ImageExpression::PcRelative, -2);
    builder.fixup(start, 4, value_symbol, 2, ImageExpression::SegmentBase, 0, ImageUnits::Paragraphs, ImageSpace::Dgroup);
    builder.fixup(table, 0, entry_symbol, 2, ImageExpression::CodeOffset, 0, ImageUnits::Bytes, ImageSpace::Code);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small));
    REQUIRE(placed.has_value());
    CHECK_EQ(placed->sections[0].name, std::string{".start"});
    CHECK_EQ(placed->sections[0].linear, 0x100u);
    CHECK_EQ(placed->sections[1].linear, 0x106u);
    CHECK_EQ(placed->bytes.size(), 0xCu);
    CHECK_EQ(u16(placed->bytes[1], placed->bytes[2]), 0x106u - 0x0101u - 2u);
    CHECK_EQ(u16(placed->bytes[4], placed->bytes[5]), 0x1000u);
    CHECK_EQ(u16(placed->bytes[8], placed->bytes[9]), 0x106u);
    CHECK_EQ(placed->fixups.size(), 3u);
    CHECK_EQ(placed->fixups[0].value, 3);
    CHECK_EQ(placed->fixups[1].value, 0x1000);
    CHECK_EQ(placed->fixups[2].value, 0x106);
    CHECK_EQ(placed->fixups[2].expression, ImageExpression::CodeOffset);
    CHECK_EQ(placed->fixups[2].space, ImageSpace::Code);
    CHECK(placed->has_entry);
    CHECK_EQ(placed->entry, 0x100u);
}

TEST_CASE("i8086 flat layout diagnoses an unknown image segment")
{
    Builder builder;
    auto text = builder.fragment("entry", ".text", ImageRole::Code, {0xC3});
    auto pointer = builder.fragment("pointer", ".data", ImageRole::Data, {0x00, 0x00, 0x00, 0x00}, 0, 2);
    auto entry_symbol = builder.symbol("entry", text, 0, ImageSymbolKind::Code);
    auto segment_symbol = builder.symbol("__dcc_dgroup_segment", std::nullopt, 0, ImageSymbolKind::SegmentValue);
    builder.symbol("pointer", pointer, 0, ImageSymbolKind::Data);
    builder.fixup(pointer, 0, entry_symbol, 2, ImageExpression::SymbolOffset);
    builder.fixup(pointer, 2, segment_symbol, 2, ImageExpression::SegmentBase, 0, ImageUnits::Paragraphs, ImageSpace::Code);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small, 0x0000, std::nullopt));
    REQUIRE(!placed.has_value());
    CHECK(placed.error().contains("the image segment is unknown (-fbase=?:0000)"));
    CHECK(placed.error().contains("pointer"));
    auto known = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small, 0x0000, 0x0800));
    REQUIRE(known.has_value());
    CHECK(known->segments[0].known);
    CHECK_EQ(known->segments[0].paragraph, 0x0800u);
    CHECK_EQ(known->find_symbol("__dcc_dgroup_segment")->value, 0x0800u);
}

TEST_CASE("i8086 flat layout diagnoses unknown allocated sections")
{
    Builder builder;
    builder.fragment("vector", ".vectors", ImageRole::Data, {0x01, 0x00});
    builder.fragment("info", ".myinfo", ImageRole::Unknown, {0x02}, 0, 1, false, false);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small));
    REQUIRE(!placed.has_value());
    CHECK(placed.error().contains("allocated section .vectors"));
    CHECK(placed.error().contains(".start*, .text*"));
    CHECK(!placed.error().contains(".myinfo"));
}

TEST_CASE("i8086 flat layout diagnoses code and stack budget overflows")
{
    for (auto model : {dcc::target::CodeModel::Small, dcc::target::CodeModel::Unreal, dcc::target::CodeModel::Unreal32})
    {
        Builder builder;
        builder.fragment("huge", ".text", ImageRole::Code, std::vector<std::uint8_t>(0x10001, 0x90));
        auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(model, 0));
        REQUIRE(!placed.has_value());
        CHECK(placed.error().contains("i8086 code (.start and .text) does not fit below offset 0x10000"));
    }
    {
        Builder builder;
        builder.fragment("code", ".text", ImageRole::Code, {0xC3});
        builder.fragment("huge", ".bss", ImageRole::Bss, {}, 0xF100, 1, true);
        auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small, 0));
        REQUIRE(!placed.has_value());
        CHECK(placed.error().contains("i8086 small model: the image plus the 4096-byte stack reserve"));
    }
    {
        Builder builder;
        builder.fragment("code", ".text", ImageRole::Code, {0xC3});
        builder.fragment("huge", ".bss", ImageRole::Bss, {}, 0xF100, 1, true);
        auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Unreal, 0));
        REQUIRE(placed.has_value());
        CHECK_EQ(placed->sections[1].linear, 0x10000u);
        CHECK_EQ(placed->memory_end, 0x1F100u);
        CHECK_EQ(placed->sections[1].reason, std::string{"unreal stack band"});
    }
}

TEST_CASE("i8086 flat layout keeps unreal stack-band holes without truncating")
{
    Builder builder;
    auto text = builder.fragment("code", ".text", ImageRole::Code, std::vector<std::uint8_t>(16, 0xEE));
    auto rodata = builder.fragment("data", ".rodata", ImageRole::Rodata, {0x11, 0x22}, 0, 2);
    builder.symbol("code", text, 0, ImageSymbolKind::Code);
    builder.symbol("data", rodata, 0, ImageSymbolKind::Data);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Unreal, 0x10, 0x1000, 0xFFE0));
    REQUIRE(placed.has_value());
    CHECK_EQ(placed->sections[0].linear, 0x10u);
    CHECK_EQ(placed->sections[1].linear, 0x10000u);
    CHECK(placed->sections[1].reason == "unreal stack band");
    CHECK_EQ(placed->bytes.size(), 0xFFF2u);
    CHECK_EQ(placed->bytes[0], 0xEE);
    CHECK_EQ(placed->bytes[16], 0x00);
    CHECK_EQ(placed->bytes[0xFFF0], 0x11);
    CHECK_EQ(placed->bytes[0xFFF1], 0x22);
    CHECK(!placed->gaps.empty());
    CHECK(std::ranges::any_of(placed->gaps, [](auto const& gap) { return gap.reason == "unreal stack band" && gap.linear == 0x20; }));
    CHECK_EQ(placed->memory_end, 0x10002u);
    CHECK_EQ(placed->code_end, 0x20u);
    CHECK_EQ(placed->stack_bottom, 0x20u);

    auto small = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small, 0x10, 0x1000, 0xFFE0));
    REQUIRE(!small.has_value());
    CHECK(small.error().contains("i8086 small model"));
}

TEST_CASE("i8086 flat layout rejects values that do not fit their fields")
{
    Builder builder;
    auto text = builder.fragment("code", ".text", ImageRole::Code, {0x00, 0x00});
    auto rodata = builder.fragment("data", ".rodata", ImageRole::Rodata, {0x33, 0x44}, 0, 2);
    auto data_symbol = builder.symbol("data", rodata, 0, ImageSymbolKind::Data);
    builder.fixup(text, 0, data_symbol, 2, ImageExpression::SymbolOffset);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Unreal, 0x10, 0x1000, 0xFFED));
    REQUIRE(!placed.has_value());
    CHECK(placed.error().contains("does not fit in an unsigned 16-bit field"));
}

TEST_CASE("i8086 flat layout rejects overlapping fixups")
{
    Builder builder;
    auto text = builder.fragment("code", ".text", ImageRole::Code, {0x00, 0x00, 0x00});
    auto data = builder.fragment("data", ".data", ImageRole::Data, {0x01, 0x00}, 0, 2);
    auto symbol = builder.symbol("data", data, 0, ImageSymbolKind::Data);
    builder.fixup(text, 0, symbol, 2, ImageExpression::SymbolOffset);
    builder.fixup(text, 1, symbol, 2, ImageExpression::SymbolOffset);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small));
    REQUIRE(!placed.has_value());
    CHECK(placed.error().contains("overlapping fixups"));
}

TEST_CASE("i8086 flat layout checks the model marker without placing metadata")
{
    {
        Builder builder;
        auto marker = builder.symbol("__dcc_i8086_model_small", std::nullopt, 0, ImageSymbolKind::Data);
        auto metadata = builder.fragment("reference", ".dcc.i8086.model", ImageRole::Metadata, {0x00, 0x00}, 0, 2, false, false);
        builder.fixup(metadata, 0, marker, 2, ImageExpression::SymbolOffset);
        auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small));
        REQUIRE(placed.has_value());
        CHECK(placed->sections.empty());
        CHECK_EQ(placed->memory_end, 0x100u);
        auto const* resolved = placed->find_symbol("__dcc_i8086_model_small");
        REQUIRE(resolved != nullptr);
        CHECK(resolved->absolute);
        CHECK_EQ(resolved->value, 0u);
    }
    {
        Builder builder;
        auto marker = builder.symbol("__dcc_i8086_model_unreal", std::nullopt, 0, ImageSymbolKind::Data);
        auto metadata = builder.fragment("reference", ".dcc.i8086.model", ImageRole::Metadata, {0x00, 0x00}, 0, 2, false, false);
        builder.fixup(metadata, 0, marker, 2, ImageExpression::SymbolOffset);
        auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small));
        REQUIRE(!placed.has_value());
        CHECK(placed.error().contains("i8086 code model mismatch"));
        CHECK(placed.error().contains("__dcc_i8086_model_unreal"));
    }
}

TEST_CASE("i8086 flat layout aligns fragments and records padding")
{
    Builder builder;
    auto first = builder.fragment("first", ".data", ImageRole::Data, {0x01});
    auto second = builder.fragment("second", ".data", ImageRole::Data, {0x02}, 0, 16);
    auto code = builder.fragment("code", ".text", ImageRole::Code, {0xC3}, 0, 8);
    builder.symbol("code", code, 0, ImageSymbolKind::Code);
    builder.symbol("first", first, 0, ImageSymbolKind::Data);
    builder.symbol("second", second, 0, ImageSymbolKind::Data);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small, 0x101));
    REQUIRE(placed.has_value());
    CHECK_EQ(placed->sections[0].linear, 0x108u);
    CHECK_EQ(placed->sections[1].linear, 0x110u);
    CHECK_EQ(placed->find_symbol("first")->value, 0x110u);
    CHECK_EQ(placed->find_symbol("second")->value, 0x120u);
    CHECK(std::ranges::any_of(placed->gaps, [](auto const& gap) { return gap.reason == "fragment alignment"; }));
    CHECK(std::ranges::any_of(placed->gaps, [](auto const& gap) { return gap.reason == "alignment" && gap.linear == 0x101; }));
    CHECK(placed->fragments.size() == 3);
    CHECK(placed->fragments[0].id != placed->fragments[1].id);
}

TEST_CASE("i8086 flat layout defines bss boundaries without bss input")
{
    Builder builder;
    auto code = builder.fragment("code", ".text", ImageRole::Code, {0xC3});
    auto data = builder.fragment("data", ".data", ImageRole::Data, {0x01, 0x02});
    builder.symbol("code", code, 0, ImageSymbolKind::Code);
    builder.symbol("data", data, 0, ImageSymbolKind::Data);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small, 0x100, 0x1000));
    REQUIRE(placed.has_value());
    CHECK_EQ(placed->bss_start, placed->memory_end);
    CHECK_EQ(placed->bss_end, placed->memory_end);
    CHECK_EQ(placed->memory_end, 0x103u);
    CHECK_EQ(placed->find_symbol("__bss_start")->value, 0x103u);
    CHECK_EQ(placed->find_symbol("__image_end")->value, 0x103u);
    CHECK_EQ(placed->image_start, 0x100u);
    CHECK_EQ(placed->bytes.size(), 3u);
}

TEST_CASE("i8086 flat layout keeps zero initialization out of the image")
{
    Builder builder;
    auto text = builder.fragment("code", ".text", ImageRole::Code, {0xC3});
    auto bss = builder.fragment("zero", ".bss", ImageRole::Bss, {}, 0x20, 4, true);
    builder.symbol("code", text, 0, ImageSymbolKind::Code);
    builder.symbol("zero", bss, 0, ImageSymbolKind::Data);
    auto placed = dcc::backend::i8086::layout_flat_image(builder.image, options(dcc::target::CodeModel::Small, 0x100));
    REQUIRE(placed.has_value());
    CHECK_EQ(placed->bss_start, 0x104u);
    CHECK_EQ(placed->bss_end, 0x124u);
    CHECK_EQ(placed->memory_end, 0x124u);
    CHECK_EQ(placed->bytes.size(), 1u);
    CHECK_EQ(placed->fragments[0].file_offset.value(), 0u);
    CHECK(!placed->fragments[1].file_offset.has_value());
}
