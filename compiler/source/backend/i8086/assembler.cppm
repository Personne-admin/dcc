export module dcc.backend.i8086.assembler;

import std;
import dcc.ir;
import dcc.target;
import dcc.backend.x86.mir;
import dcc.backend.x86.prefix;
import dcc.backend.object.layout;

using namespace dcc::backend::x86;

export namespace dcc::backend::i8086
{
    [[nodiscard]] std::expected<std::string, std::string> emit_intel_asm(dcc::ir::IrModule const& module, std::vector<MFunction> const& functions, dcc::target::TargetConfig const& target);
}

namespace dcc::backend::i8086
{
    namespace
    {
        using namespace dcc::ir;
        using dcc::backend::object::DataSection;

        struct Printer
        {
            std::string out;
            std::string error;

            void fail(std::string message)
            {
                if (error.empty())
                    error = std::move(message);
            }

            [[nodiscard]] std::string reg(VReg r, unsigned bits)
            {
                if (!r.is_physical())
                {
                    fail("i8086 assembly printer: unallocated virtual register");
                    return "?";
                }
                auto p = r.phys_reg();
                if (reg_class(p) == RegClass::Segment)
                    return std::string{segment_name(p)};
                auto index = static_cast<unsigned>(p);
                static constexpr std::array<std::string_view, 8> r8{"al", "cl", "dl", "bl", "", "", "", ""};
                static constexpr std::array<std::string_view, 8> r16{"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
                static constexpr std::array<std::string_view, 8> r32{"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
                if (index >= 8 || (bits == 8 && r8[index].empty()) || (bits != 8 && bits != 16 && bits != 32))
                {
                    fail(std::format("i8086 assembly printer: register {} cannot be used as a {}-bit operand", index, bits));
                    return "?";
                }
                return std::string{bits == 8 ? r8[index] : bits == 16 ? r16[index] : r32[index]};
            }

            [[nodiscard]] static std::string_view segment_name(PhysReg p)
            {
                switch (p)
                {
                    case PhysReg::ES:
                        return "es";
                    case PhysReg::CS:
                        return "cs";
                    case PhysReg::SS:
                        return "ss";
                    case PhysReg::DS:
                        return "ds";
                    case PhysReg::FS:
                        return "fs";
                    case PhysReg::GS:
                        return "gs";
                    default:
                        return "?";
                }
            }

            [[nodiscard]] std::string mem(MMem const& m, unsigned bits)
            {
                std::string text = bits == 8 ? "byte [" : bits == 16 ? "word [" : bits == 32 ? "dword [" : "[";
                if (m.segment != SegmentOverride::None)
                {
                    text += x86::segment_name(m.segment);
                    text += ':';
                }
                auto address = m.address_bits == 32 ? 32u : 16u;
                bool first = true;
                if (m.base.is_valid())
                {
                    text += reg(m.base, address);
                    first = false;
                }
                if (m.index.is_valid())
                {
                    if (!first)
                        text += '+';
                    text += reg(m.index, address);
                    if (m.scale != 1)
                        text += std::format("*{}", m.scale);
                    first = false;
                }
                if (!m.symbol.empty())
                {
                    if (!first)
                        text += '+';
                    text += m.symbol;
                    first = false;
                }
                if (m.disp != 0 || first)
                    text += first ? std::format("{}", m.disp) : std::format("{:+}", m.disp);
                text += ']';
                return text;
            }

            [[nodiscard]] std::string operand(MOp const& op, unsigned bits)
            {
                switch (op.kind)
                {
                    case MOpKind::Reg:
                        return reg(op.reg, bits);
                    case MOpKind::Mem:
                        return mem(op.mem, bits);
                    case MOpKind::Imm64:
                        return std::format("{}", op.imm);
                    default:
                        fail("i8086 assembly printer: unsupported operand kind");
                        return "?";
                }
            }

            void line(std::string_view text)
            {
                out += '\t';
                out += text;
                out += '\n';
            }

            void tied(std::string_view mnemonic, MInstr const& mi, unsigned bits)
            {
                auto const& d = mi.ops[0];
                if (mi.num_ops == 3)
                {
                    if (!(mi.ops[1].kind == MOpKind::Reg && d.kind == MOpKind::Reg && mi.ops[1].reg == d.reg))
                        line(std::format("mov {}, {}", operand(d, bits), operand(mi.ops[1], bits)));
                    line(std::format("{} {}, {}", mnemonic, operand(d, bits), operand(mi.ops[2], bits)));
                    return;
                }
                line(std::format("{} {}, {}", mnemonic, operand(d, bits), operand(mi.ops[1], bits)));
            }

            void instruction(MInstr const& mi)
            {
                switch (mi.opc)
                {
                    case MOpc::NOP:
                        return line("nop");
                    case MOpc::UD2:
                        return line("ud2");
                    case MOpc::RET:
                        return line("ret");
                    case MOpc::COPY:
                        if (mi.ops[0].reg != mi.ops[1].reg)
                            line(std::format("mov {}, {}", operand(mi.ops[0], 32), operand(mi.ops[1], 32)));
                        return;
                    case MOpc::MOV16rr:
                    case MOpc::MOV32rr:
                    case MOpc::MOV16ri:
                    case MOpc::MOV32ri:
                    case MOpc::MOV16rm:
                    case MOpc::MOV32rm:
                    case MOpc::MOV16mr:
                    case MOpc::MOV32mr:
                        return line(std::format("mov {}, {}", operand(mi.ops[0], operand_bits(mi.opc)), operand(mi.ops[1], operand_bits(mi.opc))));
                    case MOpc::PUSH16r:
                    case MOpc::PUSH32r:
                        return line(std::format("push {}", operand(mi.ops[0], operand_bits(mi.opc))));
                    case MOpc::POP16r:
                    case MOpc::POP32r:
                        return line(std::format("pop {}", operand(mi.ops[0], operand_bits(mi.opc))));
                    case MOpc::ADD16ri:
                        return tied("add", mi, 16);
                    case MOpc::SUB16ri:
                        return tied("sub", mi, 16);
                    case MOpc::SHR32ri8:
                        return tied("shr", mi, 32);
                    default:
                        fail(std::format("i8086 assembly printer: unsupported instruction {}", opc_name(mi.opc)));
                }
            }

            void function(MFunction const& f)
            {
                out += '\n';
                if (f.linkage == Linkage::External)
                    out += std::format("global {}:function\n", f.name());
                else if (f.linkage == Linkage::LinkOnceODR || f.linkage == Linkage::WeakODR)
                    out += std::format("global {}:function weak\n", f.name());
                out += std::format("{}:\n", f.name());
                for (auto const& block : f.blocks)
                {
                    if (block.id != f.entry_block_id)
                        out += std::format(".bb{}:\n", block.id);
                    for (auto const& mi : block.instrs)
                        instruction(mi);
                }
            }

            void global(IrGlobal const* g, DataSection section, std::set<std::string>& referenced)
            {
                auto align = std::max<std::uint64_t>(g->type ? g->type->byte_align : 1, 1);
                auto size = g->type ? object::init_type_size(g->type) : 0;
                if (g->linkage == Linkage::External)
                    out += std::format("global {}:data {}\n", g->name, size);
                else if (g->linkage == Linkage::LinkOnceODR || g->linkage == Linkage::WeakODR)
                    out += std::format("global {}:data {} weak\n", g->name, size);
                if (align > 1)
                    out += section == DataSection::Bss ? std::format("alignb {}\n", align) : std::format("align {}, db 0\n", align);
                out += std::format("{}:\n", g->name);
                if (section == DataSection::Bss)
                {
                    if (size > 0)
                        line(std::format("resb {}", size));
                    return;
                }
                std::vector<std::uint8_t> image(size, 0);
                std::vector<object::InitReloc> relocs;
                object::serialize_init_memory(image, relocs, g->init, g->type, 0, 2);
                std::ranges::sort(relocs, {}, &object::InitReloc::offset);
                std::uint64_t at = 0;
                auto bytes = [&](std::uint64_t end) {
                    while (at < end)
                    {
                        std::string text = "db ";
                        for (unsigned n = 0; at < end && n < 16; ++n, ++at)
                            text += std::format("{}0x{:02x}", n ? ", " : "", image[at]);
                        line(text);
                    }
                };
                for (auto const& reloc : relocs)
                {
                    if (reloc.size != 2 && reloc.size != 4)
                    {
                        fail(std::format("i8086 assembly printer: unsupported {}-byte address in `{}`", reloc.size, g->name));
                        return;
                    }
                    bytes(reloc.offset);
                    referenced.insert(reloc.name);
                    line(std::format("{} {}{}", reloc.size == 2 ? "dw" : "dd", reloc.name, reloc.addend ? std::format("{:+}", reloc.addend) : ""));
                    at += reloc.size;
                }
                bytes(size);
            }
        };

        [[nodiscard]] std::string_view section_directive(DataSection section)
        {
            switch (section)
            {
                case DataSection::Rodata:
                    return "section .rodata progbits alloc noexec nowrite align=1";
                case DataSection::RodataRelRO:
                    return "section .data.rel.ro progbits alloc noexec write align=1";
                case DataSection::Data:
                    return "section .data progbits alloc noexec write align=1";
                case DataSection::Bss:
                    return "section .bss nobits alloc noexec write align=1";
                case DataSection::None:
                    break;
            }
            return {};
        }

    } // namespace

