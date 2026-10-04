export module dcc.backend.x86.prefix;

import std;

export namespace dcc::backend::x86
{
    enum class EncodeMode : std::uint8_t
    {
        Long64,
        Protected32,
        Real16,
    };

    enum class SegmentOverride : std::uint8_t
    {
        None,
        CS,
        DS,
        ES,
        SS,
        FS,
        GS,
    };

    void append_legacy_prefixes(std::vector<std::uint8_t>& out, EncodeMode mode, unsigned operand_bits, unsigned address_bits,
                                SegmentOverride segment)
    {
        switch (segment)
        {
            case SegmentOverride::None:
                break;
            case SegmentOverride::CS:
                out.push_back(0x2e);
                break;
            case SegmentOverride::DS:
                out.push_back(0x3e);
                break;
            case SegmentOverride::ES:
                out.push_back(0x26);
                break;
            case SegmentOverride::SS:
                out.push_back(0x36);
                break;
            case SegmentOverride::FS:
                out.push_back(0x64);
                break;
            case SegmentOverride::GS:
                out.push_back(0x65);
                break;
        }

        unsigned default_operand = mode == EncodeMode::Real16 ? 16 : 32;
        unsigned default_address = mode == EncodeMode::Long64 ? 64 : mode == EncodeMode::Protected32 ? 32 : 16;
        if (operand_bits && operand_bits != default_operand && !(mode == EncodeMode::Long64 && operand_bits == 64))
            out.push_back(0x66);
        if (address_bits && address_bits != default_address)
            out.push_back(0x67);
    }
}
