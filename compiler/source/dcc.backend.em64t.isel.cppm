module;

#include <algorithm>

export module dcc.backend.em64t.isel;

import std;
import dcc.ir;
import dcc.ir.analysis;
import dcc.backend.em64t.mir;
import dcc.target;

namespace dcc::backend::em64t
{
    namespace
    {
        enum class CallConvKind : std::uint8_t
        {
            SysV,
            Win64,
        };

        [[nodiscard]] CallConvKind detect_cc(dcc::target::TargetConfig const& target)
        {
            return (target.os == dcc::target::Os::Windows || target.object_format == dcc::target::ObjectFormat::Coff) ? CallConvKind::Win64
                                                                                                                      : CallConvKind::SysV;
        }

        constexpr PhysReg kSysVIntArgRegs[6] = {PhysReg::RDI, PhysReg::RSI, PhysReg::RDX, PhysReg::RCX, PhysReg::R8, PhysReg::R9};
        constexpr PhysReg kSysVFloatArgRegs[8] = {PhysReg::XMM0, PhysReg::XMM1, PhysReg::XMM2, PhysReg::XMM3,
                                                  PhysReg::XMM4, PhysReg::XMM5, PhysReg::XMM6, PhysReg::XMM7};

        constexpr PhysReg kWin64IntArgRegs[4] = {PhysReg::RCX, PhysReg::RDX, PhysReg::R8, PhysReg::R9};
        constexpr PhysReg kWin64FloatArgRegs[4] = {PhysReg::XMM0, PhysReg::XMM1, PhysReg::XMM2, PhysReg::XMM3};

        constexpr PhysReg kCallerSavedSysV_GPR[10] = {
            PhysReg::RAX, PhysReg::RCX, PhysReg::RDX, PhysReg::RSI, PhysReg::RDI, PhysReg::R8, PhysReg::R9, PhysReg::R10, PhysReg::R11,
        };
        constexpr PhysReg kCallerSavedWin64_GPR[5] = {
            PhysReg::RAX, PhysReg::RCX, PhysReg::RDX, PhysReg::R8, PhysReg::R9,
        };

        [[nodiscard]] std::string_view make_imp_name(MFunction& mfunc, std::string_view name)
        {
            mfunc.owned_strings.push_back(std::make_unique<std::string>(std::string{"__imp_"} + std::string{name}));
            return *mfunc.owned_strings.back();
        }

        [[nodiscard]] bool is_dllimport_coff(dcc::target::TargetConfig const& target, dcc::ir::IrGlobalRef const* gr)
        {
            if (target.object_format != dcc::target::ObjectFormat::Coff)
                return false;
            return (gr->global && gr->global->is_dll_import) || (gr->function && gr->function->is_dll_import);
        }

        struct IselCtx
        {
            MFunction& mfunc;
            dcc::target::TargetConfig const& target;
            CallConvKind cc;
            std::unordered_map<dcc::ir::IrBasicBlock const*, std::uint32_t> ir_bb_to_mblock;
            std::unordered_map<dcc::ir::IrValue const*, VReg> value_map;
            std::unordered_map<dcc::ir::IrValue const*, std::uint32_t> alloca_to_slot;
            std::unordered_map<dcc::ir::IrValue const*, std::uint32_t> aggregate_to_slot;
            std::unordered_set<dcc::ir::IrValue const*> memory_addr_values;

            std::unordered_map<dcc::ir::IrValue const*, MOpc> branch_comparisons;

            std::uint32_t current_block_id = 0;
            bool has_error = false;
            bool uses_sret = false;
            VReg sret_ptr_vreg;

            IselCtx(MFunction& f, dcc::target::TargetConfig const& t) : mfunc(f), target(t), cc(detect_cc(t)) {}

            void append_instr(MInstr mi)
            {
                auto* blk = mfunc.block_by_id(current_block_id);
                if (blk)
                    blk->instrs.push_back(mi);
            }

            VReg get_or_create_vreg(dcc::ir::IrValue const* ir_val)
            {
                if (!ir_val)
                    return VReg{};

                auto it = value_map.find(ir_val);
                if (it != value_map.end())
                    return it->second;

                VReg v = mfunc.new_vreg();
                value_map[ir_val] = v;

                if (auto* ic = dcc::ir::ir_cast<dcc::ir::IrIntConstant>(ir_val))
                {
                    auto mi = make_mov_ri(v, ic->value, ir_val->type ? static_cast<dcc::ir::IrIntType const*>(ir_val->type)->bits : 64);
                    append_instr(mi);
                }
                else if (dcc::ir::ir_cast<dcc::ir::IrBoolConstant>(ir_val))
                {
                    auto* bc = static_cast<dcc::ir::IrBoolConstant const*>(ir_val);
                    auto mi = make_mov_ri(v, bc->value ? 1 : 0, 32);
                    append_instr(mi);
                }
                else if (dcc::ir::ir_cast<dcc::ir::IrNullConstant>(ir_val))
                {
                    auto mi = make_mov_ri(v, 0, 64);
                    append_instr(mi);
                }
                else if (dcc::ir::ir_cast<dcc::ir::IrFloatConstant>(ir_val))
                {
                    auto* fc = static_cast<dcc::ir::IrFloatConstant const*>(ir_val);
                    std::int64_t bits = 0;
                    if (ir_val->type && static_cast<dcc::ir::IrFloatType const*>(ir_val->type)->bits == 32)
                    {
                        float f = static_cast<float>(fc->value);
                        std::memcpy(&bits, &f, 4);
                    }
                    else
                    {
                        double d = fc->value;
                        std::memcpy(&bits, &d, 8);
                    }

                    VReg gpr_temp = mfunc.new_vreg();
                    auto mi = make_mov_ri(gpr_temp, bits, 64);
                    append_instr(mi);

                    MInstr movq;
                    movq.opc = MOpc::MOVQ64rr;
                    movq.num_ops = 2;
                    movq.num_defs = 1;
                    movq.ops[0] = MOp::from_reg(v);
                    movq.ops[1] = MOp::from_reg(gpr_temp);
                    append_instr(movq);
                }
                else if (dcc::ir::ir_cast<dcc::ir::IrStringConstant>(ir_val))
                {
                    auto mi = make_mov_ri(v, 0, 64);
                    append_instr(mi);
                }
                else if (auto* gr = dcc::ir::ir_cast<dcc::ir::IrGlobalRef>(ir_val))
                {
                    if (!gr->name.empty())
                    {
                        if (is_dllimport_coff(target, gr))
                        {
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(v);
                            mov.ops[1] = MOp::from_mem(MMem::make_sym_reloc(make_imp_name(mfunc, gr->name)));
                            append_instr(mov);
                        }
                        else if (target.position_independent_code)
                        {
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(v);
                            mov.ops[1] = MOp::from_mem(MMem::make_got_reloc(gr->name, static_cast<std::int32_t>(gr->addend)));
                            append_instr(mov);
                        }
                        else
                        {
                            MInstr lea;
                            lea.opc = MOpc::LEA64rm;
                            lea.num_ops = 2;
                            lea.num_defs = 1;
                            lea.ops[0] = MOp::from_reg(v);
                            lea.ops[1] = MOp::from_mem(MMem::make_sym_reloc(gr->name, static_cast<std::int32_t>(gr->addend)));
                            append_instr(lea);
                        }
                    }
                    else
                    {
                        auto mi = make_implicit_def();
                        mi.ops[0] = MOp::from_reg(v);
                        append_instr(mi);
                    }
                }
                else
                {
                    auto mi = make_implicit_def();
                    mi.ops[0] = MOp::from_reg(v);
                    append_instr(mi);
                }

                return v;
            }

            void set_vreg(dcc::ir::IrValue const* ir_val, VReg vreg)
            {
                auto existing = value_map.find(ir_val);
                if (existing != value_map.end() && existing->second != vreg)
                {
                    VReg const placeholder = existing->second;
                    bool is_forward_placeholder = false;

                    for (auto const& blk : mfunc.blocks)
                    {
                        for (auto const& instr : blk.instrs)
                        {
                            if (instr.opc == MOpc::IMPLICIT_DEF && instr.num_defs > 0 && instr.num_ops > 0 && instr.ops[0].kind == MOpKind::Reg &&
                                instr.ops[0].reg == placeholder)
                            {
                                is_forward_placeholder = true;
                                break;
                            }
                        }
                        if (is_forward_placeholder)
                            break;
                    }

                    if (is_forward_placeholder)
                    {
                        for (auto& blk : mfunc.blocks)
                        {
                            std::erase_if(blk.instrs, [&](MInstr& instr) {
                                if (instr.opc == MOpc::IMPLICIT_DEF && instr.num_defs > 0 && instr.num_ops > 0 && instr.ops[0].kind == MOpKind::Reg &&
                                    instr.ops[0].reg == placeholder)
                                    return true;

                                for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                                {
                                    auto& op = instr.ops[i];
                                    if (op.kind == MOpKind::Reg && op.reg == placeholder)
                                        op.reg = vreg;
                                    else if (op.kind == MOpKind::Mem)
                                    {
                                        if (op.mem.base == placeholder)
                                            op.mem.base = vreg;
                                        if (op.mem.index == placeholder)
                                            op.mem.index = vreg;
                                    }
                                }
                                return false;
                            });
                        }
                    }
                }

                value_map[ir_val] = vreg;
            }

            void add_implicit_defs(MInstr& mi, std::span<PhysReg const> regs)
            {
                for (auto r : regs)
                    mi.implicit_defs |= (1ULL << static_cast<std::uint8_t>(r));
            }

            void add_implicit_uses(MInstr& mi, std::span<PhysReg const> regs)
            {
                for (auto r : regs)
                    mi.implicit_uses |= (1ULL << static_cast<std::uint8_t>(r));
            }

            [[nodiscard]] VReg try_materialize(dcc::ir::IrValue const* v)
            {
                if (!v)
                    return VReg{};

                auto it = value_map.find(v);
                if (it != value_map.end())
                    return it->second;

                return get_or_create_vreg(v);
            }

            [[nodiscard]] unsigned type_bits(dcc::ir::IrType const* t) const
            {
                if (!t)
                    return 64;

                if (auto* it = dcc::ir::ir_type_cast<dcc::ir::IrIntType>(t))
                    return it->bits;

                if (auto* ft = dcc::ir::ir_type_cast<dcc::ir::IrFloatType>(t))
                    return ft->bits;

                if (t->kind == dcc::ir::IrTypeKind::Pointer || t->kind == dcc::ir::IrTypeKind::Bool)
                    return target.pointer_bits;

                return 64;
            }

            [[nodiscard]] std::uint64_t type_size(dcc::ir::IrType const* t) const { return t ? t->byte_size : 0; }

            [[nodiscard]] std::uint64_t type_align(dcc::ir::IrType const* t) const { return t ? t->byte_align : 1; }

            [[nodiscard]] bool is_float_type(dcc::ir::IrType const* t) const { return t && (t->kind == dcc::ir::IrTypeKind::Float); }

            [[nodiscard]] bool is_bool_type(dcc::ir::IrType const* t) const { return t && t->kind == dcc::ir::IrTypeKind::Bool; }
        };

    } // anonymous namespace

} // namespace dcc::backend::em64t

export namespace dcc::backend::em64t
{
    [[nodiscard]] MFunction isel_function(dcc::ir::IrFunction const& func, dcc::target::TargetConfig const& target);
}

namespace dcc::backend::em64t
{
    namespace
    {
        void fold_addresses(MFunction& func)
        {
            std::unordered_map<std::uint32_t, unsigned> definitions;
            for (auto const& block : func.blocks)
                for (auto const& inst : block.instrs)
                    for (unsigned i = 0; i < inst.num_defs; ++i)
                        if (inst.ops[i].kind == MOpKind::Reg)
                            ++definitions[inst.ops[i].reg.id];

            std::unordered_set<std::uint32_t> folded;
            for (auto& block : func.blocks)
            {
                std::unordered_map<std::uint32_t, MInstr const*> available;
                struct Match
                {
                    MMem mem;
                    std::vector<std::uint32_t> folded;
                };
                auto offset = [](Match& match, std::int64_t value, unsigned scale) {
                    if (value < std::numeric_limits<std::int32_t>::min() / static_cast<std::int64_t>(scale) ||
                        value > std::numeric_limits<std::int32_t>::max() / static_cast<std::int64_t>(scale))
                        return false;
                    auto disp = static_cast<std::int64_t>(match.mem.disp) + value * scale;
                    if (disp < std::numeric_limits<std::int32_t>::min() || disp > std::numeric_limits<std::int32_t>::max())
                        return false;
                    match.mem.disp = static_cast<std::int32_t>(disp);
                    return true;
                };
                auto match_reg = [&](auto&& self, Match& match, VReg reg, unsigned scale, unsigned depth) -> bool {
                    if (!reg.is_valid())
                        return true;
                    auto found = available.find(reg.id);
                    if (depth < 16 && found != available.end())
                    {
                        auto const& inst = *found->second;
                        Match expanded = match;
                        auto operand = [&](unsigned i, unsigned factor) {
                            if (inst.ops[i].kind == MOpKind::Reg)
                                return self(self, expanded, inst.ops[i].reg, factor, depth + 1);
                            if (inst.ops[i].kind == MOpKind::Imm64)
                                return offset(expanded, inst.ops[i].imm, factor);
                            return false;
                        };
                        auto constant = [&](unsigned i) -> std::optional<std::int64_t> {
                            auto const& op = inst.ops[i];
                            if (op.kind == MOpKind::Imm64)
                                return op.imm;
                            if (op.kind != MOpKind::Reg)
                                return std::nullopt;
                            auto it = available.find(op.reg.id);
                            if (it == available.end())
                                return std::nullopt;
                            auto const& def = *it->second;
                            if (def.ops[1].kind != MOpKind::Imm64)
                                return std::nullopt;
                            if (def.opc != MOpc::MOV64ri && def.opc != MOpc::MOV64ri32 && def.opc != MOpc::MOV32ri)
                                return std::nullopt;
                            expanded.folded.push_back(op.reg.id);
                            return def.opc == MOpc::MOV32ri ? static_cast<std::uint32_t>(def.ops[1].imm) : def.ops[1].imm;
                        };
                        bool ok = false;
                        switch (inst.opc)
                        {
                            case MOpc::MOV64ri:
                            case MOpc::MOV64ri32:
                                ok = operand(1, scale);
                                break;
                            case MOpc::MOV32ri:
                                ok = inst.ops[1].kind == MOpKind::Imm64 &&
                                     offset(expanded, static_cast<std::uint32_t>(inst.ops[1].imm), scale);
                                break;
                            case MOpc::ADD64rr:
                                ok = inst.num_ops == 3 && operand(1, scale) && operand(2, scale);
                                break;
                            case MOpc::SUB64rr:
                                if (inst.num_ops == 3)
                                    if (auto value = constant(2); value && *value != std::numeric_limits<std::int64_t>::min())
                                        ok = operand(1, scale) && offset(expanded, -*value, scale);
                                break;
                            case MOpc::IMUL64rr:
                            case MOpc::IMUL64rri:
                                if (inst.num_ops == 3)
                                {
                                    unsigned input = 1;
                                    auto factor = constant(2);
                                    if (!factor)
                                    {
                                        factor = constant(1);
                                        input = 2;
                                    }
                                    if (factor && (*factor == 1 || *factor == 2 || *factor == 4 || *factor == 8) && *factor * scale <= 8)
                                        ok = operand(input, static_cast<unsigned>(*factor) * scale);
                                }
                                break;
                            case MOpc::LEA64rm:
                                if (inst.ops[1].kind == MOpKind::Mem && inst.ops[1].mem.symbol.empty())
                                {
                                    auto const& mem = inst.ops[1].mem;
                                    if (scale * mem.scale <= 8)
                                        ok = offset(expanded, mem.disp, scale) &&
                                             self(self, expanded, mem.base, scale, depth + 1) &&
                                             self(self, expanded, mem.index, scale * mem.scale, depth + 1);
                                }
                                break;
                            default:
                                break;
                        }
                        if (ok)
                        {
                            expanded.folded.push_back(reg.id);
                            match = std::move(expanded);
                            return true;
                        }
                    }
                    if (!reg.is_virtual())
                        return false;
                    if (scale == 1 && !match.mem.base.is_valid())
                        match.mem.base = reg;
                    else if (!match.mem.index.is_valid())
                    {
                        match.mem.index = reg;
                        match.mem.scale = static_cast<std::uint8_t>(scale);
                    }
                    else
                        return false;
                    return true;
                };

                for (auto& inst : block.instrs)
                {
                    for (unsigned i = 0; i < inst.num_ops; ++i)
                    {
                        auto& op = inst.ops[i];
                        if (op.kind != MOpKind::Mem || !op.mem.symbol.empty())
                            continue;
                        Match match;
                        match.mem.disp = op.mem.disp;
                        if (match_reg(match_reg, match, op.mem.base, 1, 0) &&
                            match_reg(match_reg, match, op.mem.index, op.mem.scale, 0) &&
                            (match.mem.base.is_valid() || match.mem.index.is_valid()))
                        {
                            op.mem = match.mem;
                            folded.insert(match.folded.begin(), match.folded.end());
                        }
                    }
                    if (inst.num_defs == 1 && inst.ops[0].kind == MOpKind::Reg && inst.ops[0].reg.is_virtual() &&
                        definitions[inst.ops[0].reg.id] == 1 && inst.implicit_defs == 0 && inst.implicit_uses == 0)
                        available[inst.ops[0].reg.id] = &inst;
                }
            }

            bool changed;
            do
            {
                std::unordered_set<std::uint32_t> used;
                for (auto const& block : func.blocks)
                    for (auto const& inst : block.instrs)
                        for (unsigned i = inst.num_defs; i < inst.num_ops; ++i)
                        {
                            auto const& op = inst.ops[i];
                            if (op.kind == MOpKind::Reg)
                                used.insert(op.reg.id);
                            else if (op.kind == MOpKind::Mem)
                            {
                                used.insert(op.mem.base.id);
                                used.insert(op.mem.index.id);
                            }
                        }
                changed = false;
                for (auto& block : func.blocks)
                    changed |= std::erase_if(block.instrs, [&](MInstr const& inst) {
                        return inst.num_defs == 1 && inst.ops[0].kind == MOpKind::Reg && folded.contains(inst.ops[0].reg.id) &&
                               !used.contains(inst.ops[0].reg.id);
                    }) != 0;
            } while (changed);
        }

