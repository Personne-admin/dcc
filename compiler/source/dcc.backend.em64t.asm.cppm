export module dcc.backend.em64t.assembler;

import std;
import dcc.ir;
import dcc.backend.em64t.mir;
import dcc.target;

using namespace std::literals;

export namespace dcc::backend::em64t
{
    [[nodiscard]] std::string emit_intel_asm(ir::IrModule const& ir_mod, std::vector<MFunction> const& functions, target::TargetConfig const& target);
}

namespace
{
    using namespace dcc::backend::em64t;
    using namespace dcc::ir;
    namespace ir = dcc::ir;

    [[nodiscard]] std::string_view reg64(PhysReg r)
    {
        return phys_reg_name(r);
    }

    [[nodiscard]] std::string_view reg32(PhysReg r)
    {
        switch (r)
        {
            case PhysReg::RAX:
                return "eax"sv;
            case PhysReg::RCX:
                return "ecx"sv;
            case PhysReg::RDX:
                return "edx"sv;
            case PhysReg::RBX:
                return "ebx"sv;
            case PhysReg::RSP:
                return "esp"sv;
            case PhysReg::RBP:
                return "ebp"sv;
            case PhysReg::RSI:
                return "esi"sv;
            case PhysReg::RDI:
                return "edi"sv;
            case PhysReg::R8:
                return "r8d"sv;
            case PhysReg::R9:
                return "r9d"sv;
            case PhysReg::R10:
                return "r10d"sv;
            case PhysReg::R11:
                return "r11d"sv;
            case PhysReg::R12:
                return "r12d"sv;
            case PhysReg::R13:
                return "r13d"sv;
            case PhysReg::R14:
                return "r14d"sv;
            case PhysReg::R15:
                return "r15d"sv;
            default:
                break;
        }
        return phys_reg_name(r);
    }

    [[nodiscard]] std::string_view reg16(PhysReg r)
    {
        switch (r)
        {
            case PhysReg::RAX:
                return "ax"sv;
            case PhysReg::RCX:
                return "cx"sv;
            case PhysReg::RDX:
                return "dx"sv;
            case PhysReg::RBX:
                return "bx"sv;
            case PhysReg::RSP:
                return "sp"sv;
            case PhysReg::RBP:
                return "bp"sv;
            case PhysReg::RSI:
                return "si"sv;
            case PhysReg::RDI:
                return "di"sv;
            case PhysReg::R8:
                return "r8w"sv;
            case PhysReg::R9:
                return "r9w"sv;
            case PhysReg::R10:
                return "r10w"sv;
            case PhysReg::R11:
                return "r11w"sv;
            case PhysReg::R12:
                return "r12w"sv;
            case PhysReg::R13:
                return "r13w"sv;
            case PhysReg::R14:
                return "r14w"sv;
            case PhysReg::R15:
                return "r15w"sv;
            default:
                break;
        }
        return phys_reg_name(r);
    }

    [[nodiscard]] std::string_view reg8(PhysReg r)
    {
        switch (r)
        {
            case PhysReg::RAX:
                return "al"sv;
            case PhysReg::RCX:
                return "cl"sv;
            case PhysReg::RDX:
                return "dl"sv;
            case PhysReg::RBX:
                return "bl"sv;
            case PhysReg::RSP:
                return "spl"sv;
            case PhysReg::RBP:
                return "bpl"sv;
            case PhysReg::RSI:
                return "sil"sv;
            case PhysReg::RDI:
                return "dil"sv;
            case PhysReg::R8:
                return "r8b"sv;
            case PhysReg::R9:
                return "r9b"sv;
            case PhysReg::R10:
                return "r10b"sv;
            case PhysReg::R11:
                return "r11b"sv;
            case PhysReg::R12:
                return "r12b"sv;
            case PhysReg::R13:
                return "r13b"sv;
            case PhysReg::R14:
                return "r14b"sv;
            case PhysReg::R15:
                return "r15b"sv;
            default:
                break;
        }
        return phys_reg_name(r);
    }

    enum class RegWidth : std::uint8_t
    {
        Bits64,
        Bits32,
        Bits16,
        Bits8,
        XMM
    };

    [[nodiscard]] RegWidth opc_width(MOpc opc)
    {
        switch (opc)
        {
            case MOpc::MOV64rr:
            case MOpc::MOV64ri32:
            case MOpc::MOV64ri:
            case MOpc::MOV64rm:
            case MOpc::MOV64mr:
            case MOpc::MOV64mi32:
            case MOpc::ADD64rr:
            case MOpc::ADD64ri32:
            case MOpc::ADD64rm:
            case MOpc::SUB64rr:
            case MOpc::SUB64ri32:
            case MOpc::SUB64ri:
            case MOpc::SUB64rm:
            case MOpc::IMUL64rr:
            case MOpc::IMUL64rri32:
            case MOpc::IMUL64rri:
            case MOpc::IMUL64rm:
            case MOpc::IMUL64ri:
            case MOpc::MUL64r:
            case MOpc::IDIV64r:
            case MOpc::DIV64r:
            case MOpc::NEG64r:
            case MOpc::NOT64r:
            case MOpc::INC64r:
            case MOpc::DEC64r:
            case MOpc::AND64rr:
            case MOpc::AND64ri32:
            case MOpc::AND64rm:
            case MOpc::AND64mr:
            case MOpc::AND64ri:
            case MOpc::OR64rr:
            case MOpc::OR64ri32:
            case MOpc::OR64rm:
            case MOpc::OR64mr:
            case MOpc::OR64ri:
            case MOpc::XOR64rr:
            case MOpc::XOR64ri32:
            case MOpc::XOR64rm:
            case MOpc::XOR64mr:
            case MOpc::XOR64ri:
            case MOpc::SHL64rCL:
            case MOpc::SHL64ri8:
            case MOpc::SHR64rCL:
            case MOpc::SHR64ri8:
            case MOpc::SAR64rCL:
            case MOpc::SAR64ri8:
            case MOpc::SHL64rcl:
            case MOpc::SHL64ri:
            case MOpc::SHR64rcl:
            case MOpc::SHR64ri:
            case MOpc::SAR64rcl:
            case MOpc::SAR64ri:
            case MOpc::CMP64rr:
            case MOpc::CMP64ri32:
            case MOpc::CMP64rm:
            case MOpc::CMP64ri:
            case MOpc::TEST64rr:
            case MOpc::TEST64ri:
            case MOpc::LEA64rm:
            case MOpc::CQO:
            case MOpc::PUSH64r:
            case MOpc::POP64r:
            case MOpc::PUSH64i:
            case MOpc::PUSH64m:
            case MOpc::MOVSX64_32rr:
            case MOpc::MOVSX64_16rr:
            case MOpc::MOVSX64_8rr:
            case MOpc::MOVZX64_32rr:
            case MOpc::MOVZX64_16rr:
            case MOpc::MOVZX64_8rr:
            case MOpc::MOVSX64rr8:
            case MOpc::MOVSX64rm8:
            case MOpc::MOVSX64rr16:
            case MOpc::MOVSX64rm16:
            case MOpc::MOVZX64rr8:
            case MOpc::MOVZX64rm8:
            case MOpc::MOVZX64rr16:
            case MOpc::MOVZX64rm16:
            case MOpc::CMOV64Err:
            case MOpc::CMOV64NErr:
            case MOpc::CMOV64Lrr:
            case MOpc::CMOV64GErr:
            case MOpc::CMOV64LErr:
            case MOpc::CMOV64Grr:
            case MOpc::CMOV64Brr:
            case MOpc::CMOV64AErr:
            case MOpc::CMOV64BErr:
            case MOpc::CMOV64Arr:
            case MOpc::XCHG64rr:
            case MOpc::MOVQ64rr:
            case MOpc::LOCK_XADD64mr:
            case MOpc::LOCK_XCHG64mr:
            case MOpc::LOCK_AND64mi32:
            case MOpc::LOCK_OR64mi32:
            case MOpc::LOCK_XOR64mi32:
                return RegWidth::Bits64;

            case MOpc::MOV32rr:
            case MOpc::MOV32ri:
            case MOpc::MOV32mi:
            case MOpc::MOV32rm:
            case MOpc::MOV32mr:
            case MOpc::ADD32rr:
            case MOpc::ADD32ri:
            case MOpc::ADD32rm:
            case MOpc::SUB32rr:
            case MOpc::SUB32ri:
            case MOpc::SUB32rm:
            case MOpc::IMUL32rr:
            case MOpc::IMUL32rri:
            case MOpc::IDIV32r:
            case MOpc::DIV32r:
            case MOpc::NEG32r:
            case MOpc::NOT32r:
            case MOpc::AND32rr:
            case MOpc::AND32ri:
            case MOpc::OR32rr:
            case MOpc::OR32ri:
            case MOpc::XOR32rr:
            case MOpc::XOR32ri:
            case MOpc::SHL32rCL:
            case MOpc::SHL32ri8:
            case MOpc::SHR32rCL:
            case MOpc::SHR32ri8:
            case MOpc::SAR32rCL:
            case MOpc::SAR32ri8:
            case MOpc::CMP32rr:
            case MOpc::CMP32ri:
            case MOpc::CMP32rm:
            case MOpc::TEST32rr:
            case MOpc::CDQ:
            case MOpc::MOVSX32_16rr:
            case MOpc::MOVSX32_8rr:
            case MOpc::MOVZX32_16rr:
            case MOpc::MOVZX32_8rr:
            case MOpc::MOVZX32rr8:
            case MOpc::MOVZX32rm8:
            case MOpc::LOCK_XADD32mr:
                return RegWidth::Bits32;

            case MOpc::MOV16rr:
            case MOpc::MOV16ri:
            case MOpc::MOV16mi:
            case MOpc::MOV16rm:
            case MOpc::MOV16mr:
                return RegWidth::Bits16;

            case MOpc::MOV8rr:
            case MOpc::MOV8ri:
            case MOpc::MOV8mi:
            case MOpc::MOV8rm:
            case MOpc::MOV8mr:
            case MOpc::CMP8rr:
            case MOpc::CMP8ri:
            case MOpc::TEST8rr:
            case MOpc::SETEr:
            case MOpc::SETNEr:
            case MOpc::SETLr:
            case MOpc::SETGEr:
            case MOpc::SETLEr:
            case MOpc::SETGr:
            case MOpc::SETBr:
            case MOpc::SETAEr:
            case MOpc::SETBEr:
            case MOpc::SETAr:
                return RegWidth::Bits8;

            default:
                break;
        }
        return RegWidth::Bits64;
    }

