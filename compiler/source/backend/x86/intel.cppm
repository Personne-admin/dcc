export module dcc.backend.x86.intel;

import std;
import dcc.backend.x86.mir;
import dcc.backend.x86.prefix;

export namespace dcc::backend::x86
{
    [[nodiscard]] std::string_view intel_register_name(PhysReg r, unsigned bits)
    {
        using namespace std::literals;
        static constexpr std::array<std::string_view, 16> r32{"eax"sv, "ecx"sv, "edx"sv,  "ebx"sv,  "esp"sv,  "ebp"sv,  "esi"sv,  "edi"sv,
                                                              "r8d"sv, "r9d"sv, "r10d"sv, "r11d"sv, "r12d"sv, "r13d"sv, "r14d"sv, "r15d"sv};
        static constexpr std::array<std::string_view, 16> r16{"ax"sv,  "cx"sv,  "dx"sv,   "bx"sv,   "sp"sv,   "bp"sv,   "si"sv,   "di"sv,
                                                              "r8w"sv, "r9w"sv, "r10w"sv, "r11w"sv, "r12w"sv, "r13w"sv, "r14w"sv, "r15w"sv};
        static constexpr std::array<std::string_view, 16> r8{"al"sv,  "cl"sv,  "dl"sv,   "bl"sv,   "spl"sv,  "bpl"sv,  "sil"sv,  "dil"sv,
                                                             "r8b"sv, "r9b"sv, "r10b"sv, "r11b"sv, "r12b"sv, "r13b"sv, "r14b"sv, "r15b"sv};
        auto index = static_cast<unsigned>(r) - static_cast<unsigned>(PhysReg::RAX);
        if (reg_class(r) != RegClass::GPR || index >= 16 || bits == 64)
            return phys_reg_name(r);
        if (bits == 32)
            return r32[index];
        if (bits == 16)
            return r16[index];
        if (bits == 8)
            return r8[index];
        return phys_reg_name(r);
    }

    [[nodiscard]] std::string_view intel_size_keyword(unsigned bits)
    {
        switch (bits)
        {
            case 8:
                return "byte ";
            case 16:
                return "word ";
            case 32:
                return "dword ";
            case 64:
                return "qword ";
            default:
                return "";
        }
    }

    [[nodiscard]] std::string intel_memory_operand(MMem const& m, unsigned address_bits)
    {
        std::string r = "[";
        if (m.segment != SegmentOverride::None)
        {
            r += segment_name(m.segment);
            r += ':';
        }
        if (!m.symbol.empty() && address_bits == 64)
        {
            r += "rel ";
            r += m.symbol;
            if (m.is_got_indirect)
                r += " wrt ..got";
            r += ']';
            return r;
        }
        bool has_base = m.base.is_valid() && m.base.is_physical();
        bool has_index = m.index.is_valid() && m.index.is_physical();
        bool has_symbol = !m.symbol.empty();
        if (address_bits == 32 && !has_base && !has_index)
            r += "dword ";
        if (has_base)
            r += intel_register_name(m.base.phys_reg(), address_bits);
        if (has_index)
        {
            if (has_base)
                r += " + ";
            r += intel_register_name(m.index.phys_reg(), address_bits);
            if (m.scale > 1)
                r += std::format("*{}", static_cast<unsigned>(m.scale));
        }
        if (has_symbol)
        {
            if (has_base || has_index)
                r += " + ";
            r += m.symbol;
        }
        bool has_term = has_base || has_index || has_symbol;
        if (m.disp != 0 || !has_term)
        {
            if (has_term)
            {
                if (m.disp > 0)
                    r += std::format(" + {}", m.disp);
                else if (m.disp < 0)
                    r += std::format(" - {}", -static_cast<std::int64_t>(m.disp));
            }
            else
                r += std::to_string(m.disp);
        }
        r += ']';
        return r;
    }
} // namespace dcc::backend::x86
