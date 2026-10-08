export module dcc.backend.object.layout;

import std;
import dcc.ir;

#define into_u8 static_cast<std::uint8_t>

export namespace dcc::backend::object
{
    void w8(std::vector<std::uint8_t>& b, std::uint8_t v)
    {
        b.push_back(v);
    }
    void w16(std::vector<std::uint8_t>& b, std::uint16_t v)
    {
        b.push_back(std::uint8_t(v));
        b.push_back(std::uint8_t(v >> 8));
    }
    void w32(std::vector<std::uint8_t>& b, std::uint32_t v)
    {
        b.push_back(std::uint8_t(v));
        b.push_back(std::uint8_t(v >> 8));
        b.push_back(std::uint8_t(v >> 16));
        b.push_back(std::uint8_t(v >> 24));
    }
    void w64(std::vector<std::uint8_t>& b, std::uint64_t v)
    {
        w32(b, std::uint32_t(v));
        w32(b, std::uint32_t(v >> 32));
    }

    enum class DataSection
    {
        None,
        Rodata,
        RodataRelRO,
        Data,
        Bss
    };

    struct GlobalLayout
    {
        ir::IrGlobal const* g{};
        DataSection sec{DataSection::None};
        std::uint64_t offset{};
        std::uint64_t alignment{};
        std::uint32_t section_index{};
        std::string name_str;
    };

    [[nodiscard]] std::uint64_t align_up(std::uint64_t val, std::uint64_t alignment)
    {
        return (val + alignment - 1) / alignment * alignment;
    }

    [[nodiscard]] std::uint64_t max_global_alignment(std::vector<GlobalLayout*> const& globals)
    {
        std::uint64_t a = 1;
        for (auto* glp : globals)
            if (glp->alignment > a)
                a = glp->alignment;
        return a;
    }

    [[nodiscard]] bool has_global_ref(ir::IrValue const* val)
    {
        if (!val)
            return false;

        if (val->kind == ir::IrNodeKind::GlobalRef)
            return true;

        if (val->kind == ir::IrNodeKind::Aggregate)
        {
            auto* agg = static_cast<ir::IrAggregateInst const*>(val);
            for (auto* fv : agg->values)
                if (has_global_ref(fv))
                    return true;
        }

        return false;
    }

    [[nodiscard]] DataSection classify_global(ir::IrGlobal const* g)
    {
        if (g->is_declaration || g->is_dll_import)
            return DataSection::None;
        if (g->is_constant && g->init)
        {
            if (has_global_ref(g->init))
                return DataSection::RodataRelRO;

            return DataSection::Rodata;
        }
        if (g->init)
            return DataSection::Data;

        return DataSection::Bss;
    }

    struct InitReloc
    {
        std::uint64_t offset{};
        std::uint64_t size{};
        std::string name;
        std::int64_t addend{};
    };

    [[nodiscard]] std::uint64_t init_type_size(ir::IrType const* type)
    {
        if (!type || type->byte_size != 0)
            return type ? type->byte_size : 0;
        if (type->kind == ir::IrTypeKind::Aggregate)
        {
            auto* aggregate = static_cast<ir::IrAggregateType const*>(type);
            std::uint64_t size = 0;
            for (std::size_t i = 0; i < aggregate->members.size(); ++i)
            {
                auto offset = i < aggregate->member_offsets.size() ? aggregate->member_offsets[i] : 0;
                size = std::max(size, offset + init_type_size(aggregate->members[i]));
            }
            return size;
        }
        if (type->kind == ir::IrTypeKind::Array)
        {
            auto* array = static_cast<ir::IrArrayType const*>(type);
            return array->count * init_type_size(array->element);
        }
        return 0;
    }

    void erase_init_relocs(std::vector<InitReloc>& relocs, std::uint64_t offset, std::uint64_t size)
    {
        if (size == 0)
            return;
        auto end = offset + size;
        std::erase_if(relocs, [&](InitReloc const& reloc) { return reloc.offset < end && offset < reloc.offset + reloc.size; });
    }

