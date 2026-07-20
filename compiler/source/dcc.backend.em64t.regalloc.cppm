export module dcc.backend.em64t.regalloc;

import std;
import dcc.ir;
import dcc.backend.em64t.mir;
import dcc.target;

namespace dcc::backend::em64t
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

        [[nodiscard]] bool is_gpr_dest_xmm_opc(MOpc opc) noexcept
        {
            return opc == MOpc::CVTSD2SI64rr || opc == MOpc::CVTTSD2SI_r || opc == MOpc::CVTSD2SIrr || opc == MOpc::CVTSS2SI64rr || opc == MOpc::CVTSS2SIrr ||
                   opc == MOpc::MOVQ64rr_rev;
        }

        [[nodiscard]] RegClass infer_reg_class(MFunction const& func, VReg vreg, target::TargetConfig const& target)
        {
            for (auto const& blk : func.blocks)
            {
                for (auto const& instr : blk.instrs)
                {
                    if (is_xmm_opc(instr.opc))
                    {
                        if (is_gpr_dest_xmm_opc(instr.opc))
                        {
                            for (std::uint8_t i = instr.num_defs; i < instr.num_ops; ++i)
                                if (instr.ops[i].kind == MOpKind::Reg && instr.ops[i].reg == vreg)
                                    return RegClass::XMM;
                        }
                        else
                        {
                            for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                                if (instr.ops[i].kind == MOpKind::Reg && instr.ops[i].reg == vreg)
                                    return RegClass::XMM;
                        }
                    }

                    if (is_gpr_copy(instr.opc))
                        for (std::uint8_t i = 0; i < instr.num_ops; ++i)
                            if (instr.ops[i].kind == MOpKind::Reg && instr.ops[i].reg == vreg)
                                for (std::uint8_t j = 0; j < instr.num_ops; ++j)
                                    if (i != j && instr.ops[j].kind == MOpKind::Reg && instr.ops[j].reg.is_physical())
                                    {
                                        auto pr = instr.ops[j].reg.phys_reg();
                                        if (reg_class(pr) == RegClass::XMM)
                                            return RegClass::XMM;
                                    }

                    if (instr.opc == MOpc::MOVQ64rr)
                        if (instr.num_ops >= 2 && instr.ops[0].kind == MOpKind::Reg && instr.ops[0].reg == vreg)
                            return RegClass::XMM;

                    if (instr.opc == MOpc::MOVQ64rr_rev)
                        if (instr.num_ops >= 2 && instr.ops[1].kind == MOpKind::Reg && instr.ops[1].reg == vreg)
                            return RegClass::XMM;

                    for (int pi = 0; pi < static_cast<int>(PhysReg::Count); ++pi)
                    {
                        if (!(instr.implicit_defs & (1ULL << pi)) && !(instr.implicit_uses & (1ULL << pi)))
                            continue;

                        auto pr = static_cast<PhysReg>(pi);
                        if (reg_class(pr) != RegClass::XMM)
                            continue;

                        if (instr.ops[0].kind == MOpKind::Reg && instr.ops[0].reg == vreg)
                            return RegClass::XMM;
                        if (instr.num_ops > 0 && instr.ops[instr.num_ops - 1].kind == MOpKind::Reg && instr.ops[instr.num_ops - 1].reg == vreg)
                            return RegClass::XMM;
                    }
                }
            }
            (void)target;
            return RegClass::GPR64;
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
            RegClass reg_class = RegClass::GPR64;
            std::uint32_t start = (std::numeric_limits<std::uint32_t>::max)();
            std::uint32_t end = 0;
            PhysReg assigned = PhysReg::None;
            std::uint32_t spill_slot = (std::numeric_limits<std::uint32_t>::max)();
            bool spilled = false;
            bool crosses_call = false;
        };

        void compute_liveness(MFunction& func, target::TargetConfig const& target, std::vector<LiveRange>& ranges)
        {
            constexpr std::uint32_t kBlockStride = 512;

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
            std::unordered_map<VReg, std::uint32_t> last_use;

            for (std::size_t bi = 0; bi < func.blocks.size(); ++bi)
            {
                auto const& blk = func.blocks[bi];
                std::uint32_t base_pp = blk.id * kBlockStride;

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
                                }
                                else
                                {
                                    if (!bldef.contains(op.reg))
                                        bluse.insert(op.reg);
                                    std::uint32_t use_pp = pp;
                                    if (instr.opc == MOpc::SUB64rr || instr.opc == MOpc::SUB32rr)
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
                            if (op.mem.base.is_virtual())
                            {
                                if (!bldef.contains(op.mem.base))
                                    bluse.insert(op.mem.base);

                                auto it = last_use.find(op.mem.base);
                                if (it == last_use.end() || pp > it->second)
                                    last_use[op.mem.base] = pp;
                            }
                            if (op.mem.index.is_virtual())
                            {
                                if (!bldef.contains(op.mem.index))
                                    bluse.insert(op.mem.index);

                                auto it = last_use.find(op.mem.index);
                                if (it == last_use.end() || pp > it->second)
                                    last_use[op.mem.index] = pp;
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

                for (auto const& blk : func.blocks)
                {
                    for (auto v : blk.live_out)
                    {
                        if (v == vreg)
                        {
                            std::uint32_t blk_end = (blk.id * kBlockStride) + static_cast<std::uint32_t>(blk.instrs.size()) - 1;
                            lr.end = std::max(blk_end, lr.end);
                        }
                    }
                    for (auto v : blk.live_in)
                    {
                        if (v == vreg && lr.start > blk.id * kBlockStride)
                            lr.start = blk.id * kBlockStride;
                    }
                }

                lr.start = std::min(lr.start, lr.end);
                lr.reg_class = infer_reg_class(func, vreg, target);

                ranges.push_back(lr);
            }

            for (std::size_t bi = 0; bi < func.blocks.size(); ++bi)
            {
                auto const& blk = func.blocks[bi];
                std::uint32_t base_pp = blk.id * kBlockStride;

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
                        if (lr.start < call_point && lr.end > call_point)
                            lr.crosses_call = true;
                }
            }

            std::ranges::sort(ranges, [](LiveRange const& a, LiveRange const& b) { return a.start < b.start; });
        }

        [[nodiscard]] bool is_win64(MFunction const& func, target::TargetConfig const& target)
        {
            if (target.os == dcc::target::Os::Windows)
                return true;

            if (target.object_format == dcc::target::ObjectFormat::Coff)
                return true;

            if (func.conv == ir::CallingConv::Win64)
                return true;

            return false;
        }

        struct RegSetInfo
        {
            std::span<PhysReg const> gprs;
            std::span<PhysReg const> xmms;
            bool is_win64;
        };

        constexpr PhysReg kSysVGPR[] = {
            PhysReg::RAX, PhysReg::RCX, PhysReg::RDX, PhysReg::RSI, PhysReg::RDI, PhysReg::R8,  PhysReg::R9,
            PhysReg::R10, PhysReg::RBX, PhysReg::R12, PhysReg::R13, PhysReg::R14, PhysReg::R15,
        };

        constexpr PhysReg kWin64GPR[] = {
            PhysReg::RAX, PhysReg::RCX, PhysReg::RDX, PhysReg::R8,  PhysReg::R9,  PhysReg::R10, PhysReg::RBX,
            PhysReg::RDI, PhysReg::RSI, PhysReg::R12, PhysReg::R13, PhysReg::R14, PhysReg::R15,
        };

        constexpr PhysReg kXMM[] = {
            PhysReg::XMM0, PhysReg::XMM1, PhysReg::XMM2,  PhysReg::XMM3,  PhysReg::XMM4,  PhysReg::XMM5,  PhysReg::XMM6,  PhysReg::XMM7,
            PhysReg::XMM8, PhysReg::XMM9, PhysReg::XMM10, PhysReg::XMM11, PhysReg::XMM12, PhysReg::XMM13, PhysReg::XMM14,
        };

        constexpr PhysReg kSysVCalleeSavedGPR[] = {
            PhysReg::RBX, PhysReg::R12, PhysReg::R13, PhysReg::R14, PhysReg::R15,
        };

        constexpr PhysReg kWin64CalleeSavedGPR[] = {
            PhysReg::RBX, PhysReg::RDI, PhysReg::RSI, PhysReg::R12, PhysReg::R13, PhysReg::R14, PhysReg::R15,
        };
        [[nodiscard]] RegSetInfo get_reg_set(MFunction const& func, target::TargetConfig const& target)
        {
            bool w64 = is_win64(func, target);
            return RegSetInfo{
                .gprs = w64 ? std::span<PhysReg const>{kWin64GPR} : std::span<PhysReg const>{kSysVGPR},
                .xmms = std::span<PhysReg const>{kXMM},
                .is_win64 = w64,
            };
        }

        [[nodiscard]] bool is_callee_saved_gpr(PhysReg r, bool win64)
        {
            auto span = win64 ? std::span<PhysReg const>{kWin64CalleeSavedGPR} : std::span<PhysReg const>{kSysVCalleeSavedGPR};
            return std::ranges::find(span, r) != span.end();
        }

        void linear_scan(MFunction& func, target::TargetConfig const& target, std::vector<LiveRange>& ranges)
        {
            auto regs = get_reg_set(func, target);

            auto gprs = regs.gprs;
            auto xmms = regs.xmms;

            std::unordered_set<VReg> setcc_defs;
            for (auto const& blk : func.blocks)
                for (auto const& instr : blk.instrs)
                    if (is_setcc(instr.opc) && instr.num_defs > 0 && instr.num_ops > 0 && instr.ops[0].kind == MOpKind::Reg && instr.ops[0].reg.is_virtual())
                        setcc_defs.insert(instr.ops[0].reg);

            std::vector<LiveRange*> active;

            for (auto& range : ranges)
            {
                auto new_end = std::remove_if(active.begin(), active.end(), [&](LiveRange* a) { return a->end <= range.start; });

                active.erase(new_end, active.end());

                std::ranges::sort(active, [](LiveRange const* a, LiveRange const* b) { return a->end < b->end; });

                auto const& avail = (range.reg_class == RegClass::XMM) ? xmms : gprs;
                auto reg_is_allowed = [&](PhysReg reg) { return !setcc_defs.contains(range.vreg) || (reg != PhysReg::RSI && reg != PhysReg::RDI); };

                std::unordered_set<PhysReg> occupied;
                for (auto* a : active)
                    if (a->reg_class == range.reg_class && a->assigned != PhysReg::None)
                        occupied.insert(a->assigned);

                PhysReg free_reg = PhysReg::None;

                auto const callee_gpr_span = regs.is_win64 ? std::span<PhysReg const>{kWin64CalleeSavedGPR} : std::span<PhysReg const>{kSysVCalleeSavedGPR};

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
                        if (a->reg_class != range.reg_class)
                            continue;
                        if (!reg_is_allowed(a->assigned))
                            continue;

                        if (range.crosses_call)
                        {
                            if (a->assigned == PhysReg::None)
                                continue;
                            if (range.reg_class == RegClass::XMM)
                                continue;
                            if (!is_callee_saved_gpr(a->assigned, regs.is_win64))
                                continue;
                        }

                        if (!spill_candidate || a->end > spill_candidate->end)
                            spill_candidate = a;
                    }

                    if (spill_candidate && spill_candidate->end > range.end)
                    {
                        if (spill_candidate->spill_slot == (std::numeric_limits<std::uint32_t>::max)())
                            spill_candidate->spill_slot = func.new_frame_slot(8, 8, true);

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
                        if (range.spill_slot == (std::numeric_limits<std::uint32_t>::max)())
                            range.spill_slot = func.new_frame_slot(8, 8, true);

                        range.spilled = true;
                        range.assigned = PhysReg::None;
                    }

                    std::ranges::sort(active, [](LiveRange const* a, LiveRange const* b) { return a->end < b->end; });
                }
            }
        }

        void rewrite_function(MFunction& func, std::vector<LiveRange> const& ranges)
        {
            std::unordered_map<VReg, LiveRange const*> range_map;
            for (auto const& r : ranges)
                range_map[r.vreg] = &r;

            VReg scratch_gpr = VReg::phys(PhysReg::R11);
            VReg scratch_xmm = VReg::phys(PhysReg::XMM15);

            for (auto& blk : func.blocks)
            {
                std::vector<MInstr> new_instrs;
                new_instrs.reserve(blk.instrs.size() * 2);

                for (std::size_t ii = 0; ii < blk.instrs.size(); ++ii)
                {
                    auto& instr = blk.instrs[ii];

                    if (instr.opc == MOpc::PHI)
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

                    struct ReloadInfo
                    {
                        VReg spilled_vreg;
                        PhysReg scratch_reg;
                        RegClass rc;
                    };

                    bool is_jump_table = (instr.opc == MOpc::JUMP_TABLE);
                    PhysReg use_scratch = is_jump_table ? PhysReg::R10 : PhysReg::R11;

                    std::vector<ReloadInfo> reloads_needed;
                    std::vector<unsigned> spilled_def_indices;

                    for (std::uint8_t oi = instr.num_defs; oi < instr.num_ops; ++oi)
                    {
                        auto const& op = instr.ops[oi];
                        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                        {
                            auto it = range_map.find(op.reg);
                            if (it != range_map.end() && it->second->spilled)
                                reloads_needed.push_back({op.reg, use_scratch, RegClass::GPR64});
                        }
                        else if (op.kind == MOpKind::Mem)
                        {
                            if (op.mem.base.is_virtual())
                            {
                                auto it = range_map.find(op.mem.base);
                                if (it != range_map.end() && it->second->spilled)
                                    reloads_needed.push_back({op.mem.base, use_scratch, RegClass::GPR64});
                            }
                            if (op.mem.index.is_virtual())
                            {
                                auto it = range_map.find(op.mem.index);
                                if (it != range_map.end() && it->second->spilled)
                                    reloads_needed.push_back({op.mem.index, use_scratch, RegClass::GPR64});
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

                    for (auto const& rl : reloads_needed)
                    {
                        auto rit = range_map.find(rl.spilled_vreg);
                        if (rit == range_map.end())
                            continue;

                        auto const& lr = *rit->second;
                        if (lr.spill_slot == (std::numeric_limits<std::uint32_t>::max)())
                            continue;

                        VReg scratch = (lr.reg_class == RegClass::XMM) ? scratch_xmm : scratch_gpr;

                        MInstr reload;
                        if (lr.reg_class == RegClass::XMM)
                        {
                            reload.opc = MOpc::MOVSD_rm;
                            reload.num_ops = 2;
                            reload.num_defs = 1;
                            reload.ops[0] = MOp::from_reg(scratch);
                            reload.ops[1] = MOp::from_frame_slot(lr.spill_slot);
                        }
                        else
                        {
                            reload.opc = MOpc::MOV64rm;
                            reload.num_ops = 2;
                            reload.num_defs = 1;
                            reload.ops[0] = MOp::from_reg(scratch);
                            reload.ops[1] = MOp::from_frame_slot(lr.spill_slot);
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

                        VReg scratch = (it->second->reg_class == RegClass::XMM) ? scratch_xmm : scratch_gpr;
                        new_instr.ops[idx].reg = scratch;
                    }

                    {
                        VReg jt_scratch_gpr = is_jump_table ? VReg::phys(PhysReg::R10) : scratch_gpr;
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
                                        VReg scratch = (it->second->reg_class == RegClass::XMM) ? scratch_xmm : jt_scratch_gpr;
                                        op.reg = scratch;
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
                                            op.mem.base = jt_scratch_gpr;
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
                                            op.mem.index = jt_scratch_gpr;
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
                        new_instrs.push_back(make_mov_ri(new_instr.ops[0].reg, 0, 32));

                    new_instrs.push_back(new_instr);

                    for (auto idx : spilled_def_indices)
                    {
                        VReg v = instr.ops[idx].reg;
                        auto it = range_map.find(v);
                        if (it == range_map.end() || it->second->spill_slot == (std::numeric_limits<std::uint32_t>::max)())
                            continue;

                        auto const& lr = *it->second;
                        VReg scratch = (lr.reg_class == RegClass::XMM) ? scratch_xmm : scratch_gpr;

                        MInstr spill;
                        if (lr.reg_class == RegClass::XMM)
                        {
                            spill.opc = MOpc::MOVSD_mr;
                            spill.num_ops = 2;
                            spill.num_defs = 0;
                            spill.ops[0] = MOp::from_frame_slot(lr.spill_slot);
                            spill.ops[1] = MOp::from_reg(scratch);
                        }
                        else
                        {
                            spill.opc = MOpc::MOV64mr;
                            spill.num_ops = 2;
                            spill.num_defs = 0;
                            spill.ops[0] = MOp::from_frame_slot(lr.spill_slot);
                            spill.ops[1] = MOp::from_reg(scratch);
                        }
                        new_instrs.push_back(spill);
                    }
                }

                blk.instrs = std::move(new_instrs);
            }
        }

        void insert_callee_saves(MFunction& func, target::TargetConfig const& target, std::vector<LiveRange> const& ranges)
        {
            bool w64 = is_win64(func, target);

            std::vector<PhysReg> used_callee_saves;

            for (auto const& lr : ranges)
            {
                if (lr.spilled || lr.assigned == PhysReg::None)
                    continue;

                PhysReg pr = lr.assigned;
                if (lr.reg_class == RegClass::GPR64 && is_callee_saved_gpr(pr, w64))
                {
                    if (std::ranges::find(used_callee_saves, pr) == used_callee_saves.end())
                        used_callee_saves.push_back(pr);
                }
            }

            if (used_callee_saves.empty())
                return;

            std::ranges::sort(used_callee_saves, [](PhysReg a, PhysReg b) { return static_cast<int>(a) < static_cast<int>(b); });

            auto& entry = func.entry_block();

            for (auto pr : used_callee_saves)
            {
                MInstr push;
                push.opc = MOpc::PUSH64r;
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
                        pop.opc = MOpc::POP64r;
                        pop.num_ops = 1;
                        pop.num_defs = 1;
                        pop.ops[0] = MOp::from_reg(VReg::phys(pr));
                        blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(insert_at), pop);
                    }

                    i += num_pops;
                }
            }
        }

        [[nodiscard]] bool is_reg_move_opc(MOpc opc) noexcept
        {
            return opc == MOpc::COPY || opc == MOpc::MOVSDrr || opc == MOpc::MOVSSrr || opc == MOpc::MOVAPSrr;
        }

        [[nodiscard]] MInstr make_reg_move(VReg dst, VReg src)
        {
            if (dst.is_physical() && reg_class(dst.phys_reg()) == RegClass::XMM)
            {
                MInstr mi;
                mi.opc = MOpc::MOVSDrr;
                mi.num_ops = 2;
                mi.num_defs = 1;
                mi.ops[0] = MOp::from_reg(dst);
                mi.ops[1] = MOp::from_reg(src);
                return mi;
            }

            return make_copy(dst, src);
        }

        [[nodiscard]] bool is_phys_reg_copy(MInstr const& c) noexcept
        {
            return is_reg_move_opc(c.opc) && c.num_ops >= 2 && c.ops[0].kind == MOpKind::Reg && c.ops[1].kind == MOpKind::Reg && !c.ops[0].reg.is_virtual() &&
                   !c.ops[1].reg.is_virtual();
        }

        [[nodiscard]] bool is_frame_reload(MInstr const& c) noexcept
        {
            switch (c.opc)
            {
                case MOpc::MOV64rm:
                case MOpc::MOV32rm:
                case MOpc::MOVSD_rm:
                case MOpc::MOVSS_rm:
                case MOpc::MOVSDrm:
                case MOpc::MOVSSrm:
                    break;
                default:
                    return false;
            }

            if (c.num_ops < 2 || c.num_defs != 1)
                return false;
            if (c.ops[0].kind != MOpKind::Reg || c.ops[0].reg.is_virtual())
                return false;
            if (c.ops[1].kind == MOpKind::FrameSlot)
                return true;

            if (c.ops[1].kind != MOpKind::Mem)
                return false;

            auto const& m = c.ops[1].mem;
            if (m.index.is_valid() || !m.base.is_valid() || m.base.is_virtual())
                return false;

            auto base = m.base.phys_reg();
            return base == PhysReg::RBP || base == PhysReg::RSP;
        }

        void sequence_call_arg_setup(MFunction& func)
        {
            VReg const scratch = VReg::phys(PhysReg::R11);
            VReg const scratch_xmm_reg = VReg::phys(PhysReg::XMM15);

            for (auto& blk : func.blocks)
            {
                for (std::size_t ii = 0; ii < blk.instrs.size(); ++ii)
                {
                    switch (blk.instrs[ii].opc)
                    {
                        case MOpc::CALL:
                        case MOpc::CALL_rel32:
                        case MOpc::CALL_r64:
                        case MOpc::CALLm:
                            break;
                        default:
                            continue;
                    }

                    std::size_t span_start = ii;
                    while (span_start > 0 && (is_phys_reg_copy(blk.instrs[span_start - 1]) || is_frame_reload(blk.instrs[span_start - 1])))
                        --span_start;

                    bool has_reload = false;
                    for (std::size_t j = span_start; j < ii; ++j)
                        if (is_frame_reload(blk.instrs[j]))
                            has_reload = true;

                    if (!has_reload)
                        continue;

                    struct Item
                    {
                        VReg dst;
                        VReg src;
                        bool has_src{};
                        std::size_t reload_at{};
                        bool has_reload{};
                    };

                    std::vector<Item> items;
                    std::unordered_map<VReg, std::size_t> pending_reload;
                    bool usable = true;

                    for (std::size_t j = span_start; j < ii && usable; ++j)
                    {
                        auto const& c = blk.instrs[j];
                        if (is_frame_reload(c))
                        {
                            auto def = c.ops[0].reg;
                            if (pending_reload.contains(def))
                                usable = false;
                            else
                                pending_reload.emplace(def, j);
                            continue;
                        }

                        Item it{};
                        it.dst = c.ops[0].reg;
                        it.src = c.ops[1].reg;
                        if (auto found = pending_reload.find(it.src); found != pending_reload.end())
                        {
                            it.has_reload = true;
                            it.reload_at = found->second;
                            pending_reload.erase(found);
                        }
                        else
                            it.has_src = true;

                        items.push_back(it);
                    }

                    if (!usable || !pending_reload.empty() || items.size() < 2)
                        continue;

                    std::vector<Item> plain;
                    std::vector<Item> reloaded;
                    for (auto const& it : items)
                    {
                        if (it.has_reload)
                            reloaded.push_back(it);
                        else if (it.dst != it.src)
                            plain.push_back(it);
                    }

                    auto is_xmm_reg = [](VReg r) { return r.is_physical() && reg_class(r.phys_reg()) == RegClass::XMM; };

                    std::vector<Item> plain_gpr;
                    std::vector<Item> plain_xmm;
                    for (auto const& it : plain)
                        (is_xmm_reg(it.dst) ? plain_xmm : plain_gpr).push_back(it);

                    bool scratch_conflict = false;
                    for (auto const& it : plain_gpr)
                        if (it.dst == scratch || it.src == scratch)
                            scratch_conflict = true;
                    for (auto const& it : plain_xmm)
                        if (it.dst == scratch_xmm_reg || it.src == scratch_xmm_reg)
                            scratch_conflict = true;
                    for (auto const& it : reloaded)
                        if (it.dst == scratch || it.dst == scratch_xmm_reg)
                            scratch_conflict = true;

                    if (scratch_conflict)
                        continue;

                    auto order_group = [](std::vector<Item> remaining, VReg group_scratch, bool& ok) {
                        std::vector<Item> ordered;
                        ordered.reserve(remaining.size());
                        int guard = static_cast<int>(remaining.size()) * 4 + 4;
                        while (!remaining.empty() && guard-- > 0)
                        {
                            bool picked = false;
                            for (auto it = remaining.begin(); it != remaining.end(); ++it)
                            {
                                bool blocks_other = false;
                                for (auto const& other : remaining)
                                    if (&other != &*it && other.has_src && other.src == it->dst)
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
                            Item breaker{};
                            breaker.dst = group_scratch;
                            breaker.src = cycle.src;
                            breaker.has_src = true;
                            ordered.push_back(breaker);
                            for (auto& rm : remaining)
                                if (rm.has_src && rm.src == cycle.src)
                                    rm.src = group_scratch;
                        }

                        ok = remaining.empty();
                        return ordered;
                    };

                    bool gpr_ok = false;
                    bool xmm_ok = false;
                    auto ordered = order_group(plain_gpr, scratch, gpr_ok);
                    auto ordered_xmm = order_group(plain_xmm, scratch_xmm_reg, xmm_ok);
                    if (!gpr_ok || !xmm_ok)
                        continue;

                    ordered.insert(ordered.end(), ordered_xmm.begin(), ordered_xmm.end());

                    std::vector<MInstr> out;
                    out.reserve(ii - span_start);
                    for (auto const& it : ordered)
                        out.push_back(make_reg_move(it.dst, it.src));
                    for (auto const& it : reloaded)
                    {
                        out.push_back(blk.instrs[it.reload_at]);
                        out.push_back(make_reg_move(it.dst, blk.instrs[it.reload_at].ops[0].reg));
                    }

                    blk.instrs.erase(blk.instrs.begin() + static_cast<std::ptrdiff_t>(span_start), blk.instrs.begin() + static_cast<std::ptrdiff_t>(ii));
                    blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(span_start), out.begin(), out.end());
                    ii = span_start + out.size();
                }
            }
        }

        void resolve_parallel_copies(MFunction& func)
        {
            VReg scratch_gpr = VReg::phys(PhysReg::R11);
            VReg scratch_xmm = VReg::phys(PhysReg::XMM15);

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

                    auto is_phys_copy = [&](MInstr const& c) { return is_phys_reg_copy(c); };

                    bool is_prologue = (blk.id == func.entry_block_id) && ii == 0 && is_phys_copy(instr);

                    if (!is_call && !is_prologue)
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
                        while (run_end < blk.instrs.size() && is_phys_copy(blk.instrs[run_end]))
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
                    };
                    auto reg_is_xmm = [](VReg r) { return r.is_physical() && reg_class(r.phys_reg()) == RegClass::XMM; };

                    std::vector<Move> moves_gpr;
                    std::vector<Move> moves_xmm;
                    for (std::size_t j = run_start; j < run_end; ++j)
                    {
                        auto const& c = blk.instrs[j];
                        if (c.ops[0].reg == c.ops[1].reg)
                            continue;
                        (reg_is_xmm(c.ops[0].reg) ? moves_xmm : moves_gpr).push_back({c.ops[0].reg, c.ops[1].reg});
                    }

                    if (moves_gpr.size() + moves_xmm.size() < 2)
                    {
                        ++ii;
                        continue;
                    }

                    auto resolve_group = [&func](std::vector<Move> group, VReg group_scratch, bool& ok, bool& changed, std::uint32_t& out_temp_slot) {
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
                            if (m.dst == group_scratch) scratch_is_dst = true;
                            if (m.src == group_scratch) scratch_is_src = true;
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
                                ordered.push_back({group_scratch, cycle.src});
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
                            out_temp_slot = func.new_frame_slot(8, 8, false);

                            std::vector<Move> result;
                            for (auto const& sm : scratch_dst)
                                result.push_back({VReg{}, sm.src});

                            if (rest.size() >= 2)
                            {
                                std::unordered_set<VReg> rd;
                                for (auto const& m : rest) rd.insert(m.dst);
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
                                        if (picked) continue;
                                        auto cycle = remaining.front();
                                        result.push_back({group_scratch, cycle.src});
                                        for (auto& rm : remaining)
                                            if (rm.src == cycle.src)
                                                rm.src = group_scratch;
                                    }
                                    if (!remaining.empty()) { ok = false; return group; }
                                }
                                else
                                {
                                    for (auto const& m : rest) result.push_back(m);
                                }
                            }
                            else if (rest.size() == 1)
                                result.push_back(rest[0]);

                            for (std::size_t si = 0; si < scratch_dst.size(); ++si)
                                result.push_back({group_scratch, VReg{}});

                            changed = true;
                            return result;
                        }
                        else
                        {
                            std::vector<Move> result;
                            if (rest.size() >= 2)
                            {
                                std::unordered_set<VReg> rd;
                                for (auto const& m : rest) rd.insert(m.dst);
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
                                        if (picked) continue;
                                        auto cycle = remaining.front();
                                        result.push_back({group_scratch, cycle.src});
                                        for (auto& rm : remaining)
                                            if (rm.src == cycle.src)
                                                rm.src = group_scratch;
                                    }
                                    if (!remaining.empty()) { ok = false; return group; }
                                }
                                else
                                {
                                    for (auto const& m : rest) result.push_back(m);
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

                    bool gpr_ok = true;
                    bool xmm_ok = true;
                    bool gpr_changed = false;
                    bool xmm_changed = false;
                    std::uint32_t gpr_temp_slot = std::uint32_t(-1);
                    std::uint32_t xmm_temp_slot = std::uint32_t(-1);
                    auto res_gpr = resolve_group(moves_gpr, scratch_gpr, gpr_ok, gpr_changed, gpr_temp_slot);
                    auto res_xmm = resolve_group(moves_xmm, scratch_xmm, xmm_ok, xmm_changed, xmm_temp_slot);

                    if (!gpr_ok || !xmm_ok || (!gpr_changed && !xmm_changed))
                    {
                        ++ii;
                        continue;
                    }

                    std::size_t gpr_result_size = res_gpr.size();
                    std::vector<Move> result = res_gpr;
                    result.insert(result.end(), res_xmm.begin(), res_xmm.end());

                    blk.instrs.erase(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start), blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_end));

                    for (std::size_t ri = 0; ri < result.size(); ++ri)
                    {
                        auto const& mv = result[ri];
                        if (!mv.dst.is_valid())
                        {
                            bool is_gpr = ri < gpr_result_size;
                            std::uint32_t slot = is_gpr ? gpr_temp_slot : xmm_temp_slot;
                            MInstr store;
                            store.opc = is_gpr ? MOpc::MOV64mr : MOpc::MOVSDmr;
                            store.num_ops = 2;
                            store.num_defs = 0;
                            store.ops[0] = MOp::from_frame_slot(slot);
                            store.ops[1] = MOp::from_reg(mv.src);
                            blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start + ri), store);
                        }
                        else if (!mv.src.is_valid())
                        {
                            bool is_gpr = ri < gpr_result_size;
                            std::uint32_t slot = is_gpr ? gpr_temp_slot : xmm_temp_slot;
                            MInstr load;
                            load.opc = is_gpr ? MOpc::MOV64rm : MOpc::MOVSD_rm;
                            load.num_ops = 2;
                            load.num_defs = 1;
                            load.ops[0] = MOp::from_reg(mv.dst);
                            load.ops[1] = MOp::from_frame_slot(slot);
                            blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start + ri), load);
                        }
                        else
                            blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start + ri), make_reg_move(mv.dst, mv.src));
                    }

                    ii = run_start + result.size() + (is_call ? 1 : 0);
                }
            }
        }

        void post_check_and_fix(MFunction& func, target::TargetConfig const& target)
        {
            VReg scratch_gpr = VReg::phys(PhysReg::R11);
            VReg scratch_xmm = VReg::phys(PhysReg::XMM15);

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
                            RegClass rc = infer_reg_class(func, op.reg, target);
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

    } // anonymous namespace

} // namespace dcc::backend::em64t

export namespace dcc::backend::em64t
{
    void regalloc(MFunction& func, target::TargetConfig const& target)
    {
        eliminate_phis(func);

        std::vector<LiveRange> ranges;
        compute_liveness(func, target, ranges);

        linear_scan(func, target, ranges);

        rewrite_function(func, ranges);

        sequence_call_arg_setup(func);
        resolve_parallel_copies(func);

        insert_callee_saves(func, target, ranges);

        post_check_and_fix(func, target);
    }

} // namespace dcc::backend::em64t
