export module dcc.ir.data;

import std;
import dcc.ir;

export namespace dcc::ir
{
    bool encode_constant_data(IrValue const* value, IrRawConstant& out, std::uint64_t offset, std::string& error)
    {
        auto fail = [&](std::string message) { error = std::move(message); return false; };
        if (!value || !value->type || offset > out.bytes.size() || value->type->byte_size > out.bytes.size() - offset)
            return fail("constant data exceeds its storage");
        auto size = value->type->byte_size;
        auto clear = [&](std::uint64_t count) {
            for (auto const& reloc : out.relocations)
                if (reloc.offset < offset + count && offset < reloc.offset + reloc.width &&
                    (reloc.offset < offset || reloc.offset + reloc.width > offset + count))
                    return false;
            std::erase_if(out.relocations, [&](auto const& reloc) { return reloc.offset < offset + count && offset < reloc.offset + reloc.width; });
            std::fill_n(out.bytes.begin() + static_cast<std::ptrdiff_t>(offset), count, 0);
            return true;
        };
        auto integer = [&](std::uint64_t bits, std::uint64_t count) {
            if (count > 8 || !clear(size))
                return fail("invalid scalar constant data layout");
            for (std::uint64_t i = 0; i < count; ++i)
                out.bytes[offset + (out.little_endian ? i : count - i - 1)] = static_cast<std::uint8_t>(bits >> (i * 8));
            return true;
        };
        switch (value->kind)
        {
            case IrNodeKind::IntConstant:
                return integer(static_cast<std::uint64_t>(static_cast<IrIntConstant const*>(value)->value), size);
            case IrNodeKind::BoolConstant:
                return integer(static_cast<IrBoolConstant const*>(value)->value ? 1 : 0, size);
            case IrNodeKind::FloatConstant: {
                auto number = static_cast<IrFloatConstant const*>(value)->value;
                if (size == 4)
                    return integer(std::bit_cast<std::uint32_t>(static_cast<float>(number)), size);
                if (size == 8)
                    return integer(std::bit_cast<std::uint64_t>(number), size);
                return fail("unsupported floating constant data width");
            }
            case IrNodeKind::NullConstant:
                return clear(size) || fail("constant data partially overwrites a relocation");
            case IrNodeKind::PointerConstant: {
                auto* pointer = ir_type_cast<IrPointerType>(value->type);
                auto* address = static_cast<IrPointerConstant const*>(value);
                if (pointer && pointer->flavor == PointerFlavor::Far && !out.little_endian)
                    return fail("unsupported byte order for a segmented pointer constant");
                auto width = pointer && pointer->flavor == PointerFlavor::Far ? (size == 4 ? 2u : 4u) : size;
                if (!integer(address->offset, width))
                    return false;
                if (pointer && pointer->flavor == PointerFlavor::Far)
                {
                    out.bytes[offset + width] = static_cast<std::uint8_t>(address->segment);
                    out.bytes[offset + width + 1] = static_cast<std::uint8_t>(address->segment >> 8);
                }
                return true;
            }
            case IrNodeKind::GlobalRef: {
                auto* reference = static_cast<IrGlobalRef const*>(value);
                auto* pointer = ir_type_cast<IrPointerType>(value->type);
                if (!pointer || (pointer->flavor == PointerFlavor::Far && !out.little_endian) || !clear(size))
                    return fail("invalid symbolic constant data address");
                auto width = pointer->flavor == PointerFlavor::Far ? (size == 4 ? 2u : 4u) : size;
                if (width != 2 && width != 4 && width != 8)
                    return fail("unsupported constant data relocation width");
                out.relocations.push_back({offset, width, reference, IrDataRelocationKind::Absolute});
                if (pointer->flavor == PointerFlavor::Far)
                    out.relocations.push_back({offset + width, 2, reference, IrDataRelocationKind::Segment});
                return true;
            }
            case IrNodeKind::RawConstant: {
                auto* data = static_cast<IrRawConstant const*>(value);
                if (auto why = constant_data_error(*data); !why.empty())
                    return fail(std::move(why));
                if (data->little_endian != out.little_endian || !clear(size))
                    return fail("incompatible constant data embedding");
                std::ranges::copy(data->bytes, out.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                for (auto reloc : data->relocations)
                {
                    reloc.offset += offset;
                    out.relocations.push_back(reloc);
                }
                return true;
            }
            case IrNodeKind::StringConstant: {
                auto text = static_cast<IrStringConstant const*>(value)->value;
                if (text.size() > size || !clear(size))
                    return fail("invalid string constant data layout");
                std::ranges::copy(text, out.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                return true;
            }
            case IrNodeKind::Aggregate: {
                auto* aggregate = static_cast<IrAggregateInst const*>(value);
                if (auto* record = ir_type_cast<IrAggregateType>(value->type))
                {
                    if (aggregate->values.size() != record->members.size() || record->member_offsets.size() != record->members.size())
                        return fail("invalid aggregate constant data layout");
                    for (std::size_t i = 0; i < aggregate->values.size(); ++i)
                    {
                        auto* member = aggregate->values[i];
                        auto member_offset = record->member_offsets[i];
                        if (!member || !member->type || member_offset > size || member->type->byte_size > size - member_offset ||
                            !record->members[i] || member->type->byte_size > record->members[i]->byte_size ||
                            !encode_constant_data(member, out, offset + member_offset, error))
                            return fail(error.empty() ? "invalid constant data member" : error);
                    }
                    return true;
                }
                if (auto* array = ir_type_cast<IrArrayType>(value->type))
                {
                    if (!array->element || aggregate->values.size() != array->count ||
                        (array->count && array->element->byte_size > size / array->count))
                        return fail("invalid array constant data layout");
                    for (std::size_t i = 0; i < aggregate->values.size(); ++i)
                    {
                        auto* element = aggregate->values[i];
                        if (!element || !element->type || element->type->byte_size > array->element->byte_size)
                            return fail("constant data array element exceeds its storage");
                        if (!encode_constant_data(element, out, offset + i * array->element->byte_size, error))
                            return false;
                    }
                    return true;
                }
                if (ir_type_cast<IrSliceType>(value->type))
                {
                    if (aggregate->values.size() != 2 || !aggregate->values[0] || !aggregate->values[0]->type)
                        return fail("invalid slice constant data layout");
                    auto width = aggregate->values[0]->type->byte_size;
                    auto* length = aggregate->values[1];
                    if (width > size || !length || !length->type || length->type->byte_size > size - width)
                        return fail("constant data slice exceeds its storage");
                    if (!encode_constant_data(aggregate->values[0], out, offset, error))
                        return false;
                    return encode_constant_data(aggregate->values[1], out, offset + width, error);
                }
                return fail("unsupported aggregate constant data type");
            }
            default:
                return fail("initializer is not constant data");
        }
    }

    IrRawConstant* materialize_constant_data(IrContext& context, IrType const* storage, IrValue const* value, std::string& error)
    {
        if (!storage)
        {
            error = "constant data has no storage type";
            return nullptr;
        }
        auto* data = context.raw_constant(storage);
        if (!encode_constant_data(value, *data, 0, error))
            return nullptr;
        error = constant_data_error(*data);
        return error.empty() ? data : nullptr;
    }
}