    void write_init_bytes(std::vector<std::uint8_t>& data, std::vector<InitReloc>& relocs, std::uint64_t offset, std::span<std::uint8_t const> bytes)
    {
        if (offset >= data.size() || bytes.empty())
            return;
        auto size = std::min<std::uint64_t>(bytes.size(), data.size() - offset);
        erase_init_relocs(relocs, offset, size);
        std::ranges::copy(bytes.first(size), data.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    void zero_init_bytes(std::vector<std::uint8_t>& data, std::vector<InitReloc>& relocs, std::uint64_t offset, std::uint64_t size)
    {
        if (offset >= data.size() || size == 0)
            return;
        size = std::min<std::uint64_t>(size, data.size() - offset);
        erase_init_relocs(relocs, offset, size);
        std::fill_n(data.begin() + static_cast<std::ptrdiff_t>(offset), size, 0);
    }

    void write_addend_field(std::vector<std::uint8_t>& data, std::vector<InitReloc>& relocs, std::uint64_t offset, std::uint64_t size, std::int64_t addend)
    {
        if (offset >= data.size() || size == 0)
            return;
        size = std::min<std::uint64_t>(size, data.size() - offset);
        erase_init_relocs(relocs, offset, size);
        std::uint64_t uv = static_cast<std::uint64_t>(addend);
        for (std::uint64_t i = 0; i < size; ++i)
            data[static_cast<std::size_t>(offset + i)] = into_u8(uv >> (i * 8));
    }

    void serialize_init_memory(std::vector<std::uint8_t>& data, std::vector<InitReloc>& relocs, ir::IrValue const* val, ir::IrType const* expected_type,
                               std::uint64_t offset, std::uint64_t address_width)
    {
        if (!val || !expected_type)
            return;

        switch (val->kind)
        {
            case ir::IrNodeKind::IntConstant: {
                auto* ic = static_cast<ir::IrIntConstant const*>(val);
                auto sz = expected_type->byte_size;
                std::uint64_t uv = static_cast<std::uint64_t>(ic->value);
                std::vector<std::uint8_t> bytes(sz);
                for (std::uint64_t i = 0; i < sz; ++i)
                {
                    bytes[i] = into_u8(uv & 0xFF);
                    uv >>= 8;
                }
                write_init_bytes(data, relocs, offset, bytes);
                break;
            }
            case ir::IrNodeKind::FloatConstant: {
                auto* fc = static_cast<ir::IrFloatConstant const*>(val);
                auto bits = expected_type ? (expected_type->kind == ir::IrTypeKind::Float ? static_cast<ir::IrFloatType const*>(expected_type)->bits : 64) : 64;
                if (bits == 32)
                {
                    float f = static_cast<float>(fc->value);
                    std::uint32_t raw;
                    std::memcpy(&raw, &f, 4);
                    std::array<std::uint8_t, 4> bytes{};
                    for (std::size_t i = 0; i < bytes.size(); ++i)
                        bytes[i] = into_u8(raw >> (i * 8));
                    write_init_bytes(data, relocs, offset, bytes);
                }
                else
                {
                    double d = fc->value;
                    std::uint64_t raw;
                    std::memcpy(&raw, &d, 8);
                    std::array<std::uint8_t, 8> bytes{};
                    for (std::size_t i = 0; i < bytes.size(); ++i)
                        bytes[i] = into_u8(raw >> (i * 8));
                    write_init_bytes(data, relocs, offset, bytes);
                }
                break;
            }
            case ir::IrNodeKind::PointerConstant: {
                auto* pc = static_cast<ir::IrPointerConstant const*>(val);
                auto sz = expected_type->byte_size;
                std::uint64_t uv = pc->offset;
                std::vector<std::uint8_t> bytes(sz);
                for (std::uint64_t i = 0; i < sz && i < 8; ++i)
                {
                    bytes[i] = into_u8(uv & 0xFF);
                    uv >>= 8;
                }
                write_init_bytes(data, relocs, offset, bytes);
                break;
            }
            case ir::IrNodeKind::BoolConstant: {
                auto* bc = static_cast<ir::IrBoolConstant const*>(val);
                zero_init_bytes(data, relocs, offset, expected_type->byte_size);
                std::array<std::uint8_t, 1> byte{bc->value ? std::uint8_t{1} : std::uint8_t{0}};
                write_init_bytes(data, relocs, offset, byte);
                break;
            }
            case ir::IrNodeKind::NullConstant: {
                zero_init_bytes(data, relocs, offset, expected_type->byte_size);
                break;
            }
            case ir::IrNodeKind::StringConstant: {
                auto* sc = static_cast<ir::IrStringConstant const*>(val);
                zero_init_bytes(data, relocs, offset, expected_type->byte_size);
                auto size = std::min<std::uint64_t>(sc->value.size(), expected_type->byte_size);
                write_init_bytes(data, relocs, offset,
                                 std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(sc->value.data()), static_cast<std::size_t>(size)});
                break;
            }
            case ir::IrNodeKind::Aggregate: {
                auto* agg = static_cast<ir::IrAggregateInst const*>(val);
                auto const* agg_type = agg->type ? agg->type : expected_type;

                if (!agg_type)
                    break;

                if (agg_type->kind == ir::IrTypeKind::Aggregate)
                {
                    auto* at = static_cast<ir::IrAggregateType const*>(agg_type);
                    auto num_fields = std::min(agg->values.size(), at->members.size());
                    for (std::size_t i = 0; i < num_fields; ++i)
                    {
                        auto* fval = agg->values[i];
                        if (!fval)
                            continue;

                        auto field_off = (i < at->member_offsets.size()) ? at->member_offsets[i] : 0;
                        auto* field_type = (i < at->members.size()) ? at->members[i] : nullptr;
                        serialize_init_memory(data, relocs, fval, field_type, offset + field_off, address_width);
                    }
                }
                else if (agg_type->kind == ir::IrTypeKind::Array)
                {
                    auto* at = static_cast<ir::IrArrayType const*>(agg_type);
                    auto elem_size = at->element ? at->element->byte_size : 1;
                    auto count = std::min<std::uint64_t>(agg->values.size(), at->count);
                    for (std::uint64_t i = 0; i < count; ++i)
                    {
                        auto* fval = agg->values[i];
                        if (!fval)
                            continue;
                        serialize_init_memory(data, relocs, fval, at->element, offset + i * elem_size, address_width);
                    }
                }
                else if (agg_type->kind == ir::IrTypeKind::Slice)
                {
                    auto* st = static_cast<ir::IrSliceType const*>(agg_type);
                    auto ptr_bytes = st->byte_size / 2;

                    auto const* data_val = ir::slice_data_index < agg->values.size() ? agg->values[ir::slice_data_index] : nullptr;
                    if (data_val && data_val->type)
                        serialize_init_memory(data, relocs, data_val, data_val->type, offset, address_width);

                    auto const* len_val = ir::slice_len_index < agg->values.size() ? agg->values[ir::slice_len_index] : nullptr;
                    if (len_val && len_val->type)
                        serialize_init_memory(data, relocs, len_val, len_val->type, offset + ptr_bytes, address_width);
                }
                break;
            }
            case ir::IrNodeKind::GlobalRef: {
                auto* gr = static_cast<ir::IrGlobalRef const*>(val);
                auto sz = expected_type ? expected_type->byte_size : address_width;
                zero_init_bytes(data, relocs, offset, sz);
                if (sz < address_width)
                {
                    std::println(std::cerr, "em64t objwriter: address relocation into a {}-byte field (symbol `{}`); refusing to emit malformed object", sz,
                                 gr->name);
                    std::abort();
                }

                if (gr->addend != 0 && sz <= data.size() - offset)
                    write_addend_field(data, relocs, offset, sz, gr->addend);
                if (offset <= data.size() && sz <= data.size() - offset)
                    relocs.push_back({offset, sz, std::string{gr->name}, gr->addend});
                break;
            }
            default:
                zero_init_bytes(data, relocs, offset, expected_type->byte_size);
                break;
        }
    }

    void collect_ref_names(ir::IrValue const* val, std::unordered_set<std::string>& out)
    {
        if (!val)
            return;

        if (val->kind == ir::IrNodeKind::GlobalRef)
        {
            auto* gr = static_cast<ir::IrGlobalRef const*>(val);
            out.insert(std::string{gr->name});
        }

        else if (val->kind == ir::IrNodeKind::Aggregate)
        {
            auto* agg = static_cast<ir::IrAggregateInst const*>(val);
            for (auto* fv : agg->values)
                collect_ref_names(fv, out);
        }
    }

} // namespace dcc::backend::object