    [[nodiscard]] std::string_view reg_name_f(PhysReg r, RegWidth w)
    {
        switch (w)
        {
            case RegWidth::Bits64:
                return reg64(r);
            case RegWidth::Bits32:
                return reg32(r);
            case RegWidth::Bits16:
                return reg16(r);
            case RegWidth::Bits8:
                return reg8(r);
            case RegWidth::XMM:
                return phys_reg_name(r);
        }
        return phys_reg_name(r);
    }

    struct AsmContext
    {
        std::string& out;
        MFunction const* func{};
        std::unordered_map<std::uint32_t, std::string> block_labels;
    };

    [[nodiscard]] std::string format_op(AsmContext& ctx, MOp const& op, RegWidth w, bool with_size)
    {
        std::string r;
        switch (op.kind)
        {
            case MOpKind::None:
                r = "<none>";
                break;
            case MOpKind::Reg:
                if (op.reg.is_physical())
                {
                    auto pr = op.reg.phys_reg();
                    if (reg_class(pr) == RegClass::XMM)
                        r = phys_reg_name(pr);
                    else
                        r = reg_name_f(pr, w);
                }
                else
                {
                    r = std::format("; vreg%{}", op.reg.id);
                }
                break;
            case MOpKind::Imm64:
                r = std::to_string(op.imm);
                break;
            case MOpKind::Mem: {
                auto const& m = op.mem;
                if (with_size)
                {
                    switch (w)
                    {
                        case RegWidth::Bits8:
                            r = "byte ";
                            break;
                        case RegWidth::Bits16:
                            r = "word ";
                            break;
                        case RegWidth::Bits32:
                            r = "dword ";
                            break;
                        case RegWidth::Bits64:
                            r = "qword ";
                            break;
                        case RegWidth::XMM:
                            r = "qword ";
                            break;
                    }
                }
                r += '[';
                if (!m.symbol.empty())
                {
                    r += "rel ";
                    r += m.symbol;
                }
                else
                {
                    bool has_base = m.base.is_valid() && m.base.is_physical();
                    bool has_idx = m.index.is_valid() && m.index.is_physical();
                    if (has_base)
                        r += reg_name_f(m.base.phys_reg(), RegWidth::Bits64);

                    if (has_idx)
                    {
                        if (has_base)
                            r += " + ";
                        r += reg_name_f(m.index.phys_reg(), RegWidth::Bits64);
                        if (m.scale > 1)
                            r += std::format("*{}", static_cast<unsigned>(m.scale));
                    }

                    if (m.disp != 0 || (!has_base && !has_idx))
                    {
                        if (has_base || has_idx)
                        {
                            if (m.disp > 0)
                                r += std::format(" + {}", m.disp);
                            else if (m.disp < 0)
                                r += std::format(" - {}", -m.disp);
                        }
                        else
                            r += std::to_string(m.disp);
                    }
                }
                r += ']';
                break;
            }
            case MOpKind::FrameSlot:
                r = std::format("; frameslot{}", op.frame_slot);
                break;
            case MOpKind::Label: {
                auto it = ctx.block_labels.find(op.label);
                if (it != ctx.block_labels.end())
                    r = it->second;
                else
                    r = std::format("bb{}", op.label);
                break;
            }
            case MOpKind::Symbol:
                r = op.symbol;
                break;
        }
        return r;
    }

    void emit_instr(AsmContext& ctx, MInstr const& mi)
    {
        auto& out = ctx.out;
        auto opc = mi.opc;
        auto const& ops = mi.ops;
        auto np = mi.num_ops;

        if (opc == MOpc::PHI || opc == MOpc::IMPLICIT_DEF)
            return;

        out += "    ";

        if (opc == MOpc::COPY)
        {
            if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
            {
                auto d = ops[0].reg;
                auto s = ops[1].reg;
                if (d == s)
                {
                    out += "; COPY same-reg\n";
                    return;
                }

                if (d.is_physical() && s.is_physical())
                {
                    auto dp = d.phys_reg();
                    auto sp = s.phys_reg();
                    if (reg_class(dp) == RegClass::XMM && reg_class(sp) == RegClass::XMM)
                        out += "movaps " + std::string{phys_reg_name(dp)} + ", " + std::string{phys_reg_name(sp)} + " ; COPY (XMM-XMM)\n";
                    else
                        out += "mov " + std::string{reg64(dp)} + ", " + std::string{reg64(sp)} + " ; COPY\n";
                }
                else
                    out += "; COPY with virtual\n";
            }
            else
                out += "; COPY unrecognized\n";
            return;
        }

        auto w = opc_width(opc);
        auto rn = [&](PhysReg r) -> std::string_view { return reg_name_f(r, w); };

        switch (opc)
        {
            case MOpc::MOV64rr:
            case MOpc::MOV64ri32:
            case MOpc::MOV64ri:
            case MOpc::MOV32rr:
            case MOpc::MOV32ri:
            case MOpc::MOV16rr:
            case MOpc::MOV16ri:
            case MOpc::MOV8rr:
            case MOpc::MOV8ri: {
                if (np < 2)
                    break;

                if (ops[0].kind != MOpKind::Reg || !ops[0].reg.is_physical())
                {
                    out += "; mov with virtual\n";
                    break;
                }
                auto d = ops[0].reg.phys_reg();
                if (ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                {
                    auto s = ops[1].reg.phys_reg();
                    if (d == s)
                    {
                        out += "; mov same-reg\n";
                        break;
                    }
                    out += "mov " + std::string{rn(d)} + ", " + std::string{rn(s)} + "\n";
                }
                else if (ops[1].kind == MOpKind::Imm64)
                {
                    out += "mov " + std::string{rn(d)} + ", " + std::to_string(ops[1].imm) + "\n";
                }
                else
                {
                    out += "; mov unrecognized\n";
                }
                break;
            }

            case MOpc::MOV64rm:
            case MOpc::MOV32rm:
            case MOpc::MOV16rm:
            case MOpc::MOV8rm: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Mem)
                    out += "mov " + std::string{rn(ops[0].reg.phys_reg())} + ", " + format_op(ctx, ops[1], w, true) + "\n";
                else
                    out += "; movrm unrecognized\n";
                break;
            }

            case MOpc::MOV64mr:
            case MOpc::MOV32mr:
            case MOpc::MOV16mr:
            case MOpc::MOV8mr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "mov " + format_op(ctx, ops[0], w, true) + ", " + std::string{rn(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movmr unrecognized\n";
                break;
            }

            case MOpc::MOV64mi32:
            case MOpc::MOV32mi:
            case MOpc::MOV16mi:
            case MOpc::MOV8mi: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Imm64)
                    out += "mov " + format_op(ctx, ops[0], w, true) + ", " + std::to_string(ops[1].imm) + "\n";
                else
                    out += "; movmi unrecognized\n";
                break;
            }

