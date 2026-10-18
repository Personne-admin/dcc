export module dcc.backend.x86.folding;

import std;
import dcc.backend.x86.mir;

namespace dcc::backend::x86
{
    namespace
    {
        struct Forms
        {
            MOpc immediate{MOpc::NOP};
            MOpc memory{MOpc::NOP};
            bool commutative{false};
        };

        Forms forms(MOpc op)
        {
            switch (op)
            {
                case MOpc::ADD64rr:
                    return {MOpc::ADD64ri32, MOpc::ADD64rm, true};
                case MOpc::SUB64rr:
                    return {MOpc::SUB64ri32, MOpc::SUB64rm};
                case MOpc::AND64rr:
                    return {MOpc::AND64ri32, MOpc::AND64rm, true};
                case MOpc::OR64rr:
                    return {MOpc::OR64ri32, MOpc::OR64rm, true};
                case MOpc::XOR64rr:
                    return {MOpc::XOR64ri32, MOpc::XOR64rm, true};
                case MOpc::IMUL64rr:
                    return {MOpc::IMUL64rri32, MOpc::NOP, true};
                case MOpc::CMP64rr:
                    return {MOpc::CMP64ri32, MOpc::CMP64rm};
                case MOpc::TEST64rr:
                    return {MOpc::TEST64ri, MOpc::NOP, true};
                case MOpc::ADD32rr:
                    return {MOpc::ADD32ri, MOpc::ADD32rm, true};
                case MOpc::SUB32rr:
                    return {MOpc::SUB32ri, MOpc::SUB32rm};
                case MOpc::AND32rr:
                    return {MOpc::AND32ri, MOpc::AND32rm, true};
                case MOpc::OR32rr:
                    return {MOpc::OR32ri, MOpc::OR32rm, true};
                case MOpc::XOR32rr:
                    return {MOpc::XOR32ri, MOpc::XOR32rm, true};
                case MOpc::IMUL32rr:
                    return {MOpc::IMUL32rri, MOpc::NOP, true};
                case MOpc::CMP32rr:
                    return {MOpc::CMP32ri, MOpc::CMP32rm};
                case MOpc::TEST32rr:
                    return {MOpc::TEST32ri, MOpc::NOP, true};
                case MOpc::ADC16rr:
                    return {MOpc::ADC16ri, MOpc::NOP, true};
                case MOpc::SBB16rr:
                    return {MOpc::SBB16ri};
                case MOpc::ADC32rr:
                    return {MOpc::ADC32ri, MOpc::NOP, true};
                case MOpc::SBB32rr:
                    return {MOpc::SBB32ri};
                case MOpc::ADD16rr:
                    return {MOpc::ADD16ri, MOpc::ADD16rm, true};
                case MOpc::SUB16rr:
                    return {MOpc::SUB16ri, MOpc::SUB16rm};
                case MOpc::AND16rr:
                    return {MOpc::AND16ri, MOpc::AND16rm, true};
                case MOpc::OR16rr:
                    return {MOpc::OR16ri, MOpc::OR16rm, true};
                case MOpc::XOR16rr:
                    return {MOpc::XOR16ri, MOpc::XOR16rm, true};
                case MOpc::IMUL16rr:
                    return {MOpc::IMUL16rri, MOpc::NOP, true};
                case MOpc::CMP16rr:
                    return {MOpc::CMP16ri, MOpc::CMP16rm};
                case MOpc::TEST16rr:
                    return {MOpc::TEST16ri, MOpc::NOP, true};
                case MOpc::CMP8rr:
                    return {MOpc::CMP8ri};
                case MOpc::TEST8rr:
                    return {MOpc::TEST8ri, MOpc::NOP, true};
                default:
                    return {};
            }
        }

