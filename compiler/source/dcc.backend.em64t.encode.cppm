export module dcc.backend.em64t.encode;

import std;
import dcc.backend.em64t.mir;

export namespace dcc::backend::em64t
{
    struct Reloc
    {
        std::uint32_t offset;
        std::string_view symbol;
        enum class Kind : std::uint8_t
        {
            Abs64,
            Rel32,
            Rel32_Got,
            Rel32_Call,
        } kind;
        std::int64_t addend{0};
    };

    struct EncodeResult
    {
        std::vector<std::uint8_t> bytes;
        std::vector<Reloc> relocs;
        std::unordered_map<std::uint32_t, std::uint32_t> block_offsets;
        std::vector<std::string> warnings;
    };

    [[nodiscard]] EncodeResult encode_function(MFunction const& func);

} // namespace dcc::backend::em64t

namespace
{
    using namespace dcc::backend::em64t;

    static void emit_u8(std::vector<std::uint8_t>& buf, std::uint8_t v)
    {
        buf.push_back(v);
    }

    static void emit_u16_le(std::vector<std::uint8_t>& buf, std::uint16_t v)
    {
        buf.push_back(static_cast<std::uint8_t>(v));
        buf.push_back(static_cast<std::uint8_t>(v >> 8));
    }

    static void emit_u32_le(std::vector<std::uint8_t>& buf, std::uint32_t v)
    {
        buf.push_back(static_cast<std::uint8_t>(v));
        buf.push_back(static_cast<std::uint8_t>(v >> 8));
        buf.push_back(static_cast<std::uint8_t>(v >> 16));
        buf.push_back(static_cast<std::uint8_t>(v >> 24));
    }

    static void emit_u64_le(std::vector<std::uint8_t>& buf, std::uint64_t v)
    {
        emit_u32_le(buf, static_cast<std::uint32_t>(v));
        emit_u32_le(buf, static_cast<std::uint32_t>(v >> 32));
    }

    static void emit_rex(std::vector<std::uint8_t>& buf, bool w, bool r, bool x, bool b)
    {
        std::uint8_t rex = 0x40;
        if (w)
            rex |= 0x08;
        if (r)
            rex |= 0x04;
        if (x)
            rex |= 0x02;
        if (b)
            rex |= 0x01;
        if (rex != 0x40)
            buf.push_back(rex);
    }

    static std::uint8_t reg_x86_num(PhysReg r)
    {
        auto v = static_cast<std::uint8_t>(r);
        if (v >= static_cast<std::uint8_t>(PhysReg::XMM0) && v <= static_cast<std::uint8_t>(PhysReg::XMM15))
            return v - static_cast<std::uint8_t>(PhysReg::XMM0);
        return v;
    }

    static std::uint8_t reg_low3(PhysReg r)
    {
        return reg_x86_num(r) & 0x7;
    }
    static bool reg_is_extended(PhysReg r)
    {
        return reg_x86_num(r) >= 8;
    }

    static void emit_rex_if_extended(std::vector<std::uint8_t>& buf, bool w, PhysReg reg_field, PhysReg rm_field)
    {
        auto rv = reg_x86_num(reg_field);
        auto bv = reg_x86_num(rm_field);
        if (w || rv >= 8 || bv >= 8)
            emit_rex(buf, w, rv >= 8, false, bv >= 8);
    }

    static void emit_rex_8bit(std::vector<std::uint8_t>& buf, PhysReg r1, PhysReg r2)
    {
        auto v1 = reg_x86_num(r1);
        auto v2 = reg_x86_num(r2);
        if (v1 >= 4 || v2 >= 4)
            emit_rex(buf, false, v1 >= 8, false, v2 >= 8);
    }

