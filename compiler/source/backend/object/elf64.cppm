export module dcc.backend.object.elf64;

import std;
import dcc.ir;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.target;
import dcc.backend.object.elf;

export namespace dcc::backend::object
{
    struct Elf64ArchPolicy
    {
        std::uint16_t machine;
        std::uint32_t reloc_abs;
        std::uint32_t reloc_pc32;
        std::uint32_t reloc_got_pc32;
        std::uint32_t reloc_plt32;
        std::uint64_t address_width;
    };

    inline constexpr Elf64ArchPolicy elf64_x86_64_policy{62, 1, 2, 42, 4, 8};

    [[nodiscard]] std::vector<std::uint8_t> write_elf64(ir::IrModule const& ir_mod, x86::MModule const& mod, std::vector<x86::EncodeResult> const& encoded,
                                                        target::TargetConfig const& target, Elf64ArchPolicy const& arch)
    {
        ElfWriterPolicy const writer{
            .machine = arch.machine,
            .address_width = arch.address_width,
            .reloc_abs16 = 0,
            .reloc_abs32 = 0,
            .reloc_abs64 = arch.reloc_abs,
            .reloc_pc16 = 0,
            .reloc_pc32 = arch.reloc_pc32,
            .reloc_got_pc32 = arch.reloc_got_pc32,
            .reloc_plt32 = arch.reloc_plt32,
            .function_alignment = 16,
            .section_min_alignment = 8,
            .jump_table_entry_size = 4,
            .jump_table_reloc = arch.reloc_pc32,
            .jump_table_addend = -4,
        };
        return write_elf_object(ir_mod, mod, encoded, target, writer);
    }
}
