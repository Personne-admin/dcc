export module dcc.backend.i8086.assembler;

import std;
import dcc.ir;
import dcc.target;
import dcc.backend.x86.mir;
import dcc.backend.x86.intel;
import dcc.backend.object.layout;

using namespace dcc::backend::x86;

export namespace dcc::backend::i8086
{
    [[nodiscard]] std::expected<std::string, std::string> emit_intel_asm(dcc::ir::IrModule const& module, std::vector<MFunction> const& functions,
                                                                         dcc::target::TargetConfig const& target);
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
                    return std::string{phys_reg_name(p)};
                auto index = static_cast<unsigned>(p);
                if (reg_class(p) != RegClass::GPR || index >= 8 || (bits == 8 && index >= 4) || (bits != 8 && bits != 16 && bits != 32))
                {
                    fail(std::format("i8086 assembly printer: register {} cannot be used as a {}-bit operand", phys_reg_name(p), bits));
                    return "?";
                }
                return std::string{intel_register_name(p, bits)};
            }

            [[nodiscard]] std::string mem(MMem const& m, unsigned bits)
            {
                for (auto r : {m.base, m.index})
                    if (r.is_valid())
                        std::ignore = reg(r, m.address_bits == 32 ? 32 : 16);
                return std::string{intel_size_keyword(bits)} + intel_memory_operand(m, m.address_bits == 32 ? 32 : 16);
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

            [[nodiscard]] static bool same_register(MOp const& a, MOp const& b) { return a.kind == MOpKind::Reg && b.kind == MOpKind::Reg && a.reg == b.reg; }

            void tied(std::string_view mnemonic, MInstr const& mi, unsigned bits, bool commutative = false)
            {
                auto const& d = mi.ops[0];
                if (mi.num_ops == 3)
                {
                    auto const* source = &mi.ops[2];
                    if (same_register(d, mi.ops[2]) && !same_register(d, mi.ops[1]))
                    {
                        if (!commutative)
                        {
                            fail(std::format("i8086 assembly printer: {} destination is the right operand", opc_name(mi.opc)));
                            return;
                        }
                        source = &mi.ops[1];
                    }
                    else if (!same_register(d, mi.ops[1]))
                        line(std::format("mov {}, {}", operand(d, bits), operand(mi.ops[1], bits)));
                    line(std::format("{} {}, {}", mnemonic, operand(d, bits), operand(*source, bits)));
                    return;
                }
                line(std::format("{} {}, {}", mnemonic, operand(d, bits), operand(mi.ops[1], bits)));
            }

            void shift(std::string_view mnemonic, MInstr const& mi)
            {
                auto const& count = mi.ops[mi.num_ops - 1];
                auto bits = operand_bits(mi.opc);
                if (mi.num_ops == 3 && !same_register(mi.ops[0], mi.ops[1]))
                    line(std::format("mov {}, {}", operand(mi.ops[0], bits), operand(mi.ops[1], bits)));
                line(std::format("{} {}, {}", mnemonic, operand(mi.ops[0], bits), operand(count, 8)));
            }

            void unary(std::string_view mnemonic, MInstr const& mi, unsigned bits)
            {
                if (mi.num_ops == 2 && !same_register(mi.ops[0], mi.ops[1]))
                    line(std::format("mov {}, {}", operand(mi.ops[0], bits), operand(mi.ops[1], bits)));
                line(std::format("{} {}", mnemonic, operand(mi.ops[0], bits)));
            }

            void extend(std::string_view mnemonic, MInstr const& mi, unsigned source_bits)
            {
                line(std::format("{} {}, {}", mnemonic, operand(mi.ops[0], operand_bits(mi.opc)), operand(mi.ops[1], source_bits)));
            }

            [[nodiscard]] static std::optional<std::string_view> setcc_mnemonic(MOpc opc)
            {
                switch (opc)
                {
                    case MOpc::SETEr:
                        return "sete";
                    case MOpc::SETNEr:
                        return "setne";
                    case MOpc::SETLr:
                        return "setl";
                    case MOpc::SETLEr:
                        return "setle";
                    case MOpc::SETGr:
                        return "setg";
                    case MOpc::SETGEr:
                        return "setge";
                    case MOpc::SETBr:
                        return "setb";
                    case MOpc::SETBEr:
                        return "setbe";
                    case MOpc::SETAr:
                        return "seta";
                    case MOpc::SETAEr:
                        return "setae";
                    default:
                        return std::nullopt;
                }
            }

            [[nodiscard]] static std::optional<std::string_view> jcc_mnemonic(MOpc opc)
            {
                switch (opc)
                {
                    case MOpc::JE:
                        return "je";
                    case MOpc::JNE:
                        return "jne";
                    case MOpc::JL:
                        return "jl";
                    case MOpc::JLE:
                        return "jle";
                    case MOpc::JG:
                        return "jg";
                    case MOpc::JGE:
                        return "jge";
                    case MOpc::JB:
                        return "jb";
                    case MOpc::JBE:
                        return "jbe";
                    case MOpc::JA:
                        return "ja";
                    case MOpc::JAE:
                        return "jae";
                    default:
                        return std::nullopt;
                }
            }

            void instruction(MInstr const& mi)
            {
                if (auto jcc = jcc_mnemonic(mi.opc))
                    return line(std::format("{} near .bb{}", *jcc, mi.ops[0].label));
                if (auto setcc = setcc_mnemonic(mi.opc))
                    return line(std::format("{} {}", *setcc, operand(mi.ops[0], 8)));
                switch (mi.opc)
                {
                    case MOpc::NOP:
                        return line("nop");
                    case MOpc::JMP:
                        return line(std::format("jmp near .bb{}", mi.ops[0].label));
                    case MOpc::JUMP_TABLE: {
                        if (mi.ops[2].imm == 32)
                            return line(std::format("jmp word [nosplit {}*2 + {}]", operand(mi.ops[0], 32), mi.ops[1].symbol));
                        if (!(mi.ops[0].kind == MOpKind::Reg && mi.ops[0].reg == VReg::phys(PhysReg::RDI)))
                            line(std::format("mov di, {}", operand(mi.ops[0], 16)));
                        line("shl di, 1");
                        return line(std::format("jmp word [di + {}]", mi.ops[1].symbol));
                    }
                    case MOpc::UD2:
                        return line("ud2");
                    case MOpc::RET:
                        return line("ret");
                    case MOpc::CDQ:
                        return line("cdq");
                    case MOpc::CWD:
                        return line("cwd");
                    case MOpc::COPY:
                        if (mi.ops[0].reg != mi.ops[1].reg)
                            line(std::format("mov {}, {}", operand(mi.ops[0], 32), operand(mi.ops[1], 32)));
                        return;
                    case MOpc::MOV8mr:
                    case MOpc::MOV16rr:
                    case MOpc::MOV32rr:
                    case MOpc::MOV16ri:
                    case MOpc::MOV32ri:
                    case MOpc::MOV16rm:
                    case MOpc::MOV32rm:
                    case MOpc::MOV16mr:
                    case MOpc::MOV32mr:
                    case MOpc::MOV8mi:
                    case MOpc::MOV16mi:
                    case MOpc::MOV32mi:
                        return line(std::format("mov {}, {}", operand(mi.ops[0], operand_bits(mi.opc)), operand(mi.ops[1], operand_bits(mi.opc))));
                    case MOpc::PUSH16r:
                    case MOpc::PUSH32r:
                        return line(std::format("push {}", operand(mi.ops[0], operand_bits(mi.opc))));
                    case MOpc::POP16r:
                    case MOpc::POP32r:
                        return line(std::format("pop {}", operand(mi.ops[0], operand_bits(mi.opc))));
                    case MOpc::ADD16ri:
                    case MOpc::ADD16rr:
                    case MOpc::ADD32ri:
                    case MOpc::ADD32rr:
                        return tied("add", mi, operand_bits(mi.opc), true);
                    case MOpc::SUB16ri:
                    case MOpc::SUB16rr:
                    case MOpc::SUB32ri:
                    case MOpc::SUB32rr:
                        return tied("sub", mi, operand_bits(mi.opc));
                    case MOpc::AND16ri:
                    case MOpc::AND16rr:
                    case MOpc::AND32ri:
                    case MOpc::AND32rr:
                        return tied("and", mi, operand_bits(mi.opc), true);
                    case MOpc::OR16ri:
                    case MOpc::OR16rr:
                    case MOpc::OR32ri:
                    case MOpc::OR32rr:
                        return tied("or", mi, operand_bits(mi.opc), true);
                    case MOpc::XOR16ri:
                    case MOpc::XOR16rr:
                    case MOpc::XOR32ri:
                    case MOpc::XOR32rr:
                        return tied("xor", mi, operand_bits(mi.opc), true);
                    case MOpc::IMUL16rr:
                    case MOpc::IMUL32rr:
                        return tied("imul", mi, operand_bits(mi.opc), true);
                    case MOpc::IMUL16rri:
                    case MOpc::IMUL32rri: {
                        auto bits = operand_bits(mi.opc);
                        return line(std::format("imul {}, {}, {}", operand(mi.ops[0], bits), operand(mi.ops[1], bits), operand(mi.ops[2], bits)));
                    }
                    case MOpc::SHL16ri8:
                    case MOpc::SHL16rCL:
                    case MOpc::SHL32ri8:
                    case MOpc::SHL32rCL:
                        return shift("shl", mi);
                    case MOpc::SHR16ri8:
                    case MOpc::SHR16rCL:
                    case MOpc::SHR32ri8:
                    case MOpc::SHR32rCL:
                        return shift("shr", mi);
                    case MOpc::SAR16ri8:
                    case MOpc::SAR16rCL:
                    case MOpc::SAR32ri8:
                    case MOpc::SAR32rCL:
                        return shift("sar", mi);
                    case MOpc::CMP16rr:
                    case MOpc::CMP16ri:
                    case MOpc::CMP32rr:
                    case MOpc::CMP32ri:
                        return line(std::format("cmp {}, {}", operand(mi.ops[0], operand_bits(mi.opc)), operand(mi.ops[1], operand_bits(mi.opc))));
                    case MOpc::TEST16ri:
                        return line(std::format("test {}, {}", operand(mi.ops[0], 16), operand(mi.ops[1], 16)));
                    case MOpc::NEG16r:
                    case MOpc::NEG32r:
                        return unary("neg", mi, operand_bits(mi.opc));
                    case MOpc::NOT16r:
                    case MOpc::NOT32r:
                        return unary("not", mi, operand_bits(mi.opc));
                    case MOpc::DIV16r:
                    case MOpc::DIV32r:
                        return line(std::format("div {}", operand(mi.ops[0], operand_bits(mi.opc))));
                    case MOpc::IDIV16r:
                    case MOpc::IDIV32r:
                        return line(std::format("idiv {}", operand(mi.ops[0], operand_bits(mi.opc))));
                    case MOpc::MOVZX16_8rr:
                    case MOpc::MOVZX16rm8:
                    case MOpc::MOVZX32_8rr:
                    case MOpc::MOVZX32rm8:
                        return extend("movzx", mi, 8);
                    case MOpc::MOVZX32_16rr:
                    case MOpc::MOVZX32rm16:
                        return extend("movzx", mi, 16);
                    case MOpc::MOVSX32_16rr:
                        return extend("movsx", mi, 16);
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
                    out += std::format(".bb{}:\n", block.id);
                    for (auto const& mi : block.instrs)
                        instruction(mi);
                }
            }

            void jump_tables(MFunction const& f)
            {
                for (auto const& table : f.jump_tables)
                {
                    out += std::format("align 2, db 0\n{}:\n", table.symbol);
                    for (auto t : table.targets)
                        line(std::format("dw {}.bb{}", f.name(), t));
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
            if (!section_printer.error.empty())
                return std::unexpected(section_printer.error);
            if (!section_printer.out.empty())
                data += std::format("\n{}\n{}", section_directive(section), section_printer.out);
        }

        std::vector<std::string_view> custom_sections;
        for (auto* g : module.globals)
            if (g && !g->section.empty() && object::classify_global(g) != DataSection::None &&
                std::ranges::find(custom_sections, g->section) == custom_sections.end())
                custom_sections.push_back(g->section);

        for (auto name : custom_sections)
        {
            Printer section_printer;
            DataSection kind = DataSection::None;
            for (auto* g : module.globals)
                if (g && g->section == name && object::classify_global(g) != DataSection::None)
                {
                    if (kind == DataSection::None)
                        kind = object::classify_global(g);
                    section_printer.global(g, kind == DataSection::Bss ? DataSection::Bss : DataSection::Data, referenced);
                }

            if (!section_printer.error.empty())
                return std::unexpected(section_printer.error);

            data += std::format("\nsection {} {} alloc noexec {} align=1\n{}", name, kind == DataSection::Bss ? "nobits" : "progbits",
                                kind == DataSection::Rodata ? "nowrite" : "write", section_printer.out);
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
        Printer tables;
        for (auto const& f : functions)
            tables.jump_tables(f);
        if (!tables.out.empty())
            p.out += std::format("\n{}\n{}", section_directive(DataSection::Rodata), tables.out);
        return p.out;
    }

} // namespace dcc::backend::i8086
