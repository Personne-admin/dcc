export module dcc.backend.x86.regalloc;

import std;
import dcc.ir;
import dcc.backend.x86.mir;

export namespace dcc::backend::x86
{
    struct SpillAccess
    {
        std::uint32_t size;
        std::uint32_t align;
        MOpc store;
        MOpc load;
    };

    struct RegisterClassPolicy
    {
        RegClass cls;
        std::uint8_t bits;
        std::span<PhysReg const> allocatable;
        std::span<PhysReg const> callee_saved;
        std::span<PhysReg const> scratch_order;
        SpillAccess spill;
        SpillAccess cycle_temp;
        MOpc move;
    };

    struct RegisterPolicy
    {
        RegisterClassPolicy gpr;
        RegisterClassPolicy xmm;
        PhysReg jump_table_scratch;
        MOpc gpr_save;
        MOpc gpr_restore;
        SpillAccess xmm_save;
        bool is_win64;
        std::uint64_t byte_registers{~std::uint64_t{0}};
        std::uint64_t address16_registers{~std::uint64_t{0}};
        bool narrow_gpr{false};

        [[nodiscard]] RegisterClassPolicy const& of(RegClass cls) const noexcept { return cls == RegClass::XMM ? xmm : gpr; }
        [[nodiscard]] VReg scratch(RegClass cls) const noexcept
        {
            auto const& order = of(cls).scratch_order;
            return order.empty() ? VReg{} : VReg::phys(order.front());
        }
    };

} // namespace dcc::backend::x86

