export module dcc.backend.i8086.framelay;

import std;
import dcc.target;
import dcc.backend.x86.mir;

using namespace dcc::backend::x86;

export namespace dcc::backend::i8086
{
    void frame_layout(MFunction& func, target::TargetConfig const&)
    {
        if (func.frame_size >= 0)
            return;

        auto& entry = func.entry_block();
        std::int32_t pushed = 0;
        for (auto const& instr : entry.instrs)
        {
            if (instr.opc == MOpc::PUSH32r)
                pushed += 4;
            else if (instr.opc == MOpc::PUSH16r)
                pushed += 2;
            else
                break;
        }

        std::vector<std::uint32_t> order(func.frame_slots.size());
        std::iota(order.begin(), order.end(), 0u);
        std::ranges::stable_sort(order, [&](std::uint32_t a, std::uint32_t b) { return func.frame_slots[a].align > func.frame_slots[b].align; });

        std::int32_t offset = pushed;
        for (auto idx : order)
        {
            auto& slot = func.frame_slots[idx];
            auto align = static_cast<std::int32_t>(std::max(slot.align, 1u));
            offset = (offset + align - 1) / align * align;
            offset += static_cast<std::int32_t>(slot.size);
            slot.offset = -offset;
        }
        std::int32_t frame_size = offset - pushed + func.outgoing_args_size;
        frame_size = (frame_size + 1) & ~1;
        func.frame_size = frame_size;

        std::vector<MInstr> prologue = {make_instr(MOpc::PUSH16r, {phys_operand(PhysReg::RBP)}, 0),
                                        make_instr(MOpc::MOV16rr, {phys_operand(PhysReg::RBP), phys_operand(PhysReg::RSP)}, 1)};
        entry.instrs.insert(entry.instrs.begin(), prologue.begin(), prologue.end());
        if (frame_size > 0)
        {
            std::size_t at = 2;
            while (at < entry.instrs.size() && (entry.instrs[at].opc == MOpc::PUSH32r || entry.instrs[at].opc == MOpc::PUSH16r))
                ++at;
            entry.instrs.insert(entry.instrs.begin() + static_cast<std::ptrdiff_t>(at),
                                make_instr(MOpc::SUB16ri, {phys_operand(PhysReg::RSP), phys_operand(PhysReg::RSP), MOp::from_imm(frame_size)}, 1));
        }

        for (auto& block : func.blocks)
        {
            for (std::size_t i = 0; i < block.instrs.size(); ++i)
            {
                if (block.instrs[i].opc != MOpc::RET)
                    continue;
                std::size_t pops = i;
                while (pops > 0 && (block.instrs[pops - 1].opc == MOpc::POP32r || block.instrs[pops - 1].opc == MOpc::POP16r))
                    --pops;
                std::vector<MInstr> epilogue;
                if (frame_size > 0)
                    epilogue.push_back(make_instr(MOpc::ADD16ri, {phys_operand(PhysReg::RSP), phys_operand(PhysReg::RSP), MOp::from_imm(frame_size)}, 1));
                block.instrs.insert(block.instrs.begin() + static_cast<std::ptrdiff_t>(pops), epilogue.begin(), epilogue.end());
                i += epilogue.size();
                std::vector<MInstr> leave = {make_instr(MOpc::MOV16rr, {phys_operand(PhysReg::RSP), phys_operand(PhysReg::RBP)}, 1),
                                             make_instr(MOpc::POP16r, {phys_operand(PhysReg::RBP)}, 1)};
                block.instrs.insert(block.instrs.begin() + static_cast<std::ptrdiff_t>(i), leave.begin(), leave.end());
                i += leave.size();
            }
        }

        for (auto& block : func.blocks)
            for (auto& instr : block.instrs)
                for (std::uint8_t oi = 0; oi < instr.num_ops; ++oi)
                {
                    auto& op = instr.ops[oi];
                    if (op.kind != MOpKind::FrameSlot)
                        continue;
                    std::int32_t disp = op.frame_slot < func.frame_slots.size() ? func.frame_slots[op.frame_slot].offset : 0;
                    op = MOp::from_mem(MMem::make_base_disp(VReg::phys(PhysReg::RBP), disp));
                }
    }

} // namespace dcc::backend::i8086