    static void emit_modrm(std::vector<std::uint8_t>& buf, std::uint8_t mod, std::uint8_t reg, std::uint8_t rm)
    {
        buf.push_back(static_cast<std::uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
    }

    static void emit_sib(std::vector<std::uint8_t>& buf, std::uint8_t scale, std::uint8_t index, std::uint8_t base)
    {
        static constexpr std::uint8_t kSE[9] = {0, 0, 1, 0, 2, 0, 0, 0, 3};
        auto sc = (scale < 9) ? kSE[scale] : std::uint8_t{0};
        buf.push_back(static_cast<std::uint8_t>((sc << 6) | ((index & 7) << 3) | (base & 7)));
    }

    static PhysReg resolve_phys_reg(MOp const& op, std::vector<std::string>& wrn, char const* ctx)
    {
        if (op.kind == MOpKind::Reg && op.reg.is_physical())
            return op.reg.phys_reg();
        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
            wrn.push_back(std::format("{} virtual %{} reached encoder, using R11", ctx, op.reg.id));
        else if (op.kind != MOpKind::Reg)
            wrn.push_back(std::format("{} non-reg operand, using R11", ctx));
        return PhysReg::R11;
    }

    static void emit_sse_binop_rr(std::vector<std::uint8_t>& buf, std::uint8_t pfx, std::uint8_t sse, PhysReg d, PhysReg l, PhysReg r)
    {
        auto mov_rr = [&](PhysReg dst, PhysReg src) {
            if (dst == src)
                return;

            emit_u8(buf, pfx);
            emit_rex_if_extended(buf, false, dst, src);
            emit_u8(buf, 0x0F);
            emit_u8(buf, 0x10);
            emit_modrm(buf, 3, reg_low3(dst), reg_low3(src));
        };
        auto op_rr = [&](PhysReg dst, PhysReg src) {
            emit_u8(buf, pfx);
            emit_rex_if_extended(buf, false, dst, src);
            emit_u8(buf, 0x0F);
            emit_u8(buf, sse);
            emit_modrm(buf, 3, reg_low3(dst), reg_low3(src));
        };

        if (d == r && d != l)
        {
            bool commutative = (sse == 0x58 || sse == 0x59);
            if (commutative)
            {
                op_rr(d, l);
                return;
            }

            PhysReg tmp = PhysReg::XMM15;
            for (auto cand : {PhysReg::XMM15, PhysReg::XMM14, PhysReg::XMM13})
            {
                if (cand != d && cand != l)
                {
                    tmp = cand;
                    break;
                }
            }

            mov_rr(tmp, d);
            mov_rr(d, l);
            op_rr(d, tmp);
            return;
        }

        mov_rr(d, l);
        op_rr(d, r);
    }

    static void emit_mem(std::vector<std::uint8_t>& buf, MMem const& mem, std::uint8_t reg_code, std::vector<std::string>& wrn, char const* ctx)
    {
        bool hb = mem.base.is_valid(), hi = mem.index.is_valid();
        auto disp = mem.disp;
        auto sc = mem.scale;
        PhysReg bp = PhysReg::RBP, ip = PhysReg::None;
        bool rbp = false, rsp12 = false;
        if (hb)
        {
            if (mem.base.is_physical())
            {
                bp = mem.base.phys_reg();
                rbp = (bp == PhysReg::RBP || bp == PhysReg::R13);
                rsp12 = (bp == PhysReg::RSP || bp == PhysReg::R12);
            }
            else
            {
                wrn.push_back(std::format("{} virt base, using RBP", ctx));
                bp = PhysReg::RBP;
                rbp = true;
            }
        }
        if (hi)
        {
            if (mem.index.is_physical())
                ip = mem.index.phys_reg();
            else
            {
                wrn.push_back(std::format("{} virt index, using R11", ctx));
                ip = PhysReg::R11;
            }
        }
        std::uint8_t bc = hb ? reg_low3(bp) : 0, ic = hi ? reg_low3(ip) : 0;
        bool need_sib = hi || (hb && rsp12);
        bool rbp_fake = hb && rbp;
        if (!hb && !hi)
        {
            emit_modrm(buf, 0, reg_code, 5);
            emit_u32_le(buf, static_cast<std::uint32_t>(disp));
            return;
        }
        if (!need_sib)
        {
            if (disp == 0 && !rbp_fake)
                emit_modrm(buf, 0, reg_code, bc);
            else if (disp >= -128 && disp <= 127 && !rbp_fake)
            {
                emit_modrm(buf, 1, reg_code, bc);
                emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(disp)));
            }
            else
            {
                emit_modrm(buf, 2, reg_code, bc);
                emit_u32_le(buf, static_cast<std::uint32_t>(disp));
            }
        }
        else
        {
            std::uint8_t sib_base = hb ? bc : 5;
            std::uint8_t sib_idx = hi ? ic : 4;
            if (!hb && hi)
            {
                emit_modrm(buf, 0, reg_code, 4);
                emit_sib(buf, sc, sib_idx, 5);
                emit_u32_le(buf, static_cast<std::uint32_t>(disp));
                return;
            }
            if (disp == 0 && !rbp_fake)
            {
                emit_modrm(buf, 0, reg_code, 4);
                emit_sib(buf, sc, sib_idx, sib_base);
            }
            else if (disp >= -128 && disp <= 127 && !rbp_fake)
            {
                emit_modrm(buf, 1, reg_code, 4);
                emit_sib(buf, sc, sib_idx, sib_base);
                emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(disp)));
            }
            else
            {
                emit_modrm(buf, 2, reg_code, 4);
                emit_sib(buf, sc, sib_idx, sib_base);
                emit_u32_le(buf, static_cast<std::uint32_t>(disp));
            }
        }
    }

    static std::uint8_t jcc_byte(MOpc o)
    {
        switch (o)
        {
            case MOpc::JE:
                return 0x84;
            case MOpc::JNE:
                return 0x85;
            case MOpc::JL:
                return 0x8C;
            case MOpc::JGE:
                return 0x8D;
            case MOpc::JLE:
                return 0x8E;
            case MOpc::JG:
                return 0x8F;
            case MOpc::JB:
                return 0x82;
            case MOpc::JAE:
                return 0x83;
            case MOpc::JBE:
                return 0x86;
            case MOpc::JA:
                return 0x87;
            case MOpc::JP:
                return 0x8A;
            case MOpc::JNP:
                return 0x8B;
            case MOpc::JS:
                return 0x88;
            case MOpc::JNS:
                return 0x89;
            default:
                return 0x84;
        }
    }

    static std::uint8_t setcc_byte(MOpc o)
    {
        switch (o)
        {
            case MOpc::SETEr:
                return 0x94;
            case MOpc::SETNEr:
                return 0x95;
            case MOpc::SETLr:
                return 0x9C;
            case MOpc::SETGEr:
                return 0x9D;
            case MOpc::SETLEr:
                return 0x9E;
            case MOpc::SETGr:
                return 0x9F;
            case MOpc::SETBr:
                return 0x92;
            case MOpc::SETAEr:
                return 0x93;
            case MOpc::SETBEr:
                return 0x96;
            case MOpc::SETAr:
                return 0x97;
            default:
                return 0x94;
        }
    }

    static std::uint8_t cmov_byte(MOpc o)
    {
        switch (o)
        {
            case MOpc::CMOV64Err:
                return 0x44;
            case MOpc::CMOV64NErr:
                return 0x45;
            case MOpc::CMOV64Lrr:
                return 0x4C;
            case MOpc::CMOV64GErr:
                return 0x4D;
            case MOpc::CMOV64LErr:
                return 0x4E;
            case MOpc::CMOV64Grr:
                return 0x4F;
            case MOpc::CMOV64Brr:
                return 0x42;
            case MOpc::CMOV64AErr:
                return 0x43;
            case MOpc::CMOV64BErr:
                return 0x46;
            case MOpc::CMOV64Arr:
                return 0x47;
            default:
                return 0x44;
        }
    }

    static bool is_64bit_opc(MOpc o)
    {
        switch (o)
        {
            case MOpc::MOV64rr:
            case MOpc::MOV64ri:
            case MOpc::MOV64ri32:
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
            case MOpc::CMP64rr:
            case MOpc::CMP64ri32:
            case MOpc::CMP64rm:
            case MOpc::CMP64ri:
            case MOpc::TEST64rr:
            case MOpc::TEST64ri:
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
            case MOpc::LEA64rm:
            case MOpc::CQO:
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
            case MOpc::PUSH64r:
            case MOpc::POP64r:
            case MOpc::PUSH64i:
            case MOpc::PUSH64m:
            case MOpc::XCHG64rr:
                return true;
            default:
                return false;
        }
    }

    static std::uint8_t alu_ext(MOpc o)
    {
        switch (o)
        {
            case MOpc::ADD64ri32:
            case MOpc::ADD32ri:
            case MOpc::ADD64rr:
            case MOpc::ADD32rr:
            case MOpc::ADD64rm:
            case MOpc::ADD32rm:
                return 0;
            case MOpc::OR64ri32:
            case MOpc::OR32ri:
            case MOpc::OR64rr:
            case MOpc::OR32rr:
                return 1;
            case MOpc::AND64ri32:
            case MOpc::AND32ri:
            case MOpc::AND64rr:
            case MOpc::AND32rr:
                return 4;
            case MOpc::SUB64ri32:
            case MOpc::SUB32ri:
            case MOpc::SUB64rr:
            case MOpc::SUB32rr:
            case MOpc::SUB64ri:
                return 5;
            case MOpc::XOR64ri32:
            case MOpc::XOR32ri:
            case MOpc::XOR64rr:
            case MOpc::XOR32rr:
                return 6;
            case MOpc::CMP64ri32:
            case MOpc::CMP32ri:
            case MOpc::CMP64rr:
            case MOpc::CMP32rr:
            case MOpc::CMP8ri:
            case MOpc::CMP8rr:
            case MOpc::CMP64ri:
                return 7;
            default:
                return 0;
        }
    }

    static std::uint8_t alu_rm_r(MOpc o)
    {
        switch (o)
        {
            case MOpc::ADD64rr:
            case MOpc::ADD32rr:
            case MOpc::ADD64rm:
            case MOpc::ADD32rm:
                return 0x01;
            case MOpc::SUB64rr:
            case MOpc::SUB32rr:
            case MOpc::SUB64rm:
            case MOpc::SUB32rm:
                return 0x29;
            case MOpc::AND64rr:
            case MOpc::AND32rr:
            case MOpc::AND64rm:
                return 0x21;
            case MOpc::OR64rr:
            case MOpc::OR32rr:
            case MOpc::OR64rm:
                return 0x09;
            case MOpc::XOR64rr:
            case MOpc::XOR32rr:
            case MOpc::XOR64rm:
                return 0x31;
            case MOpc::CMP64rr:
            case MOpc::CMP32rr:
            case MOpc::CMP64rm:
                return 0x39;
            case MOpc::TEST64rr:
            case MOpc::TEST32rr:
                return 0x85;
            case MOpc::TEST8rr:
                return 0x84;
            default:
                return 0x01;
        }
    }

    struct BranchPatch
    {
        std::size_t patch_offset;
        std::uint32_t target_label;
    };

    static void encode_instr(MInstr const& instr, std::vector<std::uint8_t>& buf, std::vector<BranchPatch>& branches, std::vector<Reloc>& relocs,
                             std::vector<std::string>& wrn)
    {
        auto const& ops = instr.ops;
        std::uint8_t np = instr.num_ops;

        switch (instr.opc)
        {
            case MOpc::COPY: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "COPY");
                    auto s = resolve_phys_reg(ops[1], wrn, "COPY");
                    if (d == s)
                    {
                        break;
                    }
                    if (reg_class(d) == RegClass::XMM && reg_class(s) == RegClass::XMM)
                    {
                        wrn.emplace_back("COPY reached encoder (XMM-XMM); encoding as MOVAPSrr");
                        emit_rex_if_extended(buf, false, d, s);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x28);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                    }
                    else
                    {
                        wrn.emplace_back("COPY reached encoder (dst!=src); encoding as MOV64rr");
                        emit_rex_if_extended(buf, true, s, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(s), reg_low3(d));
                    }
                }
                else
                {
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x0B);
                }
                break;
            }
            case MOpc::IMPLICIT_DEF:
            case MOpc::PHI:
                break;

            case MOpc::MOV64rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV64rr");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOV64rr");
                    emit_rex_if_extended(buf, true, s, d);
                    emit_u8(buf, 0x89);
                    emit_modrm(buf, 3, reg_low3(s), reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV64ri: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV64ri");
                    auto v = ops[1].imm;
                    emit_rex_if_extended(buf, true, PhysReg::RAX, d);
                    emit_u8(buf, static_cast<std::uint8_t>(0xB8 + reg_low3(d)));
                    emit_u64_le(buf, static_cast<std::uint64_t>(v));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV64ri32: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV64ri32");
                    auto v = static_cast<std::int32_t>(ops[1].imm & 0xFFFFFFFF);
                    emit_rex_if_extended(buf, true, PhysReg::RAX, d);
                    emit_u8(buf, 0xC7);
                    emit_modrm(buf, 3, 0, reg_low3(d));
                    emit_u32_le(buf, static_cast<std::uint32_t>(v));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV64rm:
            case MOpc::MOV32rm: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVrm");
                    auto const& m = ops[1].mem;
                    if (!m.symbol.empty())
                    {
                        bool de = reg_is_extended(d);
                        emit_rex(buf, w64, de, false, false);
                        emit_u8(buf, 0x8B);
                        emit_modrm(buf, 0, reg_low3(d), 5);
                        Reloc rel;
                        rel.offset = static_cast<std::uint32_t>(buf.size());
                        rel.symbol = m.symbol;
                        rel.kind = m.is_got_indirect ? Reloc::Kind::Rel32_Got : Reloc::Kind::Rel32;
                        rel.addend = m.disp - 4;
                        relocs.push_back(rel);
                        emit_u32_le(buf, 0);
                    }
                    else if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b), de = reg_is_extended(d);
                        emit_rex(buf, w64, de, ie, be);
                        emit_u8(buf, 0x8B);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOVrm");
                    }
                    else
                    {
                        wrn.push_back("MOVrm: virt base [rbp+0]");
                        emit_rex_if_extended(buf, w64, d, PhysReg::RBP);
                        emit_u8(buf, 0x8B);
                        emit_modrm(buf, 1, reg_low3(d), reg_low3(PhysReg::RBP));
                        emit_u8(buf, 0);
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV64mr:
            case MOpc::MOV32mr: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVmr");
                    if (!m.symbol.empty())
                    {
                        bool se = reg_is_extended(s);
                        emit_rex(buf, w64, se, false, false);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 0, reg_low3(s), 5);
                        Reloc rel;
                        rel.offset = static_cast<std::uint32_t>(buf.size());
                        rel.symbol = m.symbol;
                        rel.kind = m.is_got_indirect ? Reloc::Kind::Rel32_Got : Reloc::Kind::Rel32;
                        rel.addend = m.disp - 4;
                        relocs.push_back(rel);
                        emit_u32_le(buf, 0);
                    }
                    else if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b), se = reg_is_extended(s);
                        emit_rex(buf, w64, se, ie, be);
                        emit_u8(buf, 0x89);
                        emit_mem(buf, m, reg_low3(s), wrn, "MOVmr");
                    }
                    else
                    {
                        wrn.push_back("MOVmr: virt base [rbp+0]");
                        emit_rex_if_extended(buf, w64, s, PhysReg::RBP);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 1, reg_low3(s), reg_low3(PhysReg::RBP));
                        emit_u8(buf, 0);
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV64mi32: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Imm64)
                {
                    auto const& m = ops[0].mem;
                    auto v = static_cast<std::int32_t>(ops[1].imm & 0xFFFFFFFF);
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex(buf, true, false, ie, be);
                        emit_u8(buf, 0xC7);
                        emit_mem(buf, m, 0, wrn, "MOV64mi32");
                        emit_u32_le(buf, static_cast<std::uint32_t>(v));
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MOV32rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV32rr");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOV32rr");
                    emit_rex_if_extended(buf, false, s, d);
                    emit_u8(buf, 0x89);
                    emit_modrm(buf, 3, reg_low3(s), reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV32ri: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV32ri");
                    auto v = static_cast<std::uint32_t>(ops[1].imm & 0xFFFFFFFF);
                    if (reg_is_extended(d))
                        emit_rex(buf, false, false, false, true);
                    emit_u8(buf, static_cast<std::uint8_t>(0xB8 + reg_low3(d)));
                    emit_u32_le(buf, v);
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV32mi: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Imm64)
                {
                    auto const& m = ops[0].mem;
                    auto v = static_cast<std::int32_t>(ops[1].imm & 0xFFFFFFFF);
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        if (ie || be)
                            emit_rex(buf, false, false, ie, be);
                        emit_u8(buf, 0xC7);
                        emit_mem(buf, m, 0, wrn, "MOV32mi");
                        emit_u32_le(buf, static_cast<std::uint32_t>(v));
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MOV8rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV8rr");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOV8rr");
                    emit_rex_8bit(buf, s, d);
                    emit_u8(buf, 0x88);
                    emit_modrm(buf, 3, reg_low3(s), reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV8ri: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV8ri");
                    auto v = static_cast<std::uint8_t>(ops[1].imm & 0xFF);
                    auto ext = reg_is_extended(d);
                    if (ext || reg_low3(d) >= 4)
                        emit_rex(buf, false, false, false, ext);
                    emit_u8(buf, static_cast<std::uint8_t>(0xB0 + reg_low3(d)));
                    emit_u8(buf, v);
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV8rm: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV8rm");
                    auto const& m = ops[1].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        if (de || ie || be || reg_low3(d) >= 4)
                            emit_rex(buf, false, de, ie, be);
                        emit_u8(buf, 0x8A);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOV8rm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV8mr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "MOV8mr");
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        if (se || ie || be || reg_low3(s) >= 4)
                            emit_rex(buf, false, se, ie, be);
                        emit_u8(buf, 0x88);
                        emit_mem(buf, m, reg_low3(s), wrn, "MOV8mr");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV8mi: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Imm64)
                {
                    auto const& m = ops[0].mem;
                    auto v = static_cast<std::uint8_t>(ops[1].imm & 0xFF);
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        if (ie || be)
                            emit_rex(buf, false, false, ie, be);
                        emit_u8(buf, 0xC6);
                        emit_mem(buf, m, 0, wrn, "MOV8mi");
                        emit_u8(buf, v);
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MOVZX64_32rr:
            case MOpc::MOVZX32rr8:
            case MOpc::MOVZX32_8rr:
            case MOpc::MOVZX64rr8: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVZX8");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVZX8");
                    emit_rex_if_extended(buf, w64, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0xB6);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVZX64_16rr:
            case MOpc::MOVZX32_16rr:
            case MOpc::MOVZX64rr16: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVZX16");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVZX16");
                    emit_rex_if_extended(buf, w64, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0xB7);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVZX64rm8:
            case MOpc::MOVZX32rm8: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVZXrm8");
                    auto const& m = ops[1].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d), ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg()),
                             be = reg_is_extended(b);
                        bool nr = de || ie || be || reg_low3(d) >= 4;
                        if (nr)
                            emit_rex(buf, w64, de, ie, be);
                        else if (w64)
                            emit_rex(buf, w64, false, false, false);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0xB6);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOVZXrm8");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVZX64rm16: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVZXrm16");
                    auto const& m = ops[1].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex_if_extended(buf, w64, d, b);
                        if (ie)
                            emit_rex(buf, w64, de, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0xB7);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOVZXrm16");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MOVSX64_32rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSXD");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVSXD");
                    emit_rex_if_extended(buf, true, d, s);
                    emit_u8(buf, 0x63);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVSX64_16rr:
            case MOpc::MOVSX32_16rr:
            case MOpc::MOVSX64rr16: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSX16");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVSX16");
                    emit_rex_if_extended(buf, w64, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0xBF);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVSX64_8rr:
            case MOpc::MOVSX32_8rr:
            case MOpc::MOVSX64rr8: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSX8");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVSX8");
                    emit_rex_if_extended(buf, w64, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0xBE);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVSX64rm8: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSXrm8");
                    auto const& m = ops[1].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d), ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg()),
                             be = reg_is_extended(b);
                        bool nr = de || ie || be || reg_low3(d) >= 4;
                        if (nr)
                            emit_rex(buf, w64, de, ie, be);
                        else if (w64)
                            emit_rex(buf, w64, false, false, false);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0xBE);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOVSXrm8");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVSX64rm16: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSXrm16");
                    auto const& m = ops[1].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex_if_extended(buf, w64, d, b);
                        if (ie)
                            emit_rex(buf, w64, de, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0xBF);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOVSXrm16");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::ADD64rr:
            case MOpc::ADD32rr:
            case MOpc::SUB64rr:
            case MOpc::SUB32rr:
            case MOpc::AND64rr:
            case MOpc::AND32rr:
            case MOpc::OR64rr:
            case MOpc::OR32rr:
            case MOpc::XOR64rr:
            case MOpc::XOR32rr: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "ALUrr");
                    auto l = resolve_phys_reg(ops[1], wrn, "ALUrr");
                    auto r = resolve_phys_reg(ops[2], wrn, "ALUrr");
                    if (d == l)
                    {
                        emit_rex_if_extended(buf, w64, r, d);
                        emit_u8(buf, alu_rm_r(instr.opc));
                        emit_modrm(buf, 3, reg_low3(r), reg_low3(d));
                    }
                    else if (d == r)
                    {
                        emit_rex_if_extended(buf, w64, l, d);
                        emit_u8(buf, alu_rm_r(instr.opc));
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                    }
                    else
                    {
                        emit_rex_if_extended(buf, true, l, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                        emit_rex_if_extended(buf, w64, r, d);
                        emit_u8(buf, alu_rm_r(instr.opc));
                        emit_modrm(buf, 3, reg_low3(r), reg_low3(d));
                    }
                }
                else if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "ALUri");
                    auto l = resolve_phys_reg(ops[1], wrn, "ALUri");
                    auto v = ops[2].imm;
                    if (d != l)
                    {
                        emit_rex_if_extended(buf, true, l, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                    }
                    if (v >= -128 && v <= 127)
                    {
                        emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                        emit_u8(buf, 0x83);
                        emit_modrm(buf, 3, alu_ext(instr.opc), reg_low3(d));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                        emit_u8(buf, 0x81);
                        emit_modrm(buf, 3, alu_ext(instr.opc), reg_low3(d));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "ALUri2");
                    auto v = ops[1].imm;
                    if (v >= -128 && v <= 127)
                    {
                        emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                        emit_u8(buf, 0x83);
                        emit_modrm(buf, 3, alu_ext(instr.opc), reg_low3(d));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                        emit_u8(buf, 0x81);
                        emit_modrm(buf, 3, alu_ext(instr.opc), reg_low3(d));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::ADD64rm:
            case MOpc::ADD32rm:
            case MOpc::SUB64rm:
            case MOpc::SUB32rm:
            case MOpc::AND64rm:
            case MOpc::OR64rm:
            case MOpc::XOR64rm:
            case MOpc::CMP64rm:
            case MOpc::CMP32rm: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "ALUr_m");
                    auto l = resolve_phys_reg(ops[1], wrn, "ALUr_m");
                    auto const& m = ops[2].mem;
                    if (d != l)
                    {
                        emit_rex_if_extended(buf, true, l, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                    }
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex(buf, w64, de, ie, be);
                        emit_u8(buf, alu_rm_r(instr.opc));
                        emit_mem(buf, m, reg_low3(d), wrn, "ALUrm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::AND64mr:
            case MOpc::OR64mr:
            case MOpc::XOR64mr: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "ALUmr");
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex(buf, w64, se, ie, be);
                        emit_u8(buf, alu_rm_r(instr.opc));
                        emit_mem(buf, m, reg_low3(s), wrn, "ALUmr");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
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
            case MOpc::CMP64ri32:
            case MOpc::CMP32ri: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "ALUri");
                    auto v = ops[1].imm;
                    if (v >= -128 && v <= 127)
                    {
                        emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                        emit_u8(buf, 0x83);
                        emit_modrm(buf, 3, alu_ext(instr.opc), reg_low3(d));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                        emit_u8(buf, 0x81);
                        emit_modrm(buf, 3, alu_ext(instr.opc), reg_low3(d));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "ALUri3");
                    auto l = resolve_phys_reg(ops[1], wrn, "ALUri3");
                    auto v = ops[2].imm;
                    if (d != l)
                    {
                        emit_rex_if_extended(buf, true, l, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                    }
                    if (v >= -128 && v <= 127)
                    {
                        emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                        emit_u8(buf, 0x83);
                        emit_modrm(buf, 3, alu_ext(instr.opc), reg_low3(d));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                        emit_u8(buf, 0x81);
                        emit_modrm(buf, 3, alu_ext(instr.opc), reg_low3(d));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::SUB64ri: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SUB64ri");
                    auto v = ops[1].imm;
                    if (v >= -128 && v <= 127)
                    {
                        emit_rex_if_extended(buf, true, PhysReg::RAX, d);
                        emit_u8(buf, 0x83);
                        emit_modrm(buf, 3, 5, reg_low3(d));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_rex_if_extended(buf, true, PhysReg::RAX, d);
                        emit_u8(buf, 0x81);
                        emit_modrm(buf, 3, 5, reg_low3(d));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SUB64ri3");
                    auto l = resolve_phys_reg(ops[1], wrn, "SUB64ri3");
                    auto v = ops[2].imm;
                    if (d != l)
                    {
                        emit_rex_if_extended(buf, true, l, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                    }
                    if (v >= -128 && v <= 127)
                    {
                        emit_rex_if_extended(buf, true, PhysReg::RAX, d);
                        emit_u8(buf, 0x83);
                        emit_modrm(buf, 3, 5, reg_low3(d));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_rex_if_extended(buf, true, PhysReg::RAX, d);
                        emit_u8(buf, 0x81);
                        emit_modrm(buf, 3, 5, reg_low3(d));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::CMP64rr:
            case MOpc::CMP32rr: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto l = resolve_phys_reg(ops[0], wrn, "CMPrr");
                    auto r = resolve_phys_reg(ops[1], wrn, "CMPrr");
                    emit_rex_if_extended(buf, w64, r, l);
                    emit_u8(buf, 0x39);
                    emit_modrm(buf, 3, reg_low3(r), reg_low3(l));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CMP8rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto l = resolve_phys_reg(ops[0], wrn, "CMP8rr");
                    auto r = resolve_phys_reg(ops[1], wrn, "CMP8rr");
                    emit_rex_8bit(buf, r, l);
                    emit_u8(buf, 0x38);
                    emit_modrm(buf, 3, reg_low3(r), reg_low3(l));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CMP64ri:
            case MOpc::CMP8ri: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CMPri");
                    auto v = ops[1].imm;
                    emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                    emit_u8(buf, 0x81);
                    emit_modrm(buf, 3, 7, reg_low3(d));
                    emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::TEST64rr:
            case MOpc::TEST32rr: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto l = resolve_phys_reg(ops[0], wrn, "TEST");
                    auto r = resolve_phys_reg(ops[1], wrn, "TEST");
                    emit_rex_if_extended(buf, w64, r, l);
                    emit_u8(buf, 0x85);
                    emit_modrm(buf, 3, reg_low3(r), reg_low3(l));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::TEST8rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto l = resolve_phys_reg(ops[0], wrn, "TEST8");
                    auto r = resolve_phys_reg(ops[1], wrn, "TEST8");
                    emit_rex_8bit(buf, r, l);
                    emit_u8(buf, 0x84);
                    emit_modrm(buf, 3, reg_low3(r), reg_low3(l));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::TEST64ri: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "TESTri");
                    auto v = ops[1].imm;
                    emit_rex_if_extended(buf, true, PhysReg::RAX, d);
                    emit_u8(buf, 0xF7);
                    emit_modrm(buf, 3, 0, reg_low3(d));
                    emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::IMUL64rr: {
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "IMULrr");
                    auto l = resolve_phys_reg(ops[1], wrn, "IMULrr");
                    auto r = resolve_phys_reg(ops[2], wrn, "IMULrr");
                    if (d == r)
                        std::swap(l, r);
                    if (d != l)
                    {
                        emit_rex_if_extended(buf, true, l, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                    }
                    emit_rex_if_extended(buf, true, d, r);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0xAF);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(r));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::IMUL64rri:
            case MOpc::IMUL64rri32: {
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "IMULrri");
                    auto s = resolve_phys_reg(ops[1], wrn, "IMULrri");
                    auto v = ops[2].imm;
                    emit_rex_if_extended(buf, true, d, s);
                    if (v >= -128 && v <= 127)
                    {
                        emit_u8(buf, 0x6B);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_u8(buf, 0x69);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::IMUL64ri: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "IMUL64ri");
                    auto v = ops[1].imm;
                    emit_rex_if_extended(buf, true, d, d);
                    if (v >= -128 && v <= 127)
                    {
                        emit_u8(buf, 0x6B);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(d));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_u8(buf, 0x69);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(d));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::IMUL64rm: {
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "IMULrm");
                    auto l = resolve_phys_reg(ops[1], wrn, "IMULrm");
                    auto const& m = ops[2].mem;
                    if (d != l)
                    {
                        emit_rex_if_extended(buf, true, l, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                    }
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex(buf, true, de, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0xAF);
                        emit_mem(buf, m, reg_low3(d), wrn, "IMULrm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MUL64r: {
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto s = resolve_phys_reg(ops[0], wrn, "MUL64r");
                    emit_rex_if_extended(buf, true, PhysReg::RAX, s);
                    emit_u8(buf, 0xF7);
                    emit_modrm(buf, 3, 4, reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::IDIV64r:
            case MOpc::DIV64r: {
                auto ext = static_cast<std::uint8_t>(instr.opc == MOpc::IDIV64r ? 7 : 6);
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto s = resolve_phys_reg(ops[0], wrn, "IDIV/DIV");
                    emit_rex_if_extended(buf, true, PhysReg::RAX, s);
                    emit_u8(buf, 0xF7);
                    emit_modrm(buf, 3, ext, reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::NEG64r:
            case MOpc::NEG32r: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "NEG");
                    emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                    emit_u8(buf, 0xF7);
                    emit_modrm(buf, 3, 3, reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::NOT64r:
            case MOpc::NOT32r: {
                bool w64 = is_64bit_opc(instr.opc);
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "NOT");
                    emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                    emit_u8(buf, 0xF7);
                    emit_modrm(buf, 3, 2, reg_low3(d));
                }
                else
                    goto ud2_lbl;
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
                bool w64 = is_64bit_opc(instr.opc);
                auto ext = static_cast<std::uint8_t>(4);
                if (instr.opc == MOpc::SHR64rCL || instr.opc == MOpc::SHR32rCL || instr.opc == MOpc::SHR64rcl)
                    ext = 5;
                else if (instr.opc == MOpc::SAR64rCL || instr.opc == MOpc::SAR32rCL || instr.opc == MOpc::SAR64rcl)
                    ext = 7;
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SHIFTcl");
                    emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                    emit_u8(buf, 0xD3);
                    emit_modrm(buf, 3, ext, reg_low3(d));
                }
                else
                    goto ud2_lbl;
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
                bool w64 = is_64bit_opc(instr.opc);
                auto ext = static_cast<std::uint8_t>(4);
                if (instr.opc == MOpc::SHR64ri8 || instr.opc == MOpc::SHR32ri8 || instr.opc == MOpc::SHR64ri)
                    ext = 5;
                else if (instr.opc == MOpc::SAR64ri8 || instr.opc == MOpc::SAR32ri8 || instr.opc == MOpc::SAR64ri)
                    ext = 7;
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SHIFTimm");
                    auto v = ops[2].imm & 0x3F;
                    emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                    emit_u8(buf, 0xC1);
                    emit_modrm(buf, 3, ext, reg_low3(d));
                    emit_u8(buf, static_cast<std::uint8_t>(v));
                }
                else if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SHIFTimm2");
                    auto v = ops[1].imm & 0x3F;
                    emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                    emit_u8(buf, 0xC1);
                    emit_modrm(buf, 3, ext, reg_low3(d));
                    emit_u8(buf, static_cast<std::uint8_t>(v));
                }
                else
                    goto ud2_lbl;
                break;
            }

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
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SETcc");
                    auto ext = reg_is_extended(d);
                    if (ext || reg_low3(d) >= 4)
                        emit_rex(buf, false, false, false, ext);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, setcc_byte(instr.opc));
                    emit_modrm(buf, 3, 0, reg_low3(d));
                }
                else
                    goto ud2_lbl;
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
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CMOV");
                    auto r = resolve_phys_reg(ops[2], wrn, "CMOV");
                    emit_rex_if_extended(buf, true, d, r);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, cmov_byte(instr.opc));
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(r));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::JMP:
            case MOpc::JMP_rel32:
            case MOpc::JE:
            case MOpc::JNE:
            case MOpc::JL:
            case MOpc::JGE:
            case MOpc::JLE:
            case MOpc::JG:
            case MOpc::JB:
            case MOpc::JAE:
            case MOpc::JBE:
            case MOpc::JA:
            case MOpc::JP:
            case MOpc::JNP:
            case MOpc::JS:
            case MOpc::JNS: {
                if (np >= 1 && ops[0].kind == MOpKind::Label)
                {
                    auto tgt = ops[0].label;
                    if (instr.opc == MOpc::JMP || instr.opc == MOpc::JMP_rel32)
                    {
                        emit_u8(buf, 0xE9);
                        branches.push_back({buf.size(), tgt});
                        emit_u32_le(buf, 0);
                    }
                    else
                    {
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, jcc_byte(instr.opc));
                        branches.push_back({buf.size(), tgt});
                        emit_u32_le(buf, 0);
                    }
                }
                else if (np >= 1 && ops[0].kind == MOpKind::Symbol)
                {
                    if (instr.opc == MOpc::JMP || instr.opc == MOpc::JMP_rel32)
                    {
                        emit_u8(buf, 0xE9);
                        Reloc r;
                        r.offset = static_cast<std::uint32_t>(buf.size());
                        r.symbol = ops[0].symbol;
                        r.kind = Reloc::Kind::Rel32;
                        r.addend = -4;
                        relocs.push_back(r);
                        emit_u32_le(buf, 0);
                    }
                    else
                    {
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, jcc_byte(instr.opc));
                        Reloc r;
                        r.offset = static_cast<std::uint32_t>(buf.size());
                        r.symbol = ops[0].symbol;
                        r.kind = Reloc::Kind::Rel32;
                        r.addend = -4;
                        relocs.push_back(r);
                        emit_u32_le(buf, 0);
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::JMP_r64: {
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "JMP_r64");
                    emit_rex_if_extended(buf, false, PhysReg::RAX, d);
                    emit_u8(buf, 0xFF);
                    emit_modrm(buf, 3, 4, reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::JUMP_TABLE: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Symbol)
                {
                    auto idx_reg = resolve_phys_reg(ops[0], wrn, "JUMP_TABLE_index");
                    std::string_view sym = ops[1].symbol;

                    bool r11_extended = reg_is_extended(PhysReg::R11);
                    bool r10_extended = reg_is_extended(PhysReg::R10);
                    bool idx_ext = reg_is_extended(idx_reg);
                    bool rax_ext = reg_is_extended(PhysReg::RAX);
                    std::uint8_t r11_low3 = reg_low3(PhysReg::R11);
                    std::uint8_t r10_low3 = reg_low3(PhysReg::R10);
                    std::uint8_t idx_low3 = reg_low3(idx_reg);
                    std::uint8_t rax_low3 = reg_low3(PhysReg::RAX);

                    {
                        bool de = r11_extended;
                        emit_rex(buf, true, de, false, false);
                        emit_u8(buf, 0x8D);
                        emit_modrm(buf, 0, reg_low3(PhysReg::R11), 5);
                        Reloc rel;
                        rel.offset = static_cast<std::uint32_t>(buf.size());
                        rel.symbol = sym;
                        rel.kind = Reloc::Kind::Rel32;
                        rel.addend = -4;
                        relocs.push_back(rel);
                        emit_u32_le(buf, 0);
                    }

                    {
                        emit_rex(buf, true, r10_extended, idx_ext, r11_extended);
                        emit_u8(buf, 0x8D);
                        emit_modrm(buf, 1, r10_low3, 4);
                        emit_sib(buf, 4, idx_low3, r11_low3);
                        emit_u8(buf, 4);
                    }

                    {
                        emit_rex(buf, true, rax_ext, idx_ext, r11_extended);
                        emit_u8(buf, 0x63);
                        emit_modrm(buf, 0, rax_low3, 4);
                        emit_sib(buf, 4, idx_low3, r11_low3);
                    }

                    {
                        bool need_rex = r10_extended || rax_ext;
                        if (need_rex)
                            emit_rex(buf, true, r10_extended, false, rax_ext);
                        else
                            emit_rex(buf, true, false, false, false);

                        emit_u8(buf, 0x01);
                        emit_modrm(buf, 3, r10_low3, rax_low3);
                    }

                    {
                        emit_rex_if_extended(buf, false, PhysReg::RAX, PhysReg::RAX);
                        emit_u8(buf, 0xFF);
                        emit_modrm(buf, 3, 4, reg_low3(PhysReg::RAX));
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::JMPm: {
                if (np >= 1 && ops[0].kind == MOpKind::Mem)
                {
                    auto const& m = ops[0].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex(buf, false, false, ie, be);
                        emit_u8(buf, 0xFF);
                        emit_mem(buf, m, 4, wrn, "JMPm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::CALL: {
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CALL");
                    emit_rex_if_extended(buf, false, PhysReg::RAX, d);
                    emit_u8(buf, 0xFF);
                    emit_modrm(buf, 3, 2, reg_low3(d));
                }
                else if (np >= 1 && ops[0].kind == MOpKind::Symbol)
                {
                    emit_u8(buf, 0xE8);
                    Reloc r;
                    r.offset = static_cast<std::uint32_t>(buf.size());
                    r.symbol = ops[0].symbol;
                    r.kind = Reloc::Kind::Rel32_Call;
                    r.addend = -4;
                    relocs.push_back(r);
                    emit_u32_le(buf, 0);
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CALL_rel32: {
                if (np >= 1 && ops[0].kind == MOpKind::Symbol)
                {
                    emit_u8(buf, 0xE8);
                    Reloc r;
                    r.offset = static_cast<std::uint32_t>(buf.size());
                    r.symbol = ops[0].symbol;
                    r.kind = Reloc::Kind::Rel32_Call;
                    r.addend = -4;
                    relocs.push_back(r);
                    emit_u32_le(buf, 0);
                }
                else if (np >= 1 && ops[0].kind == MOpKind::Label)
                {
                    emit_u8(buf, 0xE8);
                    branches.push_back({buf.size(), ops[0].label});
                    emit_u32_le(buf, 0);
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CALL_r64: {
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CALL_r64");
                    emit_rex_if_extended(buf, false, PhysReg::RAX, d);
                    emit_u8(buf, 0xFF);
                    emit_modrm(buf, 3, 2, reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CALLm: {
                if (np >= 1 && ops[0].kind == MOpKind::Mem)
                {
                    auto const& m = ops[0].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex(buf, false, false, ie, be);
                        emit_u8(buf, 0xFF);
                        emit_mem(buf, m, 2, wrn, "CALLm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::RET:
                emit_u8(buf, 0xC3);
                break;

            case MOpc::LEA64rm: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "LEA64rm");
                    auto const& m = ops[1].mem;
                    if (!m.symbol.empty())
                    {
                        bool de = reg_is_extended(d);
                        emit_rex(buf, true, de, false, false);
                        emit_u8(buf, 0x8D);
                        emit_modrm(buf, 0, reg_low3(d), 5);
                        Reloc rel;
                        rel.offset = static_cast<std::uint32_t>(buf.size());
                        rel.symbol = m.symbol;
                        rel.kind = m.is_got_indirect ? Reloc::Kind::Rel32_Got : Reloc::Kind::Rel32;
                        rel.addend = m.disp - 4;
                        relocs.push_back(rel);
                        emit_u32_le(buf, 0);
                    }
                    else if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex(buf, true, de, ie, be);
                        emit_u8(buf, 0x8D);
                        emit_mem(buf, m, reg_low3(d), wrn, "LEA64rm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::CQO:
                emit_rex(buf, true, false, false, false);
                emit_u8(buf, 0x99);
                break;
            case MOpc::CDQ:
                emit_u8(buf, 0x99);
                break;

            case MOpc::PUSH64r: {
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto s = resolve_phys_reg(ops[0], wrn, "PUSH64r");
                    if (reg_is_extended(s))
                        emit_rex(buf, false, false, false, true);
                    emit_u8(buf, static_cast<std::uint8_t>(0x50 + reg_low3(s)));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::POP64r: {
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "POP64r");
                    if (reg_is_extended(d))
                        emit_rex(buf, false, false, false, true);
                    emit_u8(buf, static_cast<std::uint8_t>(0x58 + reg_low3(d)));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::PUSHFQ:
                emit_u8(buf, 0x9C);
                break;
            case MOpc::POPFQ:
                emit_u8(buf, 0x9D);
                break;
            case MOpc::PUSH64i: {
                if (np >= 1 && ops[0].kind == MOpKind::Imm64)
                {
                    auto v = ops[0].imm;
                    if (v >= -128 && v <= 127)
                    {
                        emit_u8(buf, 0x6A);
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_u8(buf, 0x68);
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::PUSH64m: {
                if (np >= 1 && ops[0].kind == MOpKind::Mem)
                {
                    auto const& m = ops[0].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_rex(buf, false, false, ie, be);
                        emit_u8(buf, 0xFF);
                        emit_mem(buf, m, 6, wrn, "PUSH64m");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MOVSD_rr:
            case MOpc::MOVSDrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSDrr");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVSDrr");
                    emit_u8(buf, 0xF2);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x10);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVSD_rm:
            case MOpc::MOVSDrm: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSDrm");
                    auto const& m = ops[1].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF2);
                        emit_rex(buf, false, de, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x10);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOVSDrm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVQ64rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVQ64rr");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVQ64rr");
                    bool de = reg_is_extended(d);
                    bool se = reg_is_extended(s);
                    emit_u8(buf, 0x66);
                    emit_u8(buf, static_cast<std::uint8_t>(0x48 | (de ? 0x04 : 0) | (se ? 0x01 : 0)));
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x6E);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVQ64rr_rev: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVQ64rr_rev");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVQ64rr_rev");
                    bool de = reg_is_extended(d);
                    bool se = reg_is_extended(s);
                    emit_u8(buf, 0x66);

                    emit_u8(buf, static_cast<std::uint8_t>(0x48 | (se ? 0x04 : 0) | (de ? 0x01 : 0)));
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x7E);

                    emit_modrm(buf, 3, reg_low3(s), reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MOVSD_mr:
            case MOpc::MOVSDmr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVSDmr");
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF2);
                        emit_rex(buf, false, se, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x11);
                        emit_mem(buf, m, reg_low3(s), wrn, "MOVSDmr");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::ADDSD:
            case MOpc::ADDSDrr:
            case MOpc::SUBSD:
            case MOpc::SUBSDrr:
            case MOpc::MULSD:
            case MOpc::MULSDrr:
            case MOpc::DIVSD:
            case MOpc::DIVSDrr: {
                auto sse = static_cast<std::uint8_t>(0x58);
                if (instr.opc == MOpc::SUBSD || instr.opc == MOpc::SUBSDrr)
                    sse = 0x5C;
                else if (instr.opc == MOpc::MULSD || instr.opc == MOpc::MULSDrr)
                    sse = 0x59;
                else if (instr.opc == MOpc::DIVSD || instr.opc == MOpc::DIVSDrr)
                    sse = 0x5E;
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SSESD");
                    auto r = resolve_phys_reg(ops[2], wrn, "SSESD");
                    auto l = resolve_phys_reg(ops[1], wrn, "SSESD");
                    emit_sse_binop_rr(buf, 0xF2, sse, d, l, r);
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::ADDSDrm:
            case MOpc::SUBSDrm:
            case MOpc::MULSDrm:
            case MOpc::DIVSDrm: {
                auto sse = static_cast<std::uint8_t>(0x58);
                if (instr.opc == MOpc::SUBSDrm)
                    sse = 0x5C;
                else if (instr.opc == MOpc::MULSDrm)
                    sse = 0x59;
                else if (instr.opc == MOpc::DIVSDrm)
                    sse = 0x5E;
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SSESDrm");
                    auto l = resolve_phys_reg(ops[1], wrn, "SSESDrm");
                    auto const& m = ops[2].mem;
                    if (d != l)
                    {
                        emit_u8(buf, 0xF2);
                        emit_rex_if_extended(buf, false, d, l);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x10);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(l));
                    }
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d), ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg()),
                             be = reg_is_extended(b);
                        emit_u8(buf, 0xF2);
                        emit_rex(buf, false, de, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, sse);
                        emit_mem(buf, m, reg_low3(d), wrn, "SSESDrm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::UCOMISD:
            case MOpc::UCOMISDrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "UCOMISD");
                    auto s = resolve_phys_reg(ops[1], wrn, "UCOMISD");
                    emit_u8(buf, 0x66);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x2E);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::CVTSI2SD_r:
            case MOpc::CVTSI2SDrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CVTSI2SD");
                    auto s = resolve_phys_reg(ops[1], wrn, "CVTSI2SD");
                    emit_u8(buf, 0xF2);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x2A);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CVTSD2SIrr:
            case MOpc::CVTTSD2SI_r: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CVTSD2SI");
                    auto s = resolve_phys_reg(ops[1], wrn, "CVTSD2SI");
                    emit_u8(buf, 0xF2);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x2D);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CVTSD2SI64rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CVTSD2SI64");
                    auto s = resolve_phys_reg(ops[1], wrn, "CVTSD2SI64");
                    emit_u8(buf, 0xF2);
                    emit_rex_if_extended(buf, true, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x2C);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CVTSD2SS_r: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CVTSD2SS");
                    auto s = resolve_phys_reg(ops[1], wrn, "CVTSD2SS");
                    emit_u8(buf, 0xF2);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x5A);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CVTSS2SD_r: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CVTSS2SD");
                    auto s = resolve_phys_reg(ops[1], wrn, "CVTSS2SD");
                    emit_u8(buf, 0xF3);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x5A);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MOVSS_rr:
            case MOpc::MOVSSrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSSrr");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVSSrr");
                    emit_u8(buf, 0xF3);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x10);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVSS_rm:
            case MOpc::MOVSSrm: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVSSrm");
                    auto const& m = ops[1].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d), ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg()),
                             be = reg_is_extended(b);
                        emit_u8(buf, 0xF3);
                        emit_rex(buf, false, de, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x10);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOVSSrm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVSS_mr:
            case MOpc::MOVSSmr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVSSmr");
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF3);
                        emit_rex(buf, false, se, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x11);
                        emit_mem(buf, m, reg_low3(s), wrn, "MOVSSmr");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::ADDSS:
            case MOpc::ADDSSrr:
            case MOpc::SUBSS:
            case MOpc::SUBSSrr:
            case MOpc::MULSS:
            case MOpc::MULSSrr:
            case MOpc::DIVSS:
            case MOpc::DIVSSrr: {
                auto sse = static_cast<std::uint8_t>(0x58);
                if (instr.opc == MOpc::SUBSS || instr.opc == MOpc::SUBSSrr)
                    sse = 0x5C;
                else if (instr.opc == MOpc::MULSS || instr.opc == MOpc::MULSSrr)
                    sse = 0x59;
                else if (instr.opc == MOpc::DIVSS || instr.opc == MOpc::DIVSSrr)
                    sse = 0x5E;
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SSESS");
                    auto r = resolve_phys_reg(ops[2], wrn, "SSESS");
                    auto l = resolve_phys_reg(ops[1], wrn, "SSESS");
                    emit_sse_binop_rr(buf, 0xF3, sse, d, l, r);
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::ADDSSrm:
            case MOpc::SUBSSrm:
            case MOpc::MULSSrm:
            case MOpc::DIVSSrm: {
                auto sse = static_cast<std::uint8_t>(0x58);
                if (instr.opc == MOpc::SUBSSrm)
                    sse = 0x5C;
                else if (instr.opc == MOpc::MULSSrm)
                    sse = 0x59;
                else if (instr.opc == MOpc::DIVSSrm)
                    sse = 0x5E;
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "SSESSrm");
                    auto l = resolve_phys_reg(ops[1], wrn, "SSESSrm");
                    auto const& m = ops[2].mem;
                    if (d != l)
                    {
                        emit_u8(buf, 0xF3);
                        emit_rex_if_extended(buf, false, d, l);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x10);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(l));
                    }
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF3);
                        emit_rex(buf, false, de, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, sse);
                        emit_mem(buf, m, reg_low3(d), wrn, "SSESSrm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::UCOMISS:
            case MOpc::UCOMISSrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "UCOMISS");
                    auto s = resolve_phys_reg(ops[1], wrn, "UCOMISS");
                    emit_u8(buf, 0x66);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x2E);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::CVTSI2SS_r:
            case MOpc::CVTSI2SSrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CVTSI2SS");
                    auto s = resolve_phys_reg(ops[1], wrn, "CVTSI2SS");
                    emit_u8(buf, 0xF3);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x2A);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CVTTSS2SI_r:
            case MOpc::CVTSS2SIrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CVTTSS2SI");
                    auto s = resolve_phys_reg(ops[1], wrn, "CVTTSS2SI");
                    emit_u8(buf, 0xF3);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x2C);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::CVTSS2SI64rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "CVTSS2SI64");
                    auto s = resolve_phys_reg(ops[1], wrn, "CVTSS2SI64");
                    emit_u8(buf, 0xF3);
                    emit_rex_if_extended(buf, true, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x2C);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::XORPSrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "XORPS");
                    auto s = resolve_phys_reg(ops[1], wrn, "XORPS");
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x57);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::XORPDrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "XORPD");
                    auto s = resolve_phys_reg(ops[1], wrn, "XORPD");
                    emit_u8(buf, 0x66);
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x57);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVAPSrr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVAPS");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVAPS");
                    emit_rex_if_extended(buf, false, d, s);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0x28);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVAPSrm: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOVAPSrm");
                    auto const& m = ops[1].mem;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d), ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg()),
                             be = reg_is_extended(b);
                        emit_rex(buf, false, de, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x28);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOVAPSrm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOVAPSmr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "MOVAPSmr");
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s), ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg()),
                             be = reg_is_extended(b);
                        emit_rex(buf, false, se, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0x29);
                        emit_mem(buf, m, reg_low3(s), wrn, "MOVAPSmr");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::LOCK_XADD64mr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "LOCK_XADD64");
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF0);
                        emit_rex(buf, true, se, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0xC1);
                        emit_mem(buf, m, reg_low3(s), wrn, "LOCK_XADD64");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::LOCK_XADD32mr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "LOCK_XADD32");
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF0);
                        emit_rex(buf, false, se, ie, be);
                        emit_u8(buf, 0x0F);
                        emit_u8(buf, 0xC1);
                        emit_mem(buf, m, reg_low3(s), wrn, "LOCK_XADD32");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::LOCK_XCHG64mr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "LOCK_XCHG64");
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF0);
                        emit_rex(buf, true, se, ie, be);
                        emit_u8(buf, 0x87);
                        emit_mem(buf, m, reg_low3(s), wrn, "LOCK_XCHG64");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::LOCK_XADD:
            case MOpc::LOCK_XCHG:
            case MOpc::LOCK_AND:
            case MOpc::LOCK_OR:
            case MOpc::LOCK_XOR: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "LOCK");
                    std::uint8_t ob = 0, ex = 0;
                    bool is_xa = (instr.opc == MOpc::LOCK_XADD), is_xc = (instr.opc == MOpc::LOCK_XCHG);
                    if (is_xa)
                    {
                        ob = 0x0F;
                        ex = 0xC1;
                    }
                    else if (is_xc)
                    {
                        ob = 0x87;
                    }
                    else
                    {
                        switch (instr.opc)
                        {
                            case MOpc::LOCK_AND:
                                ob = 0x21;
                                break;
                            case MOpc::LOCK_OR:
                                ob = 0x09;
                                break;
                            case MOpc::LOCK_XOR:
                                ob = 0x31;
                                break;
                            default:
                                break;
                        }
                    }
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s);
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF0);
                        emit_rex(buf, false, se, ie, be);
                        if (is_xa)
                        {
                            emit_u8(buf, ob);
                            emit_u8(buf, ex);
                        }
                        else
                        {
                            emit_u8(buf, ob);
                        }
                        emit_mem(buf, m, reg_low3(s), wrn, "LOCK");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::LOCK_AND64mi32:
            case MOpc::LOCK_OR64mi32:
            case MOpc::LOCK_XOR64mi32: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Imm64)
                {
                    auto const& m = ops[0].mem;
                    auto v = static_cast<std::int32_t>(ops[1].imm & 0xFFFFFFFF);
                    auto ext = static_cast<std::uint8_t>(4);
                    if (instr.opc == MOpc::LOCK_OR64mi32)
                        ext = 1;
                    else if (instr.opc == MOpc::LOCK_XOR64mi32)
                        ext = 6;
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        emit_u8(buf, 0xF0);
                        emit_rex(buf, true, false, ie, be);
                        emit_u8(buf, 0x81);
                        emit_mem(buf, m, ext, wrn, "LOCKmi32");
                        emit_u32_le(buf, static_cast<std::uint32_t>(v));
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MFENCE:
                emit_u8(buf, 0x0F);
                emit_u8(buf, 0xAE);
                emit_u8(buf, 0xF0);
                break;
            case MOpc::LFENCE:
                emit_u8(buf, 0x0F);
                emit_u8(buf, 0xAE);
                emit_u8(buf, 0xE8);
                break;
            case MOpc::SFENCE:
                emit_u8(buf, 0x0F);
                emit_u8(buf, 0xAE);
                emit_u8(buf, 0xF8);
                break;

            case MOpc::NOP:
                emit_u8(buf, 0x90);
                break;
            case MOpc::UD2:
                emit_u8(buf, 0x0F);
                emit_u8(buf, 0x0B);
                break;
            case MOpc::XCHG64rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "XCHG");
                    auto s = resolve_phys_reg(ops[1], wrn, "XCHG");
                    emit_rex_if_extended(buf, true, s, d);
                    emit_u8(buf, 0x87);
                    emit_modrm(buf, 3, reg_low3(s), reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::MOV16rr: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV16rr");
                    auto s = resolve_phys_reg(ops[1], wrn, "MOV16rr");
                    emit_u8(buf, 0x66);
                    emit_rex_if_extended(buf, false, s, d);
                    emit_u8(buf, 0x89);
                    emit_modrm(buf, 3, reg_low3(s), reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV16ri: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV16ri");
                    auto v = static_cast<std::uint16_t>(ops[1].imm & 0xFFFF);
                    emit_u8(buf, 0x66);
                    if (reg_is_extended(d))
                        emit_rex(buf, false, false, false, true);
                    emit_u8(buf, static_cast<std::uint8_t>(0xB8 + reg_low3(d)));
                    emit_u16_le(buf, v);
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV16rm: {
                if (np >= 2 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Mem)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "MOV16rm");
                    auto const& m = ops[1].mem;
                    emit_u8(buf, 0x66);
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool de = reg_is_extended(d), ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg()),
                             be = reg_is_extended(b);
                        if (de || ie || be)
                            emit_rex(buf, false, de, ie, be);
                        emit_u8(buf, 0x8B);
                        emit_mem(buf, m, reg_low3(d), wrn, "MOV16rm");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV16mr: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Reg)
                {
                    auto const& m = ops[0].mem;
                    auto s = resolve_phys_reg(ops[1], wrn, "MOV16mr");
                    emit_u8(buf, 0x66);
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool se = reg_is_extended(s), ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg()),
                             be = reg_is_extended(b);
                        if (se || ie || be)
                            emit_rex(buf, false, se, ie, be);
                        emit_u8(buf, 0x89);
                        emit_mem(buf, m, reg_low3(s), wrn, "MOV16mr");
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::MOV16mi: {
                if (np >= 2 && ops[0].kind == MOpKind::Mem && ops[1].kind == MOpKind::Imm64)
                {
                    auto const& m = ops[0].mem;
                    auto v = static_cast<std::uint16_t>(ops[1].imm & 0xFFFF);
                    emit_u8(buf, 0x66);
                    if (m.base.is_physical())
                    {
                        auto b = m.base.phys_reg();
                        bool ie = m.index.is_valid() && m.index.is_physical() && reg_is_extended(m.index.phys_reg());
                        bool be = reg_is_extended(b);
                        if (ie || be)
                            emit_rex(buf, false, false, ie, be);
                        emit_u8(buf, 0xC7);
                        emit_mem(buf, m, 0, wrn, "MOV16mi");
                        emit_u16_le(buf, v);
                    }
                    else
                        goto ud2_lbl;
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::INC64r:
            case MOpc::DEC64r: {
                bool w64 = is_64bit_opc(instr.opc);
                auto ext = static_cast<std::uint8_t>(instr.opc == MOpc::INC64r ? 0 : 1);
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "INC/DEC");
                    emit_rex_if_extended(buf, w64, PhysReg::RAX, d);
                    emit_u8(buf, 0xFF);
                    emit_modrm(buf, 3, ext, reg_low3(d));
                }
                else
                    goto ud2_lbl;
                break;
            }

            case MOpc::IDIV32r:
            case MOpc::DIV32r: {
                auto ext = static_cast<std::uint8_t>(instr.opc == MOpc::IDIV32r ? 7 : 6);
                if (np >= 1 && ops[0].kind == MOpKind::Reg)
                {
                    auto s = resolve_phys_reg(ops[0], wrn, "IDIV32/DIV32");
                    emit_rex_if_extended(buf, false, PhysReg::RAX, s);
                    emit_u8(buf, 0xF7);
                    emit_modrm(buf, 3, ext, reg_low3(s));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::IMUL32rr: {
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Reg)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "IMUL32rr");
                    auto l = resolve_phys_reg(ops[1], wrn, "IMUL32rr");
                    auto r = resolve_phys_reg(ops[2], wrn, "IMUL32rr");
                    if (d == r)
                        std::swap(l, r);
                    if (d != l)
                    {
                        emit_rex_if_extended(buf, false, l, d);
                        emit_u8(buf, 0x89);
                        emit_modrm(buf, 3, reg_low3(l), reg_low3(d));
                    }
                    emit_rex_if_extended(buf, false, d, r);
                    emit_u8(buf, 0x0F);
                    emit_u8(buf, 0xAF);
                    emit_modrm(buf, 3, reg_low3(d), reg_low3(r));
                }
                else
                    goto ud2_lbl;
                break;
            }
            case MOpc::IMUL32rri: {
                if (np >= 3 && ops[0].kind == MOpKind::Reg && ops[1].kind == MOpKind::Reg && ops[2].kind == MOpKind::Imm64)
                {
                    auto d = resolve_phys_reg(ops[0], wrn, "IMUL32rri");
                    auto s = resolve_phys_reg(ops[1], wrn, "IMUL32rri");
                    auto v = ops[2].imm;
                    emit_rex_if_extended(buf, false, d, s);
                    if (v >= -128 && v <= 127)
                    {
                        emit_u8(buf, 0x6B);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                        emit_u8(buf, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
                    }
                    else
                    {
                        emit_u8(buf, 0x69);
                        emit_modrm(buf, 3, reg_low3(d), reg_low3(s));
                        emit_u32_le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                    }
                }
                else
                    goto ud2_lbl;
                break;
            }

            default:
                wrn.push_back(std::format("unrecognized opcode {} , emitting UD2", opc_name(instr.opc)));
                goto ud2_lbl;
        }
        return;

    ud2_lbl:
        emit_u8(buf, 0x0F);
        emit_u8(buf, 0x0B);
    }

} // anonymous namespace