namespace dcc::backend::x86
{
    namespace
    {
        [[nodiscard]] bool is_branch(MOpc opc) noexcept
        {
            switch (opc)
            {
                case MOpc::JMP:
                case MOpc::JMP_rel32:
                case MOpc::JMP_r64:
                case MOpc::JUMP_TABLE:
                case MOpc::JMPm:
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
                case MOpc::JS:
                case MOpc::JNS:
                case MOpc::JNP:
                case MOpc::RET:
                case MOpc::UD2:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool is_xmm_opc(MOpc opc) noexcept
        {
            switch (opc)
            {
                case MOpc::MOVSD_rr:
                case MOpc::MOVSD_rm:
                case MOpc::MOVSD_mr:
                case MOpc::MOVSDrr:
                case MOpc::MOVSDrm:
                case MOpc::MOVSDmr:
                case MOpc::ADDSD:
                case MOpc::SUBSD:
                case MOpc::MULSD:
                case MOpc::DIVSD:
                case MOpc::ADDSDrr:
                case MOpc::ADDSDrm:
                case MOpc::SUBSDrr:
                case MOpc::SUBSDrm:
                case MOpc::MULSDrr:
                case MOpc::MULSDrm:
                case MOpc::DIVSDrr:
                case MOpc::DIVSDrm:
                case MOpc::UCOMISD:
                case MOpc::UCOMISDrr:
                case MOpc::CVTSI2SD_r:
                case MOpc::CVTSI2SDrr:
                case MOpc::CVTSI2SD64rr:
                case MOpc::CVTSD2SS_r:
                case MOpc::CVTSS2SD_r:
                case MOpc::MOVSS_rr:
                case MOpc::MOVSS_rm:
                case MOpc::MOVSS_mr:
                case MOpc::MOVSSrr:
                case MOpc::MOVSSrm:
                case MOpc::MOVSSmr:
                case MOpc::ADDSS:
                case MOpc::SUBSS:
                case MOpc::MULSS:
                case MOpc::DIVSS:
                case MOpc::ADDSSrr:
                case MOpc::ADDSSrm:
                case MOpc::SUBSSrr:
                case MOpc::SUBSSrm:
                case MOpc::MULSSrr:
                case MOpc::MULSSrm:
                case MOpc::DIVSSrr:
                case MOpc::DIVSSrm:
                case MOpc::UCOMISS:
                case MOpc::UCOMISSrr:
                case MOpc::CVTSI2SS_r:
                case MOpc::CVTSI2SSrr:
                case MOpc::CVTSI2SS64rr:
                case MOpc::XORPSrr:
                case MOpc::XORPDrr:
                case MOpc::MOVAPSrr:
                case MOpc::MOVAPSrm:
                case MOpc::MOVAPSmr:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool is_gpr_copy(MOpc opc) noexcept
        {
            switch (opc)
            {
                case MOpc::MOV64rr:
                case MOpc::MOV32rr:
                case MOpc::MOV16rr:
                case MOpc::MOV8rr:
                case MOpc::COPY:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool is_setcc(MOpc opc) noexcept
        {
            switch (opc)
            {
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
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool is_gpr_src_xmm_opc(MOpc opc) noexcept
        {
            return opc == MOpc::CVTSI2SD_r || opc == MOpc::CVTSI2SDrr || opc == MOpc::CVTSI2SD64rr || opc == MOpc::CVTSI2SS_r || opc == MOpc::CVTSI2SSrr ||
                   opc == MOpc::CVTSI2SS64rr;
        }

        [[nodiscard]] bool is_xmm_src_gpr_opc(MOpc opc) noexcept
        {
            return opc == MOpc::CVTSD2SI64rr || opc == MOpc::CVTTSD2SI_r || opc == MOpc::CVTSD2SIrr || opc == MOpc::CVTSS2SI64rr || opc == MOpc::CVTSS2SIrr ||
                   opc == MOpc::CVTTSS2SI_r || opc == MOpc::MOVQ64rr_rev;
        }

        [[nodiscard]] std::unordered_set<VReg> infer_reg_classes(MFunction const& func)
        {
            std::unordered_set<VReg> xmm;
            for (auto const& blk : func.blocks)
            {
                for (auto const& instr : blk.instrs)
                {
                    if (is_xmm_opc(instr.opc) || is_xmm_src_gpr_opc(instr.opc))
                    {
                        if (is_gpr_src_xmm_opc(instr.opc))
                        {
                            for (std::uint8_t i = 0; i < instr.num_defs; ++i)
                                if (instr.ops[i].kind == MOpKind::Reg)
                                    xmm.insert(instr.ops[i].reg);
                        }
                        else if (is_xmm_src_gpr_opc(instr.opc))
                        {
                            for (std::uint8_t i = instr.num_defs; i < instr.num_ops; ++i)
                                if (instr.ops[i].kind == MOpKind::Reg)
                                    xmm.insert(instr.ops[i].reg);
                        }
                        else
                        {
                            for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                                if (instr.ops[i].kind == MOpKind::Reg)
                                    xmm.insert(instr.ops[i].reg);
                        }
                    }

                    if (is_gpr_copy(instr.opc))
                        for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                            if (instr.ops[i].kind == MOpKind::Reg)
                                for (std::uint8_t j = 0; j < instr.num_ops; ++j)
                                    if (i != j && instr.ops[j].kind == MOpKind::Reg && instr.ops[j].reg.is_physical())
                                    {
                                        auto pr = instr.ops[j].reg.phys_reg();
                                        if (reg_class(pr) == RegClass::XMM)
                                            xmm.insert(instr.ops[i].reg);
                                    }

                    if (instr.opc == MOpc::MOVQ64rr)
                        if (instr.num_ops >= 2 && instr.ops[0].kind == MOpKind::Reg)
                            xmm.insert(instr.ops[0].reg);

                    if (instr.opc == MOpc::MOVQ64rr_rev)
                        if (instr.num_ops >= 2 && instr.ops[1].kind == MOpKind::Reg)
                            xmm.insert(instr.ops[1].reg);

                    for (int pi = 0; pi < static_cast<int>(PhysReg::Count); ++pi)
                    {
                        if (!(instr.implicit_defs & (1ULL << pi)) && !(instr.implicit_uses & (1ULL << pi)))
                            continue;

                        auto pr = static_cast<PhysReg>(pi);
                        if (reg_class(pr) != RegClass::XMM)
                            continue;

                        if (instr.ops[0].kind == MOpKind::Reg)
                            xmm.insert(instr.ops[0].reg);
                        if (instr.num_ops > 0 && instr.ops[instr.num_ops - 1].kind == MOpKind::Reg)
                            xmm.insert(instr.ops[instr.num_ops - 1].reg);
                    }
                }
            }
            return xmm;
        }

        void eliminate_phis(MFunction& func)
        {
            for (auto& blk : func.blocks)
            {
                struct PhiEdge
                {
                    VReg dst;
                    VReg val;
                    std::uint32_t pred_id;
                };

                std::vector<PhiEdge> all_edges;
                std::vector<std::size_t> phi_indices;

                for (std::size_t i = 0; i < blk.instrs.size(); ++i)
                {
                    auto const& instr = blk.instrs[i];
                    if (instr.opc != MOpc::PHI)
                        continue;

                    if (instr.num_defs < 1 || instr.num_ops < 1)
                        continue;

                    VReg dst = instr.ops[0].reg;
                    for (std::uint8_t j = 1; j + 1 < instr.num_ops; j += 2)
                    {
                        VReg val = instr.ops[j].reg;
                        std::uint32_t pred_id = instr.ops[j + 1].label;
                        all_edges.push_back({.dst = dst, .val = val, .pred_id = pred_id});
                    }
                    phi_indices.push_back(i);
                }

                if (all_edges.empty())
                    continue;

                std::unordered_map<std::uint32_t, std::vector<std::pair<VReg, VReg>>> pred_groups;
                for (auto const& edge : all_edges)
                {
                    if (edge.dst != edge.val)
                        pred_groups[edge.pred_id].emplace_back(edge.dst, edge.val);
                }

                std::unordered_map<VReg, MInstr> merge_block_movris;
                for (auto const& mi : blk.instrs)
                {
                    if ((mi.opc == MOpc::MOV32ri || mi.opc == MOpc::MOV64ri) && mi.num_ops >= 2 && mi.ops[0].kind == MOpKind::Reg &&
                        mi.ops[0].reg.is_virtual() && mi.ops[1].kind == MOpKind::Imm64)
                    {
                        merge_block_movris[mi.ops[0].reg] = mi;
                    }
                }
                for (auto& [pred_id, copies] : pred_groups)
                {
                    if (copies.empty())
                        continue;

                    auto* pred = func.block_by_id(pred_id);
                    if (!pred || pred->instrs.empty())
                        continue;

                    std::size_t insert_pos = pred->instrs.size();
                    while (insert_pos > 0)
                        if (is_branch(pred->instrs[insert_pos - 1].opc))
                            --insert_pos;
                        else
                            break;

                    struct Move
                    {
                        VReg dst;
                        VReg src;
                    };
                    std::vector<Move> remaining;
                    remaining.reserve(copies.size());
                    for (auto const& [dst, src] : copies)
                        if (dst != src)
                            remaining.push_back({.dst = dst, .src = src});

                    if (remaining.empty())
                        continue;

                    std::vector<MInstr> emitted;
                    emitted.reserve(copies.size() * 2);

                    auto emit_move = [&](VReg dst, VReg src) {
                        auto it = merge_block_movris.find(src);
                        if (it != merge_block_movris.end())
                        {
                            MInstr new_mov = it->second;
                            new_mov.ops[0] = MOp::from_reg(dst);
                            emitted.push_back(new_mov);
                        }
                        else
                            emitted.push_back(make_copy(dst, src));
                    };

                    while (!remaining.empty())
                    {
                        bool found_ready = false;
                        for (auto it = remaining.begin(); it != remaining.end(); ++it)
                        {
                            bool dst_is_used = false;
                            for (auto const& rm : remaining)
                            {
                                if (&*it != &rm && rm.src.is_virtual() && rm.src == it->dst)
                                {
                                    dst_is_used = true;
                                    break;
                                }
                            }

                            if (!dst_is_used)
                            {
                                emit_move(it->dst, it->src);
                                remaining.erase(it);
                                found_ready = true;
                                break;
                            }
                        }
                        if (found_ready)
                            continue;

                        Move start = remaining.front();
                        VReg const start_dst = start.dst;
                        VReg const start_src = start.src;

                        VReg tmp = func.new_vreg();
                        emit_move(tmp, start_src);

                        remaining.erase(remaining.begin());

                        VReg cur = start_src;
                        while (cur != start_dst)
                        {
                            auto next_it = remaining.end();
                            for (auto rit = remaining.begin(); rit != remaining.end(); ++rit)
                            {
                                if (rit->dst == cur)
                                {
                                    next_it = rit;
                                    break;
                                }
                            }

                            if (next_it == remaining.end())
                            {
                                emit_move(start_dst, tmp);
                                break;
                            }

                            emit_move(next_it->dst, next_it->src);
                            cur = next_it->src;
                            remaining.erase(next_it);
                        }

                        if (cur == start_dst)
                            emit_move(start_dst, tmp);
                    }

                    pred->instrs.insert(pred->instrs.begin() + static_cast<std::ptrdiff_t>(insert_pos), emitted.begin(), emitted.end());
                }

                std::unordered_set<VReg> phi_source_vregs;
                for (auto const& edge : all_edges)
                    if (edge.dst != edge.val)
                        phi_source_vregs.insert(edge.val);

                std::ranges::sort(phi_indices, std::greater{});
                for (auto idx : phi_indices)
                    blk.instrs.erase(blk.instrs.begin() + static_cast<std::ptrdiff_t>(idx));

                for (std::size_t i = 0; i < blk.instrs.size();)
                {
                    auto const& mi = blk.instrs[i];
                    if ((mi.opc == MOpc::MOV32ri || mi.opc == MOpc::MOV64ri) && mi.num_ops >= 1 && mi.ops[0].kind == MOpKind::Reg &&
                        phi_source_vregs.contains(mi.ops[0].reg))
                        blk.instrs.erase(blk.instrs.begin() + static_cast<std::ptrdiff_t>(i));
                    else
                        ++i;
                }
            }
        }

        struct LiveRange
        {
            VReg vreg;
            RegClass reg_class = RegClass::GPR;
            std::uint32_t start = (std::numeric_limits<std::uint32_t>::max)();
            std::uint32_t end = 0;
            PhysReg assigned = PhysReg::None;
            std::uint32_t spill_slot = (std::numeric_limits<std::uint32_t>::max)();
            bool spilled = false;
            bool crosses_call = false;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> segments;
            double weight = 0;
            std::optional<MInstr> rematerialize;
            std::uint8_t bits = 0;
        };

        [[nodiscard]] SpillAccess spill_access(RegisterPolicy const& regs, LiveRange const& range)
        {
            if (regs.narrow_gpr && range.reg_class == RegClass::GPR && range.bits == 16)
                return {2, 2, MOpc::MOV16mr, MOpc::MOV16rm};
            return regs.of(range.reg_class).spill;
        }

        [[nodiscard]] std::unordered_map<VReg, std::uint8_t> narrow_copy_widths(MFunction& func)
        {
            std::unordered_map<VReg, std::uint8_t> widths;
            std::vector<VReg> worklist;
            auto require = [&](VReg reg, unsigned bits) {
                if (!reg.is_virtual())
                    return false;
                auto& width = widths[reg];
                auto next = static_cast<std::uint8_t>(bits > 16 ? 32 : 16);
                if (width >= next)
                    return false;
                width = next;
                worklist.push_back(reg);
                return true;
            };
            for (auto const& block : func.blocks)
                for (auto const& instr : block.instrs)
                    for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                    {
                        auto const& op = instr.ops[i];
                        if (op.kind == MOpKind::Mem)
                        {
                            require(op.mem.base, op.mem.address_bits);
                            require(op.mem.index, op.mem.address_bits);
                        }
                        else if (op.kind == MOpKind::Reg && instr.opc != MOpc::COPY)
                        {
                            unsigned bits = operand_bits(instr.opc);
                            if (i >= instr.num_defs)
                                switch (instr.opc)
                                {
                                    case MOpc::MOVZX32_16rr:
                                    case MOpc::MOVSX32_16rr:
                                    case MOpc::MOVZX32_8rr:
                                    case MOpc::MOVSX32_8rr:
                                    case MOpc::MOVZX32rr8:
                                        bits = 16;
                                        break;
                                    case MOpc::AND32ri:
                                        if (instr.num_ops >= 3 && instr.ops[2].kind == MOpKind::Imm64 && (instr.ops[2].imm & ~std::int64_t{0xffff}) == 0)
                                            bits = 16;
                                        break;
                                    case MOpc::SHL32ri8:
                                        if (instr.num_ops >= 3 && instr.ops[2].kind == MOpKind::Imm64 && instr.ops[2].imm >= 16 && instr.ops[2].imm < 32)
                                            bits = 16;
                                        break;
                                    default:
                                        break;
                                }
                            require(op.reg, bits ? bits : 32);
                        }
                    }
            std::unordered_map<VReg, std::vector<VReg>> sources;
            for (auto const& block : func.blocks)
                for (auto const& instr : block.instrs)
                    if (instr.opc == MOpc::COPY && instr.num_ops == 2 && instr.ops[0].kind == MOpKind::Reg && instr.ops[1].kind == MOpKind::Reg)
                    {
                        auto dst = instr.ops[0].reg;
                        if (dst.is_virtual())
                        {
                            sources[dst].push_back(instr.ops[1].reg);
                            require(dst, 16);
                        }
                        else
                            require(instr.ops[1].reg, 32);
                    }
            for (std::size_t i = 0; i < worklist.size(); ++i)
            {
                auto reg = worklist[i];
                if (auto found = sources.find(reg); found != sources.end())
                    for (auto source : found->second)
                        require(source, widths[reg]);
            }
            for (auto& block : func.blocks)
                for (auto& instr : block.instrs)
                    if (instr.opc == MOpc::COPY && instr.num_ops == 2 && instr.ops[0].kind == MOpKind::Reg)
                    {
                        auto dst = instr.ops[0].reg;
                        auto bits = dst.is_virtual() ? std::max<std::uint8_t>(16, widths[dst]) : 32;
                        if (bits == 16)
                            instr.opc = MOpc::MOV16rr;
                    }
            return widths;
        }

        std::uint32_t block_stride(MFunction const& func)
        {
            std::uint32_t stride = 512;
            for (auto const& block : func.blocks)
                stride = std::max(stride, static_cast<std::uint32_t>(block.instrs.size()) + 2);
            return stride;
        }

        void compute_liveness(MFunction& func, std::vector<LiveRange>& ranges)
        {

            auto stride = block_stride(func);
            for (auto& block : func.blocks)
            {
                block.live_in.clear();
                block.live_out.clear();
            }
            struct BlockLiveness
            {
                std::unordered_set<VReg> def;
                std::unordered_set<VReg> use;
            };

            std::vector<BlockLiveness> blk_live(func.blocks.size());
            std::unordered_map<std::uint32_t, std::size_t> id_to_idx;
            for (std::size_t i = 0; i < func.blocks.size(); ++i)
                id_to_idx[func.blocks[i].id] = i;

            std::unordered_map<VReg, std::uint32_t> first_def;
            std::unordered_map<VReg, std::uint32_t> last_def;
            std::unordered_map<VReg, std::uint32_t> last_use;

            for (std::size_t bi = 0; bi < func.blocks.size(); ++bi)
            {
                auto const& blk = func.blocks[bi];
                std::uint32_t base_pp = blk.id * stride;

                auto& bldef = blk_live[bi].def;
                auto& bluse = blk_live[bi].use;

                for (std::size_t ii = 0; ii < blk.instrs.size(); ++ii)
                {
                    auto const& instr = blk.instrs[ii];
                    std::uint32_t pp = base_pp + static_cast<std::uint32_t>(ii);

                    auto collect_vregs = [&](std::uint8_t start, std::uint8_t end, bool is_def) {
                        for (std::uint8_t oi = start; oi < end && oi < instr.num_ops; ++oi)
                        {
                            auto const& op = instr.ops[oi];
                            if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                            {
                                if (is_def)
                                {
                                    bldef.insert(op.reg);
                                    auto it = first_def.find(op.reg);
                                    if (it == first_def.end() || pp < it->second)
                                        first_def[op.reg] = pp;
                                    auto jt = last_def.find(op.reg);
                                    if (jt == last_def.end() || pp > jt->second)
                                        last_def[op.reg] = pp;
                                }
                                else
                                {
                                    if (!bldef.contains(op.reg))
                                        bluse.insert(op.reg);
                                    std::uint32_t use_pp = pp;
                                    if (instr.opc == MOpc::SUB64rr || instr.opc == MOpc::SUB32rr || instr.opc == MOpc::SUB16rr || instr.opc == MOpc::SBB32rr)
                                        ++use_pp;
                                    auto it = last_use.find(op.reg);
                                    if (it == last_use.end() || use_pp > it->second)
                                        last_use[op.reg] = use_pp;
                                }
                            }
                        }
                    };

                    collect_vregs(0, instr.num_defs, true);
                    collect_vregs(instr.num_defs, instr.num_ops, false);

                    for (std::uint8_t oi = 0; oi < instr.num_ops; ++oi)
                    {
                        auto const& op = instr.ops[oi];
                        if (op.kind == MOpKind::Mem)
                        {
                            auto memory_point = pp + (instr.num_defs != 0 && instr.num_ops >= 3);
                            if (op.mem.base.is_virtual())
                            {
                                if (!bldef.contains(op.mem.base))
                                    bluse.insert(op.mem.base);

                                auto it = last_use.find(op.mem.base);
                                if (it == last_use.end() || memory_point > it->second)
                                    last_use[op.mem.base] = memory_point;
                            }
                            if (op.mem.index.is_virtual())
                            {
                                if (!bldef.contains(op.mem.index))
                                    bluse.insert(op.mem.index);

                                auto it = last_use.find(op.mem.index);
                                if (it == last_use.end() || memory_point > it->second)
                                    last_use[op.mem.index] = memory_point;
                            }
                        }
                    }
                }
            }

            bool changed = true;
            while (changed)
            {
                changed = false;
                for (std::size_t bi = func.blocks.size(); bi > 0; --bi)
                {
                    auto const& blk = func.blocks[bi - 1];
                    std::size_t idx = id_to_idx[blk.id];
                    auto& blk_li = blk_live[idx];

                    std::unordered_set<VReg> new_out;
                    for (auto sid : blk.succs)
                    {
                        auto sit = id_to_idx.find(sid);
                        if (sit == id_to_idx.end())
                            continue;

                        auto const& succ_li = blk_live[sit->second];

                        for (auto v : succ_li.use)
                            new_out.insert(v);
                        for (auto v : func.block_by_id(sid) ? func.block_by_id(sid)->live_in : std::vector<VReg>{})
                            new_out.insert(v);
                    }

                    func.blocks[bi - 1].live_out.assign(new_out.begin(), new_out.end());

                    std::unordered_set<VReg> new_in = blk_li.use;
                    for (auto v : new_out)
                        if (!blk_li.def.contains(v))
                            new_in.insert(v);

                    std::vector<VReg> old_in_vec = func.blocks[bi - 1].live_in;
                    std::unordered_set<VReg> old_in(old_in_vec.begin(), old_in_vec.end());
                    if (new_in != old_in)
                    {
                        func.blocks[bi - 1].live_in.assign(new_in.begin(), new_in.end());
                        changed = true;
                    }
                }
            }

            std::unordered_set<VReg> all_vregs;
            for (auto const& [vreg, _] : first_def)
                all_vregs.insert(vreg);
            for (auto const& [vreg, _] : last_def)
                all_vregs.insert(vreg);
            for (auto const& [vreg, _] : last_use)
                all_vregs.insert(vreg);

            for (auto const& blk : func.blocks)
            {
                for (auto v : blk.live_in)
                    all_vregs.insert(v);
                for (auto v : blk.live_out)
                    all_vregs.insert(v);
            }

            ranges.reserve(all_vregs.size());
            auto xmm_vregs = infer_reg_classes(func);
            for (auto vreg : all_vregs)
            {
                LiveRange lr;
                lr.vreg = vreg;

                auto fd_it = first_def.find(vreg);
                if (fd_it != first_def.end())
                    lr.start = fd_it->second;
                else
                    lr.start = 0;

                auto lu_it = last_use.find(vreg);
                if (lu_it != last_use.end())
                    lr.end = lu_it->second;
                else
                    lr.end = lr.start;

                auto ld_it = last_def.find(vreg);
                if (ld_it != last_def.end())
                    lr.end = std::max(lr.end, ld_it->second);

                for (auto const& blk : func.blocks)
                {
                    for (auto v : blk.live_out)
                    {
                        if (v == vreg)
                        {
                            std::uint32_t blk_end = (blk.id * stride) + static_cast<std::uint32_t>(blk.instrs.size()) - 1;
                            lr.end = std::max(blk_end, lr.end);
                        }
                    }
                    for (auto v : blk.live_in)
                    {
                        if (v == vreg && lr.start > blk.id * stride)
                            lr.start = blk.id * stride;
                    }
                }

                lr.start = std::min(lr.start, lr.end);
                lr.reg_class = xmm_vregs.contains(vreg) ? RegClass::XMM : RegClass::GPR;

                ranges.push_back(lr);
            }

            std::unordered_map<VReg, LiveRange*> by_reg;
            for (auto& range : ranges)
                by_reg[range.vreg] = &range;
            for (auto const& block : func.blocks)
            {
                std::unordered_map<VReg, std::pair<std::uint32_t, std::uint32_t>> bounds;
                auto touch = [&](VReg reg, std::uint32_t point) {
                    if (!reg.is_virtual())
                        return;
                    auto [it, added] = bounds.try_emplace(reg, point, point);
                    if (!added)
                    {
                        it->second.first = std::min(it->second.first, point);
                        it->second.second = std::max(it->second.second, point);
                    }
                };
                auto base = block.id * stride;
                for (auto reg : block.live_in)
                    touch(reg, base ? base - 1 : 0);
                for (std::size_t i = 0; i < block.instrs.size(); ++i)
                {
                    auto const& instr = block.instrs[i];
                    auto point = base + static_cast<std::uint32_t>(i);
                    for (std::uint8_t j = 0; j < instr.num_ops; ++j)
                        if (auto const& op = instr.ops[j]; op.kind == MOpKind::Reg)
                        {
                            bool delayed = j >= instr.num_defs && (instr.opc == MOpc::SUB64rr || instr.opc == MOpc::SUB32rr || instr.opc == MOpc::SUB16rr ||
                                                                   instr.opc == MOpc::SBB32rr);
                            touch(op.reg, point + delayed);
                        }
                        else if (op.kind == MOpKind::Mem)
                        {
                            auto memory_point = point + (instr.num_defs != 0 && instr.num_ops >= 3);
                            touch(op.mem.base, memory_point);
                            touch(op.mem.index, memory_point);
                        }
                }
                for (auto reg : block.live_out)
                    touch(reg, base + static_cast<std::uint32_t>(block.instrs.size()));
                for (auto const& [reg, bound] : bounds)
                    if (by_reg.contains(reg))
                        by_reg.at(reg)->segments.push_back(bound);
            }

            for (std::size_t bi = 0; bi < func.blocks.size(); ++bi)
            {
                auto const& blk = func.blocks[bi];
                std::uint32_t base_pp = blk.id * stride;

                for (std::size_t ii = 0; ii < blk.instrs.size(); ++ii)
                {
                    auto const& instr = blk.instrs[ii];

                    bool is_call = false;
                    switch (instr.opc)
                    {
                        case MOpc::CALL:
                        case MOpc::CALL_rel32:
                        case MOpc::CALL_r64:
                        case MOpc::CALLm:
                            is_call = true;
                            break;
                        default:
                            break;
                    }

                    if (!is_call)
                        continue;
                    std::uint32_t call_point = base_pp + static_cast<std::uint32_t>(ii);

                    for (auto& lr : ranges)
                        if (std::ranges::any_of(lr.segments, [&](auto const& segment) { return segment.first < call_point && segment.second > call_point; }))
                            lr.crosses_call = true;
                }
            }

            std::ranges::sort(ranges, [](LiveRange const& a, LiveRange const& b) { return a.start < b.start; });
        }

        [[nodiscard]] std::unordered_map<VReg, std::uint64_t> register_constraints(MFunction const& func, RegisterPolicy const& regs)
        {
            std::unordered_map<VReg, std::uint64_t> masks;
            auto constrain = [&](VReg vreg, std::uint64_t mask) {
                if (!vreg.is_virtual() || mask == ~std::uint64_t{0})
                    return;
                auto [it, added] = masks.try_emplace(vreg, mask);
                if (!added)
                    it->second &= mask;
            };
            for (auto const& block : func.blocks)
                for (auto const& instr : block.instrs)
                    for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                    {
                        auto const& op = instr.ops[i];
                        if (op.kind == MOpKind::Mem && op.mem.address_bits == 16)
                        {
                            constrain(op.mem.base, regs.address16_registers);
                            constrain(op.mem.index, regs.address16_registers);
                        }
                        if (op.kind == MOpKind::Reg && operand_bits(instr.opc) == 8)
                            constrain(op.reg, regs.byte_registers);
                    }
            return masks;
        }

        void reuse_reloads(MFunction& func)
        {
            for (auto& block : func.blocks)
            {
                struct Cached
                {
                    std::uint32_t slot;
                    MOpc load;
                };
                std::unordered_map<PhysReg, Cached> cached;
                std::vector<MInstr> result;
                for (auto const& instr : block.instrs)
                {
                    bool load = instr.num_defs == 1 && instr.num_ops == 2 && instr.ops[0].kind == MOpKind::Reg && instr.ops[0].reg.is_physical() &&
                                instr.ops[1].kind == MOpKind::FrameSlot &&
                                (instr.opc == MOpc::MOV16rm || instr.opc == MOpc::MOV32rm || instr.opc == MOpc::MOV64rm || instr.opc == MOpc::MOVSD_rm ||
                                 instr.opc == MOpc::MOVSDrm || instr.opc == MOpc::MOVAPSrm);
                    if (load)
                    {
                        auto found = cached.find(instr.ops[0].reg.phys_reg());
                        if (found != cached.end() && found->second.slot == instr.ops[1].frame_slot && found->second.load == instr.opc)
                            continue;
                    }
                    for (std::uint8_t i = 0; i < instr.num_defs; ++i)
                        if (instr.ops[i].kind == MOpKind::Reg && instr.ops[i].reg.is_physical())
                            cached.erase(instr.ops[i].reg.phys_reg());
                    std::erase_if(cached, [&](auto const& entry) { return (instr.implicit_defs & (1ULL << static_cast<unsigned>(entry.first))) != 0; });
                    if (instr.num_defs == 0)
                        for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                            if (instr.ops[i].kind == MOpKind::FrameSlot)
                                std::erase_if(cached, [&](auto const& entry) { return entry.second.slot == instr.ops[i].frame_slot; });
                    if (load && instr.ops[1].frame_slot < func.frame_slots.size() && func.frame_slots[instr.ops[1].frame_slot].is_spill)
                        cached[instr.ops[0].reg.phys_reg()] = {instr.ops[1].frame_slot, instr.opc};
                    result.push_back(instr);
                }
                block.instrs = std::move(result);
            }
        }

        std::vector<double> block_costs(MFunction const& func)
        {
            std::unordered_map<std::uint32_t, std::size_t> indices;
            for (std::size_t i = 0; i < func.blocks.size(); ++i)
                indices[func.blocks[i].id] = i;
            auto n = func.blocks.size();
            auto words = (n + 63) / 64;
            std::vector<std::vector<std::uint64_t>> dom(n, std::vector<std::uint64_t>(words, ~std::uint64_t{0}));
            for (std::size_t i = 0; i < n; ++i)
                if (func.blocks[i].id == func.entry_block_id)
                {
                    std::ranges::fill(dom[i], 0);
                    dom[i][i / 64] = 1ULL << (i % 64);
                }
                else if (n % 64)
                    dom[i].back() &= (1ULL << (n % 64)) - 1;
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (std::size_t i = 0; i < n; ++i)
                {
                    if (func.blocks[i].id == func.entry_block_id)
                        continue;
                    std::vector<std::uint64_t> next(words);
                    bool first = true;
                    for (auto pred : func.blocks[i].preds)
                        if (auto it = indices.find(pred); it != indices.end())
                        {
                            if (first)
                                next = dom[it->second];
                            else
                                for (std::size_t word = 0; word < words; ++word)
                                    next[word] &= dom[it->second][word];
                            first = false;
                        }
                    next[i / 64] |= 1ULL << (i % 64);
                    if (next != dom[i])
                    {
                        dom[i] = std::move(next);
                        changed = true;
                    }
                }
            }
            std::vector<unsigned> depth(n);
            for (std::size_t head = 0; head < n; ++head)
            {
                std::unordered_set<std::size_t> loop;
                std::vector<std::size_t> work;
                for (std::size_t tail = 0; tail < n; ++tail)
                    if ((dom[tail][head / 64] & (1ULL << (head % 64))) &&
                        std::ranges::find(func.blocks[tail].succs, func.blocks[head].id) != func.blocks[tail].succs.end())
                    {
                        loop.insert(head);
                        if (loop.insert(tail).second)
                            work.push_back(tail);
                    }
                for (std::size_t j = 0; j < work.size(); ++j)
                    for (auto pred : func.blocks[work[j]].preds)
                        if (auto it = indices.find(pred); it != indices.end() && loop.insert(it->second).second)
                            work.push_back(it->second);
                for (auto member : loop)
                    ++depth[member];
            }
            std::vector<bool> cold(n);
            for (std::size_t i = 0; i < n; ++i)
                cold[i] = !func.blocks[i].instrs.empty() && func.blocks[i].instrs.back().opc == MOpc::UD2;
            changed = true;
            while (changed)
            {
                changed = false;
                for (std::size_t i = 0; i < n; ++i)
                    if (!cold[i] && !func.blocks[i].succs.empty() &&
                        std::ranges::all_of(func.blocks[i].succs, [&](auto id) { return indices.contains(id) && cold[indices.at(id)]; }))
                    {
                        cold[i] = true;
                        changed = true;
                    }
            }
            std::vector<double> costs(n);
            for (std::size_t i = 0; i < n; ++i)
                costs[i] = cold[i] ? 0.001 : std::pow(10.0, std::min(4u, depth[i]));
            return costs;
        }

        void allocation_costs(MFunction const& func, std::vector<LiveRange>& ranges, std::span<double const> costs)
        {
            auto n = func.blocks.size();
            std::unordered_map<VReg, LiveRange*> by_reg;
            std::unordered_map<VReg, unsigned> definitions;
            for (auto& range : ranges)
                by_reg[range.vreg] = &range;
            for (std::size_t bi = 0; bi < n; ++bi)
                for (auto const& instr : func.blocks[bi].instrs)
                {
                    auto cost = costs[bi];
                    auto charge = [&](VReg reg, bool def) {
                        if (auto it = by_reg.find(reg); it != by_reg.end())
                        {
                            it->second->weight += cost * (def ? 1 : 2);
                            if (def)
                                ++definitions[reg];
                        }
                    };
                    for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                        if (auto const& op = instr.ops[i]; op.kind == MOpKind::Reg)
                            charge(op.reg, i < instr.num_defs);
                        else if (op.kind == MOpKind::Mem)
                        {
                            charge(op.mem.base, false);
                            charge(op.mem.index, false);
                        }
                }
            for (auto const& block : func.blocks)
                for (auto const& instr : block.instrs)
                    if (instr.num_defs == 1 && instr.num_ops == 2 && instr.implicit_defs == 0 && instr.implicit_uses == 0 &&
                        instr.ops[0].kind == MOpKind::Reg && definitions[instr.ops[0].reg] == 1)
                    {
                        bool immediate = (instr.opc == MOpc::MOV64ri || instr.opc == MOpc::MOV64ri32 || instr.opc == MOpc::MOV32ri ||
                                          instr.opc == MOpc::MOV16ri || instr.opc == MOpc::MOV8ri) &&
                                         instr.ops[1].kind == MOpKind::Imm64;
                        bool address = (instr.opc == MOpc::LEA64rm || instr.opc == MOpc::LEA32rm || instr.opc == MOpc::LEA16rm) &&
                                       (instr.ops[1].kind == MOpKind::FrameSlot ||
                                        (instr.ops[1].kind == MOpKind::Mem && !instr.ops[1].mem.base.is_valid() && !instr.ops[1].mem.index.is_valid()));
                        if ((immediate || address) && by_reg.contains(instr.ops[0].reg))
                        {
                            auto& range = *by_reg.at(instr.ops[0].reg);
                            range.rematerialize = instr;
                            range.weight *= 0.25;
                        }
                    }
            for (auto& range : ranges)
                range.weight /= 1.0 + std::log2(1.0 + range.end - range.start);
            std::unordered_set<VReg> paired;
            for (auto const& [low, high] : func.register_pairs)
                if (by_reg.contains(low) && by_reg.contains(high))
                {
                    paired.insert(low);
                    paired.insert(high);
                }
            for (auto reg : paired)
                by_reg.at(reg)->weight *= 0.5;
        }

        std::vector<std::vector<std::uint64_t>> physical_liveness(MFunction const& func)
        {
            std::vector<std::vector<std::uint64_t>> result(func.blocks.size());
            std::unordered_map<std::uint32_t, std::size_t> indices;
            for (std::size_t i = 0; i < func.blocks.size(); ++i)
            {
                indices[func.blocks[i].id] = i;
                result[i].resize(func.blocks[i].instrs.size() + 1);
            }
            bool changed;
            do
            {
                changed = false;
                for (std::size_t b = func.blocks.size(); b-- > 0;)
                {
                    auto const& block = func.blocks[b];
                    std::uint64_t live = 0;
                    for (auto successor : block.succs)
                        if (indices.contains(successor))
                            live |= result[indices.at(successor)][0];
                    result[b].back() = live;
                    for (auto i = block.instrs.size(); i-- > 0;)
                    {
                        auto const& instr = block.instrs[i];
                        auto defs = instr.implicit_defs;
                        auto uses = instr.implicit_uses;
                        auto use = [&](VReg reg) {
                            if (reg.is_physical())
                                uses |= 1ULL << static_cast<unsigned>(reg.phys_reg());
                        };
                        for (std::uint8_t j = 0; j < instr.num_ops; ++j)
                        {
                            auto const& op = instr.ops[j];
                            if (op.kind == MOpKind::Reg && op.reg.is_physical())
                            {
                                if (j < instr.num_defs)
                                    defs |= 1ULL << static_cast<unsigned>(op.reg.phys_reg());
                                else
                                    use(op.reg);
                            }
                            else if (op.kind == MOpKind::Mem)
                            {
                                use(op.mem.base);
                                use(op.mem.index);
                            }
                        }
                        live = (live & ~defs) | uses;
                        if (result[b][i] != live)
                        {
                            result[b][i] = live;
                            changed = true;
                        }
                    }
                }
            } while (changed);
            return result;
        }

        void linear_scan(MFunction& func, RegisterPolicy const& regs, std::vector<LiveRange>& ranges, bool split = false)
        {
            auto stride = block_stride(func);
            auto constraints = register_constraints(func, regs);
            auto spill_slot = [&](LiveRange const& range) {
                if (auto slot = func.split_spill_homes.find(range.vreg.id); slot != func.split_spill_homes.end())
                    return slot->second;
                auto access = spill_access(regs, range);
                return func.new_frame_slot(access.size, access.align, true);
            };
            auto gprs = regs.gpr.allocatable;
            auto xmms = regs.xmm.allocatable;

            std::unordered_set<VReg> setcc_defs;
            std::unordered_set<VReg> shift_dsts;
            std::unordered_map<PhysReg, std::vector<std::uint32_t>> clobbers;
            std::unordered_map<PhysReg, std::vector<std::uint32_t>> phys_reads;
            std::unordered_map<PhysReg, std::vector<std::uint32_t>> fixed_reads;
            for (auto const& blk : func.blocks)
            {
                std::uint32_t base_pp = blk.id * stride;
                for (std::size_t ii = 0; ii < blk.instrs.size(); ++ii)
                {
                    auto const& instr = blk.instrs[ii];
                    if ((instr.opc == MOpc::COPY || (regs.narrow_gpr && instr.opc == MOpc::MOV16rr) || instr.opc == MOpc::MOVSDrr ||
                         instr.opc == MOpc::MOVSSrr || instr.opc == MOpc::MOVAPSrr) &&
                        instr.num_ops >= 2)
                        for (std::uint8_t oi = instr.num_defs; oi < instr.num_ops; ++oi)
                            if (instr.ops[oi].kind == MOpKind::Reg && instr.ops[oi].reg.is_physical())
                                phys_reads[instr.ops[oi].reg.phys_reg()].push_back(base_pp + static_cast<std::uint32_t>(ii));
                    if (is_setcc(instr.opc) && instr.num_defs > 0 && instr.num_ops > 0 && instr.ops[0].kind == MOpKind::Reg && instr.ops[0].reg.is_virtual())
                        setcc_defs.insert(instr.ops[0].reg);
                    switch (instr.opc)
                    {
                        case MOpc::SHL64rcl:
                        case MOpc::SHR64rcl:
                        case MOpc::SAR64rcl:
                        case MOpc::SHL64rCL:
                        case MOpc::SHR64rCL:
                        case MOpc::SAR64rCL:
                        case MOpc::SHL32rCL:
                        case MOpc::SHR32rCL:
                        case MOpc::SAR32rCL:
                        case MOpc::SHL16rCL:
                        case MOpc::SHR16rCL:
                        case MOpc::SAR16rCL:
                            if (instr.num_defs > 0 && instr.num_ops > 0 && instr.ops[0].kind == MOpKind::Reg && instr.ops[0].reg.is_virtual())
                                shift_dsts.insert(instr.ops[0].reg);
                            break;
                        default:
                            break;
                    }

                    auto pp = base_pp + static_cast<std::uint32_t>(ii);
                    for (std::uint8_t oi = 0; oi < instr.num_defs && oi < instr.num_ops; ++oi)
                        if (instr.ops[oi].kind == MOpKind::Reg && instr.ops[oi].reg.is_physical())
                            clobbers[instr.ops[oi].reg.phys_reg()].push_back(pp);

                    for (int pi = 0; pi < static_cast<int>(PhysReg::Count); ++pi)
                    {
                        if (instr.implicit_defs & (1ULL << pi))
                            clobbers[static_cast<PhysReg>(pi)].push_back(pp);
                        if (instr.implicit_uses & (1ULL << pi))
                            fixed_reads[static_cast<PhysReg>(pi)].push_back(pp);
                    }
                }
            }

            auto physical = physical_liveness(func);
            std::unordered_map<PhysReg, std::vector<std::pair<std::uint32_t, std::uint32_t>>> reserved;
            for (std::size_t b = 0; b < func.blocks.size(); ++b)
                for (std::size_t i = 0; i < physical[b].size(); ++i)
                    for (unsigned reg = 0; reg < static_cast<unsigned>(PhysReg::Count); ++reg)
                        if (physical[b][i] & (1ULL << reg))
                        {
                            auto point = func.blocks[b].id * stride + static_cast<std::uint32_t>(i);
                            reserved[static_cast<PhysReg>(reg)].emplace_back(point ? point - 1 : 0, point);
                        }
            std::unordered_map<VReg, LiveRange*> by_reg;
            std::unordered_map<VReg, std::vector<VReg>> affinities;
            for (auto& range : ranges)
                by_reg[range.vreg] = &range;
            for (auto const& block : func.blocks)
                for (auto const& instr : block.instrs)
                    if ((is_gpr_copy(instr.opc) || instr.opc == MOpc::MOVSDrr || instr.opc == MOpc::MOVSSrr || instr.opc == MOpc::MOVAPSrr) &&
                        instr.num_ops == 2 && instr.num_defs == 1 && instr.ops[0].kind == MOpKind::Reg && instr.ops[1].kind == MOpKind::Reg)
                    {
                        affinities[instr.ops[0].reg].push_back(instr.ops[1].reg);
                        affinities[instr.ops[1].reg].push_back(instr.ops[0].reg);
                    }
            auto interfere = [](LiveRange const& a, LiveRange const& b) {
                for (auto const& left : a.segments)
                    for (auto const& right : b.segments)
                        if (left.first < right.second && right.first < left.second)
                            return true;
                return false;
            };
            std::vector<LiveRange*> active;

            for (auto& range : ranges)
            {
                auto new_end = std::remove_if(active.begin(), active.end(), [&](LiveRange* a) { return a->end <= range.start; });

                active.erase(new_end, active.end());

                std::ranges::sort(active, [](LiveRange const* a, LiveRange const* b) { return a->end < b->end; });

                auto const& avail = (range.reg_class == RegClass::XMM) ? xmms : gprs;
                auto reg_is_allowed = [&](PhysReg reg) {
                    if (auto constraint = constraints.find(range.vreg);
                        constraint != constraints.end() && !(constraint->second & (1ULL << static_cast<unsigned>(reg))))
                        return false;
                    if (setcc_defs.contains(range.vreg) && (reg == PhysReg::RSI || reg == PhysReg::RDI))
                        return false;
                    if (shift_dsts.contains(range.vreg) && reg == PhysReg::RCX)
                        return false;

                    if (auto spans = reserved.find(reg); split && spans != reserved.end())
                        if (std::ranges::any_of(spans->second, [&](auto const& span) {
                                return std::ranges::any_of(range.segments,
                                                           [&](auto const& segment) { return segment.first < span.second && span.first < segment.second; });
                            }))
                            return false;
                    if (auto reads = fixed_reads.find(reg); split && reads != fixed_reads.end())
                        if (std::ranges::any_of(reads->second, [&](auto point) {
                                return std::ranges::any_of(range.segments,
                                                           [&](auto const& segment) { return segment.first <= point && point <= segment.second; });
                            }))
                            return false;
                    auto it = clobbers.find(reg);
                    if (it != clobbers.end() && !std::ranges::none_of(it->second, [&](std::uint32_t pp) {
                            return std::ranges::any_of(range.segments, [&](auto const& segment) { return pp > segment.first && pp < segment.second; });
                        }))
                        return false;
                    auto rd = phys_reads.find(reg);
                    if (rd == phys_reads.end())
                        return true;
                    return std::ranges::none_of(rd->second, [&](std::uint32_t pp) {
                        return std::ranges::any_of(range.segments, [&](auto const& segment) { return pp > segment.first && pp < segment.second; });
                    });
                };

                std::unordered_set<PhysReg> occupied;
                for (auto* a : active)
                    if (a->reg_class == range.reg_class && a->assigned != PhysReg::None && interfere(*a, range))
                        occupied.insert(a->assigned);

                PhysReg free_reg = PhysReg::None;

                auto const callee_gpr_span = regs.gpr.callee_saved;

                if (range.crosses_call && range.reg_class == RegClass::XMM)
                    free_reg = PhysReg::None;
                else if (range.crosses_call)
                {
                    for (auto pr : avail)
                        if (reg_is_allowed(pr) && !occupied.contains(pr) && std::ranges::find(callee_gpr_span, pr) != callee_gpr_span.end())
                        {
                            free_reg = pr;
                            break;
                        }
                }
                else
                {
                    for (auto pr : avail)
                        if (reg_is_allowed(pr) && !occupied.contains(pr))
                        {
                            free_reg = pr;
                            break;
                        }
                }

                for (auto partner : affinities[range.vreg])
                {
                    auto candidate = partner.is_physical() ? partner.phys_reg() : PhysReg::None;
                    if (auto found = by_reg.find(partner); found != by_reg.end() && !interfere(*found->second, range) && !found->second->spilled)
                        candidate = found->second->assigned;
                    if (candidate != PhysReg::None && std::ranges::find(avail, candidate) != avail.end() && !occupied.contains(candidate) &&
                        reg_is_allowed(candidate) &&
                        (!range.crosses_call || (range.reg_class == RegClass::GPR && std::ranges::find(callee_gpr_span, candidate) != callee_gpr_span.end())))
                    {
                        free_reg = candidate;
                        break;
                    }
                }
                if (free_reg != PhysReg::None)
                {
                    range.assigned = free_reg;
                    range.spilled = false;
                    active.push_back(&range);
                    std::ranges::sort(active, [](LiveRange const* a, LiveRange const* b) { return a->end < b->end; });
                }
                else
                {
                    LiveRange* spill_candidate = nullptr;
                    for (auto* a : active)
                    {
                        if (a->reg_class != range.reg_class || !interfere(*a, range))
                            continue;
                        if (std::ranges::any_of(active, [&](auto* other) { return other != a && other->assigned == a->assigned && interfere(*other, range); }))
                            continue;
                        if (!reg_is_allowed(a->assigned))
                            continue;

                        if (range.crosses_call)
                        {
                            if (a->assigned == PhysReg::None)
                                continue;
                            if (range.reg_class == RegClass::XMM)
                                continue;
                            if (std::ranges::find(regs.gpr.callee_saved, a->assigned) == regs.gpr.callee_saved.end())
                                continue;
                        }

                        if (!spill_candidate || a->weight < spill_candidate->weight || (a->weight == spill_candidate->weight && a->end > spill_candidate->end))
                            spill_candidate = a;
                    }

                    if (spill_candidate &&
                        (spill_candidate->weight < range.weight || (spill_candidate->weight == range.weight && spill_candidate->end > range.end)))
                    {
                        if (!spill_candidate->rematerialize && spill_candidate->spill_slot == (std::numeric_limits<std::uint32_t>::max)())
                            spill_candidate->spill_slot = spill_slot(*spill_candidate);

                        spill_candidate->spilled = true;
                        PhysReg freed_reg = spill_candidate->assigned;
                        spill_candidate->assigned = PhysReg::None;

                        range.assigned = freed_reg;
                        range.spilled = false;

                        auto scit = std::ranges::find(active, spill_candidate);
                        if (scit != active.end())
                            active.erase(scit);

                        active.push_back(&range);
                    }
                    else
                    {
                        if (!range.rematerialize && range.spill_slot == (std::numeric_limits<std::uint32_t>::max)())
                            range.spill_slot = spill_slot(range);

                        range.spilled = true;
                        range.assigned = PhysReg::None;
                    }

                    std::ranges::sort(active, [](LiveRange const* a, LiveRange const* b) { return a->end < b->end; });
                }
            }
        }

        bool split_spilled_ranges(MFunction& func, RegisterPolicy const& regs, std::vector<LiveRange> const& ranges)
        {
            std::unordered_map<VReg, LiveRange const*> selected;
            for (auto const& range : ranges)
                if (range.spilled && range.end > range.start)
                    selected.emplace(range.vreg, &range);
            for (auto const& block : func.blocks)
                for (auto const& instr : block.instrs)
                    if (instr.opc == MOpc::LOCK_XADD8mr || instr.opc == MOpc::LOCK_XADD16mr || instr.opc == MOpc::LOCK_XADD32mr ||
                        instr.opc == MOpc::LOCK_XADD64mr || instr.opc == MOpc::LOCK_XCHG8mr || instr.opc == MOpc::LOCK_XCHG16mr ||
                        instr.opc == MOpc::LOCK_XCHG32mr || instr.opc == MOpc::LOCK_XCHG64mr || instr.opc == MOpc::XCHG8mr || instr.opc == MOpc::XCHG16mr ||
                        instr.opc == MOpc::XCHG32mr)
                        for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                            if (instr.ops[i].kind == MOpKind::Reg)
                                selected.erase(instr.ops[i].reg);
            if (selected.empty())
                return false;
            auto stride = block_stride(func);
            auto fixed = physical_liveness(func);
            std::size_t block_index = 0;
            std::unordered_map<VReg, std::uint32_t> homes;
            auto home = [&](VReg reg) {
                auto [it, fresh] = homes.try_emplace(reg);
                if (fresh)
                {
                    auto access = spill_access(regs, *selected.at(reg));
                    it->second = func.new_frame_slot(access.size, access.align, true);
                }
                return it->second;
            };
            auto registers = [](MInstr const& instr, bool definitions) {
                std::vector<VReg> result;
                for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                {
                    auto const& op = instr.ops[i];
                    if (op.kind == MOpKind::Reg && (i < instr.num_defs) == definitions && op.reg.is_virtual())
                        result.push_back(op.reg);
                    else if (!definitions && op.kind == MOpKind::Mem)
                    {
                        if (op.mem.base.is_virtual())
                            result.push_back(op.mem.base);
                        if (op.mem.index.is_virtual())
                            result.push_back(op.mem.index);
                    }
                }
                return result;
            };
            auto pairs = func.register_pairs;
            for (auto& block : func.blocks)
            {
                auto size = block.instrs.size();
                std::vector<bool> cuts(size + 1);
                cuts[0] = cuts[size] = true;
                std::size_t tail = size;
                while (tail && is_branch(block.instrs[tail - 1].opc))
                    --tail;
                cuts[tail] = true;
                for (std::size_t i = 0; i < size; ++i)
                {
                    auto const& instr = block.instrs[i];
                    if (instr.implicit_defs || instr.opc == MOpc::CALL || instr.opc == MOpc::CALL_rel32 || instr.opc == MOpc::CALL_r64 ||
                        instr.opc == MOpc::CALLm)
                        cuts[i] = cuts[i + 1] = true;
                    if (i && i % 16 == 0)
                    {
                        auto point = block.id * stride + static_cast<std::uint32_t>(i);
                        auto pressure = std::ranges::count_if(ranges, [&](auto const& range) {
                            return range.reg_class == RegClass::GPR &&
                                   std::ranges::any_of(range.segments, [&](auto const& segment) { return segment.first <= point && point <= segment.second; });
                        });
                        if (static_cast<std::size_t>(pressure) >= regs.gpr.allocatable.size())
                            cuts[i] = true;
                    }
                }
                auto physical_copy = [](MInstr const& instr) {
                    return (is_gpr_copy(instr.opc) || instr.opc == MOpc::MOVAPSrr || instr.opc == MOpc::MOVSDrr || instr.opc == MOpc::MOVSSrr) &&
                           instr.num_ops == 2 && instr.ops[0].kind == MOpKind::Reg && instr.ops[1].kind == MOpKind::Reg &&
                           (instr.ops[0].reg.is_physical() || instr.ops[1].reg.is_physical());
                };
                for (std::size_t i = 1; i < size; ++i)
                    if ((block.instrs[i].copy_group && block.instrs[i].copy_group == block.instrs[i - 1].copy_group) ||
                        (physical_copy(block.instrs[i]) && physical_copy(block.instrs[i - 1])))
                        cuts[i] = false;
                for (std::size_t i = 0; i < size; ++i)
                    if (block.instrs[i].implicit_defs || block.instrs[i].opc == MOpc::CALL || block.instrs[i].opc == MOpc::CALL_rel32 ||
                        block.instrs[i].opc == MOpc::CALL_r64 || block.instrs[i].opc == MOpc::CALLm)
                    {
                        auto begin = i;
                        auto end = i + 1;
                        while (begin && physical_copy(block.instrs[begin - 1]))
                            --begin;
                        while (end < size && physical_copy(block.instrs[end]))
                            ++end;
                        cuts[begin] = cuts[end] = true;
                        for (auto j = begin + 1; j < end; ++j)
                            cuts[j] = false;
                    }
                auto scratch = regs.scratch(RegClass::GPR);
                auto scratch_mask = scratch.is_physical() ? 1ULL << static_cast<unsigned>(scratch.phys_reg()) : 0;
                for (std::size_t i = 1; i < size; ++i)
                    if (fixed[block_index][i] & scratch_mask)
                        cuts[i] = false;
                ++block_index;
                for (auto i = tail + 1; i < size; ++i)
                    cuts[i] = false;
                std::unordered_map<std::size_t, std::unordered_set<VReg>> live;
                std::unordered_set<VReg> current(block.live_out.begin(), block.live_out.end());
                live[size] = current;
                for (std::size_t i = size; i-- > 0;)
                {
                    for (auto reg : registers(block.instrs[i], true))
                        current.erase(reg);
                    for (auto reg : registers(block.instrs[i], false))
                        current.insert(reg);
                    if (cuts[i])
                        live[i] = current;
                }
                std::vector<MInstr> rewritten;
                for (std::size_t begin = 0; begin < size;)
                {
                    auto end = begin + 1;
                    while (end < size && !cuts[end])
                        ++end;
                    std::unordered_set<VReg> defined, input, touched, used;
                    for (auto i = begin; i < end; ++i)
                    {
                        for (auto reg : registers(block.instrs[i], false))
                            if (selected.contains(reg))
                            {
                                touched.insert(reg);
                                used.insert(reg);
                                if (!defined.contains(reg))
                                    input.insert(reg);
                            }
                        for (auto reg : registers(block.instrs[i], true))
                            if (selected.contains(reg))
                            {
                                touched.insert(reg);
                                defined.insert(reg);
                            }
                    }
                    std::vector<VReg> ordered(touched.begin(), touched.end());
                    std::ranges::sort(ordered, {}, &VReg::id);
                    std::unordered_map<VReg, VReg> children;
                    for (auto reg : ordered)
                    {
                        auto* range = selected.at(reg);
                        if (range->rematerialize && !used.contains(reg))
                            continue;
                        auto child = func.new_vreg();
                        children.emplace(reg, child);
                        if (!range->rematerialize)
                            func.split_spill_homes.emplace(child.id, home(reg));
                        if (range->rematerialize)
                        {
                            auto recipe = *range->rematerialize;
                            recipe.ops[0] = MOp::from_reg(child);
                            rewritten.push_back(recipe);
                        }
                        else if (input.contains(reg))
                            rewritten.push_back(make_instr(spill_access(regs, *range).load, {MOp::from_reg(child), MOp::from_frame_slot(home(reg))}, 1));
                    }
                    for (auto const& [low, high] : pairs)
                        if (children.contains(low) || children.contains(high))
                            func.register_pairs.emplace_back(children.contains(low) ? children.at(low) : low,
                                                             children.contains(high) ? children.at(high) : high);
                    for (auto i = begin; i < end; ++i)
                    {
                        auto instr = block.instrs[i];
                        if (instr.num_defs == 1 && instr.ops[0].kind == MOpKind::Reg && selected.contains(instr.ops[0].reg) &&
                            selected.at(instr.ops[0].reg)->rematerialize)
                            continue;
                        auto rename = [&](VReg& reg) {
                            if (children.contains(reg))
                                reg = children.at(reg);
                        };
                        for (std::uint8_t j = 0; j < instr.num_ops; ++j)
                        {
                            auto& op = instr.ops[j];
                            if (op.kind == MOpKind::Reg)
                                rename(op.reg);
                            else if (op.kind == MOpKind::Mem)
                            {
                                rename(op.mem.base);
                                rename(op.mem.index);
                            }
                        }
                        rewritten.push_back(instr);
                    }
                    for (auto reg : ordered)
                        if (defined.contains(reg) && live[end].contains(reg) && !selected.at(reg)->rematerialize)
                            rewritten.push_back(
                                make_instr(spill_access(regs, *selected.at(reg)).store, {MOp::from_frame_slot(home(reg)), MOp::from_reg(children.at(reg))}, 0));
                    begin = end;
                }
                block.instrs = std::move(rewritten);
            }
            return true;
        }

        void rewrite_function(MFunction& func, RegisterPolicy const& regs, std::vector<LiveRange> const& ranges)
        {
            auto constraints = register_constraints(func, regs);
            auto fixed = physical_liveness(func);
            std::size_t block_index = 0;
            std::unordered_map<VReg, LiveRange const*> range_map;
            for (auto const& r : ranges)
                range_map[r.vreg] = &r;

            VReg scratch_gpr = regs.scratch(RegClass::GPR);
            VReg scratch_xmm = regs.scratch(RegClass::XMM);
            auto const gpr_scratch_order = regs.gpr.scratch_order;
            auto const xmm_scratch_order = regs.xmm.scratch_order;

            std::unordered_map<PhysReg, std::uint32_t> scratch_slots;
            std::unordered_map<PhysReg, std::uint32_t> xmm_scratch_slots;

            for (auto& blk : func.blocks)
            {
                std::vector<MInstr> new_instrs;
                new_instrs.reserve(blk.instrs.size() * 2);

                for (std::size_t ii = 0; ii < blk.instrs.size(); ++ii)
                {
                    auto& instr = blk.instrs[ii];

                    if (instr.num_ops == 2)
                    {
                        auto skip_bridge = [&](MOp const& reg, MOp const& slot, bool load) {
                            if (reg.kind != MOpKind::Reg || slot.kind != MOpKind::FrameSlot)
                                return false;
                            auto found = range_map.find(reg.reg);
                            if (found == range_map.end() || !found->second->spilled || !func.split_spill_homes.contains(reg.reg.id))
                                return false;
                            auto access = spill_access(regs, *found->second);
                            return slot.frame_slot == found->second->spill_slot && instr.opc == (load ? access.load : access.store);
                        };
                        if (skip_bridge(instr.ops[0], instr.ops[1], true) || skip_bridge(instr.ops[1], instr.ops[0], false))
                            continue;
                    }
                    if (instr.opc == MOpc::PHI)
                        continue;
                    if (instr.num_defs == 1 && instr.ops[0].kind == MOpKind::Reg)
                        if (auto it = range_map.find(instr.ops[0].reg); it != range_map.end() && it->second->spilled && it->second->rematerialize)
                            continue;

                    if (instr.opc == MOpc::IMPLICIT_DEF)
                    {
                        if (instr.num_ops > 0 && instr.ops[0].kind == MOpKind::Reg)
                        {
                            VReg v = instr.ops[0].reg;
                            if (v.is_virtual())
                            {
                                auto it = range_map.find(v);
                                if (it != range_map.end() && !it->second->spilled && it->second->assigned != PhysReg::None)
                                {
                                    continue;
                                }
                                continue;
                            }
                            continue;
                        }
                        continue;
                    }

                    bool has_virtual = false;
                    auto check_virtual_op = [&](MOp const& op) {
                        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                            return true;
                        if (op.kind == MOpKind::Mem)
                            return op.mem.base.is_virtual() || op.mem.index.is_virtual();
                        return false;
                    };
                    for (std::uint8_t oi = 0; oi < instr.num_ops; ++oi)
                    {
                        if (check_virtual_op(instr.ops[oi]))
                        {
                            has_virtual = true;
                            break;
                        }
                    }

                    if (!has_virtual)
                    {
                        if (instr.opc == MOpc::COPY)
                        {
                            if (instr.num_ops >= 2 && instr.ops[0].kind == MOpKind::Reg && instr.ops[1].kind == MOpKind::Reg)
                            {
                                bool all_phys = !instr.ops[0].reg.is_virtual() && !instr.ops[1].reg.is_virtual();
                                if (all_phys)
                                    new_instrs.push_back(instr);
                            }
                        }
                        else
                            new_instrs.push_back(instr);
                        continue;
                    }

                    if (instr.num_defs == 1 && instr.num_ops == 2 && instr.ops[0].kind == MOpKind::Reg && instr.ops[1].kind == MOpKind::Reg &&
                        instr.ops[0].reg.is_virtual() && instr.ops[1].reg.is_virtual() && instr.implicit_defs == 0 && instr.implicit_uses == 0)
                    {
                        auto dst = range_map.find(instr.ops[0].reg);
                        auto src = range_map.find(instr.ops[1].reg);
                        if (dst != range_map.end() && src != range_map.end() && dst->second->spilled && src->second->spilled &&
                            dst->second->reg_class == src->second->reg_class &&
                            (instr.opc == regs.of(dst->second->reg_class).move || (regs.narrow_gpr && instr.opc == MOpc::MOV16rr)) &&
                            dst->second->spill_slot != (std::numeric_limits<std::uint32_t>::max)() &&
                            src->second->spill_slot != (std::numeric_limits<std::uint32_t>::max)())
                        {
                            if (dst->second->spill_slot != src->second->spill_slot)
                            {
                                auto source_access = spill_access(regs, *src->second);
                                auto destination_access = spill_access(regs, *dst->second);
                                VReg scratch = regs.scratch(dst->second->reg_class);
                                auto preserve = fixed[block_index][ii] & fixed[block_index][ii + 1] & (1ULL << static_cast<unsigned>(scratch.phys_reg()));
                                std::uint32_t saved_slot = 0;
                                if (preserve)
                                {
                                    auto& slots = src->second->reg_class == RegClass::GPR ? scratch_slots : xmm_scratch_slots;
                                    auto access = regs.of(src->second->reg_class).spill;
                                    auto [slot, fresh] = slots.try_emplace(scratch.phys_reg(), 0);
                                    if (fresh)
                                        slot->second = func.new_frame_slot(access.size, access.align, true);
                                    saved_slot = slot->second;
                                    new_instrs.push_back(make_instr(access.store, {MOp::from_frame_slot(saved_slot), MOp::from_reg(scratch)}, 0));
                                }
                                new_instrs.push_back(
                                    make_instr(source_access.load, {MOp::from_reg(scratch), MOp::from_frame_slot(src->second->spill_slot)}, 1));
                                new_instrs.push_back(
                                    make_instr(destination_access.store, {MOp::from_frame_slot(dst->second->spill_slot), MOp::from_reg(scratch)}, 0));
                                if (preserve)
                                    new_instrs.push_back(
                                        make_instr(regs.of(src->second->reg_class).spill.load, {MOp::from_reg(scratch), MOp::from_frame_slot(saved_slot)}, 1));
                            }
                            continue;
                        }
                    }

                    struct ReloadInfo
                    {
                        VReg spilled_vreg;
                        PhysReg scratch_reg;
                        RegClass rc;
                    };

                    bool is_jump_table = (instr.opc == MOpc::JUMP_TABLE);
                    PhysReg use_scratch = is_jump_table ? regs.jump_table_scratch : scratch_gpr.phys_reg();

                    std::vector<ReloadInfo> reloads_needed;
                    std::vector<unsigned> spilled_def_indices;

                    for (std::uint8_t oi = instr.num_defs; oi < instr.num_ops; ++oi)
                    {
                        auto const& op = instr.ops[oi];
                        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                        {
                            auto it = range_map.find(op.reg);
                            if (it != range_map.end() && it->second->spilled)
                                reloads_needed.push_back({op.reg, use_scratch, RegClass::GPR});
                        }
                        else if (op.kind == MOpKind::Mem)
                        {
                            if (op.mem.base.is_virtual())
                            {
                                auto it = range_map.find(op.mem.base);
                                if (it != range_map.end() && it->second->spilled)
                                    reloads_needed.push_back({op.mem.base, use_scratch, RegClass::GPR});
                            }
                            if (op.mem.index.is_virtual())
                            {
                                auto it = range_map.find(op.mem.index);
                                if (it != range_map.end() && it->second->spilled)
                                    reloads_needed.push_back({op.mem.index, use_scratch, RegClass::GPR});
                            }
                        }
                    }

                    for (std::uint8_t oi = 0; oi < instr.num_defs && oi < instr.num_ops; ++oi)
                    {
                        auto const& op = instr.ops[oi];
                        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                        {
                            auto it = range_map.find(op.reg);
                            if (it != range_map.end() && it->second->spilled)
                                spilled_def_indices.push_back(oi);
                        }
                    }

                    std::unordered_map<VReg, VReg> memory_scratch;
                    std::vector<std::pair<PhysReg, std::uint32_t>> saved_scratch;
                    std::unordered_map<VReg, VReg> xmm_scratch;
                    std::vector<std::pair<PhysReg, std::uint32_t>> saved_xmm_scratch;
                    auto preserve = fixed[block_index][ii] & fixed[block_index][ii + 1] & ~instr.implicit_defs;
                    for (std::uint8_t i = 0; i < instr.num_defs; ++i)
                        if (instr.ops[i].kind == MOpKind::Reg && instr.ops[i].reg.is_physical())
                            preserve &= ~(1ULL << static_cast<unsigned>(instr.ops[i].reg.phys_reg()));
                    if (!reloads_needed.empty() || !spilled_def_indices.empty())
                    {
                        for (auto cls : {RegClass::GPR, RegClass::XMM})
                        {
                            auto scratch = regs.scratch(cls);
                            if (!scratch.is_physical() || !(preserve & (1ULL << static_cast<unsigned>(scratch.phys_reg()))))
                                continue;
                            auto& slots = cls == RegClass::GPR ? scratch_slots : xmm_scratch_slots;
                            auto access = regs.of(cls).spill;
                            auto [slot, fresh] = slots.try_emplace(scratch.phys_reg(), 0);
                            if (fresh)
                                slot->second = func.new_frame_slot(access.size, access.align, true);
                            new_instrs.push_back(make_instr(access.store, {MOp::from_frame_slot(slot->second), MOp::from_reg(scratch)}, 0));
                            if (cls == RegClass::GPR)
                                saved_scratch.emplace_back(scratch.phys_reg(), slot->second);
                            else
                                saved_xmm_scratch.emplace_back(scratch.phys_reg(), slot->second);
                        }
                    }
                    bool has_memory = false;
                    std::uint64_t occupied = instr.implicit_defs | instr.implicit_uses;
                    auto occupy = [&](VReg reg) {
                        if (reg.is_virtual())
                        {
                            auto it = range_map.find(reg);
                            if (it == range_map.end() || it->second->spilled)
                                return;
                            reg = VReg::phys(it->second->assigned);
                        }
                        if (reg.is_physical())
                            occupied |= 1ULL << static_cast<unsigned>(reg.phys_reg());
                    };
                    for (unsigned oi = 0; oi < instr.num_ops; ++oi)
                    {
                        auto const& op = instr.ops[oi];
                        if (op.kind == MOpKind::Reg)
                            occupy(op.reg);
                        else if (op.kind == MOpKind::Mem)
                        {
                            has_memory = true;
                            occupy(op.mem.base);
                            occupy(op.mem.index);
                        }
                    }
                    auto pick_xmm_scratch = [&](VReg vreg) -> VReg {
                        auto found = xmm_scratch.find(vreg);
                        if (found != xmm_scratch.end())
                            return found->second;
                        PhysReg chosen = scratch_xmm.phys_reg();
                        for (auto candidate : xmm_scratch_order)
                            if ((occupied & (1ULL << static_cast<unsigned>(candidate))) == 0)
                            {
                                chosen = candidate;
                                break;
                            }
                        if (chosen != scratch_xmm.phys_reg())
                        {
                            auto [slot, inserted] = xmm_scratch_slots.try_emplace(chosen, 0);
                            if (inserted)
                                slot->second = func.new_frame_slot(regs.xmm.spill.size, regs.xmm.spill.align, true);
                            MInstr save;
                            save.opc = regs.xmm.spill.store;
                            save.num_ops = 2;
                            save.ops[0] = MOp::from_frame_slot(slot->second);
                            save.ops[1] = MOp::from_reg(VReg::phys(chosen));
                            new_instrs.push_back(save);
                            saved_xmm_scratch.emplace_back(chosen, slot->second);
                        }
                        occupied |= 1ULL << static_cast<unsigned>(chosen);
                        VReg result = VReg::phys(chosen);
                        xmm_scratch.emplace(vreg, result);
                        return result;
                    };
                    auto pick_gpr_scratch = [&](VReg vreg) -> VReg {
                        auto found = memory_scratch.find(vreg);
                        if (found != memory_scratch.end())
                            return found->second;
                        PhysReg chosen = scratch_gpr.phys_reg();
                        for (auto candidate : gpr_scratch_order)
                            if ((occupied & (1ULL << static_cast<unsigned>(candidate))) == 0 &&
                                (!constraints.contains(vreg) || (constraints.at(vreg) & (1ULL << static_cast<unsigned>(candidate)))))
                            {
                                chosen = candidate;
                                break;
                            }
                        if (chosen != scratch_gpr.phys_reg())
                        {
                            auto [slot, inserted] = scratch_slots.try_emplace(chosen, 0);
                            if (inserted)
                                slot->second = func.new_frame_slot(regs.gpr.spill.size, regs.gpr.spill.align, true);
                            MInstr save;
                            save.opc = regs.gpr.spill.store;
                            save.num_ops = 2;
                            save.ops[0] = MOp::from_frame_slot(slot->second);
                            save.ops[1] = MOp::from_reg(VReg::phys(chosen));
                            new_instrs.push_back(save);
                            saved_scratch.emplace_back(chosen, slot->second);
                        }
                        occupied |= 1ULL << static_cast<unsigned>(chosen);
                        VReg result = VReg::phys(chosen);
                        memory_scratch.emplace(vreg, result);
                        return result;
                    };
                    if (has_memory)
                    {
                        for (auto const& rl : reloads_needed)
                        {
                            auto const* range = range_map.at(rl.spilled_vreg);
                            if (range->reg_class != RegClass::GPR || memory_scratch.contains(rl.spilled_vreg))
                                continue;
                            if (constraints.contains(rl.spilled_vreg) || (occupied & (1ULL << static_cast<unsigned>(scratch_gpr.phys_reg()))))
                            {
                                std::ignore = pick_gpr_scratch(rl.spilled_vreg);
                                continue;
                            }
                            PhysReg chosen = scratch_gpr.phys_reg();
                            if (!memory_scratch.empty())
                            {
                                for (auto candidate : gpr_scratch_order.subspan(1))
                                    if ((occupied & (1ULL << static_cast<unsigned>(candidate))) == 0)
                                    {
                                        chosen = candidate;
                                        break;
                                    }
                                auto [slot, inserted] = scratch_slots.try_emplace(chosen, 0);
                                if (inserted)
                                    slot->second = func.new_frame_slot(regs.gpr.spill.size, regs.gpr.spill.align, true);
                                MInstr save;
                                save.opc = regs.gpr.spill.store;
                                save.num_ops = 2;
                                save.ops[0] = MOp::from_frame_slot(slot->second);
                                save.ops[1] = MOp::from_reg(VReg::phys(chosen));
                                new_instrs.push_back(save);
                                saved_scratch.emplace_back(chosen, slot->second);
                            }
                            occupied |= 1ULL << static_cast<unsigned>(chosen);
                            memory_scratch.emplace(rl.spilled_vreg, VReg::phys(chosen));
                        }
                    }

                    for (auto const& rl : reloads_needed)
                    {
                        auto rit = range_map.find(rl.spilled_vreg);
                        if (rit == range_map.end())
                            continue;

                        auto const& lr = *rit->second;
                        if (!lr.rematerialize && lr.spill_slot == (std::numeric_limits<std::uint32_t>::max)())
                            continue;

                        VReg scratch = (lr.reg_class == RegClass::XMM)            ? pick_xmm_scratch(rl.spilled_vreg)
                                       : memory_scratch.contains(rl.spilled_vreg) ? memory_scratch.at(rl.spilled_vreg)
                                       : is_jump_table                            ? VReg::phys(use_scratch)
                                                                                  : pick_gpr_scratch(rl.spilled_vreg);

                        MInstr reload;
                        if (lr.reg_class == RegClass::XMM)
                        {
                            reload.opc = regs.xmm.spill.load;
                            reload.num_ops = 2;
                            reload.num_defs = 1;
                            reload.ops[0] = MOp::from_reg(scratch);
                            reload.ops[1] = MOp::from_frame_slot(lr.spill_slot);
                        }
                        else
                        {
                            reload.opc = spill_access(regs, lr).load;
                            reload.num_ops = 2;
                            reload.num_defs = 1;
                            reload.ops[0] = MOp::from_reg(scratch);
                            reload.ops[1] = MOp::from_frame_slot(lr.spill_slot);
                        }
                        if (lr.rematerialize)
                        {
                            reload = *lr.rematerialize;
                            reload.ops[0] = MOp::from_reg(scratch);
                        }
                        new_instrs.push_back(reload);
                    }

                    MInstr new_instr = instr;

                    for (auto idx : spilled_def_indices)
                    {
                        VReg v = new_instr.ops[idx].reg;
                        auto it = range_map.find(v);
                        if (it == range_map.end())
                            continue;

                        VReg scratch = (it->second->reg_class == RegClass::XMM) ? pick_xmm_scratch(v) : pick_gpr_scratch(v);
                        new_instr.ops[idx].reg = scratch;
                    }

                    {
                        VReg jt_scratch_gpr = is_jump_table ? VReg::phys(regs.jump_table_scratch) : scratch_gpr;
                        for (std::uint8_t oi = instr.num_defs; oi < new_instr.num_ops; ++oi)
                        {
                            auto& op = new_instr.ops[oi];
                            if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                            {
                                auto it = range_map.find(op.reg);
                                if (it != range_map.end())
                                {
                                    if (it->second->spilled)
                                    {
                                        if (it->second->reg_class == RegClass::XMM && xmm_scratch.contains(op.reg))
                                            op.reg = xmm_scratch.at(op.reg);
                                        else
                                        {
                                            VReg scratch = (it->second->reg_class == RegClass::XMM) ? scratch_xmm : jt_scratch_gpr;
                                            op.reg = memory_scratch.contains(op.reg) ? memory_scratch.at(op.reg) : scratch;
                                        }
                                    }
                                    else if (it->second->assigned != PhysReg::None)
                                        op.reg = VReg::phys(it->second->assigned);
                                }
                            }
                            else if (op.kind == MOpKind::Mem)
                            {
                                if (op.mem.base.is_virtual())
                                {
                                    auto it = range_map.find(op.mem.base);
                                    if (it != range_map.end())
                                    {
                                        if (it->second->spilled)
                                            op.mem.base = memory_scratch.contains(op.mem.base) ? memory_scratch.at(op.mem.base) : jt_scratch_gpr;
                                        else if (it->second->assigned != PhysReg::None)
                                            op.mem.base = VReg::phys(it->second->assigned);
                                    }
                                }
                                if (op.mem.index.is_virtual())
                                {
                                    auto it = range_map.find(op.mem.index);
                                    if (it != range_map.end())
                                    {
                                        if (it->second->spilled)
                                            op.mem.index = memory_scratch.contains(op.mem.index) ? memory_scratch.at(op.mem.index) : jt_scratch_gpr;
                                        else if (it->second->assigned != PhysReg::None)
                                            op.mem.index = VReg::phys(it->second->assigned);
                                    }
                                }
                            }
                        }
                    }

                    for (std::uint8_t oi = 0; oi < new_instr.num_defs && oi < new_instr.num_ops; ++oi)
                    {
                        auto& op = new_instr.ops[oi];
                        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                        {
                            bool already_handled = false;
                            for (auto idx : spilled_def_indices)
                            {
                                if (idx == oi)
                                {
                                    already_handled = true;
                                    break;
                                }
                            }
                            if (already_handled)
                                continue;

                            auto it = range_map.find(op.reg);
                            if (it != range_map.end() && !it->second->spilled && it->second->assigned != PhysReg::None)
                                op.reg = VReg::phys(it->second->assigned);
                        }
                    }

                    if (new_instr.opc == MOpc::PHI)
                        continue;

                    if (is_setcc(new_instr.opc) && new_instr.num_defs > 0 && new_instr.num_ops > 0 && new_instr.ops[0].kind == MOpKind::Reg)
                    {
                        bool narrow = regs.narrow_gpr && range_map.contains(instr.ops[0].reg) && range_map.at(instr.ops[0].reg)->bits == 16;
                        new_instrs.push_back(narrow ? make_instr(MOpc::MOV16ri, {new_instr.ops[0], MOp::from_imm(0)}, 1)
                                                    : make_mov_ri(new_instr.ops[0].reg, 0, 32));
                    }

                    new_instrs.push_back(new_instr);

                    for (auto idx : spilled_def_indices)
                    {
                        VReg v = instr.ops[idx].reg;
                        auto it = range_map.find(v);
                        if (it == range_map.end() || it->second->spill_slot == (std::numeric_limits<std::uint32_t>::max)())
                            continue;

                        auto const& lr = *it->second;
                        VReg scratch = (lr.reg_class == RegClass::XMM && xmm_scratch.contains(v)) ? xmm_scratch.at(v)
                                       : (lr.reg_class == RegClass::XMM)                          ? scratch_xmm
                                       : memory_scratch.contains(v)                               ? memory_scratch.at(v)
                                                                                                  : scratch_gpr;

                        MInstr spill;
                        if (lr.reg_class == RegClass::XMM)
                        {
                            spill.opc = regs.xmm.spill.store;
                            spill.num_ops = 2;
                            spill.num_defs = 0;
                            spill.ops[0] = MOp::from_frame_slot(lr.spill_slot);
                            spill.ops[1] = MOp::from_reg(scratch);
                        }
                        else
                        {
                            spill.opc = spill_access(regs, lr).store;
                            spill.num_ops = 2;
                            spill.num_defs = 0;
                            spill.ops[0] = MOp::from_frame_slot(lr.spill_slot);
                            spill.ops[1] = MOp::from_reg(scratch);
                        }
                        new_instrs.push_back(spill);
                    }
                    for (auto const& [reg, slot] : saved_scratch)
                    {
                        MInstr restore;
                        restore.opc = regs.gpr.spill.load;
                        restore.num_ops = 2;
                        restore.num_defs = 1;
                        restore.ops[0] = MOp::from_reg(VReg::phys(reg));
                        restore.ops[1] = MOp::from_frame_slot(slot);
                        new_instrs.push_back(restore);
                    }
                    for (auto const& [reg, slot] : saved_xmm_scratch)
                    {
                        MInstr restore;
                        restore.opc = regs.xmm.spill.load;
                        restore.num_ops = 2;
                        restore.num_defs = 1;
                        restore.ops[0] = MOp::from_reg(VReg::phys(reg));
                        restore.ops[1] = MOp::from_frame_slot(slot);
                        new_instrs.push_back(restore);
                    }
                }

                blk.instrs = std::move(new_instrs);
                ++block_index;
            }
        }

        [[nodiscard]] bool is_call_opc(MOpc opc) noexcept
        {
            return opc == MOpc::CALL || opc == MOpc::CALL_rel32 || opc == MOpc::CALL_r64 || opc == MOpc::CALLm;
        }

        void insert_callee_saves(MFunction& func, RegisterPolicy const& regs, std::vector<LiveRange> const& ranges)
        {
            std::vector<PhysReg> used_callee_saves;
            std::vector<PhysReg> used_xmm_saves;

            auto note = [&](PhysReg pr) {
                if (reg_class(pr) == RegClass::XMM)
                {
                    if (std::ranges::find(regs.xmm.callee_saved, pr) != regs.xmm.callee_saved.end() &&
                        std::ranges::find(used_xmm_saves, pr) == used_xmm_saves.end())
                        used_xmm_saves.push_back(pr);
                }
                else if (std::ranges::find(regs.gpr.callee_saved, pr) != regs.gpr.callee_saved.end() &&
                         std::ranges::find(used_callee_saves, pr) == used_callee_saves.end())
                    used_callee_saves.push_back(pr);
            };

            for (auto const& lr : ranges)
            {
                if (lr.spilled || lr.assigned == PhysReg::None)
                    continue;
                note(lr.assigned);
            }

            for (auto const& blk : func.blocks)
            {
                for (auto const& instr : blk.instrs)
                {
                    if (is_call_opc(instr.opc))
                        continue;
                    for (std::uint8_t oi = 0; oi < instr.num_defs && oi < instr.num_ops; ++oi)
                        if (instr.ops[oi].kind == MOpKind::Reg && instr.ops[oi].reg.is_physical())
                            note(instr.ops[oi].reg.phys_reg());
                    for (int pi = 0; pi < static_cast<int>(PhysReg::Count); ++pi)
                        if (instr.implicit_defs & (1ULL << pi))
                            note(static_cast<PhysReg>(pi));
                }
            }

            if (used_callee_saves.empty() && used_xmm_saves.empty())
                return;

            std::ranges::sort(used_callee_saves, [](PhysReg a, PhysReg b) { return static_cast<int>(a) < static_cast<int>(b); });
            std::ranges::sort(used_xmm_saves, [](PhysReg a, PhysReg b) { return static_cast<int>(a) < static_cast<int>(b); });

            auto& entry = func.entry_block();

            std::vector<std::pair<PhysReg, std::uint32_t>> xmm_slots;
            for (auto pr : used_xmm_saves)
                xmm_slots.emplace_back(pr, func.new_frame_slot(regs.xmm_save.size, regs.xmm_save.align));

            for (auto const& [pr, slot] : std::views::reverse(xmm_slots))
            {
                MInstr save;
                save.opc = regs.xmm_save.store;
                save.num_ops = 2;
                save.num_defs = 0;
                save.ops[0] = MOp::from_frame_slot(slot);
                save.ops[1] = MOp::from_reg(VReg::phys(pr));
                entry.instrs.insert(entry.instrs.begin(), save);
            }

            for (auto pr : used_callee_saves)
            {
                MInstr push;
                push.opc = regs.gpr_save;
                push.num_ops = 1;
                push.num_defs = 0;
                push.ops[0] = MOp::from_reg(VReg::phys(pr));
                entry.instrs.insert(entry.instrs.begin(), push);
            }

            for (auto& blk : func.blocks)
            {
                for (std::size_t i = 0; i < blk.instrs.size(); ++i)
                {
                    if (blk.instrs[i].opc != MOpc::RET)
                        continue;

                    std::size_t insert_at = i;
                    auto num_pops = used_callee_saves.size();
                    for (auto pr : std::views::reverse(used_callee_saves))
                    {
                        MInstr pop;
                        pop.opc = regs.gpr_restore;
                        pop.num_ops = 1;
                        pop.num_defs = 1;
                        pop.ops[0] = MOp::from_reg(VReg::phys(pr));
                        blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(insert_at), pop);
                    }

                    for (auto const& [pr, slot] : xmm_slots)
                    {
                        MInstr restore;
                        restore.opc = regs.xmm_save.load;
                        restore.num_ops = 2;
                        restore.num_defs = 1;
                        restore.ops[0] = MOp::from_reg(VReg::phys(pr));
                        restore.ops[1] = MOp::from_frame_slot(slot);
                        blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(insert_at), restore);
                    }

                    i += num_pops + xmm_slots.size();
                }
            }
        }

        [[nodiscard]] bool is_reg_move_opc(MOpc opc) noexcept
        {
            return opc == MOpc::COPY || opc == MOpc::MOVSDrr || opc == MOpc::MOVSSrr || opc == MOpc::MOVAPSrr;
        }

        [[nodiscard]] MInstr make_reg_move(RegisterPolicy const& regs, VReg dst, VReg src, unsigned bits = 0)
        {
            if (dst.is_physical() && reg_class(dst.phys_reg()) == RegClass::XMM)
            {
                MInstr mi;
                mi.opc = regs.xmm.move;
                mi.num_ops = 2;
                mi.num_defs = 1;
                mi.ops[0] = MOp::from_reg(dst);
                mi.ops[1] = MOp::from_reg(src);
                return mi;
            }

            if (regs.narrow_gpr && bits == 16)
                return make_instr(MOpc::MOV16rr, {MOp::from_reg(dst), MOp::from_reg(src)}, 1);
            return make_copy(dst, src);
        }

        [[nodiscard]] bool is_phys_reg_copy(MInstr const& c) noexcept
        {
            return is_reg_move_opc(c.opc) && c.num_ops >= 2 && c.ops[0].kind == MOpKind::Reg && c.ops[1].kind == MOpKind::Reg && !c.ops[0].reg.is_virtual() &&
                   !c.ops[1].reg.is_virtual();
        }

        void resolve_parallel_copies(MFunction& func, RegisterPolicy const& regs)
        {
            VReg scratch_gpr = regs.scratch(RegClass::GPR);
            VReg scratch_xmm = regs.scratch(RegClass::XMM);

            for (auto& blk : func.blocks)
            {
                for (std::size_t ii = 0; ii < blk.instrs.size();)
                {
                    auto& instr = blk.instrs[ii];

                    bool is_call = false;
                    switch (instr.opc)
                    {
                        case MOpc::CALL:
                        case MOpc::CALL_rel32:
                        case MOpc::CALL_r64:
                        case MOpc::CALLm:
                            is_call = true;
                            break;
                        default:
                            break;
                    }

                    auto is_phys_copy = [&](MInstr const& c) {
                        return is_phys_reg_copy(c) || (regs.narrow_gpr && c.opc == MOpc::MOV16rr && c.num_ops == 2 && c.ops[0].kind == MOpKind::Reg &&
                                                       c.ops[1].kind == MOpKind::Reg && c.ops[0].reg.is_physical() && c.ops[1].reg.is_physical());
                    };

                    bool is_prologue = (blk.id == func.entry_block_id) && ii == 0 && is_phys_copy(instr);
                    bool is_group = instr.copy_group != 0 && is_phys_copy(instr);

                    if (!is_call && !is_prologue && !is_group)
                    {
                        ++ii;
                        continue;
                    }

                    std::size_t run_start = ii;
                    std::size_t run_end = ii;

                    if (is_call)
                    {
                        while (run_start > 0 && is_phys_copy(blk.instrs[run_start - 1]))
                            --run_start;
                    }
                    else
                    {
                        while (run_end < blk.instrs.size() && is_phys_copy(blk.instrs[run_end]) &&
                               (is_prologue || blk.instrs[run_end].copy_group == instr.copy_group))
                            ++run_end;
                    }

                    std::size_t run_len = run_end - run_start;
                    if (run_len < 2)
                    {
                        ++ii;
                        continue;
                    }

                    struct Move
                    {
                        VReg dst;
                        VReg src;
                        std::uint32_t group = 0;
                        unsigned bits = 0;
                    };
                    auto reg_is_xmm = [](VReg r) { return r.is_physical() && reg_class(r.phys_reg()) == RegClass::XMM; };

                    std::vector<Move> moves;
                    for (std::size_t j = run_start; j < run_end; ++j)
                    {
                        auto const& c = blk.instrs[j];
                        if (c.ops[0].reg == c.ops[1].reg)
                            continue;
                        moves.push_back({c.ops[0].reg, c.ops[1].reg, c.copy_group, regs.narrow_gpr ? (c.opc == MOpc::MOV16rr ? 16u : 32u) : 0u});
                    }
                    if (std::getenv("DCC_DEBUG_GRP") != nullptr)
                    {
                        std::string line = std::format("GRP func={}:", func.owned_name);
                        for (auto const& m : moves)
                            line += std::format(" [v{}>v{} g{}]", m.dst.id, m.src.id, m.group);
                        std::println(std::cerr, "{}", line);
                    }

                    if (moves.size() < 2)
                    {
                        ++ii;
                        continue;
                    }

                    auto resolve_group = [&func, &regs](std::vector<Move> group, VReg group_scratch, SpillAccess temp, bool& ok, bool& changed,
                                                        std::uint32_t& out_temp_slot) {
                        unsigned bits = 0;
                        for (auto const& move : group)
                            bits = std::max(bits, move.bits);
                        if (regs.narrow_gpr && bits == 16)
                            temp = {2, 2, MOpc::MOV16mr, MOpc::MOV16rm};
                        ok = true;
                        changed = false;
                        if (group.size() < 2)
                            return group;

                        std::unordered_set<VReg> dsts;
                        for (auto const& m : group)
                            dsts.insert(m.dst);

                        bool conflict = false;
                        for (auto const& m : group)
                            if (dsts.contains(m.src) && m.src != m.dst)
                                conflict = true;

                        if (!conflict)
                            return group;

                        bool scratch_is_dst = false;
                        bool scratch_is_src = false;
                        for (auto const& m : group)
                        {
                            if (m.dst == group_scratch)
                                scratch_is_dst = true;
                            if (m.src == group_scratch)
                                scratch_is_src = true;
                        }

                        if (!scratch_is_dst && !scratch_is_src)
                        {
                            std::vector<Move> ordered;
                            std::vector<Move> remaining = group;
                            int guard = static_cast<int>(remaining.size()) * 4 + 4;
                            while (!remaining.empty() && guard-- > 0)
                            {
                                bool picked = false;
                                for (auto it = remaining.begin(); it != remaining.end(); ++it)
                                {
                                    bool blocks_other = false;
                                    for (auto const& other : remaining)
                                        if (&other != &*it && other.src == it->dst)
                                            blocks_other = true;

                                    if (!blocks_other)
                                    {
                                        ordered.push_back(*it);
                                        remaining.erase(it);
                                        picked = true;
                                        break;
                                    }
                                }

                                if (picked)
                                    continue;

                                auto cycle = remaining.front();
                                ordered.push_back({group_scratch, cycle.src, 0, bits});
                                for (auto& rm : remaining)
                                    if (rm.src == cycle.src)
                                        rm.src = group_scratch;
                            }

                            if (!remaining.empty())
                            {
                                ok = false;
                                return group;
                            }

                            changed = true;
                            return ordered;
                        }

                        std::vector<Move> scratch_dst;
                        std::vector<Move> rest;
                        std::unordered_set<VReg> rest_dsts;
                        for (auto const& m : group)
                        {
                            if (m.dst == group_scratch)
                                scratch_dst.push_back(m);
                            else
                            {
                                rest.push_back(m);
                                rest_dsts.insert(m.dst);
                            }
                        }

                        bool need_temp = false;
                        for (auto const& sm : scratch_dst)
                            if (rest_dsts.contains(sm.src))
                            {
                                need_temp = true;
                                break;
                            }

                        if (need_temp)
                        {
                            out_temp_slot = func.new_frame_slot(temp.size, temp.align, false);

                            std::vector<Move> result;
                            for (auto const& sm : scratch_dst)
                                result.push_back({VReg{}, sm.src, 0, bits});

                            if (rest.size() >= 2)
                            {
                                std::unordered_set<VReg> rd;
                                for (auto const& m : rest)
                                    rd.insert(m.dst);
                                bool rest_conflict = false;
                                for (auto const& m : rest)
                                    if (rd.contains(m.src) && m.src != m.dst)
                                        rest_conflict = true;

                                if (rest_conflict)
                                {
                                    std::vector<Move> remaining = rest;
                                    int guard = static_cast<int>(remaining.size()) * 4 + 4;
                                    while (!remaining.empty() && guard-- > 0)
                                    {
                                        bool picked = false;
                                        for (auto it = remaining.begin(); it != remaining.end(); ++it)
                                        {
                                            bool blocks_other = false;
                                            for (auto const& other : remaining)
                                                if (&other != &*it && other.src == it->dst)
                                                    blocks_other = true;
                                            if (!blocks_other)
                                            {
                                                result.push_back(*it);
                                                remaining.erase(it);
                                                picked = true;
                                                break;
                                            }
                                        }
                                        if (picked)
                                            continue;
                                        auto cycle = remaining.front();
                                        result.push_back({group_scratch, cycle.src, 0, bits});
                                        for (auto& rm : remaining)
                                            if (rm.src == cycle.src)
                                                rm.src = group_scratch;
                                    }
                                    if (!remaining.empty())
                                    {
                                        ok = false;
                                        return group;
                                    }
                                }
                                else
                                {
                                    for (auto const& m : rest)
                                        result.push_back(m);
                                }
                            }
                            else if (rest.size() == 1)
                                result.push_back(rest[0]);

                            for (std::size_t si = 0; si < scratch_dst.size(); ++si)
                                result.push_back({group_scratch, VReg{}, 0, bits});

                            changed = true;
                            return result;
                        }
                        else
                        {
                            std::vector<Move> result;
                            if (rest.size() >= 2)
                            {
                                std::unordered_set<VReg> rd;
                                for (auto const& m : rest)
                                    rd.insert(m.dst);
                                bool rest_conflict = false;
                                for (auto const& m : rest)
                                    if (rd.contains(m.src) && m.src != m.dst)
                                        rest_conflict = true;

                                if (rest_conflict)
                                {
                                    std::vector<Move> remaining = rest;
                                    int guard = static_cast<int>(remaining.size()) * 4 + 4;
                                    while (!remaining.empty() && guard-- > 0)
                                    {
                                        bool picked = false;
                                        for (auto it = remaining.begin(); it != remaining.end(); ++it)
                                        {
                                            bool blocks_other = false;
                                            for (auto const& other : remaining)
                                                if (&other != &*it && other.src == it->dst)
                                                    blocks_other = true;
                                            if (!blocks_other)
                                            {
                                                result.push_back(*it);
                                                remaining.erase(it);
                                                picked = true;
                                                break;
                                            }
                                        }
                                        if (picked)
                                            continue;
                                        auto cycle = remaining.front();
                                        result.push_back({group_scratch, cycle.src, 0, bits});
                                        for (auto& rm : remaining)
                                            if (rm.src == cycle.src)
                                                rm.src = group_scratch;
                                    }
                                    if (!remaining.empty())
                                    {
                                        ok = false;
                                        return group;
                                    }
                                }
                                else
                                {
                                    for (auto const& m : rest)
                                        result.push_back(m);
                                }
                            }
                            else if (rest.size() == 1)
                            {
                                result.push_back(rest[0]);
                            }

                            for (auto const& sm : scratch_dst)
                                result.push_back(sm);

                            changed = true;
                            return result;
                        }
                    };

                    std::vector<std::vector<Move>> segments;
                    for (auto const& m : moves)
                    {
                        if (m.group != 0 && !segments.empty() && !segments.back().empty() && segments.back().front().group == m.group)
                            segments.back().push_back(m);
                        else
                            segments.push_back({m});
                    }

                    struct ResMove
                    {
                        Move mv;
                        bool is_gpr;
                    };
                    std::vector<ResMove> result;
                    bool all_ok = true;
                    bool any_changed = false;
                    std::uint32_t gpr_temp_slot = std::uint32_t(-1);
                    std::uint32_t xmm_temp_slot = std::uint32_t(-1);
                    for (auto const& seg : segments)
                    {
                        std::vector<Move> seg_gpr;
                        std::vector<Move> seg_xmm;
                        for (auto const& m : seg)
                            (reg_is_xmm(m.dst) ? seg_xmm : seg_gpr).push_back(m);

                        bool seg_gpr_ok = true;
                        bool seg_xmm_ok = true;
                        bool seg_gpr_changed = false;
                        bool seg_xmm_changed = false;
                        auto res_gpr = resolve_group(seg_gpr, scratch_gpr, regs.gpr.cycle_temp, seg_gpr_ok, seg_gpr_changed, gpr_temp_slot);
                        auto res_xmm = resolve_group(seg_xmm, scratch_xmm, regs.xmm.cycle_temp, seg_xmm_ok, seg_xmm_changed, xmm_temp_slot);

                        if (!seg_gpr_ok || !seg_xmm_ok)
                        {
                            all_ok = false;
                            break;
                        }
                        any_changed = any_changed || seg_gpr_changed || seg_xmm_changed;
                        for (auto const& m : res_gpr)
                            result.push_back({m, true});
                        for (auto const& m : res_xmm)
                            result.push_back({m, false});
                    }

                    if (!all_ok || !any_changed)
                    {
                        ++ii;
                        continue;
                    }

                    blk.instrs.erase(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start), blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_end));

                    for (std::size_t ri = 0; ri < result.size(); ++ri)
                    {
                        auto const& mv = result[ri].mv;
                        if (!mv.dst.is_valid())
                        {
                            bool is_gpr = result[ri].is_gpr;
                            std::uint32_t slot = is_gpr ? gpr_temp_slot : xmm_temp_slot;
                            MInstr store;
                            store.opc = is_gpr && regs.narrow_gpr && func.frame_slots[slot].size == 2 ? MOpc::MOV16mr
                                        : is_gpr                                                      ? regs.gpr.cycle_temp.store
                                                                                                      : regs.xmm.cycle_temp.store;
                            store.num_ops = 2;
                            store.num_defs = 0;
                            store.ops[0] = MOp::from_frame_slot(slot);
                            store.ops[1] = MOp::from_reg(mv.src);
                            blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start + ri), store);
                        }
                        else if (!mv.src.is_valid())
                        {
                            bool is_gpr = result[ri].is_gpr;
                            std::uint32_t slot = is_gpr ? gpr_temp_slot : xmm_temp_slot;
                            MInstr load;
                            load.opc = is_gpr && regs.narrow_gpr && func.frame_slots[slot].size == 2 ? MOpc::MOV16rm
                                       : is_gpr                                                      ? regs.gpr.cycle_temp.load
                                                                                                     : regs.xmm.cycle_temp.load;
                            load.num_ops = 2;
                            load.num_defs = 1;
                            load.ops[0] = MOp::from_reg(mv.dst);
                            load.ops[1] = MOp::from_frame_slot(slot);
                            blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start + ri), load);
                        }
                        else
                            blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start + ri), make_reg_move(regs, mv.dst, mv.src, mv.bits));
                    }