            case MOpc::MOVZX64_32rr:
            case MOpc::MOVZX32_16rr:
            case MOpc::MOVZX32_8rr:
            case MOpc::MOVZX32rr8:
            case MOpc::MOVZX64rr8:
            case MOpc::MOVZX64rr16:
            case MOpc::MOVZX64_16rr:
            case MOpc::MOVZX64_8rr:
            case MOpc::MOVZX32rm8:
            case MOpc::MOVZX64rm8:
            case MOpc::MOVZX64rm16: {
                if (np < 2 || ops[0].kind != MOpKind::Reg || !ops[0].reg.is_physical())
                {
                    out += "; movzx unrecognized\n";
                    break;
                }
                auto d = ops[0].reg.phys_reg();
                RegWidth dw = opc_width(opc);
                if (ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                {
                    auto sw = (dw == RegWidth::Bits64) ? RegWidth::Bits8 : RegWidth::Bits8;
                    if (opc == MOpc::MOVZX64_32rr)
                        sw = RegWidth::Bits32;
                    else if (opc == MOpc::MOVZX64_16rr || opc == MOpc::MOVZX32_16rr || opc == MOpc::MOVZX64rr16)
                        sw = RegWidth::Bits16;
                    out += "movzx " + std::string{reg_name_f(d, dw)} + ", " + std::string{reg_name_f(ops[1].reg.phys_reg(), sw)} + "\n";
                }
                else if (ops[1].kind == MOpKind::Mem)
                {
                    out += "movzx " + std::string{reg_name_f(d, dw)} + ", " + format_op(ctx, ops[1], RegWidth::Bits8, true) + "\n";
                }
                else
                    out += "; movzx unrecognized\n";
                break;
            }

            case MOpc::MOVSX64_32rr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "movsxd " + std::string{reg64(ops[0].reg.phys_reg())} + ", " + std::string{reg32(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movsxd unrecognized\n";
                break;

            case MOpc::MOVSX64_16rr:
            case MOpc::MOVSX32_16rr:
            case MOpc::MOVSX64rr16:
            case MOpc::MOVSX64_8rr:
            case MOpc::MOVSX32_8rr:
            case MOpc::MOVSX64rr8:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                {
                    auto d = ops[0].reg.phys_reg();
                    auto s = ops[1].reg.phys_reg();
                    RegWidth dw = opc_width(opc);
                    RegWidth sw = (opc == MOpc::MOVSX64_16rr || opc == MOpc::MOVSX32_16rr || opc == MOpc::MOVSX64rr16) ? RegWidth::Bits16 : RegWidth::Bits8;
                    out += "movsx " + std::string{reg_name_f(d, dw)} + ", " + std::string{reg_name_f(s, sw)} + "\n";
                }
                else
                    out += "; movsx unrecognized\n";
                break;

            case MOpc::MOVSX64rm8:
            case MOpc::MOVSX64rm16:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Mem)
                {
                    auto d = ops[0].reg.phys_reg();
                    out += "movsx " + std::string{reg64(d)} + ", " + format_op(ctx, ops[1], RegWidth::Bits8, true) + "\n";
                }
                else
                    out += "; movsxrm unrecognized\n";
                break;

            case MOpc::ADD64rr:
            case MOpc::ADD32rr:
            case MOpc::SUB64rr:
            case MOpc::SUB32rr:
            case MOpc::AND64rr:
            case MOpc::AND32rr:
            case MOpc::OR64rr:
            case MOpc::OR32rr:
            case MOpc::XOR64rr:
            case MOpc::XOR32rr:
            case MOpc::IMUL64rr:
            case MOpc::IMUL32rr: {
                bool is_sub = (opc == MOpc::SUB64rr || opc == MOpc::SUB32rr);
                std::string mnem;
                if (opc == MOpc::ADD64rr || opc == MOpc::ADD32rr)
                    mnem = "add";
                else if (opc == MOpc::SUB64rr || opc == MOpc::SUB32rr)
                    mnem = "sub";
                else if (opc == MOpc::AND64rr || opc == MOpc::AND32rr)
                    mnem = "and";
                else if (opc == MOpc::OR64rr || opc == MOpc::OR32rr)
                    mnem = "or";
                else if (opc == MOpc::XOR64rr || opc == MOpc::XOR32rr)
                    mnem = "xor";
                else
                    mnem = "imul";

                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Reg && ops[2].reg.is_physical())
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    auto r = ops[2].reg.phys_reg();
                    if (d == l)
                        out += mnem + " " + std::string{rn(d)} + ", " + std::string{rn(r)} + "\n";
                    else if (d == r && !is_sub)
                        out += mnem + " " + std::string{rn(d)} + ", " + std::string{rn(l)} + "\n";
                    else if (d == r && is_sub)
                    {
                        out += "; WARNING: sub dst==rhs, using r11\n";
                        out += "    mov r11, " + std::string{rn(l)} + "\n";
                        out += "    sub r11, " + std::string{rn(r)} + "\n";
                        out += "    mov " + std::string{rn(d)} + ", r11\n";
                    }
                    else
                    {
                        out += "mov " + std::string{rn(d)} + ", " + std::string{rn(l)} + "\n";
                        out += "    " + mnem + " " + std::string{rn(d)} + ", " + std::string{rn(r)} + "\n";
                    }
                }
                else if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Imm64)
                {
                    out += mnem + " " + std::string{rn(ops[0].reg.phys_reg())} + ", " + std::to_string(ops[1].imm) + "\n";
                }
                else
                    out += "; alu unrecognized\n";
                break;
            }

            case MOpc::ADD64ri32:
            case MOpc::SUB64ri32:
            case MOpc::AND64ri32:
            case MOpc::OR64ri32:
            case MOpc::XOR64ri32:
            case MOpc::ADD32ri:
            case MOpc::SUB32ri:
            case MOpc::AND32ri:
            case MOpc::OR32ri:
            case MOpc::XOR32ri:
            case MOpc::SUB64ri:
            case MOpc::AND64ri:
            case MOpc::OR64ri:
            case MOpc::XOR64ri: {
                std::string mnem;
                if (opc == MOpc::ADD64ri32 || opc == MOpc::ADD32ri)
                    mnem = "add";
                else if (opc == MOpc::SUB64ri32 || opc == MOpc::SUB32ri || opc == MOpc::SUB64ri)
                    mnem = "sub";
                else if (opc == MOpc::AND64ri32 || opc == MOpc::AND32ri || opc == MOpc::AND64ri)
                    mnem = "and";
                else if (opc == MOpc::OR64ri32 || opc == MOpc::OR32ri || opc == MOpc::OR64ri)
                    mnem = "or";
                else
                    mnem = "xor";

                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Imm64)
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    if (d != l)
                        out += "mov " + std::string{rn(d)} + ", " + std::string{rn(l)} + "\n    ";
                    out += mnem + " " + std::string{rn(d)} + ", " + std::to_string(ops[2].imm) + "\n";
                }
                else if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Imm64)
                {
                    out += mnem + " " + std::string{rn(ops[0].reg.phys_reg())} + ", " + std::to_string(ops[1].imm) + "\n";
                }
                else
                    out += "; aluri unrecognized\n";
                break;
            }

            case MOpc::ADD64rm:
            case MOpc::ADD32rm:
            case MOpc::SUB64rm:
            case MOpc::SUB32rm:
            case MOpc::AND64rm:
            case MOpc::OR64rm:
            case MOpc::XOR64rm:
            case MOpc::IMUL64rm: {
                std::string mnem;
                if (opc == MOpc::ADD64rm || opc == MOpc::ADD32rm)
                    mnem = "add";
                else if (opc == MOpc::SUB64rm || opc == MOpc::SUB32rm)
                    mnem = "sub";
                else if (opc == MOpc::AND64rm)
                    mnem = "and";
                else if (opc == MOpc::OR64rm)
                    mnem = "or";
                else if (opc == MOpc::XOR64rm)
                    mnem = "xor";
                else
                    mnem = "imul";

                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Mem)
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    if (d != l)
                        out += "mov " + std::string{rn(d)} + ", " + std::string{rn(l)} + "\n    ";
                    out += mnem + " " + std::string{rn(d)} + ", " + format_op(ctx, ops[2], w, true) + "\n";
                }
                else
                    out += "; alurm unrecognized\n";
                break;
            }

            case MOpc::AND64mr:
            case MOpc::OR64mr:
            case MOpc::XOR64mr: {
                std::string mnem = (opc == MOpc::AND64mr) ? "and" : (opc == MOpc::OR64mr) ? "or" : "xor";
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += mnem + " " + format_op(ctx, ops[0], RegWidth::Bits64, true) + ", " + std::string{reg64(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; alumr unrecognized\n";
                break;
            }

            case MOpc::IMUL64rri32:
            case MOpc::IMUL64rri:
            case MOpc::IMUL32rri: {
                RegWidth dw = (opc == MOpc::IMUL64rri32 || opc == MOpc::IMUL64rri) ? RegWidth::Bits64 : RegWidth::Bits32;
                auto rr = [&](PhysReg p) { return reg_name_f(p, dw); };
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Imm64)
                    out += "imul " + std::string{rr(ops[0].reg.phys_reg())} + ", " + std::string{rr(ops[1].reg.phys_reg())} + ", " +
                           std::to_string(ops[2].imm) + "\n";
                else
                    out += "; imulrri unrecognized\n";
                break;
            }

            case MOpc::IMUL64ri:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Imm64)
                    out += "imul " + std::string{reg64(ops[0].reg.phys_reg())} + ", " + std::string{reg64(ops[0].reg.phys_reg())} + ", " +
                           std::to_string(ops[1].imm) + "\n";
                else
                    out += "; imul64ri unrecognized\n";
                break;

            case MOpc::MUL64r:
                if (np >= 1 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += "mul " + std::string{reg64(ops[0].reg.phys_reg())} + "\n";
                else
                    out += "; mul unrecognized\n";
                break;

            case MOpc::IDIV64r:
            case MOpc::IDIV32r:
            case MOpc::DIV64r:
            case MOpc::DIV32r:
                if (np >= 1 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += ((opc == MOpc::IDIV64r || opc == MOpc::IDIV32r) ? "idiv " : "div ") + std::string{rn(ops[0].reg.phys_reg())} + "\n";
                else
                    out += "; idiv/div unrecognized\n";
                break;

            case MOpc::NEG64r:
            case MOpc::NEG32r:
            case MOpc::NOT64r:
            case MOpc::NOT32r:
            case MOpc::INC64r:
            case MOpc::DEC64r: {
                std::string mnem;
                if (opc == MOpc::NEG64r || opc == MOpc::NEG32r)
                    mnem = "neg";
                else if (opc == MOpc::NOT64r || opc == MOpc::NOT32r)
                    mnem = "not";
                else if (opc == MOpc::INC64r)
                    mnem = "inc";
                else
                    mnem = "dec";
                if (np >= 1 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += mnem + " " + std::string{rn(ops[0].reg.phys_reg())} + "\n";
                else
                    out += "; neg/not/inc/dec unrecognized\n";
                break;
            }

            case MOpc::SHL64rCL:
            case MOpc::SHL32rCL:
            case MOpc::SHR64rCL:
            case MOpc::SHR32rCL:
            case MOpc::SAR64rCL:
            case MOpc::SAR32rCL:
            case MOpc::SHL64rcl:
            case MOpc::SHR64rcl:
            case MOpc::SAR64rcl: {
                std::string mnem;
                if (opc == MOpc::SHL64rCL || opc == MOpc::SHL32rCL || opc == MOpc::SHL64rcl)
                    mnem = "shl";
                else if (opc == MOpc::SHR64rCL || opc == MOpc::SHR32rCL || opc == MOpc::SHR64rcl)
                    mnem = "shr";
                else
                    mnem = "sar";
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += mnem + " " + std::string{rn(ops[0].reg.phys_reg())} + ", cl\n";
                else
                    out += "; shiftcl unrecognized\n";
                break;
            }

            case MOpc::SHL64ri8:
            case MOpc::SHL32ri8:
            case MOpc::SHR64ri8:
            case MOpc::SHR32ri8:
            case MOpc::SAR64ri8:
            case MOpc::SAR32ri8:
            case MOpc::SHL64ri:
            case MOpc::SHR64ri:
            case MOpc::SAR64ri: {
                std::string mnem;
                if (opc == MOpc::SHL64ri8 || opc == MOpc::SHL32ri8 || opc == MOpc::SHL64ri)
                    mnem = "shl";
                else if (opc == MOpc::SHR64ri8 || opc == MOpc::SHR32ri8 || opc == MOpc::SHR64ri)
                    mnem = "shr";
                else
                    mnem = "sar";
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Imm64)
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    if (d != l)
                        out += "mov " + std::string{rn(d)} + ", " + std::string{rn(l)} + "\n    ";
                    out += mnem + " " + std::string{rn(d)} + ", " + std::to_string(ops[2].imm & 0x3F) + "\n";
                }
                else if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Imm64)
                    out += mnem + " " + std::string{rn(ops[0].reg.phys_reg())} + ", " + std::to_string(ops[1].imm & 0x3F) + "\n";
                else
                    out += "; shiftimm unrecognized\n";
                break;
            }

            case MOpc::CMP64rr:
            case MOpc::CMP32rr:
            case MOpc::CMP8rr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cmp " + std::string{rn(ops[0].reg.phys_reg())} + ", " + std::string{rn(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cmprr unrecognized\n";
                break;

            case MOpc::CMP64ri32:
            case MOpc::CMP32ri:
            case MOpc::CMP8ri:
            case MOpc::CMP64ri:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Imm64)
                    out += "cmp " + std::string{rn(ops[0].reg.phys_reg())} + ", " + std::to_string(ops[1].imm) + "\n";
                else
                    out += "; cmpri unrecognized\n";
                break;

            case MOpc::CMP64rm:
            case MOpc::CMP32rm:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Mem)
                    out += "cmp " + std::string{rn(ops[0].reg.phys_reg())} + ", " + format_op(ctx, ops[1], w, true) + "\n";
                else
                    out += "; cmprm unrecognized\n";
                break;

            case MOpc::TEST64rr:
            case MOpc::TEST32rr:
            case MOpc::TEST8rr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "test " + std::string{rn(ops[0].reg.phys_reg())} + ", " + std::string{rn(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; testrr unrecognized\n";
                break;

            case MOpc::TEST64ri:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Imm64)
                    out += "test " + std::string{reg64(ops[0].reg.phys_reg())} + ", " + std::to_string(ops[1].imm) + "\n";
                else
                    out += "; testri unrecognized\n";
                break;

            case MOpc::SETEr:
            case MOpc::SETNEr:
            case MOpc::SETLr:
            case MOpc::SETGEr:
            case MOpc::SETLEr:
            case MOpc::SETGr:
            case MOpc::SETBr:
            case MOpc::SETAEr:
            case MOpc::SETBEr:
            case MOpc::SETAr: {
                std::string mnem;
                switch (opc)
                {
                    case MOpc::SETEr:
                        mnem = "sete";
                        break;
                    case MOpc::SETNEr:
                        mnem = "setne";
                        break;
                    case MOpc::SETLr:
                        mnem = "setl";
                        break;
                    case MOpc::SETGEr:
                        mnem = "setge";
                        break;
                    case MOpc::SETLEr:
                        mnem = "setle";
                        break;
                    case MOpc::SETGr:
                        mnem = "setg";
                        break;
                    case MOpc::SETBr:
                        mnem = "setb";
                        break;
                    case MOpc::SETAEr:
                        mnem = "setae";
                        break;
                    case MOpc::SETBEr:
                        mnem = "setbe";
                        break;
                    case MOpc::SETAr:
                        mnem = "seta";
                        break;
                    default:
                        mnem = "sete";
                        break;
                }
                if (np >= 1 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += mnem + " " + std::string{reg8(ops[0].reg.phys_reg())} + "\n";
                else
                    out += "; setcc unrecognized\n";
                break;
            }

            case MOpc::CMOV64Err:
            case MOpc::CMOV64NErr:
            case MOpc::CMOV64Lrr:
            case MOpc::CMOV64GErr:
            case MOpc::CMOV64LErr:
            case MOpc::CMOV64Grr:
            case MOpc::CMOV64Brr:
            case MOpc::CMOV64AErr:
            case MOpc::CMOV64BErr:
            case MOpc::CMOV64Arr: {
                std::string mnem;
                switch (opc)
                {
                    case MOpc::CMOV64Err:
                        mnem = "cmove";
                        break;
                    case MOpc::CMOV64NErr:
                        mnem = "cmovne";
                        break;
                    case MOpc::CMOV64Lrr:
                        mnem = "cmovl";
                        break;
                    case MOpc::CMOV64GErr:
                        mnem = "cmovge";
                        break;
                    case MOpc::CMOV64LErr:
                        mnem = "cmovle";
                        break;
                    case MOpc::CMOV64Grr:
                        mnem = "cmovg";
                        break;
                    case MOpc::CMOV64Brr:
                        mnem = "cmovb";
                        break;
                    case MOpc::CMOV64AErr:
                        mnem = "cmovae";
                        break;
                    case MOpc::CMOV64BErr:
                        mnem = "cmovbe";
                        break;
                    case MOpc::CMOV64Arr:
                        mnem = "cmova";
                        break;
                    default:
                        mnem = "cmove";
                        break;
                }
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Reg && ops[2].reg.is_physical())
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    auto r = ops[2].reg.phys_reg();
                    if (d != l)
                        out += "mov " + std::string{reg64(d)} + ", " + std::string{reg64(l)} + "\n    ";
                    out += mnem + " " + std::string{reg64(d)} + ", " + std::string{reg64(r)} + "\n";
                }
                else
                    out += "; cmov unrecognized\n";
                break;
            }

            case MOpc::JMP:
            case MOpc::JMP_rel32:
            case MOpc::JE:
            case MOpc::JNE:
            case MOpc::JL:
            case MOpc::JLE:
            case MOpc::JG:
            case MOpc::JGE:
            case MOpc::JB:
            case MOpc::JBE:
            case MOpc::JA:
            case MOpc::JAE:
            case MOpc::JP:
            case MOpc::JNP:
            case MOpc::JS:
            case MOpc::JNS: {
                std::string mnem;
                switch (opc)
                {
                    case MOpc::JMP:
                    case MOpc::JMP_rel32:
                        mnem = "jmp";
                        break;
                    case MOpc::JE:
                        mnem = "je";
                        break;
                    case MOpc::JNE:
                        mnem = "jne";
                        break;
                    case MOpc::JL:
                        mnem = "jl";
                        break;
                    case MOpc::JLE:
                        mnem = "jle";
                        break;
                    case MOpc::JG:
                        mnem = "jg";
                        break;
                    case MOpc::JGE:
                        mnem = "jge";
                        break;
                    case MOpc::JB:
                        mnem = "jb";
                        break;
                    case MOpc::JBE:
                        mnem = "jbe";
                        break;
                    case MOpc::JA:
                        mnem = "ja";
                        break;
                    case MOpc::JAE:
                        mnem = "jae";
                        break;
                    case MOpc::JP:
                        mnem = "jp";
                        break;
                    case MOpc::JNP:
                        mnem = "jnp";
                        break;
                    case MOpc::JS:
                        mnem = "js";
                        break;
                    case MOpc::JNS:
                        mnem = "jns";
                        break;
                    default:
                        mnem = "jmp";
                        break;
                }
                if (np >= 1 && ops[0].kind == MOpKind::Label)
                {
                    auto it = ctx.block_labels.find(ops[0].label);
                    if (it != ctx.block_labels.end())
                        out += mnem + " " + it->second + "\n";
                    else
                        out += mnem + " bb" + std::to_string(ops[0].label) + "\n";
                }
                else if (np >= 1 && ops[0].kind == MOpKind::Symbol)
                    out += mnem + " " + std::string{ops[0].symbol} + "\n";
                else
                    out += "; branch unrecognized\n";
                break;
            }

            case MOpc::JMP_r64:
                if (np >= 1 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += "jmp " + std::string{reg64(ops[0].reg.phys_reg())} + "\n";
                else
                    out += "; jmp_r64 unrecognized\n";
                break;
            case MOpc::JUMP_TABLE: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Symbol)
                {
                    auto idx = ops[0].reg.phys_reg();
                    auto idx64 = reg64(idx);
                    out += "lea r11, [rel " + std::string{ops[1].symbol} + "]\n";
                    out += "    lea r10, [r11 + " + std::string{idx64} + "*4 + 4]\n";
                    out += "    movsxd rax, dword [r11 + " + std::string{idx64} + "*4]\n";
                    out += "    add rax, r10\n";
                    out += "    jmp rax\n";
                }
                else
                    out += "; JUMP_TABLE unrecognized\n";
                break;
            }

            case MOpc::JMPm:
            case MOpc::CALLm:
                if (np >= 1 && ops[0].kind == MOpKind::Mem)
                    out += ((opc == MOpc::JMPm) ? "jmp " : "call ") + format_op(ctx, ops[0], w, true) + "\n";
                else
                    out += "; jmpm/callm unrecognized\n";
                break;

            case MOpc::CALL:
            case MOpc::CALL_r64:
                if (np >= 1 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += "call " + std::string{reg64(ops[0].reg.phys_reg())} + "\n";
                else
                    out += "; call unrecognized\n";
                break;

            case MOpc::CALL_rel32:
                if (np >= 1 && ops[0].kind == MOpKind::Symbol)
                    out += "call " + std::string{ops[0].symbol} + "\n";
                else if (np >= 1 && ops[0].kind == MOpKind::Label)
                {
                    auto it = ctx.block_labels.find(ops[0].label);
                    if (it != ctx.block_labels.end())
                        out += "call " + it->second + "\n";
                    else
                        out += "call bb" + std::to_string(ops[0].label) + "\n";
                }
                else
                    out += "; call unrecognized\n";
                break;

            case MOpc::RET:
                out += "ret\n";
                break;

            case MOpc::LEA64rm:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Mem)
                    out += "lea " + std::string{reg64(ops[0].reg.phys_reg())} + ", " + format_op(ctx, ops[1], RegWidth::Bits64, false) + "\n";
                else
                    out += "; lea unrecognized\n";
                break;

            case MOpc::CQO:
                out += "cqo\n";
                break;
            case MOpc::CDQ:
                out += "cdq\n";
                break;

            case MOpc::PUSH64r:
                if (np >= 1 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += "push " + std::string{reg64(ops[0].reg.phys_reg())} + "\n";
                else
                    out += "; pushr unrecognized\n";
                break;
            case MOpc::POP64r:
                if (np >= 1 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical())
                    out += "pop " + std::string{reg64(ops[0].reg.phys_reg())} + "\n";
                else
                    out += "; popr unrecognized\n";
                break;
            case MOpc::PUSHFQ:
                out += "pushfq\n";
                break;
            case MOpc::POPFQ:
                out += "popfq\n";
                break;
            case MOpc::PUSH64i:
                if (np >= 1 && ops[0].kind == MOpKind::Imm64)
                    out += "push " + std::to_string(ops[0].imm) + "\n";
                else
                    out += "; pushi unrecognized\n";
                break;
            case MOpc::PUSH64m:
                if (np >= 1 && ops[0].kind == MOpKind::Mem)
                    out += "push " + format_op(ctx, ops[0], RegWidth::Bits64, true) + "\n";
                else
                    out += "; pushm unrecognized\n";
                break;

            case MOpc::MOVSD_rr:
            case MOpc::MOVSDrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "movsd " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movsdrr unrecognized\n";
                break;
            case MOpc::MOVSD_rm:
            case MOpc::MOVSDrm:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Mem)
                    out += "movsd " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + format_op(ctx, ops[1], RegWidth::XMM, true) + "\n";
                else
                    out += "; movsdrm unrecognized\n";
                break;
            case MOpc::MOVSD_mr:
            case MOpc::MOVSDmr:
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "movsd " + format_op(ctx, ops[0], RegWidth::XMM, true) + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movsdmr unrecognized\n";
                break;

            case MOpc::MOVQ64rr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "movq " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{reg64(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movq unrecognized\n";
                break;

            case MOpc::ADDSD:
            case MOpc::ADDSDrr:
            case MOpc::SUBSD:
            case MOpc::SUBSDrr:
            case MOpc::MULSD:
            case MOpc::MULSDrr:
            case MOpc::DIVSD:
            case MOpc::DIVSDrr: {
                std::string mnem;
                if (opc == MOpc::ADDSD || opc == MOpc::ADDSDrr)
                    mnem = "addsd";
                else if (opc == MOpc::SUBSD || opc == MOpc::SUBSDrr)
                    mnem = "subsd";
                else if (opc == MOpc::MULSD || opc == MOpc::MULSDrr)
                    mnem = "mulsd";
                else
                    mnem = "divsd";
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Reg && ops[2].reg.is_physical())
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    auto r = ops[2].reg.phys_reg();
                    if (d != l)
                        out += "movsd " + std::string{phys_reg_name(d)} + ", " + std::string{phys_reg_name(l)} + "\n    ";
                    out += mnem + " " + std::string{phys_reg_name(d)} + ", " + std::string{phys_reg_name(r)} + "\n";
                }
                else
                    out += "; ssesd unrecognized\n";
                break;
            }

            case MOpc::ADDSDrm:
            case MOpc::SUBSDrm:
            case MOpc::MULSDrm:
            case MOpc::DIVSDrm: {
                std::string mnem = (opc == MOpc::ADDSDrm) ? "addsd" : (opc == MOpc::SUBSDrm) ? "subsd" : (opc == MOpc::MULSDrm) ? "mulsd" : "divsd";
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Mem)
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    if (d != l)
                        out += "movsd " + std::string{phys_reg_name(d)} + ", " + std::string{phys_reg_name(l)} + "\n    ";
                    out += mnem + " " + std::string{phys_reg_name(d)} + ", " + format_op(ctx, ops[2], RegWidth::XMM, true) + "\n";
                }
                else
                    out += "; ssesdrm unrecognized\n";
                break;
            }

            case MOpc::UCOMISD:
            case MOpc::UCOMISDrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "ucomisd " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; ucomisd unrecognized\n";
                break;

            case MOpc::CVTSI2SD_r:
            case MOpc::CVTSI2SDrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cvtsi2sd " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{reg32(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cvtsi2sd unrecognized\n";
                break;

            case MOpc::CVTTSD2SI_r:
            case MOpc::CVTSD2SIrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cvttsd2si " + std::string{reg32(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cvttsd2si unrecognized\n";
                break;

            case MOpc::CVTSD2SI64rr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cvttsd2si " + std::string{reg64(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cvtsd2si64 unrecognized\n";
                break;

            case MOpc::CVTSD2SS_r:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cvtsd2ss " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cvtsd2ss unrecognized\n";
                break;

            case MOpc::CVTSS2SD_r:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cvtss2sd " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cvtss2sd unrecognized\n";
                break;

            case MOpc::MOVSS_rr:
            case MOpc::MOVSSrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "movss " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movssrr unrecognized\n";
                break;
            case MOpc::MOVSS_rm:
            case MOpc::MOVSSrm:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Mem)
                    out += "movss " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + format_op(ctx, ops[1], RegWidth::XMM, true) + "\n";
                else
                    out += "; movssrm unrecognized\n";
                break;
            case MOpc::MOVSS_mr:
            case MOpc::MOVSSmr:
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "movss " + format_op(ctx, ops[0], RegWidth::XMM, true) + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movssmr unrecognized\n";
                break;

            case MOpc::ADDSS:
            case MOpc::ADDSSrr:
            case MOpc::SUBSS:
            case MOpc::SUBSSrr:
            case MOpc::MULSS:
            case MOpc::MULSSrr:
            case MOpc::DIVSS:
            case MOpc::DIVSSrr: {
                std::string mnem;
                if (opc == MOpc::ADDSS || opc == MOpc::ADDSSrr)
                    mnem = "addss";
                else if (opc == MOpc::SUBSS || opc == MOpc::SUBSSrr)
                    mnem = "subss";
                else if (opc == MOpc::MULSS || opc == MOpc::MULSSrr)
                    mnem = "mulss";
                else
                    mnem = "divss";
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Reg && ops[2].reg.is_physical())
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    auto r = ops[2].reg.phys_reg();
                    if (d != l)
                        out += "movss " + std::string{phys_reg_name(d)} + ", " + std::string{phys_reg_name(l)} + "\n    ";
                    out += mnem + " " + std::string{phys_reg_name(d)} + ", " + std::string{phys_reg_name(r)} + "\n";
                }
                else
                    out += "; sses unrecognized\n";
                break;
            }

            case MOpc::ADDSSrm:
            case MOpc::SUBSSrm:
            case MOpc::MULSSrm:
            case MOpc::DIVSSrm: {
                std::string mnem = (opc == MOpc::ADDSSrm) ? "addss" : (opc == MOpc::SUBSSrm) ? "subss" : (opc == MOpc::MULSSrm) ? "mulss" : "divss";
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical() &&
                    ops[2].kind == MOpKind::Mem)
                {
                    auto d = ops[0].reg.phys_reg();
                    auto l = ops[1].reg.phys_reg();
                    if (d != l)
                        out += "movss " + std::string{phys_reg_name(d)} + ", " + std::string{phys_reg_name(l)} + "\n    ";
                    out += mnem + " " + std::string{phys_reg_name(d)} + ", " + format_op(ctx, ops[2], RegWidth::XMM, true) + "\n";
                }
                else
                    out += "; ssesrm unrecognized\n";
                break;
            }

            case MOpc::UCOMISS:
            case MOpc::UCOMISSrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "ucomiss " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; ucomiss unrecognized\n";
                break;

            case MOpc::CVTSI2SS_r:
            case MOpc::CVTSI2SSrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cvtsi2ss " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{reg32(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cvtsi2ss unrecognized\n";
                break;

            case MOpc::CVTTSS2SI_r:
            case MOpc::CVTSS2SIrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cvttss2si " + std::string{reg32(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cvttss2si unrecognized\n";
                break;

            case MOpc::CVTSS2SI64rr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "cvttss2si " + std::string{reg64(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; cvtss2si64 unrecognized\n";
                break;

            case MOpc::XORPSrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "xorps " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; xorps unrecognized\n";
                break;
            case MOpc::XORPDrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "xorpd " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; xorpd unrecognized\n";
                break;
            case MOpc::MOVAPSrr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "movaps " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movapsrr unrecognized\n";
                break;
            case MOpc::MOVAPSrm:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Mem)
                    out += "movaps " + std::string{phys_reg_name(ops[0].reg.phys_reg())} + ", " + format_op(ctx, ops[1], RegWidth::XMM, true) + "\n";
                else
                    out += "; movapsrm unrecognized\n";
                break;
            case MOpc::MOVAPSmr:
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "movaps " + format_op(ctx, ops[0], RegWidth::XMM, true) + ", " + std::string{phys_reg_name(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; movapsmr unrecognized\n";
                break;

            case MOpc::LOCK_XADD64mr:
            case MOpc::LOCK_XADD32mr:
            case MOpc::LOCK_XCHG64mr:
            case MOpc::LOCK_XADD:
            case MOpc::LOCK_XCHG:
            case MOpc::LOCK_AND:
            case MOpc::LOCK_OR:
            case MOpc::LOCK_XOR: {
                std::string mnem;
                if (opc == MOpc::LOCK_XADD64mr || opc == MOpc::LOCK_XADD32mr || opc == MOpc::LOCK_XADD)
                    mnem = "lock xadd";
                else if (opc == MOpc::LOCK_XCHG64mr || opc == MOpc::LOCK_XCHG)
                    mnem = "lock xchg";
                else if (opc == MOpc::LOCK_AND)
                    mnem = "lock and";
                else if (opc == MOpc::LOCK_OR)
                    mnem = "lock or";
                else
                    mnem = "lock xor";
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += mnem + " " + format_op(ctx, ops[0], w, true) + ", " + std::string{rn(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; lock atomic unrecognized\n";
                break;
            }

            case MOpc::LOCK_AND64mi32:
            case MOpc::LOCK_OR64mi32:
            case MOpc::LOCK_XOR64mi32: {
                std::string mnem = (opc == MOpc::LOCK_AND64mi32) ? "lock and" : (opc == MOpc::LOCK_OR64mi32) ? "lock or" : "lock xor";
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Imm64)
                    out += mnem + " " + format_op(ctx, ops[0], RegWidth::Bits64, true) + ", " + std::to_string(ops[1].imm) + "\n";
                else
                    out += "; lock mi unrecognized\n";
                break;
            }

            case MOpc::MFENCE:
                out += "mfence\n";
                break;
            case MOpc::LFENCE:
                out += "lfence\n";
                break;
            case MOpc::SFENCE:
                out += "sfence\n";
                break;

            case MOpc::UD2:
                out += "ud2\n";
                break;
            case MOpc::NOP:
                out += "nop\n";
                break;

            case MOpc::XCHG64rr:
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[0].reg.is_physical() && ops[1].kind == MOpKind::Reg && ops[1].reg.is_physical())
                    out += "xchg " + std::string{reg64(ops[0].reg.phys_reg())} + ", " + std::string{reg64(ops[1].reg.phys_reg())} + "\n";
                else
                    out += "; xchg unrecognized\n";
                break;

            default:
                out += "ud2 ; unsupported opcode\n";
                break;
        }
    }

    void emit_int_data(std::string& out, std::int64_t value, std::uint64_t size)
    {
        switch (size)
        {
            case 1:
                out += "    db " + std::to_string(static_cast<std::uint8_t>(value & 0xFF)) + "\n";
                break;
            case 2:
                out += "    dw " + std::to_string(static_cast<std::uint16_t>(value & 0xFFFF)) + "\n";
                break;
            case 4:
                out += "    dd " + std::to_string(static_cast<std::uint32_t>(value & 0xFFFFFFFF)) + "\n";
                break;
            default:
                out += "    dq " + std::to_string(value) + "\n";
                break;
        }
    }

    void emit_init_value(std::string& out, ir::IrValue const* val, ir::IrType const* expected_type, std::uint64_t byte_size)
    {
        if (!val)
        {
            if (byte_size > 0)
                out += "    times " + std::to_string(byte_size) + " db 0\n";
            return;
        }

        switch (val->kind)
        {
            case IrNodeKind::IntConstant: {
                auto* ic = static_cast<IrIntConstant const*>(val);
                emit_int_data(out, ic->value, expected_type ? expected_type->byte_size : 8);
                break;
            }
            case IrNodeKind::FloatConstant: {
                auto* fc = static_cast<IrFloatConstant const*>(val);
                auto bits = (expected_type && expected_type->kind == IrTypeKind::Float) ? static_cast<IrFloatType const*>(expected_type)->bits : 64;
                if (bits == 32)
                {
                    float f = static_cast<float>(fc->value);
                    std::uint32_t raw;
                    std::memcpy(&raw, &f, 4);
                    char buf[24];
                    std::snprintf(buf, sizeof buf, "    dd 0x%08x", raw);
                    out += buf;
                    out += '\n';
                }
                else
                {
                    double d = fc->value;
                    std::uint64_t raw;
                    std::memcpy(&raw, &d, 8);
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "    dq 0x%016lx", static_cast<unsigned long>(raw));
                    out += buf;
                    out += '\n';
                }
                break;
            }
            case IrNodeKind::BoolConstant: {
                auto* bc = static_cast<IrBoolConstant const*>(val);
                out += "    db " + std::string(bc->value ? "1" : "0") + "\n";
                break;
            }
            case IrNodeKind::NullConstant: {
                auto sz = expected_type ? expected_type->byte_size : 8;
                if (sz <= 8)
                    out += "    dq 0\n";
                else
                    out += "    times " + std::to_string(sz) + " db 0\n";
                break;
            }
            case IrNodeKind::StringConstant: {
                auto* sc = static_cast<IrStringConstant const*>(val);
                out += "    db \"";
                for (char c : sc->value)
                {
                    if (c == '"' || c == '\\')
                    {
                        out += '\\';
                        out += c;
                    }
                    else if (c >= 32 && c < 127)
                        out += c;
                    else
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\x%02x", static_cast<unsigned char>(c));
                        out += buf;
                    }
                }
                out += "\", 0\n";
                break;
            }
            case IrNodeKind::GlobalRef: {
                auto* gr = static_cast<IrGlobalRef const*>(val);
                out += "    dq " + std::string{gr->name} + "\n";
                break;
            }
            case IrNodeKind::Aggregate: {
                auto* agg = static_cast<IrAggregateInst const*>(val);
                auto const* agg_type = agg->type ? agg->type : expected_type;
                if (!agg_type)
                {
                    out += "    times " + std::to_string(byte_size > 0 ? byte_size : 1) + " db 0\n";
                    break;
                }

                if (agg_type->kind == IrTypeKind::Aggregate)
                {
                    auto* at = static_cast<IrAggregateType const*>(agg_type);
                    auto num_fields = std::min(agg->values.size(), at->members.size());
                    std::uint64_t cur_offset = 0;
                    for (std::size_t i = 0; i < num_fields; ++i)
                    {
                        auto field_off = (i < at->member_offsets.size()) ? at->member_offsets[i] : cur_offset;
                        if (field_off > cur_offset)
                            out += "    times " + std::to_string(field_off - cur_offset) + " db 0\n";

                        auto* fval = agg->values[i];
                        auto* field_type = (i < at->members.size()) ? at->members[i] : nullptr;
                        if (fval)
                            emit_init_value(out, fval, field_type, field_type ? field_type->byte_size : 0);

                        else
                        {
                            auto fsz = field_type ? field_type->byte_size : 0;
                            if (fsz > 0)
                                out += "    times " + std::to_string(fsz) + " db 0\n";
                        }
                        cur_offset = field_off + (field_type ? field_type->byte_size : 0);
                    }
                    if (byte_size > cur_offset)
                        out += "    times " + std::to_string(byte_size - cur_offset) + " db 0\n";
                }
                else if (agg_type->kind == IrTypeKind::Array)
                {
                    auto* at = static_cast<IrArrayType const*>(agg_type);
                    for (auto* fval : agg->values)
                    {
                        if (fval)
                            emit_init_value(out, fval, at->element, at->element ? at->element->byte_size : 0);
                        else
                        {
                            auto esz = at->element ? at->element->byte_size : 1;
                            out += "    times " + std::to_string(esz) + " db 0\n";
                        }
                    }
                }
                else
                    out += "    times " + std::to_string(byte_size > 0 ? byte_size : 1) + " db 0\n";
                break;
            }
            default:
                if (byte_size > 0)
                    out += "    times " + std::to_string(byte_size) + " db 0\n";
                break;
        }
    }

    [[nodiscard]] std::string emit_globals(ir::IrModule const& ir_mod, std::unordered_set<std::string>& defined_syms)
    {
        std::string out;

        for (auto* func : ir_mod.functions)
            if (func && !func->name.empty())
                defined_syms.insert(std::string{func->name});

        std::vector<std::string> externs;
        for (auto* g : ir_mod.globals)
        {
            if (!g)
                continue;

            std::string name{g->name};
            if (g->linkage == ir::Linkage::External && !g->init && !defined_syms.contains(name))
                externs.push_back(name);
        }

        std::unordered_set<std::string> refs;
        auto collect = [&](auto& self, ir::IrValue const* v) -> void {
            if (!v)
                return;

            if (v->kind == ir::IrNodeKind::GlobalRef)
            {
                auto* gr = static_cast<ir::IrGlobalRef const*>(v);
                if (!defined_syms.contains(std::string{gr->name}))
                    refs.insert(std::string{gr->name});
            }
            else if (v->kind == ir::IrNodeKind::Aggregate)
            {
                auto* agg = static_cast<ir::IrAggregateInst const*>(v);
                for (auto* fv : agg->values)
                    self(self, fv);
            }
        };
        for (auto* g : ir_mod.globals)
            if (g && g->init)
                collect(collect, g->init);

        for (auto const& n : refs)
            if (!defined_syms.contains(n))
                externs.push_back(n);

        std::ranges::sort(externs);
        externs.erase(std::unique(externs.begin(), externs.end()), externs.end());

        if (!externs.empty())
        {
            out += "; external symbols\n";
            for (auto const& name : externs)
                out += "extern " + name + "\n";
            out += "\n";
        }

        struct Gl
        {
            ir::IrGlobal const* g;
            bool constant;
            bool has_init;
            std::string name;
            std::uint64_t size;
        };
        std::vector<Gl> rodata, data, bss;

        for (auto* g : ir_mod.globals)
        {
            if (!g)
                continue;

            if (g->linkage == ir::Linkage::Internal)
                continue;

            std::string name{g->name};
            if (g->linkage == ir::Linkage::External && !g->init)
                if (!defined_syms.contains(name))
                    continue;

            auto sz = g->type ? g->type->byte_size : 0;
            if (g->is_constant && g->init)
                rodata.push_back({g, true, true, name, sz});
            else if (g->init)
                data.push_back({g, false, true, name, sz});
            else
                bss.push_back({g, false, false, name, sz});
        }

        if (!rodata.empty())
        {
            out += "section .rodata\n";
            for (auto const& gl : rodata)
            {
                if (gl.g->linkage != ir::Linkage::Internal)
                    out += "global " + gl.name + "\n";
                out += gl.name + ":\n";
                emit_init_value(out, gl.g->init, gl.g->type, gl.size);
                out += '\n';
            }
        }

        if (!data.empty())
        {
            out += "section .data\n";
            for (auto const& gl : data)
            {
                if (gl.g->linkage != ir::Linkage::Internal)
                    out += "global " + gl.name + "\n";
                out += gl.name + ":\n";
                emit_init_value(out, gl.g->init, gl.g->type, gl.size);
                out += '\n';
            }
        }

        if (!bss.empty())
        {
            out += "section .bss\n";
            for (auto const& gl : bss)
            {
                if (gl.g->linkage != ir::Linkage::Internal)
                    out += "global " + gl.name + "\n";
                out += gl.name + ":\n";
                out += "    resb " + std::to_string(gl.size > 0 ? gl.size : 1) + "\n";
            }
        }

        return out;
    }

} // anonymous namespace

