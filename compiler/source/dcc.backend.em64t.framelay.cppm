export module dcc.backend.em64t.framelay;

import std;
import dcc.ir;
import dcc.backend.em64t.mir;
import dcc.target;

namespace dcc::backend::em64t
{
    namespace
    {
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

    } // anonymous namespace

} // namespace dcc::backend::em64t

export namespace dcc::backend::em64t
{
    void frame_layout(MFunction& func, target::TargetConfig const& target)
    {
        if (func.frame_size >= 0)
            return;

        bool w64 = is_win64(func, target);

        std::vector<std::uint32_t> slot_indices;
        slot_indices.reserve(func.frame_slots.size());
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(func.frame_slots.size()); ++i)
            slot_indices.push_back(i);

        std::ranges::sort(slot_indices, [&](std::uint32_t a, std::uint32_t b) { return func.frame_slots[a].align > func.frame_slots[b].align; });

        std::int32_t current_offset = 0;
        for (auto idx : slot_indices)
        {
            auto& slot = func.frame_slots[idx];
            std::uint32_t align = std::max(slot.align, 1u);
            std::int32_t aligned_offset = current_offset;
            if (aligned_offset % static_cast<std::int32_t>(align) != 0)
                aligned_offset += static_cast<std::int32_t>(align) - (aligned_offset % static_cast<std::int32_t>(align));

            slot.offset = -(aligned_offset + static_cast<std::int32_t>(slot.size));
            current_offset = aligned_offset + static_cast<std::int32_t>(slot.size);
        }

        std::int32_t frame_slot_area = current_offset;

        std::int32_t shadow_space = w64 ? 32 : 0;
        std::int32_t raw_frame_size = frame_slot_area + shadow_space;

        auto& entry = func.entry_block();
        std::int32_t num_callee_pushes = 0;
        for (auto const& instr : entry.instrs)
            if (instr.opc == MOpc::PUSH64r)
                ++num_callee_pushes;

        std::int32_t total_pushed = 8 + (num_callee_pushes * 8);

        std::int32_t total_with_padding = total_pushed + raw_frame_size;
        std::int32_t padding = (16 - (total_with_padding % 16)) % 16;
        std::int32_t frame_size = raw_frame_size + padding;

        func.frame_size = frame_size;

        MInstr push_rbp;
        push_rbp.opc = MOpc::PUSH64r;
        push_rbp.num_ops = 1;
        push_rbp.num_defs = 0;
        push_rbp.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RBP));

        MInstr mov_rbp_rsp;
        mov_rbp_rsp.opc = MOpc::MOV64rr;
        mov_rbp_rsp.num_ops = 2;
        mov_rbp_rsp.num_defs = 1;
        mov_rbp_rsp.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RBP));
        mov_rbp_rsp.ops[1] = MOp::from_reg(VReg::phys(PhysReg::RSP));

        entry.instrs.insert(entry.instrs.begin(), {push_rbp, mov_rbp_rsp});

        if (frame_size > 0)
        {
            MInstr sub_rsp;
            sub_rsp.opc = MOpc::SUB64ri32;
            sub_rsp.num_ops = 3;
            sub_rsp.num_defs = 1;
            sub_rsp.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RSP));
            sub_rsp.ops[1] = MOp::from_reg(VReg::phys(PhysReg::RSP));
            sub_rsp.ops[2] = MOp::from_imm(frame_size);

            std::size_t insert_pos = 0;
            for (std::size_t i = 0; i < entry.instrs.size(); ++i)
            {
                if (entry.instrs[i].opc == MOpc::PUSH64r || entry.instrs[i].opc == MOpc::MOV64rr)
                    insert_pos = i + 1;
                else
                    break;
            }

            entry.instrs.insert(entry.instrs.begin() + static_cast<std::ptrdiff_t>(insert_pos), sub_rsp);
        }

        for (auto& blk : func.blocks)
        {
            for (std::size_t i = 0; i < blk.instrs.size(); ++i)
            {
                if (blk.instrs[i].opc != MOpc::RET)
                    continue;

                bool has_mov_pop = false;
                if (i >= 2)
                {
                    auto const& m1 = blk.instrs[i - 2];
                    auto const& m2 = blk.instrs[i - 1];
                    if (m1.opc == MOpc::MOV64rr && m2.opc == MOpc::POP64r)
                    {
                        if (m1.num_ops >= 2 && m1.ops[0].kind == MOpKind::Reg && m1.ops[0].reg == VReg::phys(PhysReg::RSP) && m1.ops[1].kind == MOpKind::Reg &&
                            m1.ops[1].reg == VReg::phys(PhysReg::RBP) && m2.num_ops >= 1 && m2.ops[0].kind == MOpKind::Reg &&
                            m2.ops[0].reg == VReg::phys(PhysReg::RBP))
                            has_mov_pop = true;
                    }
                }

                std::size_t epilogue_boundary = has_mov_pop ? (i - 2) : i;
                std::size_t pop_run_start = epilogue_boundary;
                while (pop_run_start > 0)
                {
                    auto const& prev = blk.instrs[pop_run_start - 1];
                    if (prev.opc == MOpc::POP64r && prev.num_ops >= 1 && prev.ops[0].kind == MOpKind::Reg && prev.ops[0].reg != VReg::phys(PhysReg::RBP))
                        --pop_run_start;
                    else
                        break;
                }

                if (func.frame_size > 0)
                {
                    MInstr add_rsp;
                    add_rsp.opc = MOpc::ADD64ri32;
                    add_rsp.num_ops = 3;
                    add_rsp.num_defs = 1;
                    add_rsp.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RSP));
                    add_rsp.ops[1] = MOp::from_reg(VReg::phys(PhysReg::RSP));
                    add_rsp.ops[2] = MOp::from_imm(func.frame_size);
                    blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(pop_run_start), add_rsp);
                    ++i;
                }

                if (!has_mov_pop)
                {
                    MInstr mov_rsp_rbp;
                    mov_rsp_rbp.opc = MOpc::MOV64rr;
                    mov_rsp_rbp.num_ops = 2;
                    mov_rsp_rbp.num_defs = 1;
                    mov_rsp_rbp.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RSP));
                    mov_rsp_rbp.ops[1] = MOp::from_reg(VReg::phys(PhysReg::RBP));

                    MInstr pop_rbp;
                    pop_rbp.opc = MOpc::POP64r;
                    pop_rbp.num_ops = 1;
                    pop_rbp.num_defs = 1;
                    pop_rbp.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RBP));

                    blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(i), pop_rbp);
                    blk.instrs.insert(blk.instrs.begin() + static_cast<std::ptrdiff_t>(i), mov_rsp_rbp);
                    i += 2;
                }
            }
        }

        VReg rbp_reg = VReg::phys(PhysReg::RBP);

        for (auto& blk : func.blocks)
        {
            for (auto& instr : blk.instrs)
            {
                for (std::uint8_t oi = 0; oi < instr.num_ops; ++oi)
                {
                    auto& op = instr.ops[oi];
                    if (op.kind == MOpKind::FrameSlot)
                    {
                        std::uint32_t slot_idx = op.frame_slot;
                        if (slot_idx < func.frame_slots.size())
                        {
                            std::int32_t offset = func.frame_slots[slot_idx].offset;
                            op = MOp::from_mem(MMem::make_base_disp(rbp_reg, offset));
                        }
                        else
                            op = MOp::from_mem(MMem::make_base_disp(rbp_reg, 0));
                    }
                }
            }
        }
    }

} // namespace dcc::backend::em64t