                    ii = run_start + result.size() + (is_call ? 1 : 0);
                }
            }
        }

        void post_check_and_fix(MFunction& func, RegisterPolicy const& regs)
        {
            VReg scratch_gpr = regs.scratch(RegClass::GPR);
            VReg scratch_xmm = regs.scratch(RegClass::XMM);
            auto xmm_vregs = infer_reg_classes(func);

            for (auto& blk : func.blocks)
            {
                for (std::size_t ii = 0; ii < blk.instrs.size();)
                {
                    auto& instr = blk.instrs[ii];

                    if (instr.opc == MOpc::PHI || instr.opc == MOpc::IMPLICIT_DEF)
                    {
                        blk.instrs.erase(blk.instrs.begin() + static_cast<std::ptrdiff_t>(ii));
                        continue;
                    }

                    bool has_virtual = false;
                    for (std::uint8_t oi = 0; oi < instr.num_ops; ++oi)
                    {
                        auto const& op = instr.ops[oi];
                        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                        {
                            has_virtual = true;
                            break;
                        }
                        if (op.kind == MOpKind::Mem)
                        {
                            if (op.mem.base.is_virtual() || op.mem.index.is_virtual())
                            {
                                has_virtual = true;
                                break;
                            }
                        }
                    }

                    if (!has_virtual)
                    {
                        ++ii;
                        continue;
                    }

                    for (std::uint8_t oi = 0; oi < instr.num_ops; ++oi)
                    {
                        auto& op = instr.ops[oi];
                        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                        {
                            RegClass rc = xmm_vregs.contains(op.reg) ? RegClass::XMM : RegClass::GPR;
                            op.reg = (rc == RegClass::XMM) ? scratch_xmm : scratch_gpr;
                        }
                        else if (op.kind == MOpKind::Mem)
                        {
                            if (op.mem.base.is_virtual())
                                op.mem.base = scratch_gpr;
                            if (op.mem.index.is_virtual())
                                op.mem.index = scratch_gpr;
                        }
                    }

                    ++ii;
                }
            }
        }

        void compact_spill_slots(MFunction& func)
        {
            std::vector<bool> used(func.frame_slots.size());
            for (auto const& block : func.blocks)
                for (auto const& instr : block.instrs)
                    for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                        if (instr.ops[i].kind == MOpKind::FrameSlot)
                            used.at(instr.ops[i].frame_slot) = true;
            std::vector<std::uint32_t> indices(func.frame_slots.size());
            std::vector<MFrameSlot> slots;
            for (std::size_t i = 0; i < func.frame_slots.size(); ++i)
                if (used[i] || !func.frame_slots[i].is_spill)
                {
                    indices[i] = static_cast<std::uint32_t>(slots.size());
                    slots.push_back(func.frame_slots[i]);
                }
            for (auto& block : func.blocks)
                for (auto& instr : block.instrs)
                    for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                        if (instr.ops[i].kind == MOpKind::FrameSlot)
                            instr.ops[i].frame_slot = indices[instr.ops[i].frame_slot];
            func.frame_slots = std::move(slots);
            func.split_spill_homes.clear();
        }

        void remove_redundant_moves(MFunction& func, RegisterPolicy const& regs)
        {
            auto is_self_move = [&](MInstr const& instr) {
                if (instr.opc != MOpc::COPY && instr.opc != MOpc::MOV64rr && !(regs.narrow_gpr && instr.opc == MOpc::MOV16rr))
                    return false;
                if (instr.num_ops != 2 || instr.ops[0].kind != MOpKind::Reg || instr.ops[1].kind != MOpKind::Reg)
                    return false;
                if (!instr.ops[0].reg.is_physical() || !instr.ops[1].reg.is_physical())
                    return false;
                return instr.ops[0].reg == instr.ops[1].reg;
            };

            for (auto& blk : func.blocks)
                std::erase_if(blk.instrs, is_self_move);
        }

        void finish_allocation(MFunction& func, RegisterPolicy const& regs, std::vector<LiveRange> const& ranges)
        {
            rewrite_function(func, regs, ranges);
            resolve_parallel_copies(func, regs);
            insert_callee_saves(func, regs, ranges);
            post_check_and_fix(func, regs);
            reuse_reloads(func);
            remove_redundant_moves(func, regs);
            compact_spill_slots(func);
        }

        std::pair<double, double> allocation_score(MFunction const& func, RegisterPolicy const& regs, std::span<double const> weights)
        {
            double dynamic = 0, size = 0;
            for (std::size_t b = 0; b < func.blocks.size(); ++b)
                for (auto const& instr : func.blocks[b].instrs)
                {
                    dynamic += weights[b];
                    size += 1 + (regs.narrow_gpr && operand_bits(instr.opc) == 32);
                }
            return {dynamic, size};
        }

    } // anonymous namespace

} // namespace dcc::backend::x86