export namespace dcc::backend::em64t
{
    [[nodiscard]] std::string emit_intel_asm(ir::IrModule const& ir_mod, std::vector<MFunction> const& functions, target::TargetConfig const&)
    {
        std::string out;
        out += "; generated by dcc em64t backend\n";
        out += "; module: ";
        out += ir_mod.name.empty() ? "<unnamed>" : std::string{ir_mod.name};
        out += "\n\n";

        std::unordered_set<std::string> defined_syms;

        if (!functions.empty())
        {
            out += "section .text\n";

            for (auto const& mfunc : functions)
            {
                std::string func_name = mfunc.owned_name;
                if (func_name.empty())
                    continue;
                defined_syms.insert(func_name);

                std::unordered_map<std::uint32_t, std::string> block_labels;
                for (auto const& bb : mfunc.blocks)
                {
                    if (bb.id == mfunc.entry_block_id)
                        continue;

                    std::string lbl;
                    if (!bb.owned_name.empty())
                        lbl = std::string{"."} + bb.owned_name;
                    else
                        lbl = std::format(".bb{}", bb.id);

                    block_labels[bb.id] = lbl;
                }

                bool is_internal = false;
                for (auto* irf : ir_mod.functions)
                {
                    if (irf && irf->name == mfunc.owned_name && irf->linkage == ir::Linkage::Internal)
                    {
                        is_internal = true;
                        break;
                    }
                }
                if (!is_internal)
                    out += "global " + func_name + "\n";

                out += func_name + ":\n";

                AsmContext ctx{out, &mfunc, block_labels};
                for (auto const& bb : mfunc.blocks)
                {
                    auto it = block_labels.find(bb.id);
                    if (it != block_labels.end())
                        out += it->second + ":\n";

                    for (auto const& mi : bb.instrs)
                        emit_instr(ctx, mi);
                }
                out += '\n';
            }
        }

        {
            bool has_jt_data = false;
            for (auto const& mfunc : functions)
                if (!mfunc.jump_tables.empty())
                {
                    has_jt_data = true;
                    break;
                }

            if (has_jt_data)
            {
                out += "section .rodata\n";
                for (auto const& mfunc : functions)
                {
                    for (auto const& jt : mfunc.jump_tables)
                    {
                        out += jt.symbol + ":\n";
                        for (auto tgt : jt.targets)
                        {
                            std::string blk_lbl;
                            auto* blk = mfunc.block_by_id(tgt);
                            if (blk && !blk->owned_name.empty())
                                blk_lbl = "." + blk->owned_name;
                            else
                                blk_lbl = std::format(".bb{}", tgt);

                            out += "    dd " + blk_lbl + " - ($ + 4)\n";
                        }
                        out += '\n';
                    }
                }
            }
        }

        out += emit_globals(ir_mod, defined_syms);
        return out;
    }

} // namespace dcc::backend::em64t