        void emit_mov(IselCtx& ctx, VReg dst, VReg src)
        {
            auto mi = make_copy(dst, src);
            ctx.append_instr(mi);
        }

        void emit_mov_ri(IselCtx& ctx, VReg dst, std::int64_t val, unsigned bits)
        {
            auto mi = make_mov_ri(dst, val, bits);
            ctx.append_instr(mi);
        }

        [[nodiscard]] VReg emit_unary_op(IselCtx& ctx, MOpc opc, VReg src)
        {
            VReg dst = ctx.mfunc.new_vreg();
            MInstr mi;
            mi.opc = opc;
            mi.num_ops = 2;
            mi.num_defs = 1;
            mi.ops[0] = MOp::from_reg(dst);
            mi.ops[1] = MOp::from_reg(src);
            ctx.append_instr(mi);
            return dst;
        }

        [[nodiscard]] VReg emit_binary_op(IselCtx& ctx, MOpc opc, VReg lhs, VReg rhs)
        {
            VReg dst = ctx.mfunc.new_vreg();
            MInstr mi;
            mi.opc = opc;
            mi.num_ops = 3;
            mi.num_defs = 1;
            mi.ops[0] = MOp::from_reg(dst);
            mi.ops[1] = MOp::from_reg(lhs);
            mi.ops[2] = MOp::from_reg(rhs);
            ctx.append_instr(mi);
            return dst;
        }

        void emit_cmp(IselCtx& ctx, VReg lhs, VReg rhs)
        {
            MInstr mi;
            mi.opc = MOpc::CMP64rr;
            mi.num_ops = 2;
            mi.num_defs = 0;
            mi.ops[0] = MOp::from_reg(lhs);
            mi.ops[1] = MOp::from_reg(rhs);
            ctx.append_instr(mi);
        }

        void emit_test(IselCtx& ctx, VReg lhs, VReg rhs)
        {
            MInstr mi;
            mi.opc = MOpc::TEST64rr;
            mi.num_ops = 2;
            mi.num_defs = 0;
            mi.ops[0] = MOp::from_reg(lhs);
            mi.ops[1] = MOp::from_reg(rhs);
            ctx.append_instr(mi);
        }

        [[nodiscard]] VReg emit_setcc(IselCtx& ctx, MOpc set_opc)
        {
            VReg dst = ctx.mfunc.new_vreg();
            MInstr mi;
            mi.opc = set_opc;
            mi.num_ops = 1;
            mi.num_defs = 1;
            mi.ops[0] = MOp::from_reg(dst);
            ctx.append_instr(mi);
            return dst;
        }

        [[nodiscard]] std::optional<MOpc> branch_comparison(dcc::ir::IrValue const* value)
        {
            using enum dcc::ir::IrNodeKind;
            if (!value)
                return std::nullopt;

            MOpc opc;
            switch (value->kind)
            {
                case CmpEq:
                    opc = MOpc::JE;
                    break;
                case CmpNe:
                    opc = MOpc::JNE;
                    break;
                case CmpLt:
                    opc = MOpc::JL;
                    break;
                case CmpLe:
                    opc = MOpc::JLE;
                    break;
                case CmpGt:
                    opc = MOpc::JG;
                    break;
                case CmpGe:
                    opc = MOpc::JGE;
                    break;
                case CmpULt:
                    opc = MOpc::JB;
                    break;
                case CmpULe:
                    opc = MOpc::JBE;
                    break;
                case CmpUGt:
                    opc = MOpc::JA;
                    break;
                case CmpUGe:
                    opc = MOpc::JAE;
                    break;
                default:
                    return std::nullopt;
            }

            auto* cmp = static_cast<dcc::ir::IrCmpEqInst const*>(value);
            if (!cmp->lhs || !cmp->rhs || !cmp->lhs->type || cmp->lhs->type->kind == dcc::ir::IrTypeKind::Float)
                return std::nullopt;
            return opc;
        }

        void emit_jcc(IselCtx& ctx, MOpc jcc_opc, std::uint32_t target_block)
        {
            MInstr mi;
            mi.opc = jcc_opc;
            mi.num_ops = 1;
            mi.num_defs = 0;
            mi.ops[0] = MOp::from_label(target_block);
            ctx.append_instr(mi);
        }

        void emit_jmp(IselCtx& ctx, std::uint32_t target_block)
        {
            MInstr mi;
            mi.opc = MOpc::JMP;
            mi.num_ops = 1;
            mi.num_defs = 0;
            mi.ops[0] = MOp::from_label(target_block);
            ctx.append_instr(mi);
        }

        [[nodiscard]] VReg emit_load(IselCtx& ctx, dcc::ir::IrType const* load_type, VReg addr)
        {
            VReg dst = ctx.mfunc.new_vreg();
            MInstr mi;
            mi.num_ops = 2;
            mi.num_defs = 1;
            mi.ops[0] = MOp::from_reg(dst);
            mi.ops[1] = MOp::from_mem(MMem::make_base_disp(addr));

            if (ctx.is_float_type(load_type))
            {
                auto bits = load_type ? static_cast<dcc::ir::IrFloatType const*>(load_type)->bits : 64;
                mi.opc = (bits == 32) ? MOpc::MOVSSrm : MOpc::MOVSDrm;
            }
            else if (ctx.is_bool_type(load_type))
                mi.opc = MOpc::MOVZX64rm8;
            else
            {
                auto bits = ctx.type_bits(load_type);
                if (bits <= 8)
                    mi.opc = MOpc::MOVZX64rm8;
                else if (bits <= 16)
                    mi.opc = MOpc::MOVZX64rm16;
                else if (bits <= 32)
                    mi.opc = MOpc::MOV32rm;
                else
                    mi.opc = MOpc::MOV64rm;
            }

            ctx.append_instr(mi);
            return dst;
        }

        [[nodiscard]] bool is_memory_type(dcc::ir::IrType const* t) noexcept
        {
            return t && (t->kind == dcc::ir::IrTypeKind::Aggregate || t->kind == dcc::ir::IrTypeKind::Array || t->kind == dcc::ir::IrTypeKind::Slice);
        }

        [[nodiscard]] MOpc store_opc_for_bits(unsigned bits) noexcept
        {
            if (bits <= 8)
                return MOpc::MOV8mr;
            if (bits <= 16)
                return MOpc::MOV16mr;
            if (bits <= 32)
                return MOpc::MOV32mr;
            return MOpc::MOV64mr;
        }

        [[nodiscard]] std::int32_t member_offset_of(IselCtx const& ctx, dcc::ir::IrType const* type, std::uint32_t index) noexcept
        {
            if (!type)
                return 0;

            if (type->kind == dcc::ir::IrTypeKind::Aggregate)
            {
                auto* at = static_cast<dcc::ir::IrAggregateType const*>(type);
                if (index < at->member_offsets.size())
                    return static_cast<std::int32_t>(at->member_offsets[index]);
                return 0;
            }

            if (type->kind == dcc::ir::IrTypeKind::Slice)
                return static_cast<std::int32_t>(index * (ctx.target.pointer_bits / 8));

            if (type->kind == dcc::ir::IrTypeKind::Array)
            {
                auto* at = static_cast<dcc::ir::IrArrayType const*>(type);
                return static_cast<std::int32_t>(index * (at->element ? at->element->byte_size : 8));
            }

            return 0;
        }

        [[nodiscard]] VReg emit_slot_addr(IselCtx& ctx, std::uint32_t slot)
        {
            VReg addr = ctx.mfunc.new_vreg();
            MInstr lea;
            lea.opc = MOpc::LEA64rm;
            lea.num_ops = 2;
            lea.num_defs = 1;
            lea.ops[0] = MOp::from_reg(addr);
            lea.ops[1] = MOp::from_frame_slot(slot);
            ctx.append_instr(lea);
            return addr;
        }

        void emit_mem_copy(IselCtx& ctx, VReg dst_addr, VReg src_addr, std::uint64_t size)
        {
            for (std::uint64_t off = 0; off < size;)
            {
                std::uint64_t chunk = 8;
                while (chunk > 1 && off + chunk > size)
                    chunk /= 2;

                MOpc load_opc = MOpc::MOV64rm;
                MOpc store_opc = MOpc::MOV64mr;
                if (chunk == 4)
                {
                    load_opc = MOpc::MOV32rm;
                    store_opc = MOpc::MOV32mr;
                }
                else if (chunk == 2)
                {
                    load_opc = MOpc::MOVZX64rm16;
                    store_opc = MOpc::MOV16mr;
                }
                else if (chunk == 1)
                {
                    load_opc = MOpc::MOVZX64rm8;
                    store_opc = MOpc::MOV8mr;
                }

                VReg tmp = ctx.mfunc.new_vreg();
                MInstr ld;
                ld.opc = load_opc;
                ld.num_ops = 2;
                ld.num_defs = 1;
                ld.ops[0] = MOp::from_reg(tmp);
                ld.ops[1] = MOp::from_mem(MMem::make_base_disp(src_addr, static_cast<std::int32_t>(off)));
                ctx.append_instr(ld);

                MInstr st;
                st.opc = store_opc;
                st.num_ops = 2;
                st.num_defs = 0;
                st.ops[0] = MOp::from_mem(MMem::make_base_disp(dst_addr, static_cast<std::int32_t>(off)));
                st.ops[1] = MOp::from_reg(tmp);
                ctx.append_instr(st);

                off += chunk;
            }
        }

        VReg emit_aggregate(IselCtx& ctx, dcc::ir::IrAggregateInst const* agg);

        [[nodiscard]] VReg memory_value_addr(IselCtx& ctx, dcc::ir::IrValue const* val)
        {
            auto it = ctx.aggregate_to_slot.find(val);
            if (it != ctx.aggregate_to_slot.end())
                return emit_slot_addr(ctx, it->second);

            if (auto const* agg = dcc::ir::ir_cast<dcc::ir::IrAggregateInst>(val))
                return emit_aggregate(ctx, agg);

            if (ctx.memory_addr_values.contains(val))
                return ctx.try_materialize(val);

            return VReg{};
        }

        VReg emit_aggregate(IselCtx& ctx, dcc::ir::IrAggregateInst const* agg)
        {
            if (auto it = ctx.aggregate_to_slot.find(agg); it != ctx.aggregate_to_slot.end())
                return emit_slot_addr(ctx, it->second);

            std::uint64_t agg_size = agg->type ? agg->type->byte_size : 0;
            std::uint32_t agg_align = static_cast<std::uint32_t>(agg->type ? agg->type->byte_align : 1);
            std::uint32_t slot_idx = ctx.mfunc.new_frame_slot(static_cast<std::uint32_t>(agg_size), agg_align);
            ctx.aggregate_to_slot[agg] = slot_idx;

            VReg addr = ctx.mfunc.new_vreg();
            {
                MInstr lea;
                lea.opc = MOpc::LEA64rm;
                lea.num_ops = 2;
                lea.num_defs = 1;
                lea.ops[0] = MOp::from_reg(addr);
                lea.ops[1] = MOp::from_frame_slot(slot_idx);
                ctx.append_instr(lea);
            }

            dcc::ir::IrType const* agg_type = agg->type;
            for (std::size_t i = 0; i < agg->values.size(); ++i)
            {
                auto const* member = agg->values[i];
                VReg val = is_memory_type(member ? member->type : nullptr) ? memory_value_addr(ctx, member) : ctx.try_materialize(member);
                if (!val.is_valid())
                    continue;

                auto index = static_cast<std::uint32_t>(i);
                std::int32_t member_offset = is_memory_type(agg_type) ? member_offset_of(ctx, agg_type, index) : static_cast<std::int32_t>(i * 8);
                dcc::ir::IrType const* field_type = nullptr;
                if (agg_type && agg_type->kind == dcc::ir::IrTypeKind::Aggregate)
                {
                    auto* at = static_cast<dcc::ir::IrAggregateType const*>(agg_type);
                    if (i < at->members.size())
                        field_type = at->members[i];
                }
                else if (agg_type && agg_type->kind == dcc::ir::IrTypeKind::Array)
                    field_type = static_cast<dcc::ir::IrArrayType const*>(agg_type)->element;

                if (!field_type)
                    field_type = agg->values[i] ? agg->values[i]->type : nullptr;

                if (is_memory_type(field_type))
                {
                    VReg member_dst = ctx.mfunc.new_vreg();
                    MInstr lea;
                    lea.opc = MOpc::LEA64rm;
                    lea.num_ops = 2;
                    lea.num_defs = 1;
                    lea.ops[0] = MOp::from_reg(member_dst);
                    lea.ops[1] = MOp::from_mem(MMem::make_base_disp(addr, member_offset));
                    ctx.append_instr(lea);
                    emit_mem_copy(ctx, member_dst, val, field_type->byte_size);
                    continue;
                }

                MMem store_mem = MMem::make_base_disp(addr, member_offset);
                MInstr store;
                if (field_type && ctx.is_float_type(field_type))
                {
                    auto bits = static_cast<dcc::ir::IrFloatType const*>(field_type)->bits;
                    store.opc = (bits == 32) ? MOpc::MOVSSmr : MOpc::MOVSDmr;
                }
                else if (ctx.is_bool_type(field_type))
                    store.opc = MOpc::MOV8mr;
                else
                    store.opc = store_opc_for_bits(field_type ? ctx.type_bits(field_type) : 64);

                store.num_ops = 2;
                store.num_defs = 0;
                store.ops[0] = MOp::from_mem(store_mem);
                store.ops[1] = MOp::from_reg(val);
                ctx.append_instr((store));
            }

            return addr;
        }

        [[nodiscard]] VReg padded_copy_addr(IselCtx& ctx, dcc::ir::IrType const* ty, VReg src)
        {
            auto size = ty ? ty->byte_size : 0;
            if (size % 8 == 0)
                return src;

            auto padded = static_cast<std::uint32_t>(((size + 7) / 8) * 8);
            auto align = static_cast<std::uint32_t>(std::max<std::uint64_t>(ty ? ty->byte_align : 8, 8));
            auto slot = ctx.mfunc.new_frame_slot(padded, align);
            VReg tmp = emit_slot_addr(ctx, slot);
            emit_mem_copy(ctx, tmp, src, size);
            return tmp;
        }

        void emit_reg_held_memory_store(IselCtx& ctx, dcc::ir::IrType const* ty, VReg dst_addr, VReg val)
        {
            auto size = ty ? ty->byte_size : 8;
            auto slot = ctx.mfunc.new_frame_slot(8, 8);
            VReg tmp = emit_slot_addr(ctx, slot);

            MInstr st;
            st.opc = MOpc::MOV64mr;
            st.num_ops = 2;
            st.num_defs = 0;
            st.ops[0] = MOp::from_mem(MMem::make_base_disp(tmp, 0));
            st.ops[1] = MOp::from_reg(val);
            ctx.append_instr(st);

            emit_mem_copy(ctx, dst_addr, tmp, size);
        }

        void emit_store(IselCtx& ctx, dcc::ir::IrType const* store_type, VReg addr, VReg val)
        {
            MInstr mi;
            mi.num_ops = 2;
            mi.num_defs = 0;
            mi.ops[0] = MOp::from_mem(MMem::make_base_disp(addr));
            mi.ops[1] = MOp::from_reg(val);

            if (ctx.is_float_type(store_type))
            {
                auto bits = store_type ? static_cast<dcc::ir::IrFloatType const*>(store_type)->bits : 64;
                mi.opc = (bits == 32) ? MOpc::MOVSSmr : MOpc::MOVSDmr;
            }
            else if (ctx.is_bool_type(store_type))
                mi.opc = MOpc::MOV8mr;
            else
                mi.opc = store_opc_for_bits(ctx.type_bits(store_type));

            ctx.append_instr(mi);
        }

