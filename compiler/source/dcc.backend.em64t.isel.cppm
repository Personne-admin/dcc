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

        struct IselCtx
        {
            MFunction& mfunc;
            dcc::target::TargetConfig const& target;
            CallConvKind cc;
            std::unordered_map<dcc::ir::IrBasicBlock const*, std::uint32_t> ir_bb_to_mblock;
            std::unordered_map<dcc::ir::IrValue const*, VReg> value_map;
            std::unordered_map<dcc::ir::IrValue const*, std::uint32_t> alloca_to_slot;
            std::unordered_map<dcc::ir::IrValue const*, std::uint32_t> aggregate_to_slot;
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
                        if (target.position_independent_code)
                        {
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(v);
                            mov.ops[1] = MOp::from_mem(MMem::make_got_reloc(gr->name));
                            append_instr(mov);
                        }
                        else
                        {
                            MInstr lea;
                            lea.opc = MOpc::LEA64rm;
                            lea.num_ops = 2;
                            lea.num_defs = 1;
                            lea.ops[0] = MOp::from_reg(v);
                            lea.ops[1] = MOp::from_mem(MMem::make_sym_reloc(gr->name));
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
            {
                auto bits = ctx.type_bits(store_type);
                if (bits <= 32)
                    mi.opc = MOpc::MOV32mr;
                else
                    mi.opc = MOpc::MOV64mr;
            }

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
                xr.num_ops = 2;
                xr.num_defs = 1;
                xr.ops[0] = MOp::from_reg(rdx);
                xr.ops[1] = MOp::from_reg(rdx);
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

        [[nodiscard]] bool aggregate_returns_in_xmm(dcc::ir::IrAggregateType const* agg, CallConvKind cc) noexcept
        {
            if (cc != CallConvKind::SysV)
                return false;
            if (!agg || agg->byte_size > 16 || agg->byte_size == 0 || agg->byte_size % 8 != 0)
                return false;
            for (auto* m : agg->members)
            {
                if (!m || m->kind != dcc::ir::IrTypeKind::Float)
                    return false;
                auto* ft = static_cast<dcc::ir::IrFloatType const*>(m);
                if (ft->bits != 64)
                    return false;
            }
            return true;
        }

        struct CallLowering
        {
            std::vector<VReg> arg_vregs;
            VReg return_vreg;
            bool has_sret = false;
            bool use_xmm_ret = false;
            std::uint32_t sret_slot_index = std::numeric_limits<std::uint32_t>::max();
        };

        [[nodiscard]] CallLowering lower_call(IselCtx& ctx, dcc::ir::IrValue const* callee, std::span<dcc::ir::IrValue* const> args,
                                              dcc::ir::IrType const* result_type)
        {
            CallLowering cl;

            VReg sret_addr;
            if (result_type && result_type->kind == dcc::ir::IrTypeKind::Aggregate)
            {
                auto* ret_agg = static_cast<dcc::ir::IrAggregateType const*>(result_type);
                if (aggregate_returns_in_xmm(ret_agg, ctx.cc))
                {
                    cl.use_xmm_ret = true;
                    cl.sret_slot_index =
                        ctx.mfunc.new_frame_slot(static_cast<std::uint32_t>(ret_agg->byte_size), static_cast<std::uint32_t>(ret_agg->byte_align));
                }
                else if (ret_agg->byte_size > 8)
                {
                    cl.has_sret = true;
                    cl.sret_slot_index =
                        ctx.mfunc.new_frame_slot(static_cast<std::uint32_t>(ret_agg->byte_size), static_cast<std::uint32_t>(ret_agg->byte_align));

                    sret_addr = ctx.mfunc.new_vreg();
                    MInstr lea_sret;
                    lea_sret.opc = MOpc::LEA64rm;
                    lea_sret.num_ops = 2;
                    lea_sret.num_defs = 1;
                    lea_sret.ops[0] = MOp::from_reg(sret_addr);
                    lea_sret.ops[1] = MOp::from_frame_slot(cl.sret_slot_index);
                    ctx.append_instr((lea_sret));
                }
            }

            for (auto* arg : args)
            {
                VReg av = ctx.try_materialize(arg);
                cl.arg_vregs.push_back(av);
            }

            unsigned int_reg_idx = cl.has_sret ? 1 : 0;
            unsigned float_reg_idx = 0;

            if (cl.has_sret)
                emit_mov(ctx, VReg::phys(PhysReg::RDI), sret_addr);

            using PhysRegSpan = std::span<PhysReg const>;
            auto const& int_regs = (ctx.cc == CallConvKind::SysV) ? PhysRegSpan{kSysVIntArgRegs} : PhysRegSpan{kWin64IntArgRegs};
            auto const& float_regs = (ctx.cc == CallConvKind::SysV) ? PhysRegSpan{kSysVFloatArgRegs} : PhysRegSpan{kWin64FloatArgRegs};
            unsigned max_int_regs = static_cast<unsigned>(int_regs.size());
            unsigned max_float_regs = static_cast<unsigned>(float_regs.size());

            for (std::size_t i = 0; i < args.size(); ++i)
            {
                auto* arg_type = args[i] ? args[i]->type : nullptr;
                bool is_float = ctx.is_float_type(arg_type);
                VReg av = cl.arg_vregs[i];

                if (is_float && float_reg_idx < max_float_regs)
                {
                    VReg phys = VReg::phys(float_regs[float_reg_idx]);
                    MInstr mov;
                    mov.opc = MOpc::MOVSDrr;
                    mov.num_ops = 2;
                    mov.num_defs = 1;
                    mov.ops[0] = MOp::from_reg(phys);
                    mov.ops[1] = MOp::from_reg(av);
                    ctx.append_instr(mov);
                    float_reg_idx++;
                }
                else if (!is_float && int_reg_idx < max_int_regs)
                {
                    VReg phys = VReg::phys(int_regs[int_reg_idx]);
                    emit_mov(ctx, phys, av);
                    int_reg_idx++;
                }
                else
                {
                    MInstr push;
                    push.opc = MOpc::PUSH64r;
                    push.num_ops = 1;
                    push.num_defs = 0;
                    push.ops[0] = MOp::from_reg(av);
                    ctx.add_implicit_defs(push, std::array{PhysReg::RSP});
                    ctx.append_instr(push);
                }
            }

            MInstr call_instr;

            if (auto* gr = dcc::ir::ir_cast<dcc::ir::IrGlobalRef>(callee))
            {
                call_instr.opc = MOpc::CALL_rel32;
                call_instr.num_ops = 1;
                call_instr.num_defs = 0;
                call_instr.ops[0] = MOp::from_symbol(gr->name);
            }
            else
            {
                VReg callee_vreg = ctx.try_materialize(callee);
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
                else if (cl.use_xmm_ret)
                {
                    VReg slot_addr = ctx.mfunc.new_vreg();
                    MInstr lea;
                    lea.opc = MOpc::LEA64rm;
                    lea.num_ops = 2;
                    lea.num_defs = 1;
                    lea.ops[0] = MOp::from_reg(slot_addr);
                    lea.ops[1] = MOp::from_frame_slot(cl.sret_slot_index);
                    ctx.append_instr(lea);

                    MInstr st0;
                    st0.opc = MOpc::MOVSDmr;
                    st0.num_ops = 2;
                    st0.num_defs = 0;
                    st0.ops[0] = MOp::from_mem(MMem::make_base_disp(slot_addr));
                    st0.ops[1] = MOp::from_reg(VReg::phys(PhysReg::XMM0));
                    ctx.append_instr(st0);

                    MInstr st1;
                    st1.opc = MOpc::MOVSDmr;
                    st1.num_ops = 2;
                    st1.num_defs = 0;
                    st1.ops[0] = MOp::from_mem(MMem::make_base_disp(slot_addr, 8));
                    st1.ops[1] = MOp::from_reg(VReg::phys(PhysReg::XMM1));
                    ctx.append_instr(st1);

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
                        if (ctx.target.position_independent_code)
                        {
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(v);
                            mov.ops[1] = MOp::from_mem(MMem::make_got_reloc(gr->name));
                            ctx.append_instr(mov);
                        }
                        else
                        {
                            MInstr lea;
                            lea.opc = MOpc::LEA64rm;
                            lea.num_ops = 2;
                            lea.num_defs = 1;
                            lea.ops[0] = MOp::from_reg(v);
                            lea.ops[1] = MOp::from_mem(MMem::make_sym_reloc(gr->name));
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

                        VReg result = ctx.mfunc.new_vreg();
                        auto mi = make_implicit_def();
                        mi.ops[0] = MOp::from_reg(result);
                        ctx.append_instr(mi);
                        ctx.set_vreg(inst, result);
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

                    if (auto* gr = ir_cast<IrGlobalRef>(ptr_val))
                    {
                        if (ctx.target.position_independent_code)
                        {
                            VReg addr = ctx.mfunc.new_vreg();
                            MInstr mov;
                            mov.opc = MOpc::MOV64rm;
                            mov.num_ops = 2;
                            mov.num_defs = 1;
                            mov.ops[0] = MOp::from_reg(addr);
                            mov.ops[1] = MOp::from_mem(MMem::make_got_reloc(gr->name));
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
                            mi.ops[1] = MOp::from_mem(MMem::make_sym_reloc(gr->name));

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
                            if (ctx.target.position_independent_code)
                            {
                                VReg addr = ctx.mfunc.new_vreg();
                                MInstr mov;
                                mov.opc = MOpc::MOV64rm;
                                mov.num_ops = 2;
                                mov.num_defs = 1;
                                mov.ops[0] = MOp::from_reg(addr);
                                mov.ops[1] = MOp::from_mem(MMem::make_got_reloc(gr->name));
                                ctx.append_instr(mov);

                                auto store_type = val_val ? val_val->type : nullptr;
                                emit_store(ctx, store_type, addr, val);
                            }
                            else
                            {
                                MInstr mi;
                                mi.num_ops = 2;
                                mi.num_defs = 0;
                                mi.ops[0] = MOp::from_mem(MMem::make_sym_reloc(gr->name));
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
                                {
                                    auto bits = ctx.type_bits(store_type);
                                    if (bits <= 32)
                                        mi.opc = MOpc::MOV32mr;
                                    else
                                        mi.opc = MOpc::MOV64mr;
                                }

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

                            if (store_type && store_type->kind == dcc::ir::IrTypeKind::Aggregate)
                            {
                                auto agg_it = ctx.aggregate_to_slot.find(val_val);
                                if (agg_it != ctx.aggregate_to_slot.end())
                                {
                                    auto copy_size = store_type->byte_size;
                                    for (std::uint64_t copy_off = 0; copy_off < copy_size; copy_off += 8)
                                    {
                                        auto chunk = std::min<std::uint64_t>(copy_size - copy_off, 8);
                                        VReg tmp = ctx.mfunc.new_vreg();
                                        MInstr load_mi;
                                        load_mi.num_ops = 2;
                                        load_mi.num_defs = 1;
                                        load_mi.ops[0] = MOp::from_reg(tmp);
                                        if (copy_off == 0)
                                            load_mi.ops[1] = MOp::from_frame_slot(agg_it->second);
                                        else
                                        {
                                            VReg addr = ctx.mfunc.new_vreg();
                                            MInstr lea;
                                            lea.opc = MOpc::LEA64rm;
                                            lea.num_ops = 2;
                                            lea.num_defs = 1;
                                            lea.ops[0] = MOp::from_reg(addr);
                                            lea.ops[1] = MOp::from_frame_slot(agg_it->second);
                                            ctx.append_instr(lea);
                                            load_mi.ops[1] = MOp::from_mem(MMem::make_base_disp(addr, static_cast<std::int32_t>(copy_off)));
                                        }
                                        load_mi.opc = (chunk <= 4) ? MOpc::MOV32rm : MOpc::MOV64rm;
                                        ctx.append_instr(load_mi);

                                        MInstr store_mi;
                                        store_mi.num_ops = 2;
                                        store_mi.num_defs = 0;
                                        if (copy_off == 0)
                                            store_mi.ops[0] = MOp::from_frame_slot(alloca_it->second);
                                        else
                                        {
                                            VReg addr = ctx.mfunc.new_vreg();
                                            MInstr lea;
                                            lea.opc = MOpc::LEA64rm;
                                            lea.num_ops = 2;
                                            lea.num_defs = 1;
                                            lea.ops[0] = MOp::from_reg(addr);
                                            lea.ops[1] = MOp::from_frame_slot(alloca_it->second);
                                            ctx.append_instr(lea);
                                            store_mi.ops[0] = MOp::from_mem(MMem::make_base_disp(addr, static_cast<std::int32_t>(copy_off)));
                                        }
                                        store_mi.ops[1] = MOp::from_reg(tmp);
                                        store_mi.opc = (chunk <= 4) ? MOpc::MOV32mr : MOpc::MOV64mr;
                                        ctx.append_instr(store_mi);
                                    }
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
                                        mi.opc = MOpc::MOV64mr;
                                        ctx.append_instr(mi);
                                    }
                                }
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
                                    {
                                        auto bits = ctx.type_bits(store_type);
                                        if (bits <= 32)
                                            mi.opc = MOpc::MOV32mr;
                                        else
                                            mi.opc = MOpc::MOV64mr;
                                    }

                                    ctx.append_instr(mi);
                                }
                            }
                        }
                        else
                        {
                            VReg addr = ctx.try_materialize(ptr_val);
                            VReg val = ctx.try_materialize(val_val);
                            if (addr.is_valid() && val.is_valid())
                            {
                                auto store_type = val_val ? val_val->type : nullptr;
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
                            if (idx.dynamic_index)
                            {
                                VReg idx_vreg = ctx.try_materialize(idx.dynamic_index);
                                std::int64_t elem_size = static_cast<std::int64_t>(cur_type ? cur_type->byte_size : 8);

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

                            if (cur_type && cur_type->kind == IrTypeKind::Array)
                                cur_type = static_cast<IrArrayType const*>(cur_type)->element;
                            else
                                cur_type = nullptr;
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

                    auto set_binop = [&](auto const* bin_inst) {
                        lhs = bin_inst->lhs;
                        rhs = bin_inst->rhs;
                        is_float = ctx.is_float_type(bin_inst->type);
                    };

                    switch (inst->kind)
                    {
                        case IrNodeKind::Add:
                            set_binop(static_cast<IrAddInst const*>(inst));
                            opc = is_float ? MOpc::ADDSDrr : MOpc::ADD64rr;
                            break;
                        case IrNodeKind::Sub:
                            set_binop(static_cast<IrSubInst const*>(inst));
                            opc = is_float ? MOpc::SUBSDrr : MOpc::SUB64rr;
                            break;
                        case IrNodeKind::Mul:
                            set_binop(static_cast<IrMulInst const*>(inst));
                            opc = is_float ? MOpc::MULSDrr : MOpc::IMUL64rr;
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
                            MInstr mi;
                            mi.opc = MOpc::DIVSDrr;
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
                            MInstr ucom;
                            ucom.opc = MOpc::UCOMISDrr;
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
                            VReg dst = ctx.mfunc.new_vreg();
                            MInstr mi;
                            mi.opc = MOpc::CVTSD2SI64rr;
                            mi.num_ops = 2;
                            mi.num_defs = 1;
                            mi.ops[0] = MOp::from_reg(dst);
                            mi.ops[1] = MOp::from_reg(op_vreg);
                            ctx.append_instr(mi);
                            ctx.set_vreg(inst, dst);
                            break;
                        }
                        case IrNodeKind::IToFp: {
                            VReg dst = ctx.mfunc.new_vreg();
                            MInstr mi;
                            mi.opc = MOpc::CVTSI2SDrr;
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

                    std::uint64_t agg_size = agg->type ? agg->type->byte_size : 0;
                    std::uint32_t agg_align = static_cast<std::uint32_t>(agg->type ? agg->type->byte_align : 1);
                    std::uint32_t slot_idx = ctx.mfunc.new_frame_slot(static_cast<std::uint32_t>(agg_size), agg_align);
                    ctx.aggregate_to_slot[inst] = slot_idx;

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

                    IrType const* agg_type = agg->type;
                    for (std::size_t i = 0; i < agg->values.size(); ++i)
                    {
                        VReg val = ctx.try_materialize(agg->values[i]);
                        if (!val.is_valid())
                            continue;

                        std::int32_t member_offset = 0;
                        IrType const* field_type = nullptr;
                        if (agg_type && agg_type->kind == IrTypeKind::Aggregate)
                        {
                            auto* at = static_cast<IrAggregateType const*>(agg_type);
                            if (i < at->member_offsets.size())
                                member_offset = static_cast<std::int32_t>(at->member_offsets[i]);
                            if (i < at->members.size())
                                field_type = at->members[i];
                        }
                        else
                            member_offset = static_cast<std::int32_t>(i * 8);

                        if (!field_type)
                            field_type = agg->values[i] ? agg->values[i]->type : nullptr;

                        MMem store_mem = MMem::make_base_disp(addr, member_offset);
                        MInstr store;
                        if (field_type && ctx.is_float_type(field_type))
                        {
                            auto bits = static_cast<IrFloatType const*>(field_type)->bits;
                            store.opc = (bits == 32) ? MOpc::MOVSSmr : MOpc::MOVSDmr;
                        }
                        else
                        {
                            auto bits = field_type ? ctx.type_bits(field_type) : 64;
                            if (bits <= 32)
                                store.opc = MOpc::MOV32mr;
                            else
                                store.opc = MOpc::MOV64mr;
                        }
                        store.num_ops = 2;
                        store.num_defs = 0;
                        store.ops[0] = MOp::from_mem(store_mem);
                        store.ops[1] = MOp::from_reg(val);
                        ctx.append_instr((store));
                    }

                    ctx.set_vreg(inst, addr);
                    break;
                }

                case IrNodeKind::Extract: {
                    auto* e = ir_cast<IrExtractInst>(inst);
                    if (!e)
                        break;

                    VReg agg_vreg = ctx.try_materialize(e->aggregate);
                    if (!agg_vreg.is_valid())
                        break;

                    auto* agg_type = e->aggregate ? e->aggregate->type : nullptr;
                    std::int32_t field_offset = 0;
                    if (agg_type && agg_type->kind == dcc::ir::IrTypeKind::Aggregate)
                    {
                        auto* at = static_cast<dcc::ir::IrAggregateType const*>(agg_type);
                        if (e->field_index < at->member_offsets.size())
                            field_offset = static_cast<std::int32_t>(at->member_offsets[e->field_index]);
                    }

                    if (field_offset != 0)
                    {
                        VReg addr = ctx.mfunc.new_vreg();
                        MInstr lea;
                        lea.opc = MOpc::LEA64rm;
                        lea.num_ops = 2;
                        lea.num_defs = 1;
                        lea.ops[0] = MOp::from_reg(addr);
                        lea.ops[1] = MOp::from_mem(MMem::make_base_disp(agg_vreg, field_offset));
                        ctx.append_instr(lea);
                        VReg result = emit_load(ctx, e->type, addr);
                        ctx.set_vreg(inst, result);
                    }
                    else
                    {
                        VReg result = emit_load(ctx, e->type, agg_vreg);
                        ctx.set_vreg(inst, result);
                    }
                    break;
                }

                case IrNodeKind::Insert: {
                    auto* ins = ir_cast<IrInsertInst>(inst);
                    if (!ins)
                        break;

                    VReg agg_vreg = ctx.try_materialize(ins->aggregate);
                    VReg val_vreg = ctx.try_materialize(ins->value);
                    if (!agg_vreg.is_valid() || !val_vreg.is_valid())
                        break;

                    std::int32_t field_offset = 0;
                    IrType const* field_type = nullptr;
                    if (ins->aggregate && ins->aggregate->type && ins->aggregate->type->kind == IrTypeKind::Aggregate)
                    {
                        auto* at = static_cast<IrAggregateType const*>(ins->aggregate->type);
                        if (ins->field_index < at->member_offsets.size())
                            field_offset = static_cast<std::int32_t>(at->member_offsets[ins->field_index]);
                        if (ins->field_index < at->members.size())
                            field_type = at->members[ins->field_index];
                    }

                    if (!field_type && ins->value)
                        field_type = ins->value->type;

                    MMem store_mem = MMem::make_base_disp(agg_vreg, field_offset);
                    MInstr store_instr;
                    if (field_type && ctx.is_float_type(field_type))
                    {
                        auto bits = static_cast<IrFloatType const*>(field_type)->bits;
                        store_instr.opc = (bits == 32) ? MOpc::MOVSSmr : MOpc::MOVSDmr;
                    }
                    else
                    {
                        auto bits = field_type ? ctx.type_bits(field_type) : 64;
                        if (bits <= 32)
                            store_instr.opc = MOpc::MOV32mr;
                        else
                            store_instr.opc = MOpc::MOV64mr;
                    }
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

                    if ((cl.has_sret || cl.use_xmm_ret) && cl.sret_slot_index != std::numeric_limits<std::uint32_t>::max())
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

                    if ((cl.has_sret || cl.use_xmm_ret) && cl.sret_slot_index != std::numeric_limits<std::uint32_t>::max())
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
                        if (ret_type && ret_type->kind == IrTypeKind::Aggregate)
                        {
                            auto* agg_type = static_cast<IrAggregateType const*>(ret_type);
                            auto total_size = agg_type->byte_size;

                            if (aggregate_returns_in_xmm(agg_type, ctx.cc))
                            {
                                VReg agg_addr = ctx.try_materialize(r->value);

                                {
                                    std::int32_t off0 = static_cast<std::int32_t>(agg_type->member_offsets[0]);
                                    VReg faddr = ctx.mfunc.new_vreg();
                                    MInstr lea;
                                    lea.opc = MOpc::LEA64rm;
                                    lea.num_ops = 2;
                                    lea.num_defs = 1;
                                    lea.ops[0] = MOp::from_reg(faddr);
                                    lea.ops[1] = MOp::from_mem(MMem::make_base_disp(agg_addr, off0));
                                    ctx.append_instr(lea);
                                    VReg fval = emit_load(ctx, agg_type->members[0], faddr);
                                    MInstr mov;
                                    mov.opc = MOpc::MOVSDrr;
                                    mov.num_ops = 2;
                                    mov.num_defs = 1;
                                    mov.ops[0] = MOp::from_reg(VReg::phys(PhysReg::XMM0));
                                    mov.ops[1] = MOp::from_reg(fval);
                                    ctx.append_instr(mov);
                                }

                                if (agg_type->members.size() > 1)
                                {
                                    std::int32_t off1 = static_cast<std::int32_t>(agg_type->member_offsets[1]);
                                    VReg faddr = ctx.mfunc.new_vreg();
                                    MInstr lea;
                                    lea.opc = MOpc::LEA64rm;
                                    lea.num_ops = 2;
                                    lea.num_defs = 1;
                                    lea.ops[0] = MOp::from_reg(faddr);
                                    lea.ops[1] = MOp::from_mem(MMem::make_base_disp(agg_addr, off1));
                                    ctx.append_instr(lea);
                                    VReg fval = emit_load(ctx, agg_type->members[1], faddr);
                                    MInstr mov;
                                    mov.opc = MOpc::MOVSDrr;
                                    mov.num_ops = 2;
                                    mov.num_defs = 1;
                                    mov.ops[0] = MOp::from_reg(VReg::phys(PhysReg::XMM1));
                                    mov.ops[1] = MOp::from_reg(fval);
                                    ctx.append_instr(mov);
                                }
                            }
                            else if (total_size <= 8)
                            {
                                VReg agg_addr = ctx.try_materialize(r->value);
                                VReg packed = ctx.mfunc.new_vreg();
                                bool first = true;

                                for (std::size_t fi = 0; fi < agg_type->members.size(); ++fi)
                                {
                                    auto* field_type = agg_type->members[fi];
                                    if (!field_type)
                                        continue;

                                    std::int32_t field_off = static_cast<std::int32_t>(agg_type->member_offsets[fi]);
                                    VReg field_addr = ctx.mfunc.new_vreg();
                                    MInstr lea_fa;
                                    lea_fa.opc = MOpc::LEA64rm;
                                    lea_fa.num_ops = 2;
                                    lea_fa.num_defs = 1;
                                    lea_fa.ops[0] = MOp::from_reg(field_addr);
                                    lea_fa.ops[1] = MOp::from_mem(MMem::make_base_disp(agg_addr, field_off));
                                    ctx.append_instr((lea_fa));

                                    VReg field_val = emit_load(ctx, field_type, field_addr);

                                    if (first)
                                    {
                                        emit_mov(ctx, packed, field_val);
                                        first = false;
                                    }
                                    else
                                    {
                                        auto bit_shift = agg_type->member_offsets[fi] * 8;
                                        VReg shifted = ctx.mfunc.new_vreg();
                                        MInstr shl;
                                        shl.opc = MOpc::SHL64ri;
                                        shl.num_ops = 3;
                                        shl.num_defs = 1;
                                        shl.ops[0] = MOp::from_reg(shifted);
                                        shl.ops[1] = MOp::from_reg(field_val);
                                        shl.ops[2] = MOp::from_imm(static_cast<std::int64_t>(bit_shift));
                                        ctx.append_instr((shl));

                                        VReg or_val = emit_binary_op(ctx, MOpc::OR64rr, packed, shifted);
                                        packed = or_val;
                                    }
                                }

                                emit_mov(ctx, VReg::phys(PhysReg::RAX), packed);
                            }
                            else
                            {
                                VReg sret_ptr = ctx.uses_sret ? ctx.sret_ptr_vreg : ctx.mfunc.new_vreg();
                                if (!ctx.uses_sret)
                                    emit_mov(ctx, sret_ptr, VReg::phys(PhysReg::RDI));

                                VReg agg_addr = ctx.try_materialize(r->value);

                                for (std::size_t fi = 0; fi < agg_type->members.size(); ++fi)
                                {
                                    auto* field_type = agg_type->members[fi];
                                    if (!field_type)
                                        continue;

                                    std::int32_t field_off = static_cast<std::int32_t>(agg_type->member_offsets[fi]);

                                    VReg field_addr = ctx.mfunc.new_vreg();
                                    MInstr lea_fa;
                                    lea_fa.opc = MOpc::LEA64rm;
                                    lea_fa.num_ops = 2;
                                    lea_fa.num_defs = 1;
                                    lea_fa.ops[0] = MOp::from_reg(field_addr);
                                    lea_fa.ops[1] = MOp::from_mem(MMem::make_base_disp(agg_addr, field_off));
                                    ctx.append_instr((lea_fa));

                                    VReg field_val = emit_load(ctx, field_type, field_addr);

                                    VReg dest_addr = ctx.mfunc.new_vreg();
                                    MInstr lea_dest;
                                    lea_dest.opc = MOpc::LEA64rm;
                                    lea_dest.num_ops = 2;
                                    lea_dest.num_defs = 1;
                                    lea_dest.ops[0] = MOp::from_reg(dest_addr);
                                    lea_dest.ops[1] = MOp::from_mem(MMem::make_base_disp(sret_ptr, field_off));
                                    ctx.append_instr((lea_dest));

                                    emit_store(ctx, field_type, dest_addr, field_val);
                                }

                                emit_mov(ctx, VReg::phys(PhysReg::RAX), sret_ptr);
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
                                             (1ULL << static_cast<std::uint8_t>(PhysReg::RSP));

                    if (r->value && r->value->type && r->value->type->kind == IrTypeKind::Aggregate)
                    {
                        auto* ret_agg_type = static_cast<IrAggregateType const*>(r->value->type);
                        if (aggregate_returns_in_xmm(ret_agg_type, ctx.cc))
                            ret_uses |= (1ULL << static_cast<std::uint8_t>(PhysReg::XMM1));
                    }

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
            if (func.func_type && func.func_type->return_type && func.func_type->return_type->kind == IrTypeKind::Aggregate)
            {
                auto* ret_agg = static_cast<IrAggregateType const*>(func.func_type->return_type);
                if (ret_agg->byte_size > 8 && !aggregate_returns_in_xmm(ret_agg, ctx.cc))
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

            for (std::size_t param_idx = 0; param_idx < func.entry_block->params.size(); ++param_idx)
            {
                auto* param = func.entry_block->params[param_idx];
                if (!param)
                    continue;

                VReg param_vreg = mfunc.new_vreg();
                ctx.set_vreg(param, param_vreg);

                VReg phys;
                bool is_float = ctx.is_float_type(param->type);
                std::size_t adj_idx = param_idx + (has_sret ? 1 : 0);
                if (ctx.cc == CallConvKind::SysV)
                {
                    if (is_float && adj_idx < 8)
                        phys = VReg::phys(kSysVFloatArgRegs[adj_idx]);
                    else if (!is_float && adj_idx < 6)
                        phys = VReg::phys(kSysVIntArgRegs[adj_idx]);
                }
                else
                {
                    if (is_float && adj_idx < 4)
                        phys = VReg::phys(kWin64FloatArgRegs[adj_idx]);
                    else if (!is_float && adj_idx < 4)
                        phys = VReg::phys(kWin64IntArgRegs[adj_idx]);
                }

                if (phys.is_valid())
                {
                    MInstr mi;
                    mi.opc = is_float ? MOpc::MOVSDrr : MOpc::MOV64rr;
                    mi.num_ops = 2;
                    mi.num_defs = 1;
                    mi.ops[0] = MOp::from_reg(param_vreg);
                    mi.ops[1] = MOp::from_reg(phys);
                    ctx.append_instr(mi);
                }
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

        return mfunc;
    }

} // namespace dcc::backend::em64t
