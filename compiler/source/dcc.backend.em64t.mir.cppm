export module dcc.backend.em64t.mir;

import std;
import dcc.ir;

export namespace dcc::backend::em64t
{
    enum class PhysReg : std::uint8_t
    {
        RAX,
        RCX,
        RDX,
        RBX,
        RSP,
        RBP,
        RSI,
        RDI,
        R8,
        R9,
        R10,
        R11,
        R12,
        R13,
        R14,
        R15,
        XMM0,
        XMM1,
        XMM2,
        XMM3,
        XMM4,
        XMM5,
        XMM6,
        XMM7,
        XMM8,
        XMM9,
        XMM10,
        XMM11,
        XMM12,
        XMM13,
        XMM14,
        XMM15,
        None,
        Count,
    };

    enum class RegClass : std::uint8_t
    {
        GPR64,
        XMM,
    };

    [[nodiscard]] RegClass reg_class(PhysReg r) noexcept
    {
        auto v = static_cast<std::uint8_t>(r);
        if (v <= static_cast<std::uint8_t>(PhysReg::R15))
            return RegClass::GPR64;

        if (v >= static_cast<std::uint8_t>(PhysReg::XMM0) && v <= static_cast<std::uint8_t>(PhysReg::XMM15))
            return RegClass::XMM;

        return RegClass::GPR64;
    }

