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

        [[nodiscard]] bool is_gpr_dest_xmm_opc(MOpc opc) noexcept
        {
            return opc == MOpc::CVTSD2SI64rr || opc == MOpc::CVTTSD2SI_r ||
                   opc == MOpc::CVTSD2SIrr || opc == MOpc::CVTSS2SI64rr ||
                   opc == MOpc::CVTSS2SIrr;
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
                    if ((mi.opc == MOpc::MOV32ri || mi.opc == MOpc::MOV64ri) &&
                        mi.num_ops >= 2 && mi.ops[0].kind == MOpKind::Reg && mi.ops[0].reg.is_virtual() &&
                        mi.ops[1].kind == MOpKind::Imm64)
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

                    std::unordered_set<VReg> dst_set;
                    std::unordered_set<VReg> src_set;
                    for (auto const& [dst, src] : copies)
                    {
                        dst_set.insert(dst);
                        src_set.insert(src);
                    }

                    bool has_conflict = false;
                    for (auto const& [dst, _] : copies)
                    {
                        if (src_set.contains(dst))
                        {
                            has_conflict = true;
                            break;
                        }
                    }

                    auto emit_phi_copy = [&](VReg dst, VReg src) {
                        auto it = merge_block_movris.find(src);
                        if (it != merge_block_movris.end())
                        {
                            MInstr new_mov = it->second;
                            new_mov.ops[0] = MOp::from_reg(dst);
                            pred->instrs.insert(pred->instrs.begin() + static_cast<std::ptrdiff_t>(insert_pos), new_mov);
                        }
                        else
                        {
                            auto copy_instr = make_copy(dst, src);
                            pred->instrs.insert(pred->instrs.begin() + static_cast<std::ptrdiff_t>(insert_pos), copy_instr);
                        }
                    };

                    if (has_conflict)
                    {
                        std::vector<std::pair<VReg, VReg>> delayed;

                        for (auto const& [dst, src] : copies)
                        {
                            if (src_set.contains(dst))
                            {
                                VReg tmp = func.new_vreg();
                                emit_phi_copy(tmp, src);
                                delayed.emplace_back(dst, tmp);
                            }
                            else
                            {
                                emit_phi_copy(dst, src);
                            }
                        }

                        for (auto const& [dst, tmp] : delayed)
                        {
                            emit_phi_copy(dst, tmp);
                        }
                    }
                    else
                    {
                        for (auto const& [dst, src] : copies)
                        {
                            emit_phi_copy(dst, src);
                        }
                    }
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
                    if ((mi.opc == MOpc::MOV32ri || mi.opc == MOpc::MOV64ri) &&
                        mi.num_ops >= 1 && mi.ops[0].kind == MOpKind::Reg &&
                        phi_source_vregs.contains(mi.ops[0].reg))
                    {
                        blk.instrs.erase(blk.instrs.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                    else
                    {
                        ++i;
                    }
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
                                    auto it = last_use.find(op.reg);
                                    if (it == last_use.end() || pp > it->second)
                                        last_use[op.reg] = pp;
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

            std::vector<LiveRange*> active;

            for (auto& range : ranges)
            {
                auto new_end = std::remove_if(active.begin(), active.end(), [&](LiveRange* a) { return a->end <= range.start; });

                active.erase(new_end, active.end());

                std::ranges::sort(active, [](LiveRange const* a, LiveRange const* b) { return a->end < b->end; });

                auto const& avail = (range.reg_class == RegClass::XMM) ? xmms : gprs;

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
                        if (!occupied.contains(pr) && std::ranges::find(callee_gpr_span, pr) != callee_gpr_span.end())
                        {
                            free_reg = pr;
                            break;
                        }
                }
                else
                {
                    for (auto pr : avail)
                        if (!occupied.contains(pr))
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

                    std::vector<ReloadInfo> reloads_needed;
                    std::vector<unsigned> spilled_def_indices;

                    for (std::uint8_t oi = instr.num_defs; oi < instr.num_ops; ++oi)
                    {
                        auto const& op = instr.ops[oi];
                        if (op.kind == MOpKind::Reg && op.reg.is_virtual())
                        {
                            auto it = range_map.find(op.reg);
                            if (it != range_map.end() && it->second->spilled)
                                reloads_needed.push_back({op.reg, PhysReg::R11, RegClass::GPR64});
                        }
                        else if (op.kind == MOpKind::Mem)
                        {
                            if (op.mem.base.is_virtual())
                            {
                                auto it = range_map.find(op.mem.base);
                                if (it != range_map.end() && it->second->spilled)
                                    reloads_needed.push_back({op.mem.base, PhysReg::R11, RegClass::GPR64});
                            }
                            if (op.mem.index.is_virtual())
                            {
                                auto it = range_map.find(op.mem.index);
                                if (it != range_map.end() && it->second->spilled)
                                    reloads_needed.push_back({op.mem.index, PhysReg::R11, RegClass::GPR64});
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
                                    VReg scratch = (it->second->reg_class == RegClass::XMM) ? scratch_xmm : scratch_gpr;
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
                                        op.mem.base = scratch_gpr;
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
                                        op.mem.index = scratch_gpr;
                                    else if (it->second->assigned != PhysReg::None)
                                        op.mem.index = VReg::phys(it->second->assigned);
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

                    if (!is_call)
                    {
                        ++ii;
                        continue;
                    }

                    std::size_t run_start = ii;
                    while (run_start > 0)
                    {
                        auto const& prev = blk.instrs[run_start - 1];
                        if (prev.opc != MOpc::COPY)
                            break;
                        if (prev.num_ops < 2)
                            break;
                        if (prev.ops[0].kind != MOpKind::Reg || prev.ops[1].kind != MOpKind::Reg)
                            break;
                        if (prev.ops[0].reg.is_virtual() || prev.ops[1].reg.is_virtual())
                            break;
                        --run_start;
                    }

                    std::size_t run_len = ii - run_start;
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
                    std::vector<Move> moves;
                    moves.reserve(run_len);
                    for (std::size_t j = run_start; j < ii; ++j)
                    {
                        auto const& c = blk.instrs[j];
                        if (c.ops[0].reg == c.ops[1].reg)
                            continue;
                        moves.push_back({c.ops[0].reg, c.ops[1].reg});
                    }

                    if (moves.size() < 2)
                    {
                        ++ii;
                        continue;
                    }

                    bool has_conflict = false;
                    {
                        std::unordered_set<VReg> dsts;
                        for (auto const& m : moves)
                            dsts.insert(m.dst);
                        for (auto const& m : moves)
                        {
                            if (dsts.contains(m.src) && m.src != m.dst)
                            {
                                has_conflict = true;
                                break;
                            }
                        }
                    }

                    if (!has_conflict)
                    {
                        ++ii;
                        continue;
                    }

                    bool has_xmm = false;
                    for (auto const& m : moves)
                    {
                        if (m.dst.is_physical() && reg_class(m.dst.phys_reg()) == RegClass::XMM)
                        {
                            has_xmm = true;
                            break;
                        }
                        if (m.src.is_physical() && reg_class(m.src.phys_reg()) == RegClass::XMM)
                        {
                            has_xmm = true;
                            break;
                        }
                    }
                    VReg scratch = has_xmm ? scratch_xmm : scratch_gpr;

                    bool scratch_is_dst = false;
                    for (auto const& m : moves)
                    {
                        if (m.dst == scratch)
                        {
                            scratch_is_dst = true;
                            break;
                        }
                    }
                    if (scratch_is_dst)
                    {
                        ++ii;
                        continue;
                    }

                    struct MoveNode
                    {
                        VReg dst;
                        VReg src;
                    };
                    std::vector<MoveNode> result;
                    std::vector<MoveNode> remaining;
                    remaining.reserve(moves.size());
                    for (auto const& m : moves)
                        remaining.push_back({m.dst, m.src});

                    constexpr int kMaxIterFactor = 4;
                    int max_iter = static_cast<int>(remaining.size()) * kMaxIterFactor;
                    for (int iter = 0; iter < max_iter && !remaining.empty(); ++iter)
                    {
                        bool found = false;
                        for (auto it = remaining.begin(); it != remaining.end(); ++it)
                        {
                            bool dst_is_src_of_other = false;
                            for (auto const& rm : remaining)
                            {
                                if (&*it != &rm && rm.src == it->dst)
                                {
                                    dst_is_src_of_other = true;
                                    break;
                                }
                            }

                            if (!dst_is_src_of_other)
                            {
                                result.push_back(*it);
                                remaining.erase(it);
                                found = true;
                                break;
                            }
                        }

                        if (!found)
                        {
                            auto const& cycle_move = remaining.front();

                            result.push_back({.dst = scratch, .src = cycle_move.src});

                            for (auto& rm : remaining)
                            {
                                if (rm.src == cycle_move.src)
                                    rm.src = scratch;
                            }
                        }
                    }

                    blk.instrs.erase(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start), blk.instrs.begin() + static_cast<std::ptrdiff_t>(ii));

                    for (std::size_t ri = 0; ri < result.size(); ++ri)
                        blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(run_start + ri), make_copy(result[ri].dst, result[ri].src));

                    ii = run_start + result.size() + 1;
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

        resolve_parallel_copies(func);

        insert_callee_saves(func, target, ranges);

        post_check_and_fix(func, target);
    }

} // namespace dcc::backend::em64t