export namespace dcc::backend::em64t
{
    [[nodiscard]] EncodeResult encode_function(MFunction const& func)
    {
        EncodeResult r;
        std::unordered_map<std::uint32_t, std::size_t> lbl_off;
        std::vector<BranchPatch> branches;

        for (auto const& blk : func.blocks)
        {
            r.block_offsets[blk.id] = static_cast<std::uint32_t>(r.bytes.size());
            lbl_off[blk.id] = r.bytes.size();

            for (auto const& instr : blk.instrs)
            {
                encode_instr(instr, r.bytes, branches, r.relocs, r.warnings);
            }
        }

        for (auto const& bp : branches)
        {
            auto it = lbl_off.find(bp.target_label);
            if (it == lbl_off.end())
            {
                r.warnings.push_back(std::format("branch target {} not found", bp.target_label));
                continue;
            }
            std::size_t to = it->second;
            std::size_t po = bp.patch_offset;
            std::int32_t disp = static_cast<std::int32_t>(to - (po + 4));
            if (po + 4 <= r.bytes.size())
            {
                r.bytes[po] = static_cast<std::uint8_t>(disp);
                r.bytes[po + 1] = static_cast<std::uint8_t>(disp >> 8);
                r.bytes[po + 2] = static_cast<std::uint8_t>(disp >> 16);
                r.bytes[po + 3] = static_cast<std::uint8_t>(disp >> 24);
            }
            else
                r.warnings.push_back("branch patch OOB");
        }

        return r;
    }

} // namespace dcc::backend::em64t