        [[nodiscard]] std::pair<VReg, VReg> emit_idiv(IselCtx& ctx, VReg dividend, VReg divisor, bool is_signed)
        {
            VReg rax = VReg::phys(PhysReg::RAX);
            VReg rdx = VReg::phys(PhysReg::RDX);

            emit_mov(ctx, rax, dividend);

            if (is_signed)
            {
                MInstr cqo;
                cqo.opc = MOpc::CQO;
                cqo.num_ops = 0;
                cqo.num_defs = 0;
                ctx.add_implicit_defs(cqo, std::array{PhysReg::RDX});
                ctx.add_implicit_uses(cqo, std::array{PhysReg::RAX});
                ctx.append_instr(cqo);
            }
            else
            {
                MInstr xr;
                xr.opc = MOpc::XOR64rr;
                xr.num_ops = 3;
                xr.num_defs = 1;
                xr.ops[0] = MOp::from_reg(rdx);
                xr.ops[1] = MOp::from_reg(rdx);
                xr.ops[2] = MOp::from_reg(rdx);
                ctx.append_instr(xr);
            }

            MInstr divi;
            divi.opc = is_signed ? MOpc::IDIV64r : MOpc::DIV64r;
            divi.num_ops = 1;
            divi.num_defs = 0;
            divi.ops[0] = MOp::from_reg(divisor);
            ctx.add_implicit_defs(divi, std::array{PhysReg::RAX, PhysReg::RDX});
            ctx.add_implicit_uses(divi, std::array{PhysReg::RAX, PhysReg::RDX});
            ctx.append_instr(divi);

            VReg quot = ctx.mfunc.new_vreg();
            emit_mov(ctx, quot, rax);

            VReg rem = ctx.mfunc.new_vreg();
            emit_mov(ctx, rem, rdx);

            return {quot, rem};
        }

        [[nodiscard]] VReg emit_shift(IselCtx& ctx, MOpc cl_opc, VReg lhs, VReg rhs)
        {
            VReg cl = VReg::phys(PhysReg::RCX);
            emit_mov(ctx, cl, rhs);

            VReg dst = ctx.mfunc.new_vreg();
            MInstr mi;
            mi.opc = cl_opc;
            mi.num_ops = 2;
            mi.num_defs = 1;
            mi.ops[0] = MOp::from_reg(dst);
            mi.ops[1] = MOp::from_reg(lhs);
            ctx.add_implicit_uses(mi, std::array{PhysReg::RCX});
            ctx.append_instr(mi);
            return dst;
        }

        [[nodiscard]] bool memory_arg_by_reference(dcc::ir::IrType const* t, CallConvKind cc) noexcept
        {
            return is_memory_type(t) && cc != CallConvKind::SysV && t->byte_size > 8;
        }

        [[nodiscard]] std::int32_t align_up_i32(std::int32_t v, std::int32_t a) noexcept
        {
            auto rem = v % a;
            if (rem == 0)
                return v;

            return v + (a - rem);
        }

        enum class ArgClass : std::uint8_t
        {
            NoClass,
            Integer,
            Sse,
            Memory,
        };

        struct ArgPiece
        {
            ArgClass cls{ArgClass::NoClass};
            unsigned reg_idx{0};
            std::int32_t stack_off{0};
        };

        struct ArgLoc
        {
            ArgPiece pieces[2];
            std::uint8_t num_pieces{0};
            std::uint32_t stack_size{8};
            bool by_reference{false};
            bool on_stack{false};
        };

        static void classify_type(IselCtx const& ctx, dcc::ir::IrType const* ty, std::uint64_t total_size, std::uint64_t byte_offset, ArgClass out[2],
                                  bool& unaligned)
        {
            if (!ty)
            {
                std::uint64_t eb = byte_offset / 8;
                if (eb < 2)
                    if (out[eb] == ArgClass::NoClass)
                        out[eb] = ArgClass::Integer;

                return;
            }

            if (total_size > 16)
            {
                for (int i = 0; i < 2; ++i)
                    out[i] = ArgClass::Memory;
                return;
            }

            std::uint64_t end_off = byte_offset + ty->byte_size;
            if (ty->byte_size > 16)
            {
                for (int i = 0; i < 2; ++i)
                    out[i] = ArgClass::Memory;
                return;
            }

            switch (ty->kind)
            {
                case dcc::ir::IrTypeKind::Void:
                case dcc::ir::IrTypeKind::Bool:
                case dcc::ir::IrTypeKind::Int:
                case dcc::ir::IrTypeKind::Pointer: {
                    for (std::uint64_t off = byte_offset; off < end_off; off += 8)
                    {
                        std::uint64_t eb = off / 8;
                        if (eb >= 2)
                        {
                            for (int i = 0; i < 2; ++i)
                                out[i] = ArgClass::Memory;
                            return;
                        }
                        if (out[eb] == ArgClass::NoClass)
                            out[eb] = ArgClass::Integer;

                        else if (out[eb] == ArgClass::Sse)
                            out[eb] = ArgClass::Integer;
                    }
                    break;
                }

                case dcc::ir::IrTypeKind::Float: {
                    auto* ft = static_cast<dcc::ir::IrFloatType const*>(ty);
                    std::uint64_t fsize = ft->bits / 8;
                    for (std::uint64_t off = byte_offset; off < byte_offset + fsize; off += 8)
                    {
                        std::uint64_t eb = off / 8;
                        if (eb >= 2)
                        {
                            for (int i = 0; i < 2; ++i)
                                out[i] = ArgClass::Memory;
                            return;
                        }

                        if (out[eb] != ArgClass::Integer && out[eb] != ArgClass::Memory)
                            out[eb] = ArgClass::Sse;
                    }
                    break;
                }

                case dcc::ir::IrTypeKind::Aggregate: {
                    auto* agg = static_cast<dcc::ir::IrAggregateType const*>(ty);
                    for (std::size_t mi = 0; mi < agg->members.size(); ++mi)
                    {
                        auto* mt = agg->members[mi];
                        if (!mt)
                            continue;

                        std::uint64_t moff = byte_offset + (mi < agg->member_offsets.size() ? agg->member_offsets[mi] : 0);
                        if (mt->byte_align > 1 && (moff % mt->byte_align) != 0)
                        {
                            unaligned = true;
                            for (int i = 0; i < 2; ++i)
                                out[i] = ArgClass::Memory;
                            return;
                        }
                    }

                    for (std::size_t mi = 0; mi < agg->members.size(); ++mi)
                    {
                        auto* mt = agg->members[mi];
                        if (!mt)
                            continue;

                        std::uint64_t moff = byte_offset + (mi < agg->member_offsets.size() ? agg->member_offsets[mi] : 0);
                        classify_type(ctx, mt, total_size, moff, out, unaligned);
                        if (out[0] == ArgClass::Memory || out[1] == ArgClass::Memory)
                            return;
                    }
                    break;
                }

                case dcc::ir::IrTypeKind::Array: {
                    auto* at = static_cast<dcc::ir::IrArrayType const*>(ty);
                    if (!at->element || at->count == 0)
                        break;

                    std::uint64_t elem_align = at->element->byte_align;
                    std::uint64_t elem_size = at->element->byte_size;

                    for (std::uint64_t ei = 0; ei < at->count; ++ei)
                    {
                        std::uint64_t eoff = byte_offset + ei * elem_size;
                        if (elem_align > 1 && (eoff % elem_align) != 0)
                        {
                            unaligned = true;
                            for (int i = 0; i < 2; ++i)
                                out[i] = ArgClass::Memory;
                            return;
                        }
                        classify_type(ctx, at->element, total_size, eoff, out, unaligned);
                        if (out[0] == ArgClass::Memory || out[1] == ArgClass::Memory)
                            return;
                    }
                    break;
                }

                case dcc::ir::IrTypeKind::Slice: {
                    std::uint64_t ptr_size = ctx.target.pointer_bits / 8;
                    for (std::uint64_t off = byte_offset; off < byte_offset + ptr_size; off += 8)
                    {
                        std::uint64_t eb = off / 8;
                        if (eb < 2 && out[eb] == ArgClass::NoClass)
                            out[eb] = ArgClass::Integer;
                    }

                    for (std::uint64_t off = byte_offset + ptr_size; off < byte_offset + 2 * ptr_size; off += 8)
                    {
                        std::uint64_t eb = off / 8;
                        if (eb < 2 && out[eb] == ArgClass::NoClass)
                            out[eb] = ArgClass::Integer;
                    }
                    break;
                }

                case dcc::ir::IrTypeKind::Func:
                    break;
            }
        }

        struct ReturnPlan
        {
            ArgClass classes[2] = {ArgClass::NoClass, ArgClass::NoClass};
            bool is_memory = false;
            std::uint8_t num_pieces = 0;
            bool has_int0 = false;
            bool has_int1 = false;
            bool has_sse0 = false;
            bool has_sse1 = false;

            [[nodiscard]] bool uses_sret() const noexcept { return is_memory; }
        };

        [[nodiscard]] ReturnPlan make_return_plan(IselCtx const& ctx, dcc::ir::IrType const* ret, CallConvKind cc) noexcept
        {
            ReturnPlan rp;
            if (!is_memory_type(ret))
                return rp;

            if (cc != CallConvKind::SysV)
            {
                rp.is_memory = true;
                return rp;
            }

            if (ret->byte_size > 16)
            {
                rp.is_memory = true;
                return rp;
            }

            if (ret->byte_size <= 8)
            {
                ArgClass cls_arr[2] = {ArgClass::NoClass, ArgClass::NoClass};
                bool unaligned = false;
                classify_type(ctx, ret, ret->byte_size, 0, cls_arr, unaligned);
                if (unaligned || cls_arr[0] == ArgClass::Memory)
                {
                    rp.is_memory = true;
                    return rp;
                }

                rp.classes[0] = cls_arr[0];
                if (cls_arr[0] != ArgClass::NoClass)
                {
                    rp.num_pieces = 1;
                    if (cls_arr[0] == ArgClass::Integer)
                        rp.has_int0 = true;
                    if (cls_arr[0] == ArgClass::Sse)
                        rp.has_sse0 = true;
                }
                return rp;
            }

            ArgClass ret_classes[2] = {ArgClass::NoClass, ArgClass::NoClass};
            bool ret_unaligned = false;
            classify_type(ctx, ret, ret->byte_size, 0, ret_classes, ret_unaligned);

            if (ret_unaligned || ret_classes[0] == ArgClass::Memory || ret_classes[1] == ArgClass::Memory ||
                (ret_classes[0] == ArgClass::NoClass && ret_classes[1] == ArgClass::NoClass))
            {
                rp.is_memory = true;
                return rp;
            }

            for (int pi = 0; pi < 2; ++pi)
            {
                rp.classes[pi] = ret_classes[pi];
                if (ret_classes[pi] != ArgClass::NoClass)
                {
                    ++rp.num_pieces;
                    if (ret_classes[pi] == ArgClass::Integer)
                    {
                        if (rp.has_int0)
                            rp.has_int1 = true;
                        else
                            rp.has_int0 = true;
                    }
                    else
                    {
                        if (rp.has_sse0)
                            rp.has_sse1 = true;
                        else
                            rp.has_sse0 = true;
                    }
                }
            }
            return rp;
        }

        [[nodiscard]] bool returns_via_sret(IselCtx const& ctx, dcc::ir::IrType const* ret, CallConvKind cc) noexcept
        {
            return make_return_plan(ctx, ret, cc).uses_sret();
        }

        [[nodiscard]] std::vector<ArgLoc> classify_args(IselCtx const& ctx, std::span<dcc::ir::IrValue* const> args, bool has_sret,
                                                        std::int32_t& out_stack_bytes)
        {
            std::vector<ArgLoc> locs(args.size());
            out_stack_bytes = 0;

            bool sysv = (ctx.cc == CallConvKind::SysV);
            unsigned max_int = sysv ? 6u : 4u;
            unsigned max_float = sysv ? 8u : 4u;

            unsigned gpr_idx = has_sret ? 1u : 0u;
            unsigned xmm_idx = 0;
            unsigned win_slot = has_sret ? 1u : 0u;

            for (std::size_t i = 0; i < args.size(); ++i)
            {
                auto* ty = args[i] ? args[i]->type : nullptr;
                auto& loc = locs[i];
                bool is_float = ctx.is_float_type(ty);

                if (!sysv)
                {
                    loc.by_reference = memory_arg_by_reference(ty, ctx.cc);
                    loc.stack_size = 8;
                    if (win_slot < max_int)
                    {
                        loc.pieces[0].cls = (is_float && !loc.by_reference) ? ArgClass::Sse : ArgClass::Integer;
                        loc.pieces[0].reg_idx = win_slot;
                        loc.num_pieces = 1;
                    }
                    else
                    {
                        loc.on_stack = true;
                        loc.stack_size = 8;
                        loc.pieces[0].cls = ArgClass::Memory;
                        loc.pieces[0].stack_off = 32 + static_cast<std::int32_t>(win_slot - max_int) * 8;
                        out_stack_bytes = std::max(out_stack_bytes, loc.pieces[0].stack_off + 8 - 32);
                    }
                    ++win_slot;
                    continue;
                }

                if (!is_memory_type(ty) && !is_float)
                {
                    if (gpr_idx < max_int)
                    {
                        loc.pieces[0].cls = ArgClass::Integer;
                        loc.pieces[0].reg_idx = gpr_idx++;
                        loc.num_pieces = 1;
                    }
                    else
                    {
                        loc.on_stack = true;
                        loc.stack_size = 8;
                        loc.pieces[0].cls = ArgClass::Memory;
                        loc.pieces[0].stack_off = out_stack_bytes;
                        out_stack_bytes += 8;
                    }
                    continue;
                }

                if (is_float && !is_memory_type(ty))
                {
                    if (xmm_idx < max_float)
                    {
                        loc.pieces[0].cls = ArgClass::Sse;
                        loc.pieces[0].reg_idx = xmm_idx++;
                        loc.num_pieces = 1;
                    }
                    else
                    {
                        loc.on_stack = true;
                        loc.stack_size = 8;
                        loc.pieces[0].cls = ArgClass::Memory;
                        loc.pieces[0].stack_off = out_stack_bytes;
                        out_stack_bytes += 8;
                    }
                    continue;
                }

                ArgClass classes[2] = {ArgClass::NoClass, ArgClass::NoClass};
                bool unaligned = false;

                classify_type(ctx, ty, ty ? ty->byte_size : 0, 0, classes, unaligned);

                if (classes[0] == ArgClass::Memory || classes[1] == ArgClass::Memory || unaligned)
                {
                    loc.on_stack = true;
                    std::uint32_t arg_align = ty ? static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>(ty->byte_align, 8), 16)) : 8u;
                    loc.stack_size = ty ? static_cast<std::uint32_t>(align_up_i32(static_cast<std::int32_t>(ty->byte_size), 8)) : 8u;
                    out_stack_bytes = align_up_i32(out_stack_bytes, static_cast<std::int32_t>(arg_align));
                    loc.pieces[0].cls = ArgClass::Memory;
                    loc.pieces[0].stack_off = out_stack_bytes;
                    loc.num_pieces = 1;
                    out_stack_bytes += static_cast<std::int32_t>(loc.stack_size);
                    continue;
                }

                unsigned needed_gpr = 0;
                unsigned needed_xmm = 0;
                for (int pi = 0; pi < 2; ++pi)
                {
                    if (classes[pi] == ArgClass::Integer)
                        ++needed_gpr;
                    else if (classes[pi] == ArgClass::Sse)
                        ++needed_xmm;
                }