        bool writes_flags(MInstr const& inst)
        {
            auto name = opc_name(inst.opc);
            if (name.starts_with("INC") || name.starts_with("DEC"))
                return false;
            if (name.starts_with("SHL") || name.starts_with("SHR") || name.starts_with("SAR"))
            {
                if (inst.num_ops == 0 || inst.ops[inst.num_ops - 1].kind != MOpKind::Imm64)
                    return false;
                auto mask = operand_bits(inst.opc) == 64 ? 63u : 31u;
                return (static_cast<std::uint64_t>(inst.ops[inst.num_ops - 1].imm) & mask) != 0;
            }
            for (auto prefix :
                 {"ADD", "SUB", "AND", "OR", "XOR", "CMP", "TEST", "IMUL", "MUL", "DIV", "IDIV", "NEG", "INC", "DEC", "SHL", "SHR", "SAR", "ADC", "SBB"})
                if (name.starts_with(prefix))
                    return true;
            return false;
        }

        bool reads_flags(MInstr const& inst)
        {
            auto name = opc_name(inst.opc);
            return (name.starts_with("J") && !name.starts_with("JMP") && inst.opc != MOpc::JUMP_TABLE) || name.starts_with("SET") || name.starts_with("CMOV") ||
                   name.starts_with("ADC") || name.starts_with("SBB") || name.starts_with("PUSHF") || inst.implicit_defs || inst.implicit_uses;
        }
    }
}