    [[nodiscard]] bool is_callee_saved_sysv(PhysReg r) noexcept
    {
        switch (r)
        {
            case PhysReg::RBX:
            case PhysReg::RBP:
            case PhysReg::R12:
            case PhysReg::R13:
            case PhysReg::R14:
            case PhysReg::R15:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] bool is_callee_saved_win64(PhysReg r) noexcept
    {
        switch (r)
        {
            case PhysReg::RBX:
            case PhysReg::RBP:
            case PhysReg::RSI:
            case PhysReg::RDI:
            case PhysReg::R12:
            case PhysReg::R13:
            case PhysReg::R14:
            case PhysReg::R15:
            case PhysReg::XMM6:
            case PhysReg::XMM7:
            case PhysReg::XMM8:
            case PhysReg::XMM9:
            case PhysReg::XMM10:
            case PhysReg::XMM11:
            case PhysReg::XMM12:
            case PhysReg::XMM13:
            case PhysReg::XMM14:
            case PhysReg::XMM15:
                return true;
            default:
                return false;
        }
    }

    struct VReg
    {
        static constexpr std::uint32_t kPhysBit = 0x80000000U;
        std::uint32_t id = 0;

        [[nodiscard]] bool is_physical() const noexcept { return (id & kPhysBit) != 0; }
        [[nodiscard]] bool is_virtual() const noexcept { return !is_physical() && id != 0; }
        [[nodiscard]] bool is_valid() const noexcept { return id != 0; }

        [[nodiscard]] PhysReg phys_reg() const noexcept { return static_cast<PhysReg>(static_cast<std::uint8_t>(id & ~kPhysBit)); }

        [[nodiscard]] bool operator==(VReg const&) const noexcept = default;
        [[nodiscard]] bool operator!=(VReg const&) const noexcept = default;

        [[nodiscard]] static VReg phys(PhysReg r) noexcept { return VReg{kPhysBit | static_cast<std::uint32_t>(r)}; }
        [[nodiscard]] static VReg virt(std::uint32_t id) noexcept { return VReg{id}; }
    };

    struct MMem
    {
        VReg base;
        VReg index;
        std::uint8_t scale = 1;
        std::int32_t disp = 0;
        std::string_view symbol{};
        bool is_got_indirect{false};

        [[nodiscard]] static MMem make_base_disp(VReg base_reg, std::int32_t d = 0) noexcept { return MMem{base_reg, VReg{}, 1, d}; }

        [[nodiscard]] static MMem make_indexed(VReg base_reg, VReg idx_reg, std::uint8_t s = 1, std::int32_t d = 0) noexcept
        {
            return MMem{.base = base_reg, .index = idx_reg, .scale = s, .disp = d};
        }

        [[nodiscard]] static MMem make_sym_reloc(std::string_view sym, std::int32_t d = 0) noexcept { return MMem{VReg{}, VReg{}, 1, d, sym}; }

        [[nodiscard]] static MMem make_got_reloc(std::string_view sym, std::int32_t d = 0) noexcept { return MMem{VReg{}, VReg{}, 1, d, sym, true}; }
    };

    enum class MOpKind : std::uint8_t
    {
        None,
        Reg,
        Imm64,
        Mem,
        FrameSlot,
        Label,
        Symbol,
    };

    struct MOp
    {
        MOpKind kind = MOpKind::None;
        union
        {
            VReg reg;
            std::int64_t imm;
            MMem mem;
            std::uint32_t frame_slot;
            std::uint32_t label;
            std::string_view symbol;
        };

        MOp() noexcept : kind(MOpKind::None), reg{} {}

        [[nodiscard]] static MOp from_reg(VReg r) noexcept
        {
            MOp op;
            op.kind = MOpKind::Reg;
            op.reg = r;
            return op;
        }

        [[nodiscard]] static MOp from_imm(std::int64_t v) noexcept
        {
            MOp op;
            op.kind = MOpKind::Imm64;
            op.imm = v;
            return op;
        }

        [[nodiscard]] static MOp from_mem(MMem m) noexcept
        {
            MOp op;
            op.kind = MOpKind::Mem;
            op.mem = m;
            return op;
        }

        [[nodiscard]] static MOp from_frame_slot(std::uint32_t slot) noexcept
        {
            MOp op;
            op.kind = MOpKind::FrameSlot;
            op.frame_slot = slot;
            return op;
        }

        [[nodiscard]] static MOp from_label(std::uint32_t id) noexcept
        {
            MOp op;
            op.kind = MOpKind::Label;
            op.label = id;
            return op;
        }

        [[nodiscard]] static MOp from_symbol(std::string_view name) noexcept
        {
            MOp op;
            op.kind = MOpKind::Symbol;
            op.symbol = name;
            return op;
        }

        [[nodiscard]] bool is_reg() const noexcept { return kind == MOpKind::Reg; }
        [[nodiscard]] bool is_imm() const noexcept { return kind == MOpKind::Imm64; }
        [[nodiscard]] bool is_mem() const noexcept { return kind == MOpKind::Mem; }
        [[nodiscard]] bool is_label() const noexcept { return kind == MOpKind::Label; }
    };

    enum class MOpc : std::uint16_t
    {
        COPY,
        PHI,
        IMPLICIT_DEF,

        MOV64rr,
        MOV64ri32,
        MOV64ri,
        MOV64mi32,
        MOV64rm,
        MOV64mr,

        MOV32rr,
        MOV32ri,
        MOV32mi,
        MOV32rm,
        MOV32mr,

        MOV16rr,
        MOV16ri,
        MOV16mi,
        MOV16rm,
        MOV16mr,

        MOV8rr,
        MOV8ri,
        MOV8mi,
        MOV8rm,
        MOV8mr,

        MOVZX64_32rr,
        MOVZX64_16rr,
        MOVZX64_8rr,
        MOVZX32_16rr,
        MOVZX32_8rr,
        MOVZX32rr8,
        MOVZX32rm8,
        MOVZX64rr8,
        MOVZX64rm8,
        MOVZX64rr16,
        MOVZX64rm16,
        MOVSX64_32rr,
        MOVSX64_16rr,
        MOVSX64_8rr,
        MOVSX32_16rr,
        MOVSX32_8rr,
        MOVSX64rr8,
        MOVSX64rm8,
        MOVSX64rr16,
        MOVSX64rm16,

        ADD64rr,
        ADD64ri32,
        ADD64rm,
        SUB64rr,
        SUB64ri32,
        SUB64ri,
        SUB64rm,
        IMUL64rr,
        IMUL64rri32,
        IMUL64rri,
        IMUL64rm,
        IMUL64ri,
        MUL64r,
        IDIV64r,
        DIV64r,
        NEG64r,
        NOT64r,
        INC64r,
        DEC64r,

        ADD32rr,
        ADD32ri,
        ADD32rm,
        SUB32rr,
        SUB32ri,
        SUB32rm,
        IMUL32rr,
        IMUL32rri,
        IDIV32r,
        DIV32r,
        NEG32r,
        NOT32r,

        AND64rr,
        AND64ri32,
        OR64rr,
        OR64ri32,
        XOR64rr,
        XOR64ri32,
        AND64rm,
        AND64mr,
        AND64ri,
        OR64rm,
        OR64mr,
        OR64ri,
        XOR64rm,
        XOR64mr,
        XOR64ri,

        AND32rr,
        AND32ri,
        OR32rr,
        OR32ri,
        XOR32rr,
        XOR32ri,

        SHL64rCL,
        SHL64ri8,
        SHR64rCL,
        SHR64ri8,
        SAR64rCL,
        SAR64ri8,
        SHL64rcl,
        SHL64ri,
        SHR64rcl,
        SHR64ri,
        SAR64rcl,
        SAR64ri,

        SHL32rCL,
        SHL32ri8,
        SHR32rCL,
        SHR32ri8,
        SAR32rCL,
        SAR32ri8,

        CMP64rr,
        CMP64ri32,
        CMP64rm,
        CMP32rr,
        CMP32ri,
        CMP32rm,
        CMP8rr,
        CMP8ri,
        CMP64ri,
        TEST64rr,
        TEST32rr,
        TEST8rr,
        TEST64ri,

        SETEr,
        SETNEr,
        SETLr,
        SETGEr,
        SETLEr,
        SETGr,
        SETBr,
        SETAEr,
        SETBEr,
        SETAr,

        CMOV64Err,
        CMOV64NErr,
        CMOV64Lrr,
        CMOV64GErr,
        CMOV64LErr,
        CMOV64Grr,
        CMOV64Brr,
        CMOV64AErr,
        CMOV64BErr,
        CMOV64Arr,

        JMP_rel32,
        JMP_r64,
        JUMP_TABLE,
        JE,
        JNE,
        JL,
        JLE,
        JG,
        JGE,
        JB,
        JBE,
        JA,
        JAE,
        JP,
        JMP,
        JS,
        JNS,
        JNP,
        JMPm,
        CALLm,

        CALL_rel32,
        CALL_r64,
        RET,
        CALL,

        LEA64rm,

        CQO,
        CDQ,

        PUSH64r,
        POP64r,
        PUSHFQ,
        POPFQ,
        PUSH64i,
        PUSH64m,

        MOVSD_rr,
        MOVSD_rm,
        MOVSD_mr,
        MOVSDrr,
        MOVSDrm,
        MOVSDmr,
        MOVQ64rr,
        ADDSD,
        SUBSD,
        MULSD,
        DIVSD,
        ADDSDrr,
        ADDSDrm,
        SUBSDrr,
        SUBSDrm,
        MULSDrr,
        MULSDrm,
        DIVSDrr,
        DIVSDrm,
        UCOMISD,
        UCOMISDrr,
        CVTSI2SD_r,
        CVTSI2SDrr,
        CVTTSD2SI_r,
        CVTSD2SIrr,
        CVTSD2SI64rr,
        CVTSD2SS_r,
        CVTSS2SD_r,

        MOVSS_rr,
        MOVSS_rm,
        MOVSS_mr,
        MOVSSrr,
        MOVSSrm,
        MOVSSmr,
        ADDSS,
        SUBSS,
        MULSS,
        DIVSS,
        ADDSSrr,
        ADDSSrm,
        SUBSSrr,
        SUBSSrm,
        MULSSrr,
        MULSSrm,
        DIVSSrr,
        DIVSSrm,
        UCOMISS,
        UCOMISSrr,
        CVTSI2SS_r,
        CVTSI2SSrr,
        CVTTSS2SI_r,
        CVTSS2SIrr,
        CVTSS2SI64rr,

        XORPSrr,
        XORPDrr,
        MOVAPSrr,
        MOVAPSrm,
        MOVAPSmr,

        LOCK_XADD64mr,
        LOCK_XADD32mr,
        LOCK_XCHG64mr,
        LOCK_AND64mi32,
        LOCK_OR64mi32,
        LOCK_XOR64mi32,
        LOCK_XADD,
        LOCK_XCHG,
        LOCK_AND,
        LOCK_OR,
        LOCK_XOR,
        MFENCE,
        LFENCE,
        SFENCE,

        UD2,
        NOP,
        XCHG64rr,
    };

    struct MInstr
    {
        MOpc opc = MOpc::NOP;
        std::array<MOp, 32> ops{};
        std::uint8_t num_ops = 0;
        std::uint8_t num_defs = 0;
        std::uint64_t implicit_defs = 0;
        std::uint64_t implicit_uses = 0;
        std::uint32_t src_line = 0;

        [[nodiscard]] bool has_side_effects() const noexcept
        {
            switch (opc)
            {
                case MOpc::MOV64mr:
                case MOpc::MOV32mr:
                case MOpc::MOV16mi:
                case MOpc::MOV16mr:
                case MOpc::MOV8mi:
                case MOpc::MOV8mr:
                case MOpc::MOV64mi32:
                case MOpc::MOV32mi:
                case MOpc::MOVSD_mr:
                case MOpc::MOVSDmr:
                case MOpc::MOVSS_mr:
                case MOpc::MOVSSmr:
                case MOpc::MOVAPSmr:
                case MOpc::AND64mr:
                case MOpc::OR64mr:
                case MOpc::XOR64mr:
                case MOpc::PUSH64r:
                case MOpc::PUSH64i:
                case MOpc::PUSH64m:
                case MOpc::POP64r:
                case MOpc::PUSHFQ:
                case MOpc::POPFQ:
                case MOpc::CALL:
                case MOpc::CALL_rel32:
                case MOpc::CALL_r64:
                case MOpc::CALLm:
                case MOpc::RET:
                case MOpc::JMP:
                case MOpc::JMP_rel32:
                case MOpc::JMP_r64:
                case MOpc::JUMP_TABLE:
                case MOpc::JMPm:
                case MOpc::JE:
                case MOpc::JNE:
                case MOpc::JB:
                case MOpc::JAE:
                case MOpc::JBE:
                case MOpc::JA:
                case MOpc::JL:
                case MOpc::JGE:
                case MOpc::JLE:
                case MOpc::JG:
                case MOpc::JS:
                case MOpc::JNS:
                case MOpc::JP:
                case MOpc::JNP:
                case MOpc::LOCK_XADD:
                case MOpc::LOCK_XADD64mr:
                case MOpc::LOCK_XADD32mr:
                case MOpc::LOCK_XCHG:
                case MOpc::LOCK_XCHG64mr:
                case MOpc::LOCK_AND:
                case MOpc::LOCK_AND64mi32:
                case MOpc::LOCK_OR:
                case MOpc::LOCK_OR64mi32:
                case MOpc::LOCK_XOR:
                case MOpc::LOCK_XOR64mi32:
                case MOpc::MFENCE:
                case MOpc::LFENCE:
                case MOpc::SFENCE:
                case MOpc::UD2:
                case MOpc::XCHG64rr:
                    return true;
                default:
                    return false;
            }
        }
    };

    [[nodiscard]] MInstr make_copy(VReg dst, VReg src)
    {
        MInstr mi;
        mi.opc = MOpc::COPY;
        mi.num_ops = 2;
        mi.num_defs = 1;
        mi.ops[0] = MOp::from_reg(dst);
        mi.ops[1] = MOp::from_reg(src);
        return mi;
    }

    [[nodiscard]] MInstr make_mov_ri(VReg dst, std::int64_t value, unsigned bits)
    {
        MInstr mi;
        if (bits <= 32)
            mi.opc = MOpc::MOV32ri;
        else
            mi.opc = MOpc::MOV64ri;

        mi.num_ops = 2;
        mi.num_defs = 1;
        mi.ops[0] = MOp::from_reg(dst);
        mi.ops[1] = MOp::from_imm(value);
        return mi;
    }

    [[nodiscard]] MInstr make_phi()
    {
        MInstr mi;
        mi.opc = MOpc::PHI;
        mi.num_ops = 0;
        mi.num_defs = 0;
        return mi;
    }

    [[nodiscard]] MInstr make_implicit_def()
    {
        MInstr mi;
        mi.opc = MOpc::IMPLICIT_DEF;
        mi.num_ops = 1;
        mi.num_defs = 1;
        return mi;
    }

    struct MBlock
    {
        std::uint32_t id{};
        std::vector<MInstr> instrs;
        std::vector<std::uint32_t> succs;
        std::vector<std::uint32_t> preds;
        std::vector<VReg> live_in;
        std::vector<VReg> live_out;

        std::string owned_name;

        [[nodiscard]] std::string_view name() const noexcept { return owned_name; }

        [[nodiscard]] std::string display_name() const
        {
            if (!owned_name.empty())
                return owned_name;

            return std::format("bb{}", id);
        }
    };

    struct MFrameSlot
    {
        std::uint32_t size{};
        std::uint32_t align{};
        std::int32_t offset{};
        bool is_spill{false};
    };

    struct MJumpTable
    {
        std::uint32_t id{};
        std::int64_t min_value{};
        std::int64_t max_value{};
        std::vector<std::uint32_t> targets;
        std::uint32_t default_target{};
        std::string symbol;
    };

    struct MFunction
    {
        ir::CallingConv conv{ir::CallingConv::Cdecl};
        bool is_variadic{false};
        std::vector<MBlock> blocks;
        std::vector<MFrameSlot> frame_slots;
        std::uint32_t next_vreg_id{1};
        std::uint32_t next_block_id{0};
        std::vector<MJumpTable> jump_tables;
        std::uint32_t next_jump_table_id{0};

        std::string owned_name;

        [[nodiscard]] std::string_view name() const noexcept { return owned_name; }
        std::uint32_t entry_block_id{0};
        std::int32_t frame_size{-1};
        std::int32_t src_line{0};

        [[nodiscard]] VReg new_vreg()
        {
            VReg v = VReg::virt(next_vreg_id);
            ++next_vreg_id;
            return v;
        }

        [[nodiscard]] std::uint32_t new_frame_slot(std::uint32_t size, std::uint32_t align, bool is_spill = false)
        {
            frame_slots.push_back(MFrameSlot{.size = size, .align = align, .offset = 0, .is_spill = is_spill});
            return static_cast<std::uint32_t>(frame_slots.size() - 1);
        }

        [[nodiscard]] MBlock& entry_block()
        {
            auto it = std::ranges::find_if(blocks, [this](MBlock const& b) { return b.id == entry_block_id; });
            if (it != blocks.end())
                return *it;

            if (!blocks.empty())
                return blocks.front();

            throw std::runtime_error("MFunction has no blocks");
        }

        [[nodiscard]] MBlock* block_by_id(std::uint32_t id)
        {
            auto it = std::ranges::find_if(blocks, [id](MBlock const& b) { return b.id == id; });
            return it != blocks.end() ? &*it : nullptr;
        }

        [[nodiscard]] MBlock const* block_by_id(std::uint32_t id) const
        {
            auto it = std::ranges::find_if(blocks, [id](MBlock const& b) { return b.id == id; });
            return it != blocks.end() ? &*it : nullptr;
        }

        [[nodiscard]] MBlock& create_block()
        {
            auto id = next_block_id++;
            auto& blk = blocks.emplace_back();
            blk.id = id;
            return blk;
        }

        [[nodiscard]] MBlock& create_block(std::string_view blk_name)
        {
            auto id = next_block_id++;
            auto& blk = blocks.emplace_back();
            blk.id = id;
            blk.owned_name = blk_name;
            return blk;
        }
    };

    struct MModule
    {
        std::vector<MFunction> functions;
        std::vector<std::string> global_names;
        std::string name;
        std::int32_t src_line = 0;
    };

    [[nodiscard]] std::string_view opc_name(MOpc opc)
    {
        using namespace std::literals;
        switch (opc)
        {
            case MOpc::COPY:
                return "COPY"sv;
            case MOpc::PHI:
                return "PHI"sv;
            case MOpc::IMPLICIT_DEF:
                return "IMPLICIT_DEF"sv;

            case MOpc::MOV64rr:
                return "MOV64rr"sv;
            case MOpc::MOV64ri32:
                return "MOV64ri32"sv;
            case MOpc::MOV64ri:
                return "MOV64ri"sv;
            case MOpc::MOV64mi32:
                return "MOV64mi32"sv;
            case MOpc::MOV64rm:
                return "MOV64rm"sv;
            case MOpc::MOV64mr:
                return "MOV64mr"sv;

            case MOpc::MOV32rr:
                return "MOV32rr"sv;
            case MOpc::MOV32ri:
                return "MOV32ri"sv;
            case MOpc::MOV32mi:
                return "MOV32mi"sv;
            case MOpc::MOV32rm:
                return "MOV32rm"sv;
            case MOpc::MOV32mr:
                return "MOV32mr"sv;

            case MOpc::MOV16rr:
                return "MOV16rr"sv;
            case MOpc::MOV16ri:
                return "MOV16ri"sv;
            case MOpc::MOV16mi:
                return "MOV16mi"sv;
            case MOpc::MOV16rm:
                return "MOV16rm"sv;
            case MOpc::MOV16mr:
                return "MOV16mr"sv;

            case MOpc::MOV8rr:
                return "MOV8rr"sv;
            case MOpc::MOV8ri:
                return "MOV8ri"sv;
            case MOpc::MOV8mi:
                return "MOV8mi"sv;
            case MOpc::MOV8rm:
                return "MOV8rm"sv;
            case MOpc::MOV8mr:
                return "MOV8mr"sv;

            case MOpc::MOVZX64_32rr:
                return "MOVZX64_32rr"sv;
            case MOpc::MOVZX64_16rr:
                return "MOVZX64_16rr"sv;
            case MOpc::MOVZX64_8rr:
                return "MOVZX64_8rr"sv;
            case MOpc::MOVZX32_16rr:
                return "MOVZX32_16rr"sv;
            case MOpc::MOVZX32_8rr:
                return "MOVZX32_8rr"sv;
            case MOpc::MOVZX32rr8:
                return "MOVZX32rr8"sv;
            case MOpc::MOVZX32rm8:
                return "MOVZX32rm8"sv;
            case MOpc::MOVZX64rr8:
                return "MOVZX64rr8"sv;
            case MOpc::MOVZX64rm8:
                return "MOVZX64rm8"sv;
            case MOpc::MOVZX64rr16:
                return "MOVZX64rr16"sv;
            case MOpc::MOVZX64rm16:
                return "MOVZX64rm16"sv;
            case MOpc::MOVSX64_32rr:
                return "MOVSX64_32rr"sv;
            case MOpc::MOVSX64_16rr:
                return "MOVSX64_16rr"sv;
            case MOpc::MOVSX64_8rr:
                return "MOVSX64_8rr"sv;
            case MOpc::MOVSX32_16rr:
                return "MOVSX32_16rr"sv;
            case MOpc::MOVSX32_8rr:
                return "MOVSX32_8rr"sv;
            case MOpc::MOVSX64rr8:
                return "MOVSX64rr8"sv;
            case MOpc::MOVSX64rm8:
                return "MOVSX64rm8"sv;
            case MOpc::MOVSX64rr16:
                return "MOVSX64rr16"sv;
            case MOpc::MOVSX64rm16:
                return "MOVSX64rm16"sv;

            case MOpc::ADD64rr:
                return "ADD64rr"sv;
            case MOpc::ADD64ri32:
                return "ADD64ri32"sv;
            case MOpc::ADD64rm:
                return "ADD64rm"sv;
            case MOpc::SUB64rr:
                return "SUB64rr"sv;
            case MOpc::SUB64ri32:
                return "SUB64ri32"sv;
            case MOpc::SUB64ri:
                return "SUB64ri"sv;
            case MOpc::SUB64rm:
                return "SUB64rm"sv;
            case MOpc::IMUL64rr:
                return "IMUL64rr"sv;
            case MOpc::IMUL64rri32:
                return "IMUL64rri32"sv;
            case MOpc::IMUL64rri:
                return "IMUL64rri"sv;
            case MOpc::IMUL64rm:
                return "IMUL64rm"sv;
            case MOpc::IMUL64ri:
                return "IMUL64ri"sv;
            case MOpc::MUL64r:
                return "MUL64r"sv;
            case MOpc::IDIV64r:
                return "IDIV64r"sv;
            case MOpc::DIV64r:
                return "DIV64r"sv;
            case MOpc::NEG64r:
                return "NEG64r"sv;
            case MOpc::NOT64r:
                return "NOT64r"sv;
            case MOpc::INC64r:
                return "INC64r"sv;
            case MOpc::DEC64r:
                return "DEC64r"sv;

            case MOpc::ADD32rr:
                return "ADD32rr"sv;
            case MOpc::ADD32ri:
                return "ADD32ri"sv;
            case MOpc::ADD32rm:
                return "ADD32rm"sv;
            case MOpc::SUB32rr:
                return "SUB32rr"sv;
            case MOpc::SUB32ri:
                return "SUB32ri"sv;
            case MOpc::SUB32rm:
                return "SUB32rm"sv;
            case MOpc::IMUL32rr:
                return "IMUL32rr"sv;
            case MOpc::IMUL32rri:
                return "IMUL32rri"sv;
            case MOpc::IDIV32r:
                return "IDIV32r"sv;
            case MOpc::DIV32r:
                return "DIV32r"sv;
            case MOpc::NEG32r:
                return "NEG32r"sv;
            case MOpc::NOT32r:
                return "NOT32r"sv;

            case MOpc::AND64rr:
                return "AND64rr"sv;
            case MOpc::AND64ri32:
                return "AND64ri32"sv;
            case MOpc::OR64rr:
                return "OR64rr"sv;
            case MOpc::OR64ri32:
                return "OR64ri32"sv;
            case MOpc::XOR64rr:
                return "XOR64rr"sv;
            case MOpc::XOR64ri32:
                return "XOR64ri32"sv;
            case MOpc::AND64rm:
                return "AND64rm"sv;
            case MOpc::AND64mr:
                return "AND64mr"sv;
            case MOpc::AND64ri:
                return "AND64ri"sv;
            case MOpc::OR64rm:
                return "OR64rm"sv;
            case MOpc::OR64mr:
                return "OR64mr"sv;
            case MOpc::OR64ri:
                return "OR64ri"sv;
            case MOpc::XOR64rm:
                return "XOR64rm"sv;
            case MOpc::XOR64mr:
                return "XOR64mr"sv;
            case MOpc::XOR64ri:
                return "XOR64ri"sv;

            case MOpc::AND32rr:
                return "AND32rr"sv;
            case MOpc::AND32ri:
                return "AND32ri"sv;
            case MOpc::OR32rr:
                return "OR32rr"sv;
            case MOpc::OR32ri:
                return "OR32ri"sv;
            case MOpc::XOR32rr:
                return "XOR32rr"sv;
            case MOpc::XOR32ri:
                return "XOR32ri"sv;

            case MOpc::SHL64rCL:
                return "SHL64rCL"sv;
            case MOpc::SHL64ri8:
                return "SHL64ri8"sv;
            case MOpc::SHR64rCL:
                return "SHR64rCL"sv;
            case MOpc::SHR64ri8:
                return "SHR64ri8"sv;
            case MOpc::SAR64rCL:
                return "SAR64rCL"sv;
            case MOpc::SAR64ri8:
                return "SAR64ri8"sv;
            case MOpc::SHL64rcl:
                return "SHL64rcl"sv;
            case MOpc::SHL64ri:
                return "SHL64ri"sv;
            case MOpc::SHR64rcl:
                return "SHR64rcl"sv;
            case MOpc::SHR64ri:
                return "SHR64ri"sv;
            case MOpc::SAR64rcl:
                return "SAR64rcl"sv;
            case MOpc::SAR64ri:
                return "SAR64ri"sv;

            case MOpc::SHL32rCL:
                return "SHL32rCL"sv;
            case MOpc::SHL32ri8:
                return "SHL32ri8"sv;
            case MOpc::SHR32rCL:
                return "SHR32rCL"sv;
            case MOpc::SHR32ri8:
                return "SHR32ri8"sv;
            case MOpc::SAR32rCL:
                return "SAR32rCL"sv;
            case MOpc::SAR32ri8:
                return "SAR32ri8"sv;

            case MOpc::CMP64rr:
                return "CMP64rr"sv;
            case MOpc::CMP64ri32:
                return "CMP64ri32"sv;
            case MOpc::CMP64rm:
                return "CMP64rm"sv;
            case MOpc::CMP32rr:
                return "CMP32rr"sv;
            case MOpc::CMP32ri:
                return "CMP32ri"sv;
            case MOpc::CMP32rm:
                return "CMP32rm"sv;
            case MOpc::CMP8rr:
                return "CMP8rr"sv;
            case MOpc::CMP8ri:
                return "CMP8ri"sv;
            case MOpc::CMP64ri:
                return "CMP64ri"sv;
            case MOpc::TEST64rr:
                return "TEST64rr"sv;
            case MOpc::TEST32rr:
                return "TEST32rr"sv;
            case MOpc::TEST8rr:
                return "TEST8rr"sv;
            case MOpc::TEST64ri:
                return "TEST64ri"sv;

            case MOpc::SETEr:
                return "SETEr"sv;
            case MOpc::SETNEr:
                return "SETNEr"sv;
            case MOpc::SETLr:
                return "SETLr"sv;
            case MOpc::SETGEr:
                return "SETGEr"sv;
            case MOpc::SETLEr:
                return "SETLEr"sv;
            case MOpc::SETGr:
                return "SETGr"sv;
            case MOpc::SETBr:
                return "SETBr"sv;
            case MOpc::SETAEr:
                return "SETAEr"sv;
            case MOpc::SETBEr:
                return "SETBEr"sv;
            case MOpc::SETAr:
                return "SETAr"sv;

            case MOpc::CMOV64Err:
                return "CMOV64Err"sv;
            case MOpc::CMOV64NErr:
                return "CMOV64NErr"sv;
            case MOpc::CMOV64Lrr:
                return "CMOV64Lrr"sv;
            case MOpc::CMOV64GErr:
                return "CMOV64GErr"sv;
            case MOpc::CMOV64LErr:
                return "CMOV64LErr"sv;
            case MOpc::CMOV64Grr:
                return "CMOV64Grr"sv;
            case MOpc::CMOV64Brr:
                return "CMOV64Brr"sv;
            case MOpc::CMOV64AErr:
                return "CMOV64AErr"sv;
            case MOpc::CMOV64BErr:
                return "CMOV64BErr"sv;
            case MOpc::CMOV64Arr:
                return "CMOV64Arr"sv;

            case MOpc::JMP_rel32:
                return "JMP_rel32"sv;
            case MOpc::JMP_r64:
                return "JMP_r64"sv;
            case MOpc::JUMP_TABLE:
                return "JUMP_TABLE"sv;
            case MOpc::JE:
                return "JE"sv;
            case MOpc::JNE:
                return "JNE"sv;
            case MOpc::JL:
                return "JL"sv;
            case MOpc::JLE:
                return "JLE"sv;
            case MOpc::JG:
                return "JG"sv;
            case MOpc::JGE:
                return "JGE"sv;
            case MOpc::JB:
                return "JB"sv;
            case MOpc::JBE:
                return "JBE"sv;
            case MOpc::JA:
                return "JA"sv;
            case MOpc::JAE:
                return "JAE"sv;
            case MOpc::JP:
                return "JP"sv;
            case MOpc::JMP:
                return "JMP"sv;
            case MOpc::JS:
                return "JS"sv;
            case MOpc::JNS:
                return "JNS"sv;
            case MOpc::JNP:
                return "JNP"sv;
            case MOpc::JMPm:
                return "JMPm"sv;
            case MOpc::CALLm:
                return "CALLm"sv;

            case MOpc::CALL_rel32:
                return "CALL_rel32"sv;
            case MOpc::CALL_r64:
                return "CALL_r64"sv;
            case MOpc::RET:
                return "RET"sv;
            case MOpc::CALL:
                return "CALL"sv;

            case MOpc::LEA64rm:
                return "LEA64rm"sv;

            case MOpc::CQO:
                return "CQO"sv;
            case MOpc::CDQ:
                return "CDQ"sv;

            case MOpc::PUSH64r:
                return "PUSH64r"sv;
            case MOpc::POP64r:
                return "POP64r"sv;
            case MOpc::PUSHFQ:
                return "PUSHFQ"sv;
            case MOpc::POPFQ:
                return "POPFQ"sv;
            case MOpc::PUSH64i:
                return "PUSH64i"sv;
            case MOpc::PUSH64m:
                return "PUSH64m"sv;

            case MOpc::MOVSD_rr:
                return "MOVSD_rr"sv;
            case MOpc::MOVSD_rm:
                return "MOVSD_rm"sv;
            case MOpc::MOVSD_mr:
                return "MOVSD_mr"sv;
            case MOpc::MOVSDrr:
                return "MOVSDrr"sv;
            case MOpc::MOVSDrm:
                return "MOVSDrm"sv;
            case MOpc::MOVSDmr:
                return "MOVSDmr"sv;
            case MOpc::MOVQ64rr:
                return "MOVQ64rr"sv;
            case MOpc::ADDSD:
                return "ADDSD"sv;
            case MOpc::SUBSD:
                return "SUBSD"sv;
            case MOpc::MULSD:
                return "MULSD"sv;
            case MOpc::DIVSD:
                return "DIVSD"sv;
            case MOpc::ADDSDrr:
                return "ADDSDrr"sv;
            case MOpc::ADDSDrm:
                return "ADDSDrm"sv;
            case MOpc::SUBSDrr:
                return "SUBSDrr"sv;
            case MOpc::SUBSDrm:
                return "SUBSDrm"sv;
            case MOpc::MULSDrr:
                return "MULSDrr"sv;
            case MOpc::MULSDrm:
                return "MULSDrm"sv;
            case MOpc::DIVSDrr:
                return "DIVSDrr"sv;
            case MOpc::DIVSDrm:
                return "DIVSDrm"sv;
            case MOpc::UCOMISD:
                return "UCOMISD"sv;
            case MOpc::UCOMISDrr:
                return "UCOMISDrr"sv;
            case MOpc::CVTSI2SD_r:
                return "CVTSI2SD_r"sv;
            case MOpc::CVTSI2SDrr:
                return "CVTSI2SDrr"sv;
            case MOpc::CVTTSD2SI_r:
                return "CVTTSD2SI_r"sv;
            case MOpc::CVTSD2SIrr:
                return "CVTSD2SIrr"sv;
            case MOpc::CVTSD2SI64rr:
                return "CVTSD2SI64rr"sv;
            case MOpc::CVTSD2SS_r:
                return "CVTSD2SS_r"sv;
            case MOpc::CVTSS2SD_r:
                return "CVTSS2SD_r"sv;

            case MOpc::MOVSS_rr:
                return "MOVSS_rr"sv;
            case MOpc::MOVSS_rm:
                return "MOVSS_rm"sv;
            case MOpc::MOVSS_mr:
                return "MOVSS_mr"sv;
            case MOpc::MOVSSrr:
                return "MOVSSrr"sv;
            case MOpc::MOVSSrm:
                return "MOVSSrm"sv;
            case MOpc::MOVSSmr:
                return "MOVSSmr"sv;
            case MOpc::ADDSS:
                return "ADDSS"sv;
            case MOpc::SUBSS:
                return "SUBSS"sv;
            case MOpc::MULSS:
                return "MULSS"sv;
            case MOpc::DIVSS:
                return "DIVSS"sv;
            case MOpc::ADDSSrr:
                return "ADDSSrr"sv;
            case MOpc::ADDSSrm:
                return "ADDSSrm"sv;
            case MOpc::SUBSSrr:
                return "SUBSSrr"sv;
            case MOpc::SUBSSrm:
                return "SUBSSrm"sv;
            case MOpc::MULSSrr:
                return "MULSSrr"sv;
            case MOpc::MULSSrm:
                return "MULSSrm"sv;
            case MOpc::DIVSSrr:
                return "DIVSSrr"sv;
            case MOpc::DIVSSrm:
                return "DIVSSrm"sv;
            case MOpc::UCOMISS:
                return "UCOMISS"sv;
            case MOpc::UCOMISSrr:
                return "UCOMISSrr"sv;
            case MOpc::CVTSI2SS_r:
                return "CVTSI2SS_r"sv;
            case MOpc::CVTSI2SSrr:
                return "CVTSI2SSrr"sv;
            case MOpc::CVTTSS2SI_r:
                return "CVTTSS2SI_r"sv;
            case MOpc::CVTSS2SIrr:
                return "CVTSS2SIrr"sv;
            case MOpc::CVTSS2SI64rr:
                return "CVTSS2SI64rr"sv;

            case MOpc::XORPSrr:
                return "XORPSrr"sv;
            case MOpc::XORPDrr:
                return "XORPDrr"sv;
            case MOpc::MOVAPSrr:
                return "MOVAPSrr"sv;
            case MOpc::MOVAPSrm:
                return "MOVAPSrm"sv;
            case MOpc::MOVAPSmr:
                return "MOVAPSmr"sv;

            case MOpc::LOCK_XADD64mr:
                return "LOCK_XADD64mr"sv;
            case MOpc::LOCK_XADD32mr:
                return "LOCK_XADD32mr"sv;
            case MOpc::LOCK_XCHG64mr:
                return "LOCK_XCHG64mr"sv;
            case MOpc::LOCK_AND64mi32:
                return "LOCK_AND64mi32"sv;
            case MOpc::LOCK_OR64mi32:
                return "LOCK_OR64mi32"sv;
            case MOpc::LOCK_XOR64mi32:
                return "LOCK_XOR64mi32"sv;
            case MOpc::LOCK_XADD:
                return "LOCK_XADD"sv;
            case MOpc::LOCK_XCHG:
                return "LOCK_XCHG"sv;
            case MOpc::LOCK_AND:
                return "LOCK_AND"sv;
            case MOpc::LOCK_OR:
                return "LOCK_OR"sv;
            case MOpc::LOCK_XOR:
                return "LOCK_XOR"sv;
            case MOpc::MFENCE:
                return "MFENCE"sv;
            case MOpc::LFENCE:
                return "LFENCE"sv;
            case MOpc::SFENCE:
                return "SFENCE"sv;

            case MOpc::UD2:
                return "UD2"sv;
            case MOpc::NOP:
                return "NOP"sv;
            case MOpc::XCHG64rr:
                return "XCHG64rr"sv;
        }
        return "?OPC?"sv;
    }

    [[nodiscard]] std::string_view phys_reg_name(PhysReg r)
    {
        using namespace std::literals;
        switch (r)
        {
            case PhysReg::Count:
                return "?count?"sv;
            case PhysReg::None:
                return "noreg"sv;
            case PhysReg::RAX:
                return "rax"sv;
            case PhysReg::RCX:
                return "rcx"sv;
            case PhysReg::RDX:
                return "rdx"sv;
            case PhysReg::RBX:
                return "rbx"sv;
            case PhysReg::RSP:
                return "rsp"sv;
            case PhysReg::RBP:
                return "rbp"sv;
            case PhysReg::RSI:
                return "rsi"sv;
            case PhysReg::RDI:
                return "rdi"sv;
            case PhysReg::R8:
                return "r8"sv;
            case PhysReg::R9:
                return "r9"sv;
            case PhysReg::R10:
                return "r10"sv;
            case PhysReg::R11:
                return "r11"sv;
            case PhysReg::R12:
                return "r12"sv;
            case PhysReg::R13:
                return "r13"sv;
            case PhysReg::R14:
                return "r14"sv;
            case PhysReg::R15:
                return "r15"sv;
            case PhysReg::XMM0:
                return "xmm0"sv;
            case PhysReg::XMM1:
                return "xmm1"sv;
            case PhysReg::XMM2:
                return "xmm2"sv;
            case PhysReg::XMM3:
                return "xmm3"sv;
            case PhysReg::XMM4:
                return "xmm4"sv;
            case PhysReg::XMM5:
                return "xmm5"sv;
            case PhysReg::XMM6:
                return "xmm6"sv;
            case PhysReg::XMM7:
                return "xmm7"sv;
            case PhysReg::XMM8:
                return "xmm8"sv;
            case PhysReg::XMM9:
                return "xmm9"sv;
            case PhysReg::XMM10:
                return "xmm10"sv;
            case PhysReg::XMM11:
                return "xmm11"sv;
            case PhysReg::XMM12:
                return "xmm12"sv;
            case PhysReg::XMM13:
                return "xmm13"sv;
            case PhysReg::XMM14:
                return "xmm14"sv;
            case PhysReg::XMM15:
                return "xmm15"sv;
        }
        return "?reg?"sv;
    }

    [[nodiscard]] std::string_view reg_class_name(RegClass rc)
    {
        using namespace std::literals;
        switch (rc)
        {
            case RegClass::GPR64:
                return "GPR64"sv;
            case RegClass::XMM:
                return "XMM"sv;
        }
        return "?"sv;
    }

    [[nodiscard]] std::string format_vreg(VReg vreg)
    {
        if (!vreg.is_valid())
            return "%noreg";

        if (vreg.is_physical())
            return std::string{phys_reg_name(vreg.phys_reg())};

        return std::format("%{}", vreg.id);
    }

    [[nodiscard]] std::string format_mem(MMem const& mem)
    {
        if (!mem.symbol.empty())
        {
            std::string r = "[rip + ";
            r += mem.symbol;
            if (mem.is_got_indirect)
                r += " @GOTPCREL";
            if (mem.disp > 0)
                r += std::format(" + {}", mem.disp);
            else if (mem.disp < 0)
                r += std::format(" - {}", -mem.disp);
            r += "]";
            return r;
        }

        std::string r = "[";
        if (mem.base.is_valid())
            r += format_vreg(mem.base);
        if (mem.index.is_valid())
        {
            r += " + ";
            r += format_vreg(mem.index);
            if (mem.scale > 1)
                r += std::format("*{}", static_cast<unsigned>(mem.scale));
        }
        if (mem.disp != 0)
        {
            if (mem.disp > 0)
                r += std::format(" + {}", mem.disp);
            else
                r += std::format(" - {}", -mem.disp);
        }
        r += "]";
        return r;
    }

    [[nodiscard]] std::string format_op(MOp const& op)
    {
        using namespace std::literals;
        switch (op.kind)
        {
            case MOpKind::None:
                return "<none>";
            case MOpKind::Reg:
                return format_vreg(op.reg);
            case MOpKind::Imm64:
                return std::format("${}", op.imm);
            case MOpKind::Mem:
                return format_mem(op.mem);
            case MOpKind::FrameSlot:
                return std::format("fs{}", op.frame_slot);
            case MOpKind::Label:
                return std::format("bb{}", op.label);
            case MOpKind::Symbol:
                return std::string{op.symbol};
        }
        return "?op?";
    }

    [[nodiscard]] std::string format_instr(MInstr const& mi)
    {
        std::string r = "  ";
        r += opc_name(mi.opc);

        for (int i = 0; i < static_cast<int>(PhysReg::Count); ++i)
        {
            if (mi.implicit_defs & (1ULL << i))
            {
                auto pr = static_cast<PhysReg>(i);
                if (pr == PhysReg::None)
                    continue;

                r += ' ';
                r += phys_reg_name(pr);
            }
        }

        for (std::uint8_t i = 0; i < mi.num_ops; ++i)
        {
            if (i == mi.num_defs && mi.num_defs > 0)
                r += " <-";
            r += ' ';
            r += format_op(mi.ops[static_cast<std::size_t>(i)]);
        }

        for (int i = 0; i < static_cast<int>(PhysReg::Count); ++i)
        {
            if (mi.implicit_uses & (1ULL << i))
            {
                auto pr = static_cast<PhysReg>(i);
                if (pr == PhysReg::None)
                    continue;
                r += ' ';
                r += phys_reg_name(pr);
            }
        }

        if (mi.has_side_effects())
            r += " [side-effect]";

        return r;
    }

} // namespace dcc::backend::em64t

template <> struct std::hash<dcc::backend::em64t::VReg>
{
    std::size_t operator()(dcc::backend::em64t::VReg const& v) const noexcept { return std::hash<std::uint32_t>{}(v.id); }
};