    std::expected<std::string, std::string> emit_intel_asm(IrModule const& module, std::vector<MFunction> const& functions, target::TargetConfig const&)
    {
        Printer p;
        p.out = "bits 16\n";
        std::set<std::string> defined;
        std::set<std::string> referenced;
        for (auto const& f : functions)
            defined.emplace(f.name());
        for (auto* g : module.globals)
            if (g && object::classify_global(g) != DataSection::None)
                defined.emplace(g->name);

        std::string data;
        for (auto section : {DataSection::Rodata, DataSection::RodataRelRO, DataSection::Data, DataSection::Bss})
        {
            Printer section_printer;
            for (auto* g : module.globals)
                if (g && g->section.empty() && object::classify_global(g) == section)
                    section_printer.global(g, section, referenced);
            for (auto* g : module.globals)
                if (g && !g->section.empty())
                    section_printer.fail(std::format("i8086 assembly printer: global `{}` in section `{}` is not supported yet", g->name, g->section));
            if (!section_printer.error.empty())
                return std::unexpected(section_printer.error);
            if (!section_printer.out.empty())
                data += std::format("\n{}\n{}", section_directive(section), section_printer.out);
        }

        for (auto const& name : referenced)
            if (!defined.contains(name))
                p.out += std::format("extern {}\n", name);

        p.out += "\nsection .text progbits alloc exec nowrite align=1\n";
        for (auto const& f : functions)
            p.function(f);
        if (!p.error.empty())
            return std::unexpected(p.error);
        p.out += data;
        return p.out;
    }

} // namespace dcc::backend::i8086