export namespace dcc::backend::x86
{
    void fold_operands(MFunction& func, unsigned pointer_bits, std::unordered_set<VReg> const& foldable_loads)
    {
        std::unordered_map<VReg, unsigned> definitions;
        std::unordered_map<VReg, unsigned> uses;
        for (auto const& block : func.blocks)
            for (auto const& inst : block.instrs)
                for (unsigned i = 0; i < inst.num_ops; ++i)
                    if (auto const& op = inst.ops[i]; op.kind == MOpKind::Reg)
                        ++(i < inst.num_defs ? definitions : uses)[op.reg];
                    else if (op.kind == MOpKind::Mem)
                    {
                        ++uses[op.mem.base];
                        ++uses[op.mem.index];
                    }
        std::unordered_set<VReg> candidates;
        for (auto& block : func.blocks)
        {
            std::unordered_map<VReg, MInstr const*> available;
            std::unordered_map<VReg, MOp> loads;
            auto immediate = [&](auto&& self, VReg reg, unsigned depth) -> std::optional<std::int64_t> {
                if (depth > 8 || !available.contains(reg))
                    return std::nullopt;
                auto const& def = *available.at(reg);
                if (def.num_ops != 2)
                    return std::nullopt;
                if (def.opc == MOpc::COPY && def.ops[1].kind == MOpKind::Reg)
                    return self(self, def.ops[1].reg, depth + 1);
                if (def.ops[1].kind != MOpKind::Imm64)
                    return std::nullopt;
                switch (def.opc)
                {
                    case MOpc::MOV64ri:
                    case MOpc::MOV64ri32:
                        return def.ops[1].imm;
                    case MOpc::MOV32ri:
                        return static_cast<std::uint32_t>(def.ops[1].imm);
                    case MOpc::MOV16ri:
                        return static_cast<std::uint16_t>(def.ops[1].imm);
                    case MOpc::MOV8ri:
                        return static_cast<std::uint8_t>(def.ops[1].imm);
                    default:
                        return std::nullopt;
                }
            };
            auto address = [&](auto&& self, MMem& result, VReg reg, unsigned scale, unsigned bits, unsigned depth) -> bool {
                if (!reg.is_valid())
                    return true;
                if (!reg.is_virtual())
                {
                    if (reg != VReg::phys(PhysReg::RBP) || scale != 1 || result.base.is_valid())
                        return false;
                    result.base = reg;
                    if (depth != 0 && pointer_bits != 64 && result.segment == SegmentOverride::None)
                        result.segment = SegmentOverride::DS;
                    return true;
                }
                auto offset = [&](MMem& mem, std::int64_t n) {
                    auto sum = static_cast<std::int64_t>(mem.disp) + n;
                    if (bits == 64 && (sum < std::numeric_limits<std::int32_t>::min() || sum > std::numeric_limits<std::int32_t>::max()))
                        return false;
                    mem.disp = bits == 16 ? static_cast<std::int16_t>(sum) : static_cast<std::int32_t>(sum);
                    return true;
                };
                if (auto imm = immediate(immediate, reg, 0))
                {
                    if (bits != 64 || (*imm >= std::numeric_limits<std::int32_t>::min() / static_cast<std::int64_t>(scale) &&
                                       *imm <= std::numeric_limits<std::int32_t>::max() / static_cast<std::int64_t>(scale)))
                        if (offset(result, bits == 16   ? static_cast<std::int16_t>(static_cast<std::uint64_t>(*imm) * scale)
                                           : bits == 32 ? static_cast<std::int32_t>(static_cast<std::uint64_t>(*imm) * scale)
                                                        : *imm * scale))
                        {
                            candidates.insert(reg);
                            return true;
                        }
                }
                if (depth < 16 && available.contains(reg))
                {
                    auto const& def = *available.at(reg);
                    auto expanded = result;
                    auto operand = [&](unsigned i, unsigned factor) {
                        if (def.ops[i].kind == MOpKind::Reg)
                            return self(self, expanded, def.ops[i].reg, factor, bits, depth + 1);
                        if (def.ops[i].kind != MOpKind::Imm64)
                            return false;
                        auto n = def.ops[i].imm;
                        if (bits == 64 && (n < std::numeric_limits<std::int32_t>::min() / static_cast<std::int64_t>(factor) ||
                                           n > std::numeric_limits<std::int32_t>::max() / static_cast<std::int64_t>(factor)))
                            return false;
                        return offset(expanded, bits == 16   ? static_cast<std::int16_t>(static_cast<std::uint64_t>(n) * factor)
                                                : bits == 32 ? static_cast<std::int32_t>(static_cast<std::uint64_t>(n) * factor)
                                                             : n * factor);
                    };
                    auto width = operand_bits(def.opc);
                    bool ok = false;
                    if (def.opc == MOpc::COPY && def.ops[1].kind == MOpKind::Reg)
                        ok = operand(1, scale);
                    else if (width == bits && (def.opc == MOpc::ADD64rr || def.opc == MOpc::ADD32rr || def.opc == MOpc::ADD16rr || def.opc == MOpc::ADD64ri32 ||
                                               def.opc == MOpc::ADD32ri || def.opc == MOpc::ADD16ri))
                        ok = operand(1, scale) && operand(2, scale);
                    else if (width == bits && bits != 16 && (def.opc == MOpc::IMUL64rri32 || def.opc == MOpc::IMUL32rri))
                    {
                        auto n = def.ops[2].imm;
                        if (n == 1 || n == 2 || n == 4 || n == 8)
                            if (n * scale <= 8)
                                ok = operand(1, static_cast<unsigned>(n) * scale);
                    }
                    else if (width == bits && (def.opc == MOpc::LEA64rm || def.opc == MOpc::LEA32rm || def.opc == MOpc::LEA16rm) &&
                             def.ops[1].kind == MOpKind::Mem && !def.ops[1].mem.symbol.empty() && scale == 1 && expanded.symbol.empty() &&
                             !expanded.base.is_valid() && !expanded.index.is_valid())
                    {
                        auto const& mem = def.ops[1].mem;
                        ok = offset(expanded, mem.disp);
                        expanded.symbol = mem.symbol;
                        expanded.is_got_indirect = mem.is_got_indirect;
                    }
                    else if (width == bits && (def.opc == MOpc::LEA64rm || def.opc == MOpc::LEA32rm || def.opc == MOpc::LEA16rm) &&
                             def.ops[1].kind == MOpKind::Mem && def.ops[1].mem.symbol.empty())
                    {
                        auto const& mem = def.ops[1].mem;
                        if (scale * mem.scale <= (bits == 16 ? 1u : 8u))
                            ok = offset(expanded, static_cast<std::int64_t>(mem.disp) * scale) && self(self, expanded, mem.base, scale, bits, depth + 1) &&
                                 self(self, expanded, mem.index, scale * mem.scale, bits, depth + 1);
                    }
                    if (ok)
                    {
                        result = expanded;
                        candidates.insert(reg);
                        return true;
                    }
                }
                if (bits == 64 && !result.symbol.empty())
                    return false;
                if (scale == 1 && !result.base.is_valid())
                    result.base = reg;
                else if (bits != 16 && !result.index.is_valid())
                {
                    result.index = reg;
                    result.scale = static_cast<std::uint8_t>(scale);
                }
                else
                    return false;
                return true;
            };
            for (auto& inst : block.instrs)
            {
                auto form = forms(inst.opc);
                unsigned rhs = inst.num_ops ? inst.num_ops - 1 : 0;
                auto bits = operand_bits(inst.opc);
                auto normalize = [&](std::int64_t n) -> std::int64_t {
                    return bits == 8 ? static_cast<std::int8_t>(n) : bits == 16 ? static_cast<std::int16_t>(n) : bits == 32 ? static_cast<std::int32_t>(n) : n;
                };
                auto fits = [&](std::int64_t n) {
                    return bits != 64 || (n >= std::numeric_limits<std::int32_t>::min() && n <= std::numeric_limits<std::int32_t>::max());
                };
                if (form.immediate != MOpc::NOP && inst.num_ops >= 2)
                {
                    auto constant = [&](unsigned i) { return inst.ops[i].kind == MOpKind::Reg ? immediate(immediate, inst.ops[i].reg, 0) : std::nullopt; };
                    auto value = constant(rhs);
                    if ((!value || !fits(*value)) && form.commutative)
                        if (auto left = constant(inst.num_defs); left && fits(*left))
                        {
                            std::swap(inst.ops[inst.num_defs], inst.ops[rhs]);
                            value = left;
                        }
                    if (value && fits(*value))
                    {
                        candidates.insert(inst.ops[rhs].reg);
                        inst.ops[rhs] = MOp::from_imm(normalize(*value));
                        inst.opc = form.immediate;
                    }
                }
                if ((inst.opc == MOpc::MOV64mr || inst.opc == MOpc::MOV32mr || inst.opc == MOpc::MOV16mr || inst.opc == MOpc::MOV8mr) &&
                    inst.ops[1].kind == MOpKind::Reg)
                    if (auto n = immediate(immediate, inst.ops[1].reg, 0); n && fits(*n))
                    {
                        candidates.insert(inst.ops[1].reg);
                        inst.ops[1] = MOp::from_imm(normalize(*n));
                        inst.opc = bits == 64 ? MOpc::MOV64mi32 : bits == 32 ? MOpc::MOV32mi : bits == 16 ? MOpc::MOV16mi : MOpc::MOV8mi;
                    }
                if ((inst.opc == MOpc::PUSH64r || inst.opc == MOpc::PUSH32r || inst.opc == MOpc::PUSH16r) && inst.ops[0].kind == MOpKind::Reg)
                    if (auto n = immediate(immediate, inst.ops[0].reg, 0); n && fits(*n))
                    {
                        candidates.insert(inst.ops[0].reg);
                        inst.ops[0] = MOp::from_imm(normalize(*n));
                        inst.opc = bits == 16 ? MOpc::PUSH16i : bits == 32 ? MOpc::PUSH32i : MOpc::PUSH64i;
                    }
                if (form.memory != MOpc::NOP && inst.opc != form.immediate && inst.num_ops >= 2)
                {
                    auto loaded = [&](unsigned i) { return inst.ops[i].kind == MOpKind::Reg && loads.contains(inst.ops[i].reg); };
                    if (!loaded(rhs) && form.commutative && loaded(inst.num_defs))
                        std::swap(inst.ops[inst.num_defs], inst.ops[rhs]);
                    if (loaded(rhs))
                    {
                        auto reg = inst.ops[rhs].reg;
                        inst.ops[rhs] = loads.at(reg);
                        inst.opc = form.memory;
                        candidates.insert(reg);
                    }
                }
                for (unsigned i = 0; i < inst.num_ops; ++i)
                    if (auto& op = inst.ops[i]; op.kind == MOpKind::Mem && op.mem.symbol.empty() && !opc_name(inst.opc).starts_with("LOCK"))
                    {
                        MMem folded = op.mem;
                        folded.base = {};
                        folded.index = {};
                        folded.scale = 1;
                        auto width = op.mem.address_bits ? op.mem.address_bits : pointer_bits;
                        if (address(address, folded, op.mem.base, 1, width, 0) && address(address, folded, op.mem.index, op.mem.scale, width, 0) &&
                            (folded.base.is_valid() || folded.index.is_valid() || !folded.symbol.empty() || op.mem.absolute))
                            op.mem = folded;
                    }
                bool physical = false;
                bool memory_read = false;
                bool memory_write = false;
                for (unsigned i = 0; i < inst.num_ops; ++i)
                {
                    auto const& op = inst.ops[i];
                    physical |= op.kind == MOpKind::Reg && op.reg.is_physical();
                    bool memory = op.kind == MOpKind::Mem || op.kind == MOpKind::FrameSlot;
                    memory_read |= inst.num_defs != 0 && memory && !opc_name(inst.opc).starts_with("LEA");
                    memory_write |= memory && (inst.num_defs == 0 || i < inst.num_defs);
                }
                bool eligible = inst.num_defs == 1 && inst.ops[0].kind == MOpKind::Reg && foldable_loads.contains(inst.ops[0].reg);
                if (inst.has_side_effects() || inst.implicit_defs || inst.implicit_uses || physical || memory_write || inst.opc == MOpc::NOP ||
                    (memory_read && !eligible))
                    loads.clear();
                if (opc_name(inst.opc).starts_with("LOCK"))
                    available.clear();
                if (inst.num_defs == 1 && inst.ops[0].kind == MOpKind::Reg && inst.ops[0].reg.is_virtual() && definitions[inst.ops[0].reg] == 1 &&
                    !inst.implicit_defs && !inst.implicit_uses)
                {
                    auto reg = inst.ops[0].reg;
                    available[reg] = &inst;
                    if (foldable_loads.contains(reg) && uses[reg] == 1 && inst.num_ops == 2 &&
                        (inst.opc == MOpc::MOV64rm || inst.opc == MOpc::MOV32rm || inst.opc == MOpc::MOV16rm))
                        loads[reg] = inst.ops[1];
                }
            }
        }
        bool changed;
        do
        {
            uses.clear();
            for (auto const& block : func.blocks)
                for (auto const& inst : block.instrs)
                    for (unsigned i = inst.num_defs; i < inst.num_ops; ++i)
                        if (auto const& op = inst.ops[i]; op.kind == MOpKind::Reg)
                            ++uses[op.reg];
                        else if (op.kind == MOpKind::Mem)
                        {
                            ++uses[op.mem.base];
                            ++uses[op.mem.index];
                        }
            changed = false;
            for (auto& block : func.blocks)
            {
                std::vector<bool> remove(block.instrs.size());
                bool flags = !block.succs.empty();
                for (std::size_t i = block.instrs.size(); i-- > 0;)
                {
                    auto const& inst = block.instrs[i];
                    bool can_remove = !writes_flags(inst) || !flags;
                    if (can_remove && inst.num_defs == 1 && inst.ops[0].kind == MOpKind::Reg && candidates.contains(inst.ops[0].reg) &&
                        !uses[inst.ops[0].reg] && !inst.has_side_effects() && !inst.implicit_defs && !inst.implicit_uses)
                        remove[i] = changed = true;
                    else
                    {
                        if (writes_flags(inst))
                            flags = false;
                        if (reads_flags(inst))
                            flags = true;
                    }
                }
                std::size_t index = 0;
                std::erase_if(block.instrs, [&](auto const&) { return remove[index++]; });
            }
        } while (changed);
        for (auto& block : func.blocks)
        {
            std::vector<MInstr> instructions;
            for (auto inst : block.instrs)
            {
                auto name = opc_name(inst.opc);
                if (inst.num_defs == 1 && inst.num_ops == 3 && name.ends_with("rm") && inst.ops[0].kind == MOpKind::Reg && inst.ops[1].kind == MOpKind::Reg &&
                    inst.ops[2].kind == MOpKind::Mem && inst.ops[0].reg != inst.ops[1].reg)
                {
                    instructions.push_back(make_copy(inst.ops[0].reg, inst.ops[1].reg));
                    inst.ops[1] = inst.ops[0];
                }
                instructions.push_back(inst);
            }
            block.instrs = std::move(instructions);
        }
    }
}
