export module dcc.backend.object.elf32;

import std;
import dcc.ir;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.target;
import dcc.backend.object.elf;

export namespace dcc::backend::object
{
    struct Elf32ArchPolicy
    {
        std::uint16_t machine;
        std::uint32_t reloc_abs16;
        std::uint32_t reloc_abs32;
        std::uint32_t reloc_pc16;
        std::uint32_t reloc_pc32;
        std::uint32_t reloc_plt32;
        std::uint64_t address_width;
        std::uint64_t function_alignment;
        std::uint64_t section_min_alignment;
        std::uint64_t jump_table_entry_size;
        std::uint32_t jump_table_reloc;
    };

    inline constexpr Elf32ArchPolicy elf32_i8086_policy{3, 20, 1, 21, 2, 4, 2, 1, 1, 2, 20};

    [[nodiscard]] std::vector<std::uint8_t> write_elf32(ir::IrModule const& ir_mod, x86::MModule const& mod, std::vector<x86::EncodeResult> const& encoded,
                                                        target::TargetConfig const& target, Elf32ArchPolicy const& arch)
    {
        ElfWriterPolicy const writer{
            .machine = arch.machine,
            .address_width = arch.address_width,
            .reloc_abs16 = arch.reloc_abs16,
            .reloc_abs32 = arch.reloc_abs32,
            .reloc_abs64 = 0,
            .reloc_pc16 = arch.reloc_pc16,
            .reloc_pc32 = arch.reloc_pc32,
            .reloc_got_pc32 = 0,
            .reloc_plt32 = arch.reloc_plt32,
            .function_alignment = arch.function_alignment,
            .section_min_alignment = arch.section_min_alignment,
            .jump_table_entry_size = arch.jump_table_entry_size,
            .jump_table_reloc = arch.jump_table_reloc,
            .jump_table_addend = 0,
            .class32 = true,
            .rela = false,
        };
        return write_elf_object(ir_mod, mod, encoded, target, writer);
    }
}