export namespace dcc::backend::x86
{
    void allocate_registers(MFunction& func, RegisterPolicy const& regs)
    {
        eliminate_phis(func);

        auto widths = regs.narrow_gpr ? narrow_copy_widths(func) : std::unordered_map<VReg, std::uint8_t>{};
        std::vector<LiveRange> ranges;
        compute_liveness(func, ranges);
        if (regs.narrow_gpr)
            for (auto& range : ranges)
                range.bits = std::max<std::uint8_t>(16, widths[range.vreg]);

        auto costs = block_costs(func);
        allocation_costs(func, ranges, costs);

        auto frame_count = func.frame_slots.size();
        linear_scan(func, regs, ranges);
        if (std::ranges::none_of(ranges, [](auto const& range) { return range.spilled && range.end > range.start; }))
        {
            finish_allocation(func, regs, ranges);
            return;
        }
        MFunction whole;
        whole.conv = func.conv;
        whole.linkage = func.linkage;
        whole.is_variadic = func.is_variadic;
        whole.owned_name = func.owned_name;
        whole.blocks = func.blocks;
        whole.frame_slots = func.frame_slots;
        whole.entry_block_id = func.entry_block_id;
        whole.next_vreg_id = func.next_vreg_id;
        whole.next_copy_group = func.next_copy_group;
        whole.outgoing_args_size = func.outgoing_args_size;
        whole.register_pairs = func.register_pairs;
        finish_allocation(whole, regs, ranges);
        func.frame_slots.resize(frame_count);
        if (split_spilled_ranges(func, regs, ranges))
        {
            ranges.clear();
            compute_liveness(func, ranges);
            auto split_widths = regs.narrow_gpr ? narrow_copy_widths(func) : std::unordered_map<VReg, std::uint8_t>{};
            if (regs.narrow_gpr)
                for (auto& range : ranges)
                    range.bits = std::max<std::uint8_t>(16, split_widths[range.vreg]);
            allocation_costs(func, ranges, costs);
            linear_scan(func, regs, ranges, true);
            finish_allocation(func, regs, ranges);
            auto candidate = allocation_score(func, regs, costs);
            auto baseline = allocation_score(whole, regs, costs);
            if ((candidate.first < baseline.first && candidate.second <= baseline.second) ||
                (candidate.first <= baseline.first && candidate.second < baseline.second))
                return;
        }
        func.blocks = std::move(whole.blocks);
        func.frame_slots = std::move(whole.frame_slots);
        func.register_pairs = std::move(whole.register_pairs);
        func.split_spill_homes.clear();
        func.next_vreg_id = whole.next_vreg_id;
        func.next_copy_group = whole.next_copy_group;
    }

} // namespace dcc::backend::x86