                bool can_allocate = (needed_gpr == 0 || (gpr_idx + needed_gpr <= max_int)) && (needed_xmm == 0 || (xmm_idx + needed_xmm <= max_float));
                if (!can_allocate)
                {
                    loc.on_stack = true;
                    std::uint32_t arg_align = ty ? static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>(ty->byte_align, 8), 16)) : 8u;
                    loc.stack_size = ty ? static_cast<std::uint32_t>(align_up_i32(static_cast<std::int32_t>(ty->byte_size), 8)) : 8u;
                    out_stack_bytes = align_up_i32(out_stack_bytes, static_cast<std::int32_t>(arg_align));
                    loc.pieces[0].cls = ArgClass::Memory;
                    loc.pieces[0].stack_off = out_stack_bytes;
                    loc.num_pieces = 1;
                    out_stack_bytes += static_cast<std::int32_t>(loc.stack_size);
                    continue;
                }

                for (int pi = 0; pi < 2; ++pi)
                {
                    if (classes[pi] == ArgClass::NoClass)
                        continue;

                    auto& piece = loc.pieces[loc.num_pieces];
                    piece.cls = classes[pi];

                    if (classes[pi] == ArgClass::Integer)
                    {
                        piece.reg_idx = gpr_idx++;
                        loc.num_pieces++;
                    }
                    else if (classes[pi] == ArgClass::Sse)
                    {
                        piece.reg_idx = xmm_idx++;
                        loc.num_pieces++;
                    }
                }

                if (loc.num_pieces == 0 && classes[0] != ArgClass::NoClass)
                {
                    loc.pieces[0].cls = classes[0];
                    loc.num_pieces = 1;
                }
            }

            out_stack_bytes = align_up_i32(out_stack_bytes, 16);
            return locs;
        }

        struct CallLowering
        {
            std::vector<VReg> arg_vregs;
            VReg return_vreg;
            bool has_sret = false;
            std::uint32_t sret_slot_index = std::numeric_limits<std::uint32_t>::max();
        };

        [[nodiscard]] CallLowering lower_call(IselCtx& ctx, dcc::ir::IrValue const* callee, std::span<dcc::ir::IrValue* const> args,
                                              dcc::ir::IrType const* result_type)
        {
            CallLowering cl;

            std::uint32_t callee_slot = std::numeric_limits<std::uint32_t>::max();
            if (!dcc::ir::ir_cast<dcc::ir::IrGlobalRef>(callee))
            {
                VReg callee_vreg = ctx.try_materialize(callee);
                callee_slot = ctx.mfunc.new_frame_slot(8, 8);

                MInstr store;
                store.opc = MOpc::MOV64mr;
                store.num_ops = 2;
                store.num_defs = 0;
                store.ops[0] = MOp::from_frame_slot(callee_slot);
                store.ops[1] = MOp::from_reg(callee_vreg);
                ctx.append_instr(store);
            }

            VReg sret_addr;
            ReturnPlan ret_plan;
            if (is_memory_type(result_type))
            {
                ret_plan = make_return_plan(ctx, result_type, ctx.cc);

                if (ret_plan.uses_sret())
                {
                    cl.has_sret = true;
                    cl.sret_slot_index =
                        ctx.mfunc.new_frame_slot(static_cast<std::uint32_t>(result_type->byte_size), static_cast<std::uint32_t>(result_type->byte_align));

                    sret_addr = ctx.mfunc.new_vreg();
                    MInstr lea_sret;
                    lea_sret.opc = MOpc::LEA64rm;
                    lea_sret.num_ops = 2;
                    lea_sret.num_defs = 1;
                    lea_sret.ops[0] = MOp::from_reg(sret_addr);
                    lea_sret.ops[1] = MOp::from_frame_slot(cl.sret_slot_index);
                    ctx.append_instr((lea_sret));
                }
                else
                    cl.sret_slot_index = ctx.mfunc.new_frame_slot(static_cast<std::uint32_t>(result_type->byte_size),
                                                                  static_cast<std::uint32_t>(std::min<std::uint64_t>(result_type->byte_align, 16)));
            }

            std::int32_t stack_bytes = 0;
            auto arg_locs = classify_args(ctx, args, cl.has_sret, stack_bytes);
            ctx.mfunc.outgoing_args_size = std::max(ctx.mfunc.outgoing_args_size, stack_bytes);

            std::vector<std::array<VReg, 2>> arg_piece_vregs(args.size());
            std::vector<VReg> arg_mem_addr(args.size());

            for (std::size_t i = 0; i < args.size(); ++i)
            {
                auto* arg = args[i];
                auto* arg_ty = arg ? arg->type : nullptr;
                auto const& loc = arg_locs[i];

                if (is_memory_type(arg_ty))
                {
                    VReg src = memory_value_addr(ctx, arg);
                    if (!src.is_valid())
                    {
                        VReg av = ctx.try_materialize(arg);
                        if (av.is_valid())
                            arg_piece_vregs[i][0] = av;
                    }
                    else if (loc.by_reference)
                    {
                        auto slot = ctx.mfunc.new_frame_slot(static_cast<std::uint32_t>(arg_ty->byte_size), static_cast<std::uint32_t>(arg_ty->byte_align));
                        VReg copy = emit_slot_addr(ctx, slot);
                        emit_mem_copy(ctx, copy, src, arg_ty->byte_size);
                        arg_piece_vregs[i][0] = copy;
                    }
                    else if (loc.on_stack)
                        arg_mem_addr[i] = src;
                    else
                    {
                        VReg read_from = padded_copy_addr(ctx, arg_ty, src);
                        for (std::uint8_t pi = 0; pi < loc.num_pieces; ++pi)
                        {
                            std::int32_t piece_off = static_cast<std::int32_t>(pi) * 8;
                            VReg pv = ctx.mfunc.new_vreg();
                            auto const& piece = loc.pieces[pi];
                            MInstr ld;

                            if (piece.cls == ArgClass::Sse)
                                ld.opc = MOpc::MOVSDrm;
                            else
                                ld.opc = MOpc::MOV64rm;

                            ld.num_ops = 2;
                            ld.num_defs = 1;
                            ld.ops[0] = MOp::from_reg(pv);
                            ld.ops[1] = MOp::from_mem(MMem::make_base_disp(read_from, piece_off));
                            ctx.append_instr(ld);
                            arg_piece_vregs[i][pi] = pv;
                        }
                    }
                }
                else
                {
                    VReg av = ctx.try_materialize(arg);
                    if (av.is_valid())
                        arg_piece_vregs[i][0] = av;
                }
            }

            using PhysRegSpan = std::span<PhysReg const>;
            auto const& int_regs = (ctx.cc == CallConvKind::SysV) ? PhysRegSpan{kSysVIntArgRegs} : PhysRegSpan{kWin64IntArgRegs};
            auto const& float_regs = (ctx.cc == CallConvKind::SysV) ? PhysRegSpan{kSysVFloatArgRegs} : PhysRegSpan{kWin64FloatArgRegs};

            VReg rsp = VReg::phys(PhysReg::RSP);

            for (std::size_t i = 0; i < args.size(); ++i)
            {
                auto const& loc = arg_locs[i];
                if (!loc.on_stack)
                    continue;

                if (arg_mem_addr[i].is_valid())
                {
                    VReg dst = ctx.mfunc.new_vreg();
                    MInstr lea;
                    lea.opc = MOpc::LEA64rm;
                    lea.num_ops = 2;
                    lea.num_defs = 1;
                    lea.ops[0] = MOp::from_reg(dst);
                    lea.ops[1] = MOp::from_mem(MMem::make_base_disp(rsp, loc.pieces[0].stack_off));
                    ctx.append_instr(lea);
                    emit_mem_copy(ctx, dst, arg_mem_addr[i], args[i]->type->byte_size);
                    continue;
                }

                VReg av = arg_piece_vregs[i][0];
                if (!av.is_valid())
                    continue;

                MInstr st;
                st.num_ops = 2;
                st.num_defs = 0;
                st.ops[0] = MOp::from_mem(MMem::make_base_disp(rsp, loc.pieces[0].stack_off));
                st.ops[1] = MOp::from_reg(av);
                st.opc = (!loc.by_reference && ctx.is_float_type(args[i] ? args[i]->type : nullptr)) ? MOpc::MOVSDmr : MOpc::MOV64mr;
                ctx.append_instr(st);
            }

            if (cl.has_sret)
                emit_mov(ctx, VReg::phys(int_regs[0]), sret_addr);

            for (std::size_t i = 0; i < args.size(); ++i)
            {
                auto const& loc = arg_locs[i];
                if (loc.on_stack)
                    continue;

                for (std::uint8_t pi = 0; pi < loc.num_pieces; ++pi)
                {
                    VReg av = arg_piece_vregs[i][pi];
                    if (!av.is_valid())
                        continue;

                    auto const& piece = loc.pieces[pi];
                    switch (piece.cls)
                    {
                        case ArgClass::Integer:
                            emit_mov(ctx, VReg::phys(int_regs[piece.reg_idx]), av);
                            break;
                        case ArgClass::Sse: {
                            MInstr mov;
                            mov.opc = MOpc::MOVSDrr;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(VReg::phys(float_regs[piece.reg_idx]));
                            mov.ops[1] = MOp::from_reg(av);
                            ctx.append_instr(mov);
                            break;
                        }
                        case ArgClass::NoClass:
                        case ArgClass::Memory:
                            break;
                    }
                }
            }

            MInstr call_instr;

            if (auto* gr = dcc::ir::ir_cast<dcc::ir::IrGlobalRef>(callee))
            {
                if (is_dllimport_coff(ctx.target, gr))
                {
                    auto imp_name = make_imp_name(ctx.mfunc, gr->name);
                    VReg scratch = VReg::phys(PhysReg::R11);
                    MInstr mov;
                    mov.opc = MOpc::MOV64rm;
                    mov.num_ops = 2;
                    mov.num_defs = 1;
                    mov.ops[0] = MOp::from_reg(scratch);
                    mov.ops[1] = MOp::from_mem(MMem::make_sym_reloc(imp_name));
                    ctx.add_implicit_defs(mov, std::array{PhysReg::R11});
                    ctx.append_instr(mov);

                    call_instr.opc = MOpc::CALL_r64;
                    call_instr.num_ops = 1;
                    call_instr.num_defs = 0;
                    call_instr.ops[0] = MOp::from_reg(scratch);
                }
                else
                {
                    call_instr.opc = MOpc::CALL_rel32;
                    call_instr.num_ops = 1;
                    call_instr.num_defs = 0;
                    call_instr.ops[0] = MOp::from_symbol(gr->name);
                }
            }
            else
            {
                VReg callee_vreg = VReg::phys(PhysReg::R11);
                MInstr load;
                load.opc = MOpc::MOV64rm;
                load.num_ops = 2;
                load.num_defs = 1;
                load.ops[0] = MOp::from_reg(callee_vreg);
                load.ops[1] = MOp::from_frame_slot(callee_slot);
                ctx.append_instr(load);

                call_instr.opc = MOpc::CALL;
                call_instr.num_ops = 1;
                call_instr.num_defs = 0;
                call_instr.ops[0] = MOp::from_reg(callee_vreg);
            }

            if (ctx.cc == CallConvKind::SysV)
            {
                ctx.add_implicit_defs(call_instr, kCallerSavedSysV_GPR);
                ctx.add_implicit_defs(call_instr, kSysVFloatArgRegs);
            }
            else
            {
                ctx.add_implicit_defs(call_instr, kCallerSavedWin64_GPR);
                ctx.add_implicit_defs(call_instr, kSysVFloatArgRegs);
            }

            ctx.append_instr(call_instr);

            if (result_type && result_type->kind != dcc::ir::IrTypeKind::Void)
            {
                if (cl.has_sret)
                {
                    cl.return_vreg = sret_addr;
                }
                else if (is_memory_type(result_type))
                {
                    ReturnPlan const& rp = ret_plan;

                    VReg rax_v;
                    if (rp.has_int0)
                    {
                        rax_v = ctx.mfunc.new_vreg();
                        emit_mov(ctx, rax_v, VReg::phys(PhysReg::RAX));
                    }
                    VReg rdx_v;
                    if (rp.has_int1)
                    {
                        rdx_v = ctx.mfunc.new_vreg();
                        emit_mov(ctx, rdx_v, VReg::phys(PhysReg::RDX));
                    }

                    VReg slot_addr = ctx.mfunc.new_vreg();
                    {
                        MInstr lea;
                        lea.opc = MOpc::LEA64rm;
                        lea.num_ops = 2;
                        lea.num_defs = 1;
                        lea.ops[0] = MOp::from_reg(slot_addr);
                        lea.ops[1] = MOp::from_frame_slot(cl.sret_slot_index);
                        ctx.append_instr(lea);
                    }

                    unsigned caller_int_idx = 0;
                    unsigned caller_sse_idx = 0;
                    for (int pi = 0; pi < 2; ++pi)
                    {
                        if (rp.classes[pi] == ArgClass::NoClass)
                            continue;

                        std::int32_t piece_off = static_cast<std::int32_t>(pi) * 8;
                        std::uint64_t piece_size = std::min<std::uint64_t>(8, result_type->byte_size - static_cast<std::uint64_t>(pi) * 8);

                        if (rp.classes[pi] == ArgClass::Integer)
                        {
                            VReg val = (caller_int_idx == 0) ? rax_v : rdx_v;
                            ++caller_int_idx;
                            MInstr st;
                            st.opc = (piece_size <= 4) ? MOpc::MOV32mr : MOpc::MOV64mr;
                            st.num_ops = 2;
                            st.num_defs = 0;
                            st.ops[0] = MOp::from_mem(MMem::make_base_disp(slot_addr, piece_off));
                            st.ops[1] = MOp::from_reg(val);
                            ctx.append_instr(st);
                        }
                        else
                        {
                            PhysReg xmm_reg = (caller_sse_idx == 0) ? PhysReg::XMM0 : PhysReg::XMM1;
                            ++caller_sse_idx;

                            MInstr st;
                            if (piece_size >= 8)
                                st.opc = MOpc::MOVSDmr;
                            else
                                st.opc = MOpc::MOVSSmr;

                            st.num_ops = 2;
                            st.num_defs = 0;
                            st.ops[0] = MOp::from_mem(MMem::make_base_disp(slot_addr, piece_off));
                            st.ops[1] = MOp::from_reg(VReg::phys(xmm_reg));
                            ctx.append_instr(st);
                        }
                    }

                    cl.return_vreg = slot_addr;
                }
                else
                {
                    VReg ret_phys = VReg::phys(ctx.is_float_type(result_type) ? PhysReg::XMM0 : PhysReg::RAX);
                    VReg ret_vreg = ctx.mfunc.new_vreg();
                    if (ctx.is_float_type(result_type))
                    {
                        MInstr mov;
                        mov.opc = MOpc::MOVSDrr;
                        mov.num_ops = 2;
                        mov.num_defs = 1;
                        mov.ops[0] = MOp::from_reg(ret_vreg);
                        mov.ops[1] = MOp::from_reg(ret_phys);
                        ctx.append_instr(mov);
                    }
                    else
                        emit_mov(ctx, ret_vreg, ret_phys);

                    cl.return_vreg = ret_vreg;
                }
            }

            return cl;
        }

        void lower_instruction(IselCtx& ctx, dcc::ir::IrValue const* inst)
        {
            if (!inst)
                return;

            using namespace dcc::ir;

            switch (inst->kind)
            {
                case IrNodeKind::IntConstant:
                case IrNodeKind::FloatConstant:
                case IrNodeKind::BoolConstant:
                case IrNodeKind::NullConstant:
                case IrNodeKind::StringConstant:
                case IrNodeKind::Local:
                    break;

                case IrNodeKind::GlobalRef: {
                    auto* gr = ir_cast<IrGlobalRef>(inst);
                    if (gr && !gr->name.empty())
                    {
                        VReg v = ctx.mfunc.new_vreg();
                        if (is_dllimport_coff(ctx.target, gr))
                        {
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(v);
                            mov.ops[1] = MOp::from_mem(MMem::make_sym_reloc(make_imp_name(ctx.mfunc, gr->name)));
                            ctx.append_instr(mov);
                        }
                        else if (ctx.target.position_independent_code)
                        {
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(v);
                            mov.ops[1] = MOp::from_mem(MMem::make_got_reloc(gr->name, static_cast<std::int32_t>(gr->addend)));
                            ctx.append_instr(mov);
                        }
                        else
                        {
                            MInstr lea;
                            lea.opc = MOpc::LEA64rm;
                            lea.num_ops = 2;
                            lea.num_defs = 1;
                            lea.ops[0] = MOp::from_reg(v);
                            lea.ops[1] = MOp::from_mem(MMem::make_sym_reloc(gr->name, static_cast<std::int32_t>(gr->addend)));
                            ctx.append_instr(lea);
                        }
                        ctx.set_vreg(inst, v);
                    }
                    else
                    {
                        VReg v = ctx.mfunc.new_vreg();
                        auto mi = make_implicit_def();
                        mi.ops[0] = MOp::from_reg(v);
                        ctx.append_instr(mi);
                        ctx.set_vreg(inst, v);
                    }
                    break;
                }

                case IrNodeKind::Alloca: {
                    auto* a = ir_cast<IrAllocaInst>(inst);
                    if (!a)
                        break;

                    if (a->count)
                    {
                        VReg count_vreg = ctx.try_materialize(a->count);
                        VReg size_vreg = ctx.mfunc.new_vreg();
                        auto sz = static_cast<std::int64_t>(a->allocated_type ? a->allocated_type->byte_size : 1);
                        emit_mov_ri(ctx, size_vreg, sz, 64);
                        VReg total = emit_binary_op(ctx, MOpc::IMUL64rr, count_vreg, size_vreg);

                        VReg rsp = VReg::phys(PhysReg::RSP);
                        MInstr sub;
                        sub.opc = MOpc::SUB64rr;
                        sub.num_ops = 2;
                        sub.num_defs = 1;
                        sub.ops[0] = MOp::from_reg(rsp);
                        sub.ops[1] = MOp::from_reg(total);
                        ctx.append_instr((sub));

                        VReg result = ctx.mfunc.new_vreg();
                        emit_mov(ctx, result, rsp);
                        ctx.set_vreg(inst, result);
                    }
                    else
                    {
                        auto slot_size = a->allocated_type ? a->allocated_type->byte_size : 1;
                        auto slot_align = a->allocated_type ? a->allocated_type->byte_align : 1;
                        auto slot_idx = ctx.mfunc.new_frame_slot(static_cast<std::uint32_t>(slot_size), static_cast<std::uint32_t>(slot_align));
                        ctx.alloca_to_slot[inst] = slot_idx;

                        ctx.set_vreg(inst, emit_slot_addr(ctx, slot_idx));
                    }
                    break;
                }

                case IrNodeKind::Load:
                case IrNodeKind::LoadVolatile: {
                    IrValue const* ptr_val = nullptr;
                    IrType const* load_type = inst->type;

                    if (auto* l = ir_cast<IrLoadInst>(inst))
                        ptr_val = l->pointer;
                    else if (auto* lv = ir_cast<IrLoadVolatileInst>(inst))
                        ptr_val = lv->pointer;

                    if (is_memory_type(load_type))
                    {
                        auto alloca_it = ctx.alloca_to_slot.find(ptr_val);
                        ctx.memory_addr_values.insert(inst);
                        if (alloca_it != ctx.alloca_to_slot.end())
                        {
                            ctx.aggregate_to_slot[inst] = alloca_it->second;
                            ctx.set_vreg(inst, emit_slot_addr(ctx, alloca_it->second));
                        }
                        else
                        {
                            VReg addr = ctx.try_materialize(ptr_val);
                            if (addr.is_valid())
                                ctx.set_vreg(inst, addr);
                        }
                        break;
                    }

                    if (auto* gr = ir_cast<IrGlobalRef>(ptr_val))
                    {
                        if (is_dllimport_coff(ctx.target, gr))
                        {
                            VReg addr = ctx.mfunc.new_vreg();
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(addr);
                            mov.ops[1] = MOp::from_mem(MMem::make_sym_reloc(make_imp_name(ctx.mfunc, gr->name)));
                            ctx.append_instr(mov);

                            VReg result = emit_load(ctx, load_type, addr);
                            ctx.set_vreg(inst, result);
                        }
                        else if (ctx.target.position_independent_code)
                        {
                            VReg addr = ctx.mfunc.new_vreg();
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(addr);
                            mov.ops[1] = MOp::from_mem(MMem::make_got_reloc(gr->name, static_cast<std::int32_t>(gr->addend)));
                            ctx.append_instr(mov);

                            VReg result = emit_load(ctx, load_type, addr);
                            ctx.set_vreg(inst, result);
                        }
                        else
                        {
                            VReg dst = ctx.mfunc.new_vreg();
                            MInstr mi;
                            mi.num_ops = 2;
                            mi.num_defs = 1;
                            mi.ops[0] = MOp::from_reg(dst);
                            mi.ops[1] = MOp::from_mem(MMem::make_sym_reloc(gr->name, static_cast<std::int32_t>(gr->addend)));

                            if (ctx.is_float_type(load_type))
                            {
                                auto bits = load_type ? static_cast<IrFloatType const*>(load_type)->bits : 64;
                                mi.opc = (bits == 32) ? MOpc::MOVSSrm : MOpc::MOVSDrm;
                            }
                            else if (ctx.is_bool_type(load_type))
                                mi.opc = MOpc::MOVZX64rm8;
                            else
                            {
                                auto bits = ctx.type_bits(load_type);
                                if (bits <= 8)
                                    mi.opc = MOpc::MOVZX64rm8;
                                else if (bits <= 16)
                                    mi.opc = MOpc::MOVZX64rm16;
                                else if (bits <= 32)
                                    mi.opc = MOpc::MOV32rm;
                                else
                                    mi.opc = MOpc::MOV64rm;
                            }

                            ctx.append_instr(mi);
                            ctx.set_vreg(inst, dst);
                        }
                    }
                    else
                    {
                        auto alloca_it = ctx.alloca_to_slot.find(ptr_val);

                        if (alloca_it != ctx.alloca_to_slot.end())
                        {
                            VReg dst = ctx.mfunc.new_vreg();
                            MInstr mi;
                            mi.num_ops = 2;
                            mi.num_defs = 1;
                            mi.ops[0] = MOp::from_reg(dst);
                            mi.ops[1] = MOp::from_frame_slot(alloca_it->second);

                            if (ctx.is_float_type(load_type))
                            {
                                auto bits = load_type ? static_cast<dcc::ir::IrFloatType const*>(load_type)->bits : 64;
                                mi.opc = (bits == 32) ? MOpc::MOVSSrm : MOpc::MOVSDrm;
                            }
                            else if (ctx.is_bool_type(load_type))
                                mi.opc = MOpc::MOVZX64rm8;
                            else
                            {
                                auto bits = ctx.type_bits(load_type);
                                if (bits <= 8)
                                    mi.opc = MOpc::MOVZX64rm8;
                                else if (bits <= 16)
                                    mi.opc = MOpc::MOVZX64rm16;
                                else if (bits <= 32)
                                    mi.opc = MOpc::MOV32rm;
                                else
                                    mi.opc = MOpc::MOV64rm;
                            }

                            ctx.append_instr(mi);
                            ctx.set_vreg(inst, dst);
                        }
                        else
                        {
                            VReg addr = ctx.try_materialize(ptr_val);
                            if (addr.is_valid())
                            {
                                VReg result = emit_load(ctx, load_type, addr);
                                ctx.set_vreg(inst, result);
                            }
                        }
                    }
                    break;
                }

                case IrNodeKind::Store:
                case IrNodeKind::StoreVolatile: {
                    IrValue const* val_val = nullptr;
                    IrValue const* ptr_val = nullptr;

                    if (auto* s = ir_cast<IrStoreInst>(inst))
                    {
                        val_val = s->value;
                        ptr_val = s->pointer;
                    }
                    else if (auto* sv = ir_cast<IrStoreVolatileInst>(inst))
                    {
                        val_val = sv->value;
                        ptr_val = sv->pointer;
                    }

                    if (auto* gr = ir_cast<IrGlobalRef>(ptr_val))
                    {
                        VReg val = ctx.try_materialize(val_val);
                        if (val.is_valid())
                        {
                            if (is_dllimport_coff(ctx.target, gr))
                            {
                                VReg addr = ctx.mfunc.new_vreg();
                                MInstr mov;
                                mov.opc = MOpc::MOV64rm;
                                mov.num_ops = 2;
                                mov.num_defs = 1;
                                mov.ops[0] = MOp::from_reg(addr);
                                mov.ops[1] = MOp::from_mem(MMem::make_sym_reloc(make_imp_name(ctx.mfunc, gr->name)));
                                ctx.append_instr(mov);

                                auto store_type = val_val ? val_val->type : nullptr;
                                emit_store(ctx, store_type, addr, val);
                            }
                            else if (ctx.target.position_independent_code)
                            {
                                VReg addr = ctx.mfunc.new_vreg();
                                MInstr mov;
                                mov.opc = MOpc::MOV64rm;
                                mov.num_ops = 2;
                                mov.num_defs = 1;
                                mov.ops[0] = MOp::from_reg(addr);
                                mov.ops[1] = MOp::from_mem(MMem::make_got_reloc(gr->name, static_cast<std::int32_t>(gr->addend)));
                                ctx.append_instr(mov);

                                auto store_type = val_val ? val_val->type : nullptr;
                                emit_store(ctx, store_type, addr, val);
                            }
                            else
                            {
                                MInstr mi;
                                mi.num_ops = 2;
                                mi.num_defs = 0;
                                mi.ops[0] = MOp::from_mem(MMem::make_sym_reloc(gr->name, static_cast<std::int32_t>(gr->addend)));
                                mi.ops[1] = MOp::from_reg(val);

                                auto store_type = val_val ? val_val->type : nullptr;
                                if (ctx.is_float_type(store_type))
                                {
                                    auto bits = store_type ? static_cast<IrFloatType const*>(store_type)->bits : 64;
                                    mi.opc = (bits == 32) ? MOpc::MOVSSmr : MOpc::MOVSDmr;
                                }
                                else if (ctx.is_bool_type(store_type))
                                    mi.opc = MOpc::MOV8mr;
                                else
                                    mi.opc = store_opc_for_bits(ctx.type_bits(store_type));

                                ctx.append_instr(mi);
                            }
                        }
                    }
                    else
                    {
                        auto alloca_it = ctx.alloca_to_slot.find(ptr_val);
                        if (alloca_it != ctx.alloca_to_slot.end())
                        {
                            auto* store_type = val_val ? val_val->type : nullptr;

                            VReg mem_src = is_memory_type(store_type) ? memory_value_addr(ctx, val_val) : VReg{};
                            if (mem_src.is_valid())
                                emit_mem_copy(ctx, emit_slot_addr(ctx, alloca_it->second), mem_src, store_type->byte_size);
                            else if (is_memory_type(store_type) && store_type->byte_size < 8)
                            {
                                VReg val = ctx.try_materialize(val_val);
                                if (val.is_valid())
                                    emit_reg_held_memory_store(ctx, store_type, emit_slot_addr(ctx, alloca_it->second), val);
                            }
                            else
                            {
                                VReg val = ctx.try_materialize(val_val);
                                if (val.is_valid())
                                {
                                    MInstr mi;
                                    mi.num_ops = 2;
                                    mi.num_defs = 0;
                                    mi.ops[0] = MOp::from_frame_slot(alloca_it->second);
                                    mi.ops[1] = MOp::from_reg(val);

                                    if (ctx.is_float_type(store_type))
                                    {
                                        auto bits = store_type ? static_cast<dcc::ir::IrFloatType const*>(store_type)->bits : 64;
                                        mi.opc = (bits == 32) ? MOpc::MOVSSmr : MOpc::MOVSDmr;
                                    }
                                    else if (ctx.is_bool_type(store_type))
                                        mi.opc = MOpc::MOV8mr;
                                    else
                                        mi.opc = store_opc_for_bits(ctx.type_bits(store_type));

                                    ctx.append_instr(mi);
                                }
                            }
                        }
                        else
                        {
                            auto store_type = val_val ? val_val->type : nullptr;
                            VReg addr = ctx.try_materialize(ptr_val);

                            VReg mem_src = is_memory_type(store_type) ? memory_value_addr(ctx, val_val) : VReg{};
                            if (mem_src.is_valid())
                            {
                                if (addr.is_valid())
                                    emit_mem_copy(ctx, addr, mem_src, store_type->byte_size);
                            }
                            else if (is_memory_type(store_type) && store_type->byte_size < 8)
                            {
                                VReg val = ctx.try_materialize(val_val);
                                if (addr.is_valid() && val.is_valid())
                                    emit_reg_held_memory_store(ctx, store_type, addr, val);
                            }
                            else
                            {
                                VReg val = ctx.try_materialize(val_val);
                                if (addr.is_valid() && val.is_valid())
                                    emit_store(ctx, store_type, addr, val);
                            }
                        }
                    }
                    break;
                }

                case IrNodeKind::Gep: {
                    auto* g = ir_cast<IrGepInst>(inst);
                    if (!g)
                        break;

                    auto base_slot_it = ctx.alloca_to_slot.find(g->base);
                    bool is_alloca_base = (base_slot_it != ctx.alloca_to_slot.end());

                    VReg current_addr;
                    if (is_alloca_base)
                    {
                        current_addr = ctx.mfunc.new_vreg();
                        MInstr lea_base;
                        lea_base.opc = MOpc::LEA64rm;
                        lea_base.num_ops = 2;
                        lea_base.num_defs = 1;
                        lea_base.ops[0] = MOp::from_reg(current_addr);
                        lea_base.ops[1] = MOp::from_frame_slot(base_slot_it->second);
                        ctx.append_instr(lea_base);
                    }
                    else
                        current_addr = ctx.try_materialize(g->base);

                    IrType const* cur_type = nullptr;
                    if (g->base && g->base->type && g->base->type->kind == IrTypeKind::Pointer)
                        cur_type = static_cast<IrPointerType const*>(g->base->type)->pointee;

                    std::int64_t static_offset = 0;

                    for (auto const& idx : g->indices)
                    {
                        if (idx.kind == IrGepInst::IndexKind::Field)
                        {
                            if (cur_type && cur_type->kind == IrTypeKind::Aggregate)
                            {
                                auto* agg = static_cast<IrAggregateType const*>(cur_type);
                                if (idx.field_index < agg->member_offsets.size())
                                {
                                    static_offset += static_cast<std::int64_t>(agg->member_offsets[idx.field_index]);
                                    if (idx.field_index < agg->members.size())
                                        cur_type = agg->members[idx.field_index];
                                }
                            }
                            else if (cur_type && cur_type->kind == IrTypeKind::Slice)
                            {
                                static_offset += static_cast<std::int64_t>(idx.field_index * (ctx.target.pointer_bits / 8));
                                cur_type = nullptr;
                            }
                            else
                            {
                                static_offset += static_cast<std::int64_t>(idx.field_index * 8);
                                cur_type = nullptr;
                            }
                        }
                        else
                        {
                            bool indexes_array = cur_type && cur_type->kind == IrTypeKind::Array;
                            IrType const* elem_type = indexes_array ? static_cast<IrArrayType const*>(cur_type)->element : cur_type;

                            if (idx.dynamic_index)
                            {
                                VReg idx_vreg = ctx.try_materialize(idx.dynamic_index);
                                std::int64_t elem_size = static_cast<std::int64_t>(elem_type ? elem_type->byte_size : 8);

                                if (static_offset != 0)
                                {
                                    VReg new_addr = ctx.mfunc.new_vreg();
                                    MInstr add;
                                    add.opc = MOpc::LEA64rm;
                                    add.num_ops = 2;
                                    add.num_defs = 1;
                                    add.ops[0] = MOp::from_reg(new_addr);
                                    add.ops[1] = MOp::from_mem(MMem::make_base_disp(current_addr, static_cast<std::int32_t>(static_offset)));
                                    ctx.append_instr((add));
                                    current_addr = new_addr;
                                    static_offset = 0;
                                }

                                if (elem_size > 1)
                                {
                                    VReg scaled = ctx.mfunc.new_vreg();
                                    MInstr mul;
                                    mul.opc = MOpc::IMUL64rri;
                                    mul.num_ops = 3;
                                    mul.num_defs = 1;
                                    mul.ops[0] = MOp::from_reg(scaled);
                                    mul.ops[1] = MOp::from_reg(idx_vreg);
                                    mul.ops[2] = MOp::from_imm(elem_size);
                                    ctx.append_instr((mul));
                                    idx_vreg = scaled;
                                }

                                VReg new_addr2 = ctx.mfunc.new_vreg();
                                MInstr add2;
                                add2.opc = MOpc::ADD64rr;
                                add2.num_ops = 3;
                                add2.num_defs = 1;
                                add2.ops[0] = MOp::from_reg(new_addr2);
                                add2.ops[1] = MOp::from_reg(current_addr);
                                add2.ops[2] = MOp::from_reg(idx_vreg);
                                ctx.append_instr((add2));
                                current_addr = new_addr2;
                            }

                            cur_type = indexes_array ? elem_type : nullptr;
                        }
                    }

                    if (static_offset != 0)
                    {
                        VReg result = ctx.mfunc.new_vreg();
                        MInstr add;
                        add.opc = MOpc::LEA64rm;
                        add.num_ops = 2;
                        add.num_defs = 1;
                        add.ops[0] = MOp::from_reg(result);
                        add.ops[1] = MOp::from_mem(MMem::make_base_disp(current_addr, static_cast<std::int32_t>(static_offset)));
                        ctx.append_instr((add));
                        ctx.set_vreg(inst, result);
                    }
                    else
                        ctx.set_vreg(inst, current_addr);
                    break;
                }

                case IrNodeKind::Add:
                case IrNodeKind::Sub:
                case IrNodeKind::Mul:
                case IrNodeKind::And:
                case IrNodeKind::Or:
                case IrNodeKind::Xor: {
                    IrValue const* lhs = nullptr;
                    IrValue const* rhs = nullptr;
                    bool is_float = false;
                    MOpc opc = MOpc::NOP;

                    bool is_f32 = false;
                    auto set_binop = [&](auto const* bin_inst) {
                        lhs = bin_inst->lhs;
                        rhs = bin_inst->rhs;
                        is_float = ctx.is_float_type(bin_inst->type);
                        is_f32 = is_float && static_cast<IrFloatType const*>(bin_inst->type)->bits == 32;
                    };

                    switch (inst->kind)
                    {
                        case IrNodeKind::Add:
                            set_binop(static_cast<IrAddInst const*>(inst));
                            opc = is_float ? (is_f32 ? MOpc::ADDSSrr : MOpc::ADDSDrr) : MOpc::ADD64rr;
                            break;
                        case IrNodeKind::Sub:
                            set_binop(static_cast<IrSubInst const*>(inst));
                            opc = is_float ? (is_f32 ? MOpc::SUBSSrr : MOpc::SUBSDrr) : MOpc::SUB64rr;
                            break;
                        case IrNodeKind::Mul:
                            set_binop(static_cast<IrMulInst const*>(inst));
                            opc = is_float ? (is_f32 ? MOpc::MULSSrr : MOpc::MULSDrr) : MOpc::IMUL64rr;
                            break;
                        case IrNodeKind::And:
                            set_binop(static_cast<IrAndInst const*>(inst));
                            opc = MOpc::AND64rr;
                            break;
                        case IrNodeKind::Or:
                            set_binop(static_cast<IrOrInst const*>(inst));
                            opc = MOpc::OR64rr;
                            break;
                        case IrNodeKind::Xor:
                            set_binop(static_cast<IrXorInst const*>(inst));
                            opc = MOpc::XOR64rr;
                            break;
                        default:
                            break;
                    }

                    VReg lhs_v = ctx.try_materialize(lhs);
                    VReg rhs_v = ctx.try_materialize(rhs);
                    if (lhs_v.is_valid() && rhs_v.is_valid())
                    {
                        if (is_float)
                        {
                            VReg dst = ctx.mfunc.new_vreg();
                            MInstr mi;
                            mi.opc = opc;
                            mi.num_ops = 3;
                            mi.num_defs = 1;
                            mi.ops[0] = MOp::from_reg(dst);
                            mi.ops[1] = MOp::from_reg(lhs_v);
                            mi.ops[2] = MOp::from_reg(rhs_v);
                            ctx.append_instr(mi);
                            ctx.set_vreg(inst, dst);
                        }
                        else
                        {
                            VReg dst = emit_binary_op(ctx, opc, lhs_v, rhs_v);
                            ctx.set_vreg(inst, dst);
                        }
                    }
                    break;
                }

                case IrNodeKind::SDiv:
                case IrNodeKind::SRem:
                case IrNodeKind::UDiv:
                case IrNodeKind::URem: {
                    IrValue const* lhs = nullptr;
                    IrValue const* rhs = nullptr;
                    bool is_signed = false;

                    auto set_div = [&](IrValue const* l, IrValue const* r, bool s) {
                        lhs = l;
                        rhs = r;
                        is_signed = s;
                    };

                    switch (inst->kind)
                    {
                        case IrNodeKind::SDiv:
                            set_div(static_cast<IrSDivInst const*>(inst)->lhs, static_cast<IrSDivInst const*>(inst)->rhs, true);
                            break;
                        case IrNodeKind::SRem:
                            set_div(static_cast<IrSRemInst const*>(inst)->lhs, static_cast<IrSRemInst const*>(inst)->rhs, true);
                            break;
                        case IrNodeKind::UDiv:
                            set_div(static_cast<IrUDivInst const*>(inst)->lhs, static_cast<IrUDivInst const*>(inst)->rhs, false);
                            break;
                        case IrNodeKind::URem:
                            set_div(static_cast<IrURemInst const*>(inst)->lhs, static_cast<IrURemInst const*>(inst)->rhs, false);
                            break;
                        default:
                            break;
                    }

                    VReg lhs_v = ctx.try_materialize(lhs);
                    VReg rhs_v = ctx.try_materialize(rhs);
                    if (lhs_v.is_valid() && rhs_v.is_valid())
                    {
                        auto [quot, rem] = emit_idiv(ctx, lhs_v, rhs_v, is_signed);
                        bool want_rem = (inst->kind == IrNodeKind::SRem || inst->kind == IrNodeKind::URem);
                        ctx.set_vreg(inst, want_rem ? rem : quot);
                    }
                    break;
                }

                case IrNodeKind::FDiv: {
                    auto* fdiv = ir_cast<IrFDivInst>(inst);
                    if (fdiv)
                    {
                        VReg lhs = ctx.try_materialize(fdiv->lhs);
                        VReg rhs = ctx.try_materialize(fdiv->rhs);
                        if (lhs.is_valid() && rhs.is_valid())
                        {
                            VReg dst = ctx.mfunc.new_vreg();
                            bool fdiv_f32 = fdiv->type && fdiv->type->kind == IrTypeKind::Float && static_cast<IrFloatType const*>(fdiv->type)->bits == 32;
                            MInstr mi;
                            mi.opc = fdiv_f32 ? MOpc::DIVSSrr : MOpc::DIVSDrr;
                            mi.num_ops = 3;
                            mi.num_defs = 1;
                            mi.ops[0] = MOp::from_reg(dst);
                            mi.ops[1] = MOp::from_reg(lhs);
                            mi.ops[2] = MOp::from_reg(rhs);
                            ctx.append_instr(mi);
                            ctx.set_vreg(inst, dst);
                        }
                    }
                    break;
                }

                case IrNodeKind::FRem: {
                    auto* frem = ir_cast<IrFRemInst>(inst);
                    if (frem)
                    {
                        VReg lhs = ctx.try_materialize(frem->lhs);
                        ctx.set_vreg(inst, lhs);
                    }
                    break;
                }

                case IrNodeKind::Shl:
                case IrNodeKind::LShr:
                case IrNodeKind::AShr: {
                    IrValue const* lhs = nullptr;
                    IrValue const* rhs = nullptr;
                    MOpc cl_opc = MOpc::SHL64rcl;

                    switch (inst->kind)
                    {
                        case IrNodeKind::Shl: {
                            auto* s = ir_cast<IrShlInst>(inst);
                            if (s)
                            {
                                lhs = s->lhs;
                                rhs = s->rhs;
                            }
                            cl_opc = MOpc::SHL64rcl;
                            break;
                        }
                        case IrNodeKind::LShr: {
                            auto* s = ir_cast<IrLShrInst>(inst);
                            if (s)
                            {
                                lhs = s->lhs;
                                rhs = s->rhs;
                            }
                            cl_opc = MOpc::SHR64rcl;
                            break;
                        }
                        case IrNodeKind::AShr: {
                            auto* s = ir_cast<IrAShrInst>(inst);
                            if (s)
                            {
                                lhs = s->lhs;
                                rhs = s->rhs;
                            }
                            cl_opc = MOpc::SAR64rcl;
                            break;
                        }
                        default:
                            break;
                    }

                    VReg lhs_v = ctx.try_materialize(lhs);
                    VReg rhs_v = ctx.try_materialize(rhs);
                    if (lhs_v.is_valid() && rhs_v.is_valid())
                    {
                        VReg dst = emit_shift(ctx, cl_opc, lhs_v, rhs_v);
                        ctx.set_vreg(inst, dst);
                    }
                    break;
                }

                case IrNodeKind::CmpEq:
                case IrNodeKind::CmpNe:
                case IrNodeKind::CmpLt:
                case IrNodeKind::CmpLe:
                case IrNodeKind::CmpGt:
                case IrNodeKind::CmpGe:
                case IrNodeKind::CmpULt:
                case IrNodeKind::CmpULe:
                case IrNodeKind::CmpUGt:
                case IrNodeKind::CmpUGe:
                case IrNodeKind::CmpOLt:
                case IrNodeKind::CmpOLe:
                case IrNodeKind::CmpOGt:
                case IrNodeKind::CmpOGe: {
                    IrValue const* lhs = nullptr;
                    IrValue const* rhs = nullptr;
                    MOpc set_opc = MOpc::SETEr;

                    auto* c = static_cast<IrCmpEqInst const*>(inst);
                    lhs = c->lhs;
                    rhs = c->rhs;
                    bool is_float = lhs && ctx.is_float_type(lhs->type);

                    switch (inst->kind)
                    {
                        case IrNodeKind::CmpEq:
                            set_opc = MOpc::SETEr;
                            break;
                        case IrNodeKind::CmpNe:
                            set_opc = MOpc::SETNEr;
                            break;
                        case IrNodeKind::CmpLt:
                            set_opc = MOpc::SETLr;
                            break;
                        case IrNodeKind::CmpLe:
                            set_opc = MOpc::SETLEr;
                            break;
                        case IrNodeKind::CmpGt:
                            set_opc = MOpc::SETGr;
                            break;
                        case IrNodeKind::CmpGe:
                            set_opc = MOpc::SETGEr;
                            break;
                        case IrNodeKind::CmpULt:
                            set_opc = MOpc::SETBr;
                            break;
                        case IrNodeKind::CmpULe:
                            set_opc = MOpc::SETBEr;
                            break;
                        case IrNodeKind::CmpUGt:
                            set_opc = MOpc::SETAr;
                            break;
                        case IrNodeKind::CmpUGe:
                            set_opc = MOpc::SETAEr;
                            break;
                        case IrNodeKind::CmpOLt:
                        case IrNodeKind::CmpOLe:
                        case IrNodeKind::CmpOGt:
                        case IrNodeKind::CmpOGe:
                            set_opc = (inst->kind == IrNodeKind::CmpOLt)   ? MOpc::SETBr
                                      : (inst->kind == IrNodeKind::CmpOLe) ? MOpc::SETBEr
                                      : (inst->kind == IrNodeKind::CmpOGt) ? MOpc::SETAr
                                                                           : MOpc::SETAEr;
                            break;
                        default:
                            break;
                    }

                    VReg lhs_v = ctx.try_materialize(lhs);
                    VReg rhs_v = ctx.try_materialize(rhs);
                    if (lhs_v.is_valid() && rhs_v.is_valid())
                    {
                        if (is_float)
                        {
                            bool cmp_f32 = lhs && lhs->type && lhs->type->kind == IrTypeKind::Float && static_cast<IrFloatType const*>(lhs->type)->bits == 32;
                            MInstr ucom;
                            ucom.opc = cmp_f32 ? MOpc::UCOMISSrr : MOpc::UCOMISDrr;
                            ucom.num_ops = 2;
                            ucom.num_defs = 0;
                            ucom.ops[0] = MOp::from_reg(lhs_v);
                            ucom.ops[1] = MOp::from_reg(rhs_v);
                            ctx.append_instr((ucom));
                        }
                        else
                            emit_cmp(ctx, lhs_v, rhs_v);

                        VReg result = emit_setcc(ctx, set_opc);
                        ctx.set_vreg(inst, result);
                    }
                    break;
                }

                case IrNodeKind::Neg: {
                    auto* n = ir_cast<IrNegInst>(inst);
                    if (n)
                    {
                        VReg op = ctx.try_materialize(n->operand);
                        if (op.is_valid())
                        {
                            bool is_float = ctx.is_float_type(n->type);
                            if (is_float)
                            {
                                VReg dst = ctx.mfunc.new_vreg();
                                VReg zero = ctx.mfunc.new_vreg();
                                emit_mov_ri(ctx, zero, 0, 64);
                                MInstr sub;
                                sub.opc = MOpc::SUBSDrr;
                                sub.num_ops = 3;
                                sub.num_defs = 1;
                                sub.ops[0] = MOp::from_reg(dst);
                                sub.ops[1] = MOp::from_reg(zero);
                                sub.ops[2] = MOp::from_reg(op);
                                ctx.append_instr((sub));
                                ctx.set_vreg(inst, dst);
                            }
                            else
                            {
                                VReg dst = emit_unary_op(ctx, MOpc::NEG64r, op);
                                ctx.set_vreg(inst, dst);
                            }
                        }
                    }
                    break;
                }

                case IrNodeKind::Not: {
                    auto* n = ir_cast<IrNotInst>(inst);
                    if (n)
                    {
                        VReg op = ctx.try_materialize(n->operand);
                        if (op.is_valid())
                        {
                            VReg all_ones = ctx.mfunc.new_vreg();
                            emit_mov_ri(ctx, all_ones, -1, 64);
                            VReg dst = emit_binary_op(ctx, MOpc::XOR64rr, op, all_ones);
                            ctx.set_vreg(inst, dst);
                        }
                    }
                    break;
                }

                case IrNodeKind::Zext:
                case IrNodeKind::Sext:
                case IrNodeKind::Trunc:
                case IrNodeKind::FpExt:
                case IrNodeKind::FpTrunc:
                case IrNodeKind::FpToI:
                case IrNodeKind::IToFp:
                case IrNodeKind::PtrToI:
                case IrNodeKind::IToPtr:
                case IrNodeKind::Bitcast:
                case IrNodeKind::Segcast: {
                    IrValue const* operand = nullptr;

                    auto get_operand = [](IrValue const* v) -> IrValue const* {
                        switch (v->kind)
                        {
                            case IrNodeKind::Zext:
                                return static_cast<IrZextInst const*>(v)->operand;
                            case IrNodeKind::Sext:
                                return static_cast<IrSextInst const*>(v)->operand;
                            case IrNodeKind::Trunc:
                                return static_cast<IrTruncInst const*>(v)->operand;
                            case IrNodeKind::FpExt:
                                return static_cast<IrFpExtInst const*>(v)->operand;
                            case IrNodeKind::FpTrunc:
                                return static_cast<IrFpTruncInst const*>(v)->operand;
                            case IrNodeKind::FpToI:
                                return static_cast<IrFpToIInst const*>(v)->operand;
                            case IrNodeKind::IToFp:
                                return static_cast<IrIToFpInst const*>(v)->operand;
                            case IrNodeKind::PtrToI:
                                return static_cast<IrPtrToIInst const*>(v)->operand;
                            case IrNodeKind::IToPtr:
                                return static_cast<IrIToPtrInst const*>(v)->operand;
                            case IrNodeKind::Bitcast:
                                return static_cast<IrBitcastInst const*>(v)->operand;
                            case IrNodeKind::Segcast:
                                return static_cast<IrSegcastInst const*>(v)->operand;
                            default:
                                return nullptr;
                        }
                    };

                    operand = get_operand(inst);
                    VReg op_vreg = ctx.try_materialize(operand);
                    if (!op_vreg.is_valid())
                        break;

                    switch (inst->kind)
                    {
                        case IrNodeKind::Zext: {
                            bool is_bool = operand && operand->type && operand->type->kind == IrTypeKind::Bool;
                            if (is_bool)
                            {
                                emit_test(ctx, op_vreg, op_vreg);
                                VReg setcc = emit_setcc(ctx, MOpc::SETNEr);
                                ctx.set_vreg(inst, setcc);
                            }
                            else
                            {
                                VReg dst = emit_unary_op(ctx, MOpc::MOV64rr, op_vreg);
                                ctx.set_vreg(inst, dst);
                            }
                            break;
                        }
                        case IrNodeKind::Sext: {
                            VReg dst = emit_unary_op(ctx, MOpc::MOVSX64rr8, op_vreg);
                            ctx.set_vreg(inst, dst);
                            break;
                        }
                        case IrNodeKind::Trunc: {
                            VReg dst = emit_unary_op(ctx, MOpc::MOV64rr, op_vreg);
                            ctx.set_vreg(inst, dst);
                            break;
                        }
                        case IrNodeKind::FpExt:
                        case IrNodeKind::FpTrunc:
                        case IrNodeKind::FpToI: {
                            auto src_bits = (operand && operand->type && operand->type->kind == IrTypeKind::Float)
                                                ? static_cast<IrFloatType const*>(operand->type)->bits
                                                : 64u;

                            MOpc conv = MOpc::CVTSD2SI64rr;
                            if (inst->kind == IrNodeKind::FpExt)
                                conv = MOpc::CVTSS2SD_r;
                            else if (inst->kind == IrNodeKind::FpTrunc)
                                conv = MOpc::CVTSD2SS_r;
                            else if (src_bits == 32)
                                conv = MOpc::CVTSS2SI64rr;

                            VReg dst = ctx.mfunc.new_vreg();
                            MInstr mi;
                            mi.opc = conv;
                            mi.num_ops = 2;
                            mi.num_defs = 1;
                            mi.ops[0] = MOp::from_reg(dst);
                            mi.ops[1] = MOp::from_reg(op_vreg);
                            ctx.append_instr(mi);
                            ctx.set_vreg(inst, dst);
                            break;
                        }
                        case IrNodeKind::IToFp: {
                            auto dst_bits = (inst->type && inst->type->kind == IrTypeKind::Float) ? static_cast<IrFloatType const*>(inst->type)->bits : 64u;
                            VReg dst = ctx.mfunc.new_vreg();
                            MInstr mi;
                            mi.opc = (dst_bits == 32) ? MOpc::CVTSI2SSrr : MOpc::CVTSI2SDrr;
                            mi.num_ops = 2;
                            mi.num_defs = 1;
                            mi.ops[0] = MOp::from_reg(dst);
                            mi.ops[1] = MOp::from_reg(op_vreg);
                            ctx.append_instr(mi);
                            ctx.set_vreg(inst, dst);
                            break;
                        }
                        case IrNodeKind::PtrToI:
                        case IrNodeKind::IToPtr:
                        case IrNodeKind::Bitcast:
                        case IrNodeKind::Segcast: {
                            VReg dst = emit_unary_op(ctx, MOpc::MOV64rr, op_vreg);
                            ctx.set_vreg(inst, dst);
                            break;
                        }
                        default:
                            break;
                    }
                    break;
                }

                case IrNodeKind::Aggregate: {
                    auto* agg = ir_cast<IrAggregateInst>(inst);
                    if (!agg)
                        break;

                    ctx.set_vreg(inst, emit_aggregate(ctx, agg));
                    break;
                }

                case IrNodeKind::Extract: {
                    auto* e = ir_cast<IrExtractInst>(inst);
                    if (!e)
                        break;

                    auto* agg_type = e->aggregate ? e->aggregate->type : nullptr;
                    VReg agg_vreg = is_memory_type(agg_type) ? memory_value_addr(ctx, e->aggregate) : ctx.try_materialize(e->aggregate);
                    if (!agg_vreg.is_valid())
                        break;

                    std::int32_t field_offset = member_offset_of(ctx, agg_type, e->field_index);

                    VReg field_addr = agg_vreg;
                    if (field_offset != 0)
                    {
                        field_addr = ctx.mfunc.new_vreg();
                        MInstr lea;
                        lea.opc = MOpc::LEA64rm;
                        lea.num_ops = 2;
                        lea.num_defs = 1;
                        lea.ops[0] = MOp::from_reg(field_addr);
                        lea.ops[1] = MOp::from_mem(MMem::make_base_disp(agg_vreg, field_offset));
                        ctx.append_instr(lea);
                    }

                    if (is_memory_type(e->type))
                    {
                        ctx.memory_addr_values.insert(inst);
                        ctx.set_vreg(inst, field_addr);
                    }
                    else
                        ctx.set_vreg(inst, emit_load(ctx, e->type, field_addr));
                    break;
                }

                case IrNodeKind::Insert: {
                    auto* ins = ir_cast<IrInsertInst>(inst);
                    if (!ins)
                        break;

                    auto* ins_agg_ty = ins->aggregate ? ins->aggregate->type : nullptr;
                    VReg agg_vreg = is_memory_type(ins_agg_ty) ? memory_value_addr(ctx, ins->aggregate) : ctx.try_materialize(ins->aggregate);
                    VReg val_vreg =
                        is_memory_type(ins->value ? ins->value->type : nullptr) ? memory_value_addr(ctx, ins->value) : ctx.try_materialize(ins->value);
                    if (!agg_vreg.is_valid() || !val_vreg.is_valid())
                        break;

                    std::int32_t field_offset = member_offset_of(ctx, ins_agg_ty, ins->field_index);
                    IrType const* field_type = nullptr;
                    if (ins_agg_ty && ins_agg_ty->kind == IrTypeKind::Aggregate)
                    {
                        auto* at = static_cast<IrAggregateType const*>(ins_agg_ty);
                        if (ins->field_index < at->members.size())
                            field_type = at->members[ins->field_index];
                    }
                    else if (ins_agg_ty && ins_agg_ty->kind == IrTypeKind::Array)
                        field_type = static_cast<IrArrayType const*>(ins_agg_ty)->element;

                    if (!field_type && ins->value)
                        field_type = ins->value->type;

                    if (is_memory_type(field_type))
                    {
                        VReg member_dst = ctx.mfunc.new_vreg();
                        MInstr lea;
                        lea.opc = MOpc::LEA64rm;
                        lea.num_ops = 2;
                        lea.num_defs = 1;
                        lea.ops[0] = MOp::from_reg(member_dst);
                        lea.ops[1] = MOp::from_mem(MMem::make_base_disp(agg_vreg, field_offset));
                        ctx.append_instr(lea);
                        emit_mem_copy(ctx, member_dst, val_vreg, field_type->byte_size);
                        ctx.set_vreg(inst, agg_vreg);
                        break;
                    }

                    MMem store_mem = MMem::make_base_disp(agg_vreg, field_offset);
                    MInstr store_instr;
                    if (field_type && ctx.is_float_type(field_type))
                    {
                        auto bits = static_cast<IrFloatType const*>(field_type)->bits;
                        store_instr.opc = (bits == 32) ? MOpc::MOVSSmr : MOpc::MOVSDmr;
                    }
                    else if (ctx.is_bool_type(field_type))
                        store_instr.opc = MOpc::MOV8mr;
                    else
                        store_instr.opc = store_opc_for_bits(field_type ? ctx.type_bits(field_type) : 64);
                    store_instr.num_ops = 2;
                    store_instr.num_defs = 0;
                    store_instr.ops[0] = MOp::from_mem(store_mem);
                    store_instr.ops[1] = MOp::from_reg(val_vreg);
                    ctx.append_instr((store_instr));

                    ctx.set_vreg(inst, agg_vreg);
                    break;
                }

                case IrNodeKind::Phi: {
                    auto* p = ir_cast<IrPhiInst>(inst);
                    if (!p)
                        break;

                    VReg result = ctx.mfunc.new_vreg();
                    auto phi = make_phi();
                    phi.num_ops = 1;
                    phi.num_defs = 1;
                    phi.ops[0] = MOp::from_reg(result);

                    for (auto const& inc : p->incoming)
                    {
                        VReg val = ctx.try_materialize(inc.value);
                        if (!val.is_valid())
                            continue;

                        if (phi.num_ops + 2 <= phi.ops.size())
                        {
                            phi.ops[static_cast<std::size_t>(phi.num_ops)] = MOp::from_reg(val);
                            phi.num_ops++;
                            auto it = ctx.ir_bb_to_mblock.find(inc.block);
                            if (it != ctx.ir_bb_to_mblock.end())
                            {
                                phi.ops[static_cast<std::size_t>(phi.num_ops)] = MOp::from_label(it->second);
                                phi.num_ops++;
                            }
                        }
                    }

                    ctx.append_instr((phi));
                    ctx.set_vreg(inst, result);
                    break;
                }

                case IrNodeKind::Call: {
                    auto* c = ir_cast<IrCallInst>(inst);
                    if (!c)
                        break;

                    auto cl = lower_call(ctx, c->callee, c->args, c->type);
                    if (cl.return_vreg.is_valid())
                        ctx.set_vreg(inst, cl.return_vreg);

                    if (cl.sret_slot_index != std::numeric_limits<std::uint32_t>::max())
                        ctx.aggregate_to_slot[inst] = cl.sret_slot_index;

                    break;
                }

                case IrNodeKind::CallTail: {
                    auto* ct = ir_cast<IrCallTailInst>(inst);
                    if (!ct)
                        break;

                    auto cl = lower_call(ctx, ct->callee, ct->args, ct->type);
                    if (cl.return_vreg.is_valid())
                        ctx.set_vreg(inst, cl.return_vreg);

                    if (cl.sret_slot_index != std::numeric_limits<std::uint32_t>::max())
                        ctx.aggregate_to_slot[inst] = cl.sret_slot_index;

                    break;
                }

                case IrNodeKind::AtomicLoad: {
                    auto* al = ir_cast<IrAtomicLoadInst>(inst);
                    if (!al)
                        break;

                    VReg addr = ctx.try_materialize(al->pointer);
                    if (addr.is_valid())
                    {
                        VReg result = emit_load(ctx, al->type, addr);
                        if (al->ordering >= IrMemoryOrdering::Acquire)
                        {
                            MInstr fence;
                            fence.opc = MOpc::MFENCE;
                            ctx.append_instr((fence));
                        }
                        ctx.set_vreg(inst, result);
                    }
                    break;
                }

                case IrNodeKind::AtomicStore: {
                    auto* as = ir_cast<IrAtomicStoreInst>(inst);
                    if (!as)
                        break;

                    VReg addr = ctx.try_materialize(as->pointer);
                    VReg val = ctx.try_materialize(as->value);
                    if (addr.is_valid() && val.is_valid())
                    {
                        MInstr xchg;
                        xchg.opc = MOpc::LOCK_XCHG;
                        xchg.num_ops = 2;
                        xchg.num_defs = 0;
                        xchg.ops[0] = MOp::from_mem(MMem::make_base_disp(addr));
                        xchg.ops[1] = MOp::from_reg(val);
                        ctx.append_instr((xchg));

                        if (as->ordering >= IrMemoryOrdering::Release)
                        {
                            MInstr fence;
                            fence.opc = MOpc::MFENCE;
                            ctx.append_instr((fence));
                        }
                    }
                    break;
                }

                case IrNodeKind::AtomicRmw: {
                    auto* ar = ir_cast<IrAtomicRmwInst>(inst);
                    if (!ar)
                        break;

                    VReg addr = ctx.try_materialize(ar->pointer);
                    VReg val = ctx.try_materialize(ar->value);
                    if (!addr.is_valid() || !val.is_valid())
                        break;

                    MOpc lock_opc = MOpc::LOCK_XADD;
                    switch (ar->op)
                    {
                        case IrAtomicRmwOp::Xchg:
                            lock_opc = MOpc::LOCK_XCHG;
                            break;
                        case IrAtomicRmwOp::Add:
                            lock_opc = MOpc::LOCK_XADD;
                            break;
                        case IrAtomicRmwOp::Sub:
                            lock_opc = MOpc::LOCK_XADD;
                            break;
                        case IrAtomicRmwOp::And:
                            lock_opc = MOpc::LOCK_AND;
                            break;
                        case IrAtomicRmwOp::Or:
                            lock_opc = MOpc::LOCK_OR;
                            break;
                        case IrAtomicRmwOp::Xor:
                            lock_opc = MOpc::LOCK_XOR;
                            break;
                    }

                    VReg result = ctx.mfunc.new_vreg();
                    emit_mov(ctx, result, val);

                    MInstr rmw;
                    rmw.opc = lock_opc;
                    rmw.num_ops = 2;
                    rmw.num_defs = 0;
                    rmw.ops[0] = MOp::from_mem(MMem::make_base_disp(addr));
                    rmw.ops[1] = MOp::from_reg(result);
                    ctx.append_instr((rmw));

                    ctx.set_vreg(inst, result);
                    break;
                }

                case IrNodeKind::Fence: {
                    auto* f = ir_cast<IrFenceInst>(inst);
                    if (!f)
                        break;

                    MInstr fence;
                    switch (f->ordering)
                    {
                        case IrMemoryOrdering::Acquire:
                            fence.opc = MOpc::LFENCE;
                            break;
                        case IrMemoryOrdering::Release:
                            fence.opc = MOpc::SFENCE;
                            break;
                        default:
                            fence.opc = MOpc::MFENCE;
                            break;
                    }
                    ctx.append_instr((fence));
                    break;
                }

                default:
                    break;
            }
        }

        void lower_terminator(IselCtx& ctx, dcc::ir::IrNode const* term)
        {
            using namespace dcc::ir;

            if (!term)
                return;

            switch (term->kind)
            {
                case IrNodeKind::Br: {
                    auto* b = static_cast<IrBrInst const*>(term);
                    auto it = ctx.ir_bb_to_mblock.find(b->target);
                    if (it != ctx.ir_bb_to_mblock.end())
                        emit_jmp(ctx, it->second);
                    break;
                }

                case IrNodeKind::BrCond: {
                    auto* bc = static_cast<IrBrCondInst const*>(term);
                    auto fused = ctx.branch_comparisons.find(bc->condition);
                    if (fused != ctx.branch_comparisons.end())
                    {
                        auto* cmp = static_cast<IrCmpEqInst const*>(bc->condition);
                        VReg lhs = ctx.try_materialize(cmp->lhs);
                        VReg rhs = ctx.try_materialize(cmp->rhs);
                        emit_cmp(ctx, lhs, rhs);
                        emit_jcc(ctx, fused->second, ctx.ir_bb_to_mblock.at(bc->true_target));
                        emit_jmp(ctx, ctx.ir_bb_to_mblock.at(bc->false_target));
                        break;
                    }
                    VReg cond = ctx.try_materialize(bc->condition);
                    auto true_it = ctx.ir_bb_to_mblock.find(bc->true_target);
                    auto false_it = ctx.ir_bb_to_mblock.find(bc->false_target);

                    if (!cond.is_valid() || true_it == ctx.ir_bb_to_mblock.end() || false_it == ctx.ir_bb_to_mblock.end())
                        break;

                    emit_test(ctx, cond, cond);
                    emit_jcc(ctx, MOpc::JNE, true_it->second);
                    emit_jmp(ctx, false_it->second);
                    break;
                }

                case IrNodeKind::Ret: {
                    auto* r = static_cast<IrRetInst const*>(term);

                    if (r->value)
                    {
                        auto* ret_type = r->value->type;

                        if (is_memory_type(ret_type))
                        {
                            ReturnPlan rp = make_return_plan(ctx, ret_type, ctx.cc);

                            if (rp.uses_sret())
                            {
                                VReg src = memory_value_addr(ctx, r->value);
                                if (!src.is_valid())
                                    src = padded_copy_addr(ctx, ret_type, ctx.try_materialize(r->value));

                                VReg sret_ptr = ctx.uses_sret ? ctx.sret_ptr_vreg : ctx.mfunc.new_vreg();
                                if (!ctx.uses_sret)
                                    emit_mov(ctx, sret_ptr, VReg::phys(PhysReg::RDI));

                                emit_mem_copy(ctx, sret_ptr, src, ret_type->byte_size);
                                emit_mov(ctx, VReg::phys(PhysReg::RAX), sret_ptr);
                            }
                            else
                            {
                                VReg src = memory_value_addr(ctx, r->value);
                                if (!src.is_valid())
                                {
                                    VReg val = ctx.try_materialize(r->value);
                                    if (val.is_valid())
                                        src = padded_copy_addr(ctx, ret_type, val);
                                }
                                if (!src.is_valid())
                                    break;

                                src = padded_copy_addr(ctx, ret_type, src);

                                emit_mov(ctx, VReg::phys(PhysReg::R11), src);

                                VReg piece_temps[2];
                                for (int pi = 0; pi < 2; ++pi)
                                {
                                    if (rp.classes[pi] == ArgClass::NoClass)
                                        continue;

                                    std::int32_t piece_off = static_cast<std::int32_t>(pi) * 8;
                                    std::uint64_t piece_size = std::min<std::uint64_t>(8, ret_type->byte_size - static_cast<std::uint64_t>(pi) * 8);

                                    VReg tmp = ctx.mfunc.new_vreg();
                                    MInstr ld;
                                    if (rp.classes[pi] == ArgClass::Sse)
                                    {
                                        if (piece_size >= 8)
                                            ld.opc = MOpc::MOVSDrm;
                                        else
                                            ld.opc = MOpc::MOVSSrm;
                                    }
                                    else
                                    {
                                        ld.opc = (piece_size <= 4) ? MOpc::MOV32rm : MOpc::MOV64rm;
                                    }
                                    ld.num_ops = 2;
                                    ld.num_defs = 1;
                                    ld.ops[0] = MOp::from_reg(tmp);
                                    ld.ops[1] = MOp::from_mem(MMem::make_base_disp(VReg::phys(PhysReg::R11), piece_off));
                                    ctx.append_instr(ld);
                                    piece_temps[pi] = tmp;
                                }

                                unsigned int_out_idx = 0;
                                unsigned sse_out_idx = 0;
                                for (int pi = 0; pi < 2; ++pi)
                                {
                                    if (rp.classes[pi] == ArgClass::NoClass)
                                        continue;

                                    VReg tmp = piece_temps[pi];
                                    if (!tmp.is_valid())
                                        continue;

                                    if (rp.classes[pi] == ArgClass::Integer)
                                    {
                                        PhysReg preg = (int_out_idx == 0) ? PhysReg::RAX : PhysReg::RDX;
                                        ++int_out_idx;
                                        emit_mov(ctx, VReg::phys(preg), tmp);
                                    }
                                    else
                                    {
                                        PhysReg preg = (sse_out_idx == 0) ? PhysReg::XMM0 : PhysReg::XMM1;
                                        ++sse_out_idx;
                                        MInstr mov;
                                        mov.opc = MOpc::MOVSDrr;
                                        mov.num_ops = 2;
                                        mov.num_defs = 1;
                                        mov.ops[0] = MOp::from_reg(VReg::phys(preg));
                                        mov.ops[1] = MOp::from_reg(tmp);
                                        ctx.append_instr(mov);
                                    }
                                }
                            }
                        }
                        else
                        {
                            VReg ret_vreg = ctx.try_materialize(r->value);
                            if (ret_vreg.is_valid())
                            {
                                bool is_float = ctx.is_float_type(r->value->type);
                                VReg ret_phys = VReg::phys(is_float ? PhysReg::XMM0 : PhysReg::RAX);
                                emit_mov(ctx, ret_phys, ret_vreg);
                            }
                        }
                    }

                    MInstr ret;
                    ret.opc = MOpc::RET;
                    ret.num_ops = 0;
                    ret.num_defs = 0;
                    std::uint64_t ret_uses = (1ULL << static_cast<std::uint8_t>(PhysReg::RAX)) | (1ULL << static_cast<std::uint8_t>(PhysReg::XMM0)) |
                                             (1ULL << static_cast<std::uint8_t>(PhysReg::RSP)) | (1ULL << static_cast<std::uint8_t>(PhysReg::RDX)) |
                                             (1ULL << static_cast<std::uint8_t>(PhysReg::XMM1));

                    ret.implicit_uses = ret_uses;
                    ctx.append_instr((ret));
                    break;
                }

                case IrNodeKind::Unreachable: {
                    MInstr ud2;
                    ud2.opc = MOpc::UD2;
                    ctx.append_instr((ud2));
                    break;
                }

                case IrNodeKind::Switch: {
                    auto* sw = static_cast<IrSwitchInst const*>(term);
                    VReg val = ctx.try_materialize(sw->value);

                    if (!val.is_valid())
                        break;

                    auto def_it = ctx.ir_bb_to_mblock.find(sw->default_target);
                    if (def_it == ctx.ir_bb_to_mblock.end())
                        break;

                    bool const is_int_switch = sw->value && sw->value->type && sw->value->type->kind == ir::IrTypeKind::Int;
                    bool const is_bool_switch = sw->value && sw->value->type && sw->value->type->kind == ir::IrTypeKind::Bool;

                    std::int64_t cmp_min = std::numeric_limits<std::int64_t>::max();
                    std::int64_t cmp_max = std::numeric_limits<std::int64_t>::min();
                    std::uint64_t expanded_count = 0;
                    bool compute_ok = is_int_switch && !is_bool_switch;
                    if (compute_ok)
                    {
                        for (auto const& c : sw->cases)
                        {
                            if (c.start > c.end)
                            {
                                compute_ok = false;
                                break;
                            }
                            std::uint64_t c_count = static_cast<std::uint64_t>(c.end) - static_cast<std::uint64_t>(c.start) + 1;
                            if (c_count > std::numeric_limits<std::uint64_t>::max() - expanded_count)
                            {
                                compute_ok = false;
                                break;
                            }
                            expanded_count += c_count;
                            cmp_min = std::min(c.start, cmp_min);
                            cmp_max = std::max(c.end, cmp_max);
                        }
                    }

                    bool use_jump_table = false;
                    std::int64_t jt_span = 0;
                    if (compute_ok && expanded_count >= 6)
                    {
                        std::uint64_t const u_span = static_cast<std::uint64_t>(cmp_max) - static_cast<std::uint64_t>(cmp_min) + 1;
                        jt_span = static_cast<std::int64_t>(u_span);

                        std::uint64_t const max_allowed_span =
                            (expanded_count <= std::numeric_limits<std::uint64_t>::max() / 4) ? expanded_count * 4 : std::numeric_limits<std::uint64_t>::max();

                        if (jt_span > 0 && u_span <= max_allowed_span)
                            use_jump_table = true;
                    }

                    if (use_jump_table)
                    {
                        std::uint32_t jt_id = ctx.mfunc.next_jump_table_id++;
                        std::string func_name = ctx.mfunc.owned_name.empty() ? "anon" : ctx.mfunc.owned_name;

                        std::string sanitized;
                        for (char ch : func_name)
                            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')
                                sanitized += ch;
                            else
                                sanitized += '_';

                        std::string sym_name = std::format(".Ljt_{}_{}", sanitized, jt_id);

                        VReg index_vreg = ctx.mfunc.new_vreg();
                        emit_mov(ctx, index_vreg, val);

                        if (cmp_min != 0)
                        {
                            MInstr sub;
                            sub.opc = MOpc::SUB64ri32;
                            sub.num_ops = 2;
                            sub.num_defs = 1;
                            sub.ops[0] = MOp::from_reg(index_vreg);
                            sub.ops[1] = MOp::from_imm(cmp_min);

                            ctx.append_instr(sub);
                        }

                        std::int64_t span_minus_one = jt_span - 1;
                        if (span_minus_one > 0)
                        {
                            VReg cmp_reg = ctx.mfunc.new_vreg();
                            emit_mov_ri(ctx, cmp_reg, span_minus_one, 64);
                            emit_cmp(ctx, index_vreg, cmp_reg);
                            emit_jcc(ctx, MOpc::JA, def_it->second);
                        }
                        else if (span_minus_one == 0)
                        {
                            emit_test(ctx, index_vreg, index_vreg);
                            emit_jcc(ctx, MOpc::JNE, def_it->second);
                        }

                        std::vector<std::uint32_t> targets(static_cast<std::size_t>(jt_span), def_it->second);
                        for (auto const& c : sw->cases)
                        {
                            auto case_it = ctx.ir_bb_to_mblock.find(c.target);
                            if (case_it == ctx.ir_bb_to_mblock.end())
                                continue;

                            for (std::int64_t v = c.start; v <= c.end; ++v)
                            {
                                std::size_t idx = static_cast<std::size_t>(v - cmp_min);
                                if (idx < targets.size())
                                    targets[idx] = case_it->second;
                            }
                        }

                        MJumpTable jt;
                        jt.id = jt_id;
                        jt.min_value = cmp_min;
                        jt.max_value = cmp_max;
                        jt.targets = targets;
                        jt.default_target = def_it->second;
                        jt.symbol = sym_name;
                        ctx.mfunc.jump_tables.push_back(std::move(jt));

                        auto& jt_ref = ctx.mfunc.jump_tables.back();

                        MInstr jti;
                        jti.opc = MOpc::JUMP_TABLE;
                        jti.num_ops = 2;
                        jti.num_defs = 0;
                        jti.ops[0] = MOp::from_reg(index_vreg);
                        jti.ops[1] = MOp::from_symbol(jt_ref.symbol);
                        jti.implicit_defs = (1ULL << static_cast<std::uint8_t>(PhysReg::R11)) | (1ULL << static_cast<std::uint8_t>(PhysReg::R10)) |
                                            (1ULL << static_cast<std::uint8_t>(PhysReg::RAX));
                        ctx.append_instr(jti);

                        std::unordered_set<std::uint32_t> seen_succs;
                        for (auto t : targets)
                            seen_succs.insert(t);
                        seen_succs.insert(def_it->second);

                        auto* cur_blk = ctx.mfunc.block_by_id(ctx.current_block_id);
                        if (cur_blk)
                            for (auto s : seen_succs)
                                cur_blk->succs.push_back(s);
                    }
                    else
                    {
                        for (auto const& c : sw->cases)
                        {
                            auto case_it = ctx.ir_bb_to_mblock.find(c.target);
                            if (case_it == ctx.ir_bb_to_mblock.end())
                                continue;

                            if (c.start == c.end)
                            {
                                VReg cmp_val = ctx.mfunc.new_vreg();
                                emit_mov_ri(ctx, cmp_val, c.start, 64);
                                emit_cmp(ctx, val, cmp_val);
                                emit_jcc(ctx, MOpc::JE, case_it->second);
                            }
                            else
                            {
                                std::int64_t low = c.start;
                                std::uint64_t u_range = static_cast<std::uint64_t>(c.end) - static_cast<std::uint64_t>(c.start);
                                std::int64_t range = static_cast<std::int64_t>(u_range);

                                VReg sub_result = ctx.mfunc.new_vreg();
                                MInstr sub;
                                sub.opc = MOpc::SUB64rr;
                                sub.num_ops = 3;
                                sub.num_defs = 1;
                                sub.ops[0] = MOp::from_reg(sub_result);
                                sub.ops[1] = MOp::from_reg(val);
                                sub.ops[2] = MOp::from_imm(low);
                                ctx.append_instr(sub);

                                VReg range_reg = ctx.mfunc.new_vreg();
                                emit_mov_ri(ctx, range_reg, range, 64);
                                emit_cmp(ctx, sub_result, range_reg);
                                emit_jcc(ctx, MOpc::JBE, case_it->second);
                            }
                        }

                        emit_jmp(ctx, def_it->second);
                    }
                    break;
                }

                default:
                    break;
            }
        }

    } // anonymous namespace

    [[nodiscard]] MFunction isel_function(dcc::ir::IrFunction const& func, dcc::target::TargetConfig const& target)
    {
        using namespace dcc::ir;

        MFunction mfunc;
        mfunc.owned_name = func.name.empty() ? "<unnamed>" : std::string{func.name};
        mfunc.src_line = static_cast<std::int32_t>(func.decl_line);

        IselCtx ctx(mfunc, target);
        auto use_def = analysis::UseDef::build(func);
        std::unordered_map<IrValue const*, std::size_t> terminator_uses;
        for (auto* block : func.blocks)
        {
            if (!block || !block->terminator)
                continue;
            auto* term = block->terminator;
            switch (term->kind)
            {
                case IrNodeKind::BrCond: ++terminator_uses[static_cast<IrBrCondInst const*>(term)->condition]; break;
                case IrNodeKind::Ret: ++terminator_uses[static_cast<IrRetInst const*>(term)->value]; break;
                case IrNodeKind::Switch: ++terminator_uses[static_cast<IrSwitchInst const*>(term)->value]; break;
                default: break;
            }
        }
        for (auto* block : func.blocks)
        {
            if (!block || !block->terminator || block->terminator->kind != IrNodeKind::BrCond)
                continue;
            auto* branch = static_cast<IrBrCondInst const*>(block->terminator);
            auto opc = branch_comparison(branch->condition);
            if (opc && use_def.use_count(branch->condition) + terminator_uses[branch->condition] == 1 &&
                std::find(block->instructions.begin(), block->instructions.end(), branch->condition) != block->instructions.end())
                ctx.branch_comparisons.emplace(branch->condition, *opc);
        }

        for (auto* ir_bb : func.blocks)
        {
            if (!ir_bb)
                continue;

            auto& mbb = mfunc.create_block(ir_bb->has_name() ? ir_bb->name : std::string{});
            ctx.ir_bb_to_mblock[ir_bb] = mbb.id;

            if (ir_bb == func.entry_block)
                mfunc.entry_block_id = mbb.id;
        }

        if (mfunc.blocks.empty())
        {
            auto& mbb = mfunc.create_block("entry");
            mfunc.entry_block_id = mbb.id;
        }

        if (func.entry_block)
        {
            bool has_sret = false;
            if (func.func_type)
            {
                auto* ret_type = func.func_type->return_type;

                if (returns_via_sret(ctx, ret_type, ctx.cc))
                {
                    has_sret = true;
                    ctx.uses_sret = true;
                    ctx.sret_ptr_vreg = mfunc.new_vreg();

                    {
                        MInstr mi;
                        mi.opc = MOpc::MOV64rr;
                        mi.num_ops = 2;
                        mi.num_defs = 1;
                        mi.ops[0] = MOp::from_reg(ctx.sret_ptr_vreg);
                        mi.ops[1] = MOp::from_reg(VReg::phys(PhysReg::RDI));
                        ctx.append_instr(mi);
                    }
                }
            }

            std::int32_t in_stack_bytes = 0;
            auto param_locs = classify_args(ctx, func.entry_block->params, has_sret, in_stack_bytes);

            using PhysRegSpan = std::span<PhysReg const>;
            auto const& in_int_regs = (ctx.cc == CallConvKind::SysV) ? PhysRegSpan{kSysVIntArgRegs} : PhysRegSpan{kWin64IntArgRegs};
            auto const& in_float_regs = (ctx.cc == CallConvKind::SysV) ? PhysRegSpan{kSysVFloatArgRegs} : PhysRegSpan{kWin64FloatArgRegs};

            std::vector<std::array<VReg, 2>> param_piece_regs(func.entry_block->params.size());

            for (std::size_t param_idx = 0; param_idx < func.entry_block->params.size(); ++param_idx)
            {
                auto* param = func.entry_block->params[param_idx];
                if (!param)
                    continue;

                auto const& loc = param_locs[param_idx];

                for (std::uint8_t pi = 0; pi < loc.num_pieces; ++pi)
                {
                    auto const& piece = loc.pieces[pi];
                    switch (piece.cls)
                    {
                        case ArgClass::Sse: {
                            VReg v = mfunc.new_vreg();
                            MInstr mi;
                            mi.opc = MOpc::MOVSDrr;
                            mi.num_ops = 2;
                            mi.num_defs = 1;
                            mi.ops[0] = MOp::from_reg(v);
                            mi.ops[1] = MOp::from_reg(VReg::phys(in_float_regs[piece.reg_idx]));
                            ctx.append_instr(mi);
                            param_piece_regs[param_idx][pi] = v;
                            break;
                        }
                        case ArgClass::Integer: {
                            VReg v = mfunc.new_vreg();
                            emit_mov(ctx, v, VReg::phys(in_int_regs[piece.reg_idx]));
                            param_piece_regs[param_idx][pi] = v;
                            break;
                        }
                        case ArgClass::NoClass:
                        case ArgClass::Memory:
                            break;
                    }
                }
            }

            std::int32_t incoming_base = (ctx.cc == CallConvKind::SysV) ? 16 : 16;

            for (std::size_t param_idx = 0; param_idx < func.entry_block->params.size(); ++param_idx)
            {
                auto* param = func.entry_block->params[param_idx];
                if (!param)
                    continue;

                auto const& loc = param_locs[param_idx];
                auto* param_ty = param->type;

                if (loc.on_stack)
                {
                    VReg rbp = VReg::phys(PhysReg::RBP);
                    std::int32_t disp = incoming_base + loc.pieces[0].stack_off;

                    if (is_memory_type(param_ty) && !loc.by_reference)
                    {
                        VReg addr = mfunc.new_vreg();
                        MInstr lea;
                        lea.opc = MOpc::LEA64rm;
                        lea.num_ops = 2;
                        lea.num_defs = 1;
                        lea.ops[0] = MOp::from_reg(addr);
                        lea.ops[1] = MOp::from_mem(MMem::make_base_disp(rbp, disp));
                        ctx.append_instr(lea);
                        ctx.memory_addr_values.insert(param);
                        ctx.set_vreg(param, addr);
                        continue;
                    }

                    VReg v = mfunc.new_vreg();
                    MInstr ld;
                    ld.num_ops = 2;
                    ld.num_defs = 1;
                    ld.ops[0] = MOp::from_reg(v);
                    ld.ops[1] = MOp::from_mem(MMem::make_base_disp(rbp, disp));
                    ld.opc = (!loc.by_reference && ctx.is_float_type(param_ty)) ? MOpc::MOVSDrm : MOpc::MOV64rm;
                    ctx.append_instr(ld);

                    if (loc.by_reference)
                        ctx.memory_addr_values.insert(param);

                    ctx.set_vreg(param, v);
                    continue;
                }

                if (loc.by_reference)
                {
                    ctx.memory_addr_values.insert(param);
                    if (param_piece_regs[param_idx][0].is_valid())
                        ctx.set_vreg(param, param_piece_regs[param_idx][0]);
                    continue;
                }

                if (is_memory_type(param_ty) && loc.num_pieces > 0)
                {
                    std::uint32_t slot_align = param_ty ? static_cast<std::uint32_t>(std::min<std::uint64_t>(param_ty->byte_align, 16)) : 8u;
                    std::uint32_t slot_size = param_ty ? static_cast<std::uint32_t>(align_up_i32(static_cast<std::int32_t>(param_ty->byte_size), 8)) : 8u;
                    auto slot = mfunc.new_frame_slot(std::max(slot_size, 8u), std::max(slot_align, 8u));
                    VReg base = emit_slot_addr(ctx, slot);

                    for (std::uint8_t pi = 0; pi < loc.num_pieces; ++pi)
                    {
                        VReg pv = param_piece_regs[param_idx][pi];
                        if (!pv.is_valid())
                            continue;

                        auto const& piece = loc.pieces[pi];
                        MInstr st;
                        if (piece.cls == ArgClass::Sse)
                            st.opc = MOpc::MOVSDmr;
                        else
                            st.opc = MOpc::MOV64mr;
                        st.num_ops = 2;
                        st.num_defs = 0;
                        st.ops[0] = MOp::from_mem(MMem::make_base_disp(base, static_cast<std::int32_t>(pi) * 8));
                        st.ops[1] = MOp::from_reg(pv);
                        ctx.append_instr(st);
                    }

                    ctx.aggregate_to_slot[param] = slot;
                    ctx.memory_addr_values.insert(param);
                    ctx.set_vreg(param, base);
                    continue;
                }

                if (param_piece_regs[param_idx][0].is_valid())
                    ctx.set_vreg(param, param_piece_regs[param_idx][0]);
            }
        }

        for (auto* ir_bb : func.blocks)
        {
            if (!ir_bb)
                continue;

            auto mbb_it = ctx.ir_bb_to_mblock.find(ir_bb);
            if (mbb_it == ctx.ir_bb_to_mblock.end())
                continue;

            ctx.current_block_id = mbb_it->second;

            for (auto* inst : ir_bb->instructions)
            {
                if (!inst)
                    continue;
                if (!ctx.branch_comparisons.contains(inst))
                    lower_instruction(ctx, inst);
            }

            lower_terminator(ctx, ir_bb->terminator);
        }

        for (auto& mbb : mfunc.blocks)
            for (auto const& mi : mbb.instrs)
                if (mi.opc == MOpc::JMP || mi.opc == MOpc::JE || mi.opc == MOpc::JNE || mi.opc == MOpc::JB || mi.opc == MOpc::JAE || mi.opc == MOpc::JBE ||
                    mi.opc == MOpc::JA || mi.opc == MOpc::JL || mi.opc == MOpc::JGE || mi.opc == MOpc::JLE || mi.opc == MOpc::JG || mi.opc == MOpc::JS ||
                    mi.opc == MOpc::JNS || mi.opc == MOpc::JP || mi.opc == MOpc::JNP)
                {
                    for (std::uint8_t i = 0; i < mi.num_ops; ++i)
                    {
                        if (mi.ops[i].is_label())
                            mbb.succs.push_back(mi.ops[i].label);
                    }
                }

        for (auto& mbb : mfunc.blocks)
            for (auto succ_id : mbb.succs)
            {
                auto* succ = mfunc.block_by_id(succ_id);
                if (succ)
                    succ->preds.push_back(mbb.id);
            }

        fold_addresses(mfunc);
        return mfunc;
    }

} // namespace dcc::backend::em64t
