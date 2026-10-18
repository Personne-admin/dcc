export module dcc.backend.i8086.isel;

import std;
import dcc.ir;
import dcc.ir.analysis;
import dcc.target;
import dcc.backend.inline_asm;
import dcc.backend.x86.mir;
import dcc.backend.x86.folding;

using namespace dcc::backend::x86;

export namespace dcc::backend::i8086
{
    [[nodiscard]] bool uses_c_abi(dcc::ir::IrFunction const& func);

    [[nodiscard]] MFunction isel_function(dcc::ir::IrFunction const& func, dcc::target::TargetConfig const& target, std::vector<std::string>& diags);

    void remove_fallthrough_jumps(MFunction& func);

} // namespace dcc::backend::i8086

namespace dcc::backend::i8086
{
    namespace
    {
        using namespace dcc::ir;

        [[nodiscard]] std::string kind_name(IrNodeKind kind)
        {
            switch (kind)
            {
                case IrNodeKind::Gep:
                    return "gep";
                case IrNodeKind::Br:
                    return "br";
                case IrNodeKind::BrCond:
                    return "br.cond";
                case IrNodeKind::Switch:
                    return "switch";
                case IrNodeKind::Unreachable:
                    return "unreachable";
                case IrNodeKind::Phi:
                    return "phi";
                case IrNodeKind::Call:
                case IrNodeKind::CallTail:
                    return "call";
                case IrNodeKind::PtrToI:
                    return "ptrtoint";
                case IrNodeKind::IToPtr:
                    return "inttoptr";
                case IrNodeKind::Bitcast:
                    return "bitcast";
                case IrNodeKind::Extract:
                    return "extract";
                case IrNodeKind::Insert:
                    return "insert";
                case IrNodeKind::Aggregate:
                    return "aggregate";
                case IrNodeKind::InlineAsm:
                    return "inline asm";
                default:
                    return std::format("kind {}", static_cast<int>(kind));
            }
        }

        [[nodiscard]] std::optional<MOpc> setcc_for(IrNodeKind kind)
        {
            switch (kind)
            {
                case IrNodeKind::CmpEq:
                    return MOpc::SETEr;
                case IrNodeKind::CmpNe:
                    return MOpc::SETNEr;
                case IrNodeKind::CmpLt:
                    return MOpc::SETLr;
                case IrNodeKind::CmpLe:
                    return MOpc::SETLEr;
                case IrNodeKind::CmpGt:
                    return MOpc::SETGr;
                case IrNodeKind::CmpGe:
                    return MOpc::SETGEr;
                case IrNodeKind::CmpULt:
                    return MOpc::SETBr;
                case IrNodeKind::CmpULe:
                    return MOpc::SETBEr;
                case IrNodeKind::CmpUGt:
                    return MOpc::SETAr;
                case IrNodeKind::CmpUGe:
                    return MOpc::SETAEr;
                default:
                    return std::nullopt;
            }
        }

        [[nodiscard]] MOpc jcc_for(MOpc setcc)
        {
            switch (setcc)
            {
                case MOpc::SETEr:
                    return MOpc::JE;
                case MOpc::SETNEr:
                    return MOpc::JNE;
                case MOpc::SETLr:
                    return MOpc::JL;
                case MOpc::SETLEr:
                    return MOpc::JLE;
                case MOpc::SETGr:
                    return MOpc::JG;
                case MOpc::SETGEr:
                    return MOpc::JGE;
                case MOpc::SETBr:
                    return MOpc::JB;
                case MOpc::SETBEr:
                    return MOpc::JBE;
                case MOpc::SETAr:
                    return MOpc::JA;
                default:
                    return MOpc::JAE;
            }
        }

        [[nodiscard]] std::optional<MOpc> inverse_jcc(MOpc opc)
        {
            switch (opc)
            {
                case MOpc::JE:
                    return MOpc::JNE;
                case MOpc::JNE:
                    return MOpc::JE;
                case MOpc::JL:
                    return MOpc::JGE;
                case MOpc::JGE:
                    return MOpc::JL;
                case MOpc::JLE:
                    return MOpc::JG;
                case MOpc::JG:
                    return MOpc::JLE;
                case MOpc::JB:
                    return MOpc::JAE;
                case MOpc::JAE:
                    return MOpc::JB;
                case MOpc::JBE:
                    return MOpc::JA;
                case MOpc::JA:
                    return MOpc::JBE;
                default:
                    return std::nullopt;
            }
        }

        template <typename T> [[nodiscard]] std::pair<IrValue*, IrValue*> operands(IrNode const* inst)
        {
            auto const* typed = static_cast<T const*>(inst);
            return {typed->lhs, typed->rhs};
        }

        [[nodiscard]] std::pair<IrValue*, IrValue*> binary_operands(IrNode const* inst)
        {
            switch (inst->kind)
            {
                case IrNodeKind::Add:
                    return operands<IrAddInst>(inst);
                case IrNodeKind::Sub:
                    return operands<IrSubInst>(inst);
                case IrNodeKind::Mul:
                    return operands<IrMulInst>(inst);
                case IrNodeKind::UDiv:
                    return operands<IrUDivInst>(inst);
                case IrNodeKind::SDiv:
                    return operands<IrSDivInst>(inst);
                case IrNodeKind::URem:
                    return operands<IrURemInst>(inst);
                case IrNodeKind::SRem:
                    return operands<IrSRemInst>(inst);
                case IrNodeKind::And:
                    return operands<IrAndInst>(inst);
                case IrNodeKind::Or:
                    return operands<IrOrInst>(inst);
                case IrNodeKind::Xor:
                    return operands<IrXorInst>(inst);
                case IrNodeKind::Shl:
                    return operands<IrShlInst>(inst);
                case IrNodeKind::LShr:
                    return operands<IrLShrInst>(inst);
                case IrNodeKind::AShr:
                    return operands<IrAShrInst>(inst);
                case IrNodeKind::CmpEq:
                    return operands<IrCmpEqInst>(inst);
                case IrNodeKind::CmpNe:
                    return operands<IrCmpNeInst>(inst);
                case IrNodeKind::CmpLt:
                    return operands<IrCmpLtInst>(inst);
                case IrNodeKind::CmpLe:
                    return operands<IrCmpLeInst>(inst);
                case IrNodeKind::CmpGt:
                    return operands<IrCmpGtInst>(inst);
                case IrNodeKind::CmpGe:
                    return operands<IrCmpGeInst>(inst);
                case IrNodeKind::CmpULt:
                    return operands<IrCmpULtInst>(inst);
                case IrNodeKind::CmpULe:
                    return operands<IrCmpULeInst>(inst);
                case IrNodeKind::CmpUGt:
                    return operands<IrCmpUGtInst>(inst);
                case IrNodeKind::CmpUGe:
                    return operands<IrCmpUGeInst>(inst);
                default:
                    return {nullptr, nullptr};
            }
        }

        struct Isel
        {
            IrFunction const& func;
            target::TargetConfig const& target;
            std::vector<std::string>& diags;
            MFunction mfunc;
            MBlock* block{};
            bool c_abi{};
            bool preserve_es{};
            VReg sret_pointer;
            struct Pair
            {
                VReg low;
                VReg high;
            };
            std::unordered_map<IrValue const*, Pair> pairs;
            std::unordered_map<IrValue const*, VReg> values;
            std::unordered_map<IrValue const*, std::vector<VReg>> asm_results;
            std::unordered_map<IrValue const*, std::uint32_t> slots;
            std::unordered_map<IrBasicBlock const*, std::uint32_t> block_ids;
            std::unordered_map<IrBasicBlock const*, std::uint32_t> block_tails;
            std::unordered_set<IrValue const*> fused;
            std::unordered_map<std::uint32_t, std::size_t> jump_table_blocks;
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> split_edges;

            struct PendingPhi
            {
                IrPhiInst const* phi;
                IrBasicBlock const* block;
                VReg result;
            };
            std::vector<PendingPhi> pending_phis;

            Isel(IrFunction const& f, target::TargetConfig const& t, std::vector<std::string>& d) : func(f), target(t), diags(d) {}

            void unsupported(std::string_view what)
            {
                if (diags.empty())
                    diags.push_back(std::format("i8086 backend: {} in function `{}` is not supported yet", what, func.name));
            }

            void append(MInstr const& mi) { block->instrs.push_back(mi); }

            [[nodiscard]] static unsigned scalar_bits(IrType const* type)
            {
                if (!type)
                    return 0;
                switch (type->kind)
                {
                    case IrTypeKind::Bool:
                        return 8;
                    case IrTypeKind::Int:
                        return static_cast<IrIntType const*>(type)->bits;
                    case IrTypeKind::Pointer:
                        return static_cast<IrPointerType const*>(type)->flavor != PointerFlavor::Far ? static_cast<unsigned>(type->byte_size * 8) : 0;
                    default:
                        return 0;
                }
            }

            [[nodiscard]] static bool wide(IrType const* type) { return type && type->kind == IrTypeKind::Int && type->byte_size == 8; }

            [[nodiscard]] static bool far(IrType const* type)
            {
                auto* pointer = ir_type_cast<IrPointerType>(type);
                return pointer && pointer->flavor == PointerFlavor::Far;
            }

            [[nodiscard]] std::optional<Pair> pair(IrValue const* v)
            {
                if (auto found = pairs.find(v); found != pairs.end())
                    return found->second;
                if (far(v->type))
                {
                    if (auto* pointer = ir_cast<IrPointerConstant>(v))
                    {
                        Pair result{emit(MOpc::MOV32ri, {MOp::from_imm(static_cast<std::int32_t>(pointer->offset))}),
                                    emit(MOpc::MOV32ri, {MOp::from_imm(pointer->segment)})};
                        pairs[v] = result;
                        return result;
                    }
                    if (ir_cast<IrNullConstant>(v))
                    {
                        Pair result{emit(MOpc::MOV32ri, {MOp::from_imm(0)}), emit(MOpc::MOV32ri, {MOp::from_imm(0)})};
                        pairs[v] = result;
                        return result;
                    }
                    if (auto* reference = ir_cast<IrGlobalRef>(v); reference && reference->global)
                    {
                        MMem memory{};
                        memory.symbol = reference->global->name;
                        memory.disp = static_cast<std::int32_t>(reference->addend);
                        memory.address_bits = target.far_offset_bits();
                        auto offset = emit(target.far_offset_bits() == 16 ? MOpc::LEA16rm : MOpc::LEA32rm, {MOp::from_mem(memory)});
                        MMem segment_address{};
                        segment_address.symbol = "__dcc_dgroup_segment";
                        segment_address.address_bits = 16;
                        auto segment = emit(MOpc::LEA16rm, {MOp::from_mem(segment_address)});
                        Pair result{extend(offset, target.far_offset_bits(), false, 32), extend(segment, 16, false, 32)};
                        pairs[v] = result;
                        return result;
                    }
                }
                if (auto* imm = ir_cast<IrIntConstant>(v))
                {
                    auto bits = static_cast<std::uint64_t>(imm->value);
                    Pair result{emit(MOpc::MOV32ri, {MOp::from_imm(static_cast<std::int32_t>(bits))}),
                                emit(MOpc::MOV32ri, {MOp::from_imm(static_cast<std::int32_t>(bits >> 32))})};
                    pairs[v] = result;
                    return result;
                }
                unsupported("an unavailable i64 pair");
                return std::nullopt;
            }

            void store_pair(Pair src, MMem mem)
            {
                append(make_instr(MOpc::MOV32mr, {MOp::from_mem(mem), MOp::from_reg(src.low)}, 0));
                mem.disp += 4;
                append(make_instr(MOpc::MOV32mr, {MOp::from_mem(mem), MOp::from_reg(src.high)}, 0));
            }

            [[nodiscard]] Pair load_pair(MMem mem)
            {
                auto low = emit(MOpc::MOV32rm, {MOp::from_mem(mem)});
                mem.disp += 4;
                return {low, emit(MOpc::MOV32rm, {MOp::from_mem(mem)})};
            }

            [[nodiscard]] VReg read_segment(PhysReg reg)
            {
                auto value = emit(MOpc::MOV16rs, {phys_operand(reg)});
                return emit(MOpc::MOVZX32_16rr, {MOp::from_reg(value)});
            }

            [[nodiscard]] Pair load_far(MOp memory)
            {
                auto reg = PhysReg::ES;
                if (target.has_i386_segments() && memory.kind == MOpKind::Mem)
                {
                    if (memory.mem.segment == SegmentOverride::ES)
                        reg = PhysReg::FS;
                    else if (memory.mem.segment == SegmentOverride::FS)
                        reg = PhysReg::GS;
                }
                auto operation = reg == PhysReg::FS   ? (target.far_offset_bits() == 16 ? MOpc::LFS16rm : MOpc::LFS32rm)
                                 : reg == PhysReg::GS ? (target.far_offset_bits() == 16 ? MOpc::LGS16rm : MOpc::LGS32rm)
                                                      : (target.far_offset_bits() == 16 ? MOpc::LES16rm : MOpc::LES32rm);
                append(make_instr(MOpc::PUSHSEG, {phys_operand(reg)}, 0));
                auto offset = emit(operation, {memory});
                block->instrs.back().implicit_defs |= bit(reg);
                auto segment = read_segment(reg);
                append(make_instr(MOpc::POPSEG, {phys_operand(reg)}, 1));
                return {extend(offset, target.far_offset_bits(), false, 32), segment};
            }

            void store_far(Pair source, MMem memory)
            {
                auto bits = target.far_offset_bits();
                append(make_instr(bits == 16 ? MOpc::MOV16mr : MOpc::MOV32mr, {MOp::from_mem(memory), MOp::from_reg(source.low)}, 0));
                memory.disp += bits / 8;
                append(make_instr(MOpc::MOV16mr, {MOp::from_mem(memory), MOp::from_reg(source.high)}, 0));
                if (bits == 32)
                {
                    memory.disp += 2;
                    append(make_instr(MOpc::MOV16mi, {MOp::from_mem(memory), MOp::from_imm(0)}, 0));
                }
            }

            struct FarAccess
            {
                Isel& isel;
                bool active;

                FarAccess(Isel& selector, IrValue const* pointer) : isel(selector), active(far(pointer->type))
                {
                    if (active)
                    {
                        auto source = isel.pair(pointer);
                        active = source.has_value();
                        if (source)
                        {
                            isel.append(make_instr(MOpc::PUSHSEG, {phys_operand(PhysReg::ES)}, 0));
                            isel.append(make_instr(MOpc::MOV16sr, {phys_operand(PhysReg::ES), MOp::from_reg(source->high)}, 1));
                        }
                    }
                }

                ~FarAccess()
                {
                    if (active)
                        isel.append(make_instr(MOpc::POPSEG, {phys_operand(PhysReg::ES)}, 1));
                }
            };

            [[nodiscard]] VReg condition(MOpc cc)
            {
                append(make_instr(cc, {phys_operand(PhysReg::RAX)}, 1));
                return emit(MOpc::MOVZX16_8rr, {phys_operand(PhysReg::RAX)});
            }

            [[nodiscard]] VReg compare_pair(Pair l, Pair r, MOpc cc)
            {
                if (cc == MOpc::SETEr || cc == MOpc::SETNEr)
                {
                    auto a = emit(MOpc::XOR32rr, {MOp::from_reg(l.low), MOp::from_reg(r.low)});
                    auto b = emit(MOpc::XOR32rr, {MOp::from_reg(l.high), MOp::from_reg(r.high)});
                    auto both = emit(MOpc::OR32rr, {MOp::from_reg(a), MOp::from_reg(b)});
                    append(make_instr(MOpc::CMP32ri, {MOp::from_reg(both), MOp::from_imm(0)}, 0));
                    return condition(cc);
                }
                bool less = cc == MOpc::SETLr || cc == MOpc::SETLEr || cc == MOpc::SETBr || cc == MOpc::SETBEr;
                bool signed_op = cc == MOpc::SETLr || cc == MOpc::SETLEr || cc == MOpc::SETGr || cc == MOpc::SETGEr;
                bool inclusive = cc == MOpc::SETLEr || cc == MOpc::SETBEr || cc == MOpc::SETGEr || cc == MOpc::SETAEr;
                append(make_instr(MOpc::CMP32rr, {MOp::from_reg(l.high), MOp::from_reg(r.high)}, 0));
                auto high = condition(signed_op ? (less ? MOpc::SETLr : MOpc::SETGr) : (less ? MOpc::SETBr : MOpc::SETAr));
                append(make_instr(MOpc::CMP32rr, {MOp::from_reg(l.high), MOp::from_reg(r.high)}, 0));
                auto equal = condition(MOpc::SETEr);
                append(make_instr(MOpc::CMP32rr, {MOp::from_reg(l.low), MOp::from_reg(r.low)}, 0));
                auto low = condition(less ? (inclusive ? MOpc::SETBEr : MOpc::SETBr) : (inclusive ? MOpc::SETAEr : MOpc::SETAr));
                auto tied = emit(MOpc::AND16rr, {MOp::from_reg(equal), MOp::from_reg(low)});
                return emit(MOpc::OR16rr, {MOp::from_reg(high), MOp::from_reg(tied)});
            }

            void wide_arithmetic(IrValue const* inst)
            {
                auto [lhs, rhs] = binary_operands(inst);
                auto l = pair(lhs), r = pair(rhs);
                if (!l || !r)
                    return;
                Pair out;
                if (inst->kind == IrNodeKind::Mul)
                {
                    auto cross_a = emit(MOpc::IMUL32rr, {MOp::from_reg(l->low), MOp::from_reg(r->high)});
                    auto cross_b = emit(MOpc::IMUL32rr, {MOp::from_reg(l->high), MOp::from_reg(r->low)});
                    auto cross = emit(MOpc::ADD32rr, {MOp::from_reg(cross_a), MOp::from_reg(cross_b)});
                    copy_to(PhysReg::RAX, l->low, 32);
                    MInstr mul = make_instr(MOpc::MUL32r, {MOp::from_reg(r->low)}, 0);
                    mul.implicit_uses = bit(PhysReg::RAX);
                    mul.implicit_defs = bit(PhysReg::RAX) | bit(PhysReg::RDX);
                    append(mul);
                    out.low = copy_from(PhysReg::RAX);
                    auto high = copy_from(PhysReg::RDX);
                    out.high = emit(MOpc::ADD32rr, {MOp::from_reg(high), MOp::from_reg(cross)});
                }
                else
                {
                    MOpc low = inst->kind == IrNodeKind::Add   ? MOpc::ADD32rr
                               : inst->kind == IrNodeKind::Sub ? MOpc::SUB32rr
                               : inst->kind == IrNodeKind::And ? MOpc::AND32rr
                               : inst->kind == IrNodeKind::Or  ? MOpc::OR32rr
                                                               : MOpc::XOR32rr;
                    MOpc high = inst->kind == IrNodeKind::Add ? MOpc::ADC32rr : inst->kind == IrNodeKind::Sub ? MOpc::SBB32rr : low;
                    out.low = emit(low, {MOp::from_reg(l->low), MOp::from_reg(r->low)});
                    out.high = emit(high, {MOp::from_reg(l->high), MOp::from_reg(r->high)});
                }
                pairs[inst] = out;
            }

            void wide_shift(IrValue const* inst)
            {
                auto [lhs, rhs] = binary_operands(inst);
                auto input = pair(lhs);
                if (!input)
                    return;
                bool left = inst->kind == IrNodeKind::Shl;
                bool arithmetic = inst->kind == IrNodeKind::AShr;
                auto single = left ? MOpc::SHL32ri8 : arithmetic ? MOpc::SAR32ri8 : MOpc::SHR32ri8;
                auto double_op = left ? MOpc::SHLD32rri8 : MOpc::SHRD32rri8;
                if (auto imm = ir_cast<IrIntConstant>(rhs))
                {
                    auto count = static_cast<unsigned>(imm->value) & 63;
                    if (!count)
                        return void(pairs[inst] = *input);
                    if (count >= 32)
                    {
                        auto shifted = emit(single, {MOp::from_reg(left ? input->low : input->high), MOp::from_imm(count - 32)});
                        auto fill =
                            arithmetic ? emit(MOpc::SAR32ri8, {MOp::from_reg(input->high), MOp::from_imm(31)}) : emit(MOpc::MOV32ri, {MOp::from_imm(0)});
                        pairs[inst] = left ? Pair{fill, shifted} : Pair{shifted, fill};
                        return;
                    }
                    copy_to(PhysReg::RAX, left ? input->high : input->low, 32);
                    MInstr shift = make_instr(double_op, {phys_operand(PhysReg::RAX), MOp::from_reg(left ? input->low : input->high), MOp::from_imm(count)}, 1);
                    shift.implicit_uses = bit(PhysReg::RAX);
                    append(shift);
                    auto combined = copy_from(PhysReg::RAX);
                    auto shifted = emit(single, {MOp::from_reg(left ? input->low : input->high), MOp::from_imm(count)});
                    pairs[inst] = left ? Pair{shifted, combined} : Pair{combined, shifted};
                    return;
                }
                std::optional<VReg> count;
                if (wide(rhs->type))
                {
                    if (auto src = pair(rhs))
                        count = src->low;
                }
                else
                    count = value(rhs);
                if (!count)
                    return;
                append(make_instr(MOpc::TEST16ri, {MOp::from_reg(*count), MOp::from_imm(32)}, 0));
                auto large = condition(MOpc::SETNEr);
                auto widened = emit(MOpc::MOVZX32_16rr, {MOp::from_reg(large)});
                auto mask = emit(MOpc::NEG32r, {MOp::from_reg(widened)});
                auto inverse = emit(MOpc::NOT32r, {MOp::from_reg(mask)});
                auto fill = arithmetic ? emit(MOpc::SAR32ri8, {MOp::from_reg(input->high), MOp::from_imm(31)}) : emit(MOpc::MOV32ri, {MOp::from_imm(0)});
                copy_to(PhysReg::RCX, *count, 32);
                copy_to(PhysReg::RAX, input->low, 32);
                copy_to(PhysReg::RDX, input->high, 32);
                MInstr combined = make_instr(
                    left ? MOpc::SHLD32rrCL : MOpc::SHRD32rrCL,
                    {phys_operand(left ? PhysReg::RDX : PhysReg::RAX), phys_operand(left ? PhysReg::RAX : PhysReg::RDX), phys_operand(PhysReg::RCX)}, 1);
                combined.implicit_uses = bit(PhysReg::RAX) | bit(PhysReg::RDX) | bit(PhysReg::RCX);
                append(combined);
                auto reg = left ? PhysReg::RAX : PhysReg::RDX;
                MInstr shifted = make_instr(left         ? MOpc::SHL32rCL
                                            : arithmetic ? MOpc::SAR32rCL
                                                         : MOpc::SHR32rCL,
                                            {phys_operand(reg), phys_operand(reg), phys_operand(PhysReg::RCX)}, 1);
                append(shifted);
                auto low = copy_from(PhysReg::RAX), high = copy_from(PhysReg::RDX);
                auto select = [&](VReg ordinary, VReg large_value) {
                    auto a = emit(MOpc::AND32rr, {MOp::from_reg(ordinary), MOp::from_reg(inverse)});
                    auto b = emit(MOpc::AND32rr, {MOp::from_reg(large_value), MOp::from_reg(mask)});
                    return emit(MOpc::OR32rr, {MOp::from_reg(a), MOp::from_reg(b)});
                };
                pairs[inst] = left ? Pair{select(low, fill), select(high, low)} : Pair{select(low, high), select(high, fill)};
            }

            [[nodiscard]] unsigned value_bits(IrType const* type)
            {
                auto bits = scalar_bits(type);
                if (bits != 8 && bits != 16 && bits != 32)
                {
                    unsupported(
                        std::format("a {}-byte {} value", type ? type->byte_size : 0, type && type->kind == IrTypeKind::Int ? "integer" : "non-integer"));
                    return 0;
                }
                return bits;
            }

            [[nodiscard]] static unsigned op_width(unsigned bits) noexcept { return bits <= 16 ? 16 : 32; }

            [[nodiscard]] static std::optional<std::int64_t> constant(IrValue const* value)
            {
                if (auto* integer = ir_cast<IrIntConstant>(value))
                    return static_cast<std::int32_t>(static_cast<std::uint32_t>(integer->value));
                if (auto* pointer = ir_cast<IrPointerConstant>(value); pointer && static_cast<IrPointerType const*>(pointer->type)->flavor != PointerFlavor::Far)
                    return static_cast<std::int32_t>(pointer->offset);
                if (ir_cast<IrNullConstant>(value))
                    return 0;
                if (auto* boolean = ir_cast<IrBoolConstant>(value))
                    return boolean->value ? 1 : 0;
                return std::nullopt;
            }

            [[nodiscard]] static std::int64_t fit(std::int64_t value, unsigned bits, bool signed_value, unsigned width)
            {
                std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
                auto raw = static_cast<std::uint64_t>(value) & mask;
                std::int64_t extended = signed_value && bits < 64 && (raw >> (bits - 1)) != 0 ? static_cast<std::int64_t>(raw | ~mask) : static_cast<std::int64_t>(raw);
                return width == 16 ? static_cast<std::int16_t>(extended) : static_cast<std::int32_t>(extended);
            }

            [[nodiscard]] std::optional<VReg> value(IrValue const* v)
            {
                if (!v)
                {
                    unsupported("a missing operand");
                    return std::nullopt;
                }
                if (auto found = values.find(v); found != values.end())
                    return found->second;
                if (auto found = slots.find(v); found != slots.end())
                    return slot_address(found->second);
                if (auto* ref = ir_cast<IrGlobalRef>(v))
                {
                    auto bits = value_bits(v->type);
                    if (!bits)
                        return std::nullopt;
                    auto name = ref->global ? ref->global->name : ref->function ? ref->function->name : std::string_view{};
                    if (name.empty())
                    {
                        unsupported("an unnamed global address");
                        return std::nullopt;
                    }
                    MMem mem{};
                    mem.symbol = name;
                    mem.disp = static_cast<std::int32_t>(ref->addend);
                    mem.address_bits = target.pointer_bits;
                    return emit(bits == 16 ? MOpc::LEA16rm : MOpc::LEA32rm, {MOp::from_mem(mem)});
                }
                if (memory_type(v->type))
                    return memory_value(v);
                if (auto imm = constant(v))
                {
                    auto bits = value_bits(v->type);
                    if (!bits)
                        return std::nullopt;
                    VReg r = mfunc.new_vreg();
                    auto width = op_width(bits);
                    append(make_instr(width == 16 ? MOpc::MOV16ri : MOpc::MOV32ri, {MOp::from_reg(r), MOp::from_imm(fit(*imm, bits, false, width))}, 1));
                    return r;
                }
                unsupported(std::format("an operand of IR {}", kind_name(v->kind)));
                return std::nullopt;
            }

            [[nodiscard]] static bool has_based_type(IrType const* type)
            {
                std::unordered_set<IrType const*> seen;
                auto visit = [&](auto&& self, IrType const* t) -> bool {
                    if (!t || !seen.insert(t).second)
                        return false;
                    if (t->kind == IrTypeKind::Pointer)
                    {
                        auto* p = static_cast<IrPointerType const*>(t);
                        return p->flavor != PointerFlavor::Near || self(self, p->pointee);
                    }
                    if (t->kind == IrTypeKind::Slice)
                        return static_cast<IrSliceType const*>(t)->flavor != PointerFlavor::Near;
                    if (t->kind == IrTypeKind::Array)
                        return self(self, static_cast<IrArrayType const*>(t)->element);
                    if (t->kind == IrTypeKind::Aggregate)
                        return std::ranges::any_of(static_cast<IrAggregateType const*>(t)->members, [&](IrType const* m) { return self(self, m); });
                    return false;
                };
                return visit(visit, type);
            }

            [[nodiscard]] static SegmentOverride based_segment(IrType const* type)
            {
                if (!type || type->kind != IrTypeKind::Pointer)
                    return SegmentOverride::None;
                auto* pointer = static_cast<IrPointerType const*>(type);
                if (pointer->flavor == PointerFlavor::Far)
                    return SegmentOverride::ES;
                if (pointer->flavor != PointerFlavor::Based)
                    return SegmentOverride::None;
                switch (pointer->seg)
                {
                    case Segment::Cs: return SegmentOverride::CS;
                    case Segment::Ds: return SegmentOverride::DS;
                    case Segment::Es: return SegmentOverride::ES;
                    case Segment::Ss: return SegmentOverride::SS;
                    case Segment::Fs: return SegmentOverride::FS;
                    case Segment::Gs: return SegmentOverride::GS;
                    default: return SegmentOverride::None;
                }
            }

            [[nodiscard]] std::uint8_t address_bits(IrType const* type) const
            {
                return based_segment(type) == SegmentOverride::None ? target.pointer_bits : target.far_offset_bits();
            }

            [[nodiscard]] MMem pointer_memory(VReg base, IrType const* type, std::int32_t offset = 0) const
            {
                auto mem = MMem::make_base_disp(base, offset);
                mem.address_bits = address_bits(type);
                mem.segment = based_segment(type);
                return mem;
            }

            [[nodiscard]] std::optional<MOp> address(IrValue const* pointer)
            {
                if (auto found = slots.find(pointer); found != slots.end())
                    return MOp::from_frame_slot(found->second);
                if (far(pointer->type))
                {
                    if (auto source = pair(pointer))
                        return MOp::from_mem(pointer_memory(source->low, pointer->type));
                    return std::nullopt;
                }
                auto segment = based_segment(pointer->type);
                if (segment != SegmentOverride::None)
                {
                    auto offset = constant(pointer);
                    if (auto* make = ir_cast<IrMakePointerInst>(pointer))
                        offset = constant(make->offset);
                    if (offset)
                    {
                        MMem mem{};
                        mem.disp = static_cast<std::int32_t>(*offset);
                        mem.address_bits = address_bits(pointer->type);
                        mem.segment = segment;
                        mem.absolute = true;
                        return MOp::from_mem(mem);
                    }
                }
                if (auto* ref = ir_cast<IrGlobalRef>(pointer); ref && ref->global)
                {
                    MMem mem{};
                    mem.symbol = ref->global->name;
                    mem.disp = static_cast<std::int32_t>(ref->addend);
                    mem.address_bits = address_bits(pointer->type);
                    mem.segment = segment;
                    return MOp::from_mem(mem);
                }
                auto reg = value(pointer);
                if (reg)
                    return MOp::from_mem(pointer_memory(*reg, pointer->type));
                return std::nullopt;
            }

            [[nodiscard]] VReg emit(MOpc opc, std::initializer_list<MOp> sources)
            {
                VReg d = mfunc.new_vreg();
                MInstr mi = make_instr(opc, {MOp::from_reg(d)}, 1);
                for (auto const& op : sources)
                    mi.ops[mi.num_ops++] = op;
                append(mi);
                return d;
            }

            [[nodiscard]] VReg extend(VReg v, unsigned bits, bool signed_value, unsigned width)
            {
                if (bits >= width)
                    return v;
                if (bits == 16)
                    return emit(signed_value ? MOpc::MOVSX32_16rr : MOpc::MOVZX32_16rr, {MOp::from_reg(v)});
                if (!signed_value)
                    return emit(width == 16 ? MOpc::AND16ri : MOpc::AND32ri, {MOp::from_reg(v), MOp::from_imm(0xFF)});
                auto shift = static_cast<std::int64_t>(width - 8);
                auto shifted = emit(width == 16 ? MOpc::SHL16ri8 : MOpc::SHL32ri8, {MOp::from_reg(v), MOp::from_imm(shift)});
                return emit(width == 16 ? MOpc::SAR16ri8 : MOpc::SAR32ri8, {MOp::from_reg(shifted), MOp::from_imm(shift)});
            }

            void copy_to(PhysReg reg, VReg v, unsigned width)
            {
                append(make_instr(width == 16 ? MOpc::MOV16rr : MOpc::COPY, {phys_operand(reg), MOp::from_reg(v)}, 1));
            }

            [[nodiscard]] VReg copy_from(PhysReg reg)
            {
                VReg d = mfunc.new_vreg();
                append(make_instr(MOpc::COPY, {MOp::from_reg(d), phys_operand(reg)}, 1));
                return d;
            }

            [[nodiscard]] static bool memory_type(IrType const* type)
            {
                return type && (type->kind == IrTypeKind::Aggregate || type->kind == IrTypeKind::Array || type->kind == IrTypeKind::Slice);
            }

            [[nodiscard]] static bool indirect_return(IrType const* type)
            {
                return memory_type(type) && type->byte_size != 1 && type->byte_size != 2 && type->byte_size != 4;
            }

            [[nodiscard]] MMem near_memory(VReg base, std::int32_t offset = 0) const
            {
                auto mem = MMem::make_base_disp(base, offset);
                mem.address_bits = target.pointer_bits;
                return mem;
            }

            [[nodiscard]] VReg slot_address(std::uint32_t slot)
            {
                auto address = emit(MOpc::LEA16rm, {MOp::from_frame_slot(slot)});
                return target.pointer_bits == 32 ? emit(MOpc::MOVZX32_16rr, {MOp::from_reg(address)}) : address;
            }

            [[nodiscard]] VReg temporary(IrType const* type)
            {
                if (type->byte_size > 65535)
                {
                    unsupported("an aggregate larger than the 16-bit stack");
                    return {};
                }
                return slot_address(mfunc.new_frame_slot(static_cast<std::uint32_t>(type->byte_size),
                                                         std::min<std::uint32_t>(static_cast<std::uint32_t>(std::max<std::uint64_t>(type->byte_align, 1)), 2)));
            }

            [[nodiscard]] VReg offset_address(VReg base, std::uint64_t offset, unsigned bits = 0)
            {
                if (!offset)
                    return base;
                if (!bits)
                    bits = target.pointer_bits;
                return emit(bits == 16 ? MOpc::ADD16ri : MOpc::ADD32ri,
                            {MOp::from_reg(base), MOp::from_imm(fit(static_cast<std::int64_t>(offset), bits, false, bits))});
            }

            void bulk_memory(VReg destination, VReg source, std::uint64_t size, bool zero, IrType const* destination_type = nullptr,
                             IrType const* source_type = nullptr, SegmentOverride source_override = SegmentOverride::None, bool reverse = false)
            {
                if (!size)
                    return;
                if (size > std::numeric_limits<std::uint32_t>::max())
                    return unsupported("a memory operation larger than the 32-bit offset space");
                if (size <= 8)
                {
                    for (std::uint64_t offset = 0; offset < size;)
                    {
                        auto bytes = size - offset >= 4 ? 4u : size - offset >= 2 ? 2u : 1u;
                        auto dst = MOp::from_mem(pointer_memory(destination, destination_type, static_cast<std::int32_t>(offset)));
                        if (zero)
                            append(make_instr(bytes == 4 ? MOpc::MOV32mi : bytes == 2 ? MOpc::MOV16mi : MOpc::MOV8mi, {dst, MOp::from_imm(0)}, 0));
                        else
                        {
                            auto loaded = emit(bytes == 4   ? MOpc::MOV32rm
                                               : bytes == 2 ? MOpc::MOV16rm
                                                            : MOpc::MOVZX16rm8,
                                               {MOp::from_mem(pointer_memory(source, source_type, static_cast<std::int32_t>(offset)))});
                            append(make_instr(bytes == 4 ? MOpc::MOV32mr : bytes == 2 ? MOpc::MOV16mr : MOpc::MOV8mr, {dst, MOp::from_reg(loaded)}, 0));
                        }
                        offset += bytes;
                    }
                    return;
                }
                auto source_segment = source_override == SegmentOverride::None ? based_segment(source_type) : source_override;
                auto destination_segment = based_segment(destination_type);
                bool const save_es = preserve_es || source_segment != SegmentOverride::None || destination_segment != SegmentOverride::None;
                bool const save_fs = !zero && source_segment == SegmentOverride::ES && destination_segment != SegmentOverride::ES;
                auto phys_segment = [](SegmentOverride segment) {
                    switch (segment)
                    {
                        case SegmentOverride::CS: return PhysReg::CS;
                        case SegmentOverride::ES: return PhysReg::ES;
                        case SegmentOverride::SS: return PhysReg::SS;
                        case SegmentOverride::FS: return PhysReg::FS;
                        case SegmentOverride::GS: return PhysReg::GS;
                        default: return PhysReg::DS;
                    }
                };
                if (save_es)
                    append(make_instr(MOpc::PUSHSEG, {phys_operand(PhysReg::ES)}, 0));
                if (save_fs)
                {
                    append(make_instr(MOpc::PUSHSEG, {phys_operand(PhysReg::FS)}, 0));
                    append(make_instr(MOpc::PUSHSEG, {phys_operand(PhysReg::ES)}, 0));
                    append(make_instr(MOpc::POPSEG, {phys_operand(PhysReg::FS)}, 1));
                    source_segment = SegmentOverride::FS;
                }
                if (destination_segment != SegmentOverride::ES)
                {
                    append(make_instr(MOpc::PUSHSEG, {phys_operand(phys_segment(destination_segment))}, 0));
                    append(make_instr(MOpc::POPSEG, {phys_operand(PhysReg::ES)}, 1));
                }
                auto width = std::max<unsigned>(address_bits(destination_type), zero ? 16u : address_bits(source_type));
                if (size > 65535)
                    width = 32;
                if (reverse)
                {
                    source = offset_address(extend(source, address_bits(source_type), false, width), size - 1, width);
                    destination = offset_address(extend(destination, address_bits(destination_type), false, width), size - 1, width);
                }
                if (!zero)
                    copy_to(PhysReg::RSI, extend(source, reverse ? width : address_bits(source_type), false, width), width);
                copy_to(PhysReg::RDI, extend(destination, reverse ? width : address_bits(destination_type), false, width), width);
                append(make_instr(width == 16 ? MOpc::MOV16ri : MOpc::MOV32ri,
                                  {phys_operand(PhysReg::RCX), MOp::from_imm(static_cast<std::int64_t>(reverse ? size : size / 2))}, 1));
                if (zero)
                    append(make_instr(MOpc::MOV16ri, {phys_operand(PhysReg::RAX), MOp::from_imm(0)}, 1));
                auto strings = [&](bool tail) {
                    MInstr rep = make_instr(
                        zero ? (width == 16 ? (tail ? MOpc::REPSTOS : MOpc::REPSTOSW) : (tail ? MOpc::REPSTOS_A32 : MOpc::REPSTOSW_A32))
                             : (width == 16 ? (tail ? MOpc::REPMOVS : MOpc::REPMOVSW) : (tail ? MOpc::REPMOVS_A32 : MOpc::REPMOVSW_A32)),
                        {}, 0);
                    if (!zero && source_segment != SegmentOverride::None && source_segment != SegmentOverride::DS)
                    {
                        MMem source_mem{};
                        source_mem.segment = source_segment;
                        rep.ops[rep.num_ops++] = MOp::from_mem(source_mem);
                    }
                    rep.implicit_defs = bit(PhysReg::RDI) | bit(PhysReg::RCX) | (zero ? 0 : bit(PhysReg::RSI));
                    rep.implicit_uses = rep.implicit_defs | bit(PhysReg::ES) | (zero ? bit(PhysReg::RAX) : bit(phys_segment(source_segment)));
                    append(rep);
                };
                if (reverse)
                    append(make_instr(MOpc::STD, {}, 0));
                strings(reverse);
                if (reverse)
                    append(make_instr(MOpc::CLD, {}, 0));
                if (!reverse && (size & 1))
                {
                    append(make_instr(width == 16 ? MOpc::MOV16ri : MOpc::MOV32ri, {phys_operand(PhysReg::RCX), MOp::from_imm(1)}, 1));
                    strings(true);
                }
                if (save_fs)
                    append(make_instr(MOpc::POPSEG, {phys_operand(PhysReg::FS)}, 1));
                if (save_es)
                    append(make_instr(MOpc::POPSEG, {phys_operand(PhysReg::ES)}, 1));
            }

            [[nodiscard]] std::optional<VReg> pointer_address(IrValue const* pointer)
            {
                if (far(pointer->type))
                {
                    if (auto source = pair(pointer))
                        return source->low;
                    return std::nullopt;
                }
                return value(pointer);
            }

            [[nodiscard]] std::uint64_t member_offset(IrType const* type, std::uint32_t field)
            {
                if (type->kind == IrTypeKind::Aggregate)
                    return static_cast<IrAggregateType const*>(type)->member_offsets.at(field);
                if (type->kind == IrTypeKind::Array)
                    return static_cast<IrArrayType const*>(type)->element->byte_size * field;
                if (type->kind == IrTypeKind::Slice)
                {
                    if (static_cast<IrSliceType const*>(type)->flavor == PointerFlavor::Far)
                        return (target.far_offset_bits() == 16 ? 4u : 8u) * field;
                    return (static_cast<IrSliceType const*>(type)->flavor == PointerFlavor::Based ? target.far_offset_bits() : target.pointer_bits) / 8 * field;
                }
                return target.pointer_bits / 8 * field;
            }

            void store_at(IrValue const* stored, VReg destination)
            {
                if (far(stored->type))
                {
                    if (auto source = pair(stored))
                        store_far(*source, near_memory(destination));
                    return;
                }
                if (wide(stored->type))
                {
                    if (auto src = pair(stored))
                        store_pair(*src, near_memory(destination));
                    return;
                }
                auto src = value(stored);
                if (!src)
                    return;
                if (memory_type(stored->type))
                    return bulk_memory(destination, *src, stored->type->byte_size, false);
                auto bits = value_bits(stored->type);
                if (bits)
                    append(make_instr(bits == 8    ? MOpc::MOV8mr
                                      : bits == 16 ? MOpc::MOV16mr
                                                   : MOpc::MOV32mr,
                                      {MOp::from_mem(near_memory(destination)), MOp::from_reg(*src)}, 0));
            }

            [[nodiscard]] static bool zero_value(IrValue const* v)
            {
                if (!v || ir_cast<IrNullConstant>(v))
                    return true;
                if (auto* integer = ir_cast<IrIntConstant>(v))
                    return integer->value == 0;
                if (auto imm = constant(v))
                    return *imm == 0;
                if (auto const* agg = ir_cast<IrAggregateInst>(v))
                    return std::ranges::all_of(agg->values, [](IrValue const* member) { return zero_value(member); });
                return false;
            }

            [[nodiscard]] std::optional<VReg> memory_value(IrValue const* v)
            {
                auto dst = temporary(v->type);
                if (!dst.is_valid())
                    return std::nullopt;
                if (auto const* agg = ir_cast<IrAggregateInst>(v))
                {
                    std::uint64_t covered = 0;
                    bool complete = !agg->values.empty();
                    for (std::uint32_t i = 0; i < agg->values.size(); ++i)
                    {
                        auto* member = agg->values[i];
                        if (!member || !member->type || zero_value(member) || member_offset(v->type, i) != covered)
                        {
                            complete = false;
                            break;
                        }
                        covered += member->type->byte_size;
                    }
                    complete = complete && covered == v->type->byte_size;
                    if (!complete)
                        bulk_memory(dst, {}, v->type->byte_size, true);
                    for (std::uint32_t i = 0; i < agg->values.size(); ++i)
                        if (complete || !zero_value(agg->values[i]))
                            store_at(agg->values[i], offset_address(dst, member_offset(v->type, i)));
                }
                else if (auto const* str = ir_cast<IrStringConstant>(v); str && v->type->kind == IrTypeKind::Array)
                {
                    bulk_memory(dst, {}, v->type->byte_size, true);
                    for (std::uint32_t i = 0; i < std::min<std::uint64_t>(str->value.size(), v->type->byte_size); ++i)
                        append(make_instr(
                            MOpc::MOV8mi,
                            {MOp::from_mem(near_memory(dst, static_cast<std::int32_t>(i))), MOp::from_imm(static_cast<unsigned char>(str->value[i]))}, 0));
                }
                else if (ir_cast<IrNullConstant>(v))
                    bulk_memory(dst, {}, v->type->byte_size, true);
                else
                {
                    unsupported(std::format("an aggregate operand of IR {}", kind_name(v->kind)));
                    return std::nullopt;
                }
                values[v] = dst;
                return dst;
            }

            void lower_alloca(IrAllocaInst const& alloca)
            {
                auto* type = alloca.allocated_type;
                auto count = alloca.count ? constant(alloca.count) : std::optional<std::int64_t>{1};
                if (!count || *count < 0 || static_cast<std::uint64_t>(*count) * type->byte_size > 65535)
                    return unsupported("a dynamic or oversized local allocation");
                auto align = alloca.alignment ? alloca.alignment : type->byte_align;
                if (!memory_type(type) && !wide(type) && !far(type) && !value_bits(type))
                    return;
                slots[&alloca] = mfunc.new_frame_slot(static_cast<std::uint32_t>(type->byte_size * static_cast<std::uint64_t>(*count)),
                                                      std::min<std::uint32_t>(static_cast<std::uint32_t>(std::max<std::uint64_t>(align, 1)), 2));
            }

            void load_at(IrValue const* inst, VReg source)
            {
                if (far(inst->type))
                {
                    pairs[inst] = load_far(MOp::from_mem(near_memory(source)));
                    return;
                }
                if (wide(inst->type))
                {
                    pairs[inst] = load_pair(near_memory(source));
                    return;
                }
                if (memory_type(inst->type))
                {
                    auto dst = temporary(inst->type);
                    bulk_memory(dst, source, inst->type->byte_size, false);
                    values[inst] = dst;
                }
                else if (auto bits = value_bits(inst->type))
                    values[inst] = emit(bits == 8 ? MOpc::MOVZX16rm8 : bits == 16 ? MOpc::MOV16rm : MOpc::MOV32rm, {MOp::from_mem(near_memory(source))});
            }

            void lower_load(IrValue const* inst, IrValue const* pointer)
            {
                FarAccess access(*this, pointer);
                if (far(inst->type))
                {
                    if (auto memory = address(pointer))
                        pairs[inst] = load_far(*memory);
                    return;
                }
                if (wide(inst->type))
                {
                    if (auto source = pointer_address(pointer))
                        pairs[inst] = load_pair(pointer_memory(*source, pointer->type));
                    return;
                }
                if (memory_type(inst->type))
                {
                    auto src = pointer_address(pointer);
                    if (src)
                    {
                        auto dst = temporary(inst->type);
                        bulk_memory(dst, *src, inst->type->byte_size, false, nullptr, pointer->type);
                        values[inst] = dst;
                    }
                    return;
                }
                auto bits = value_bits(inst->type);
                auto mem = bits ? address(pointer) : std::nullopt;
                if (mem)
                    values[inst] = emit(bits == 8 ? MOpc::MOVZX16rm8 : bits == 16 ? MOpc::MOV16rm : MOpc::MOV32rm, {*mem});
            }

            void lower_store(IrValue const* stored, IrValue const* pointer)
            {
                auto* large_source = stored && memory_type(stored->type) && stored->type->byte_size > 65535 ? ir_cast<IrLoadInst>(stored) : nullptr;
                auto source_binding = large_source ? based_segment(large_source->pointer->type) : SegmentOverride::None;
                bool borrow = large_source && (far(large_source->pointer->type) || (far(pointer->type) && source_binding == SegmentOverride::ES));
                auto scratch = based_segment(pointer->type) == SegmentOverride::FS ? PhysReg::GS : PhysReg::FS;
                struct RestoreSegment
                {
                    Isel& selector;
                    PhysReg reg;
                    bool active;
                    ~RestoreSegment()
                    {
                        if (active)
                            selector.append(make_instr(MOpc::POPSEG, {phys_operand(reg)}, 1));
                    }
                } restore{*this, scratch, borrow};
                if (borrow)
                {
                    append(make_instr(MOpc::PUSHSEG, {phys_operand(scratch)}, 0));
                    if (far(large_source->pointer->type))
                        append(make_instr(MOpc::MOV16sr, {phys_operand(scratch), MOp::from_reg(pair(large_source->pointer)->high)}, 1));
                    else
                    {
                        append(make_instr(MOpc::PUSHSEG, {phys_operand(PhysReg::ES)}, 0));
                        append(make_instr(MOpc::POPSEG, {phys_operand(scratch)}, 1));
                    }
                    source_binding = scratch == PhysReg::FS ? SegmentOverride::FS : SegmentOverride::GS;
                }
                FarAccess access(*this, pointer);
                if (far(stored->type))
                {
                    auto destination = pointer_address(pointer);
                    if (auto source = pair(stored); source && destination)
                        store_far(*source, pointer_memory(*destination, pointer->type));
                    return;
                }
                if (wide(stored->type))
                {
                    auto destination = pointer_address(pointer);
                    if (auto src = pair(stored); src && destination)
                        store_pair(*src, pointer_memory(*destination, pointer->type));
                    return;
                }
                if (memory_type(stored ? stored->type : nullptr) && stored->type->byte_size > 65535)
                {
                    if (target.code_model != target::CodeModel::Unreal && target.code_model != target::CodeModel::Unreal32)
                        return unsupported("a large memory operation in the small model");
                    auto destination = pointer_address(pointer);
                    if (!destination)
                        return;
                    if (zero_value(stored))
                        return bulk_memory(*destination, {}, stored->type->byte_size, true, pointer->type);
                    auto* load = ir_cast<IrLoadInst>(stored);
                    if (!load || !fused.contains(stored))
                        return unsupported("a large aggregate value requiring temporary storage");
                    auto source = pointer_address(load->pointer);
                    if (!source)
                        return;
                    auto segment = source_binding;
                    auto linear = [&](VReg offset, IrType const* type, SegmentOverride binding) {
                        auto reg = binding == SegmentOverride::ES   ? PhysReg::ES
                                   : binding == SegmentOverride::CS ? PhysReg::CS
                                   : binding == SegmentOverride::SS ? PhysReg::SS
                                   : binding == SegmentOverride::FS ? PhysReg::FS
                                   : binding == SegmentOverride::GS ? PhysReg::GS
                                                                    : PhysReg::DS;
                        auto base = emit(MOpc::SHL32ri8, {MOp::from_reg(read_segment(reg)), MOp::from_imm(4)});
                        return emit(MOpc::ADD32rr, {MOp::from_reg(extend(offset, address_bits(type), false, 32)), MOp::from_reg(base)});
                    };
                    auto src_linear = linear(*source, load->pointer->type, segment);
                    auto dst_linear = linear(*destination, pointer->type, based_segment(pointer->type));
                    auto current = block->id;
                    auto forward = mfunc.create_block("copy.forward").id;
                    auto backward = mfunc.create_block("copy.backward").id;
                    auto done = mfunc.create_block("copy.done").id;
                    block = mfunc.block_by_id(current);
                    append(make_instr(MOpc::CMP32rr, {MOp::from_reg(dst_linear), MOp::from_reg(src_linear)}, 0));
                    append(make_instr(MOpc::JBE, {MOp::from_label(forward)}, 0));
                    auto distance = emit(MOpc::SUB32rr, {MOp::from_reg(dst_linear), MOp::from_reg(src_linear)});
                    append(make_instr(MOpc::CMP32ri, {MOp::from_reg(distance), MOp::from_imm(static_cast<std::int64_t>(stored->type->byte_size))}, 0));
                    append(make_instr(MOpc::JAE, {MOp::from_label(forward)}, 0));
                    append(make_instr(MOpc::JMP, {MOp::from_label(backward)}, 0));
                    block = mfunc.block_by_id(forward);
                    bulk_memory(*destination, *source, stored->type->byte_size, false, pointer->type, load->pointer->type, segment);
                    append(make_instr(MOpc::JMP, {MOp::from_label(done)}, 0));
                    block = mfunc.block_by_id(backward);
                    bulk_memory(*destination, *source, stored->type->byte_size, false, pointer->type, load->pointer->type, segment, true);
                    append(make_instr(MOpc::JMP, {MOp::from_label(done)}, 0));
                    block = mfunc.block_by_id(done);
                    return;
                }
                if (memory_type(stored ? stored->type : nullptr))
                {
                    auto dst = pointer_address(pointer);
                    if (dst)
                        if (auto src = value(stored))
                            bulk_memory(*dst, *src, stored->type->byte_size, false, pointer->type);
                    return;
                }
                auto bits = value_bits(stored ? stored->type : nullptr);
                auto mem = bits ? address(pointer) : std::nullopt;
                if (!mem)
                    return;
                if (auto imm = constant(stored))
                {
                    auto opc = bits == 8 ? MOpc::MOV8mi : bits == 16 ? MOpc::MOV16mi : MOpc::MOV32mi;
                    append(make_instr(opc, {*mem, MOp::from_imm(fit(*imm, bits, false, op_width(bits)))}, 0));
                    return;
                }
                auto v = value(stored);
                if (!v)
                    return;
                if (bits == 8)
                {
                    append(make_instr(MOpc::MOV8mr, {*mem, MOp::from_reg(*v)}, 0));
                    return;
                }
                append(make_instr(bits == 16 ? MOpc::MOV16mr : MOpc::MOV32mr, {*mem, MOp::from_reg(*v)}, 0));
            }

            void lower_gep(IrGepInst const& gep)
            {
                auto ptr = pointer_address(gep.base);
                if (!ptr)
                    return;
                auto result = *ptr;
                auto bits = address_bits(gep.base->type);
                IrType const* type = nullptr;
                if (gep.base && gep.base->type && gep.base->type->kind == IrTypeKind::Pointer)
                    type = static_cast<IrPointerType const*>(gep.base->type)->pointee;
                else if (gep.base && gep.base->type &&
                         (gep.base->type->kind == IrTypeKind::Aggregate || gep.base->type->kind == IrTypeKind::Array))
                    type = gep.base->type;
                for (auto const& index : gep.indices)
                {
                    if (index.kind == IrGepInst::IndexKind::Field)
                    {
                        auto field_off = type ? member_offset(type, index.field_index)
                                              : static_cast<std::uint64_t>(target.pointer_bits) / 8 * index.field_index;
                        result = offset_address(result, field_off, bits);
                        if (type && type->kind == IrTypeKind::Aggregate)
                            type = static_cast<IrAggregateType const*>(type)->members.at(index.field_index);
                        else if (type && type->kind == IrTypeKind::Slice)
                            type = index.field_index == 0 ? gep.type : nullptr;
                        else
                            type = nullptr;
                    }
                    else
                    {
                        auto elem = type && type->kind == IrTypeKind::Array ? static_cast<IrArrayType const*>(type)->element : type;
                        auto stride = elem ? elem->byte_size : 1;
                        if (auto imm = constant(index.dynamic_index))
                            result = offset_address(result, static_cast<std::uint64_t>(*imm) * stride, bits);
                        else if (auto dynamic = value(index.dynamic_index))
                        {
                            auto* integer = ir_type_cast<IrIntType>(index.dynamic_index->type);
                            auto index_value =
                                extend(*dynamic, scalar_bits(index.dynamic_index->type),
                                       (far(gep.base->type) || based_segment(gep.base->type) != SegmentOverride::None) && integer && integer->is_signed, bits);
                            auto scaled = stride == 1 ? index_value
                                                      : emit(bits == 16 ? MOpc::IMUL16rri : MOpc::IMUL32rri,
                                                             {MOp::from_reg(index_value), MOp::from_imm(static_cast<std::int64_t>(stride))});
                            result = emit(bits == 16 ? MOpc::ADD16rr : MOpc::ADD32rr, {MOp::from_reg(result), MOp::from_reg(scaled)});
                        }
                        else
                            return;
                        type = elem;
                    }
                }
                if (far(gep.type))
                {
                    auto source = pair(gep.base);
                    if (source)
                        pairs[&gep] = {extend(result, bits, false, 32), source->high};
                }
                else
                    values[&gep] = result;
            }

            void lower_arithmetic(IrNode const* inst, IrType const* type)
            {
                if (wide(type))
                    return wide_arithmetic(static_cast<IrValue const*>(inst));
                auto [lhs, rhs] = binary_operands(inst);
                auto bits = value_bits(type);
                auto l = bits ? value(lhs) : std::nullopt;
                if (!l)
                    return;
                bool const narrow = op_width(bits) == 16;
                MOpc rr{};
                MOpc ri{};
                switch (inst->kind)
                {
                    case IrNodeKind::Add:
                        rr = narrow ? MOpc::ADD16rr : MOpc::ADD32rr, ri = narrow ? MOpc::ADD16ri : MOpc::ADD32ri;
                        break;
                    case IrNodeKind::Sub:
                        rr = narrow ? MOpc::SUB16rr : MOpc::SUB32rr, ri = narrow ? MOpc::SUB16ri : MOpc::SUB32ri;
                        break;
                    case IrNodeKind::And:
                        rr = narrow ? MOpc::AND16rr : MOpc::AND32rr, ri = narrow ? MOpc::AND16ri : MOpc::AND32ri;
                        break;
                    case IrNodeKind::Or:
                        rr = narrow ? MOpc::OR16rr : MOpc::OR32rr, ri = narrow ? MOpc::OR16ri : MOpc::OR32ri;
                        break;
                    case IrNodeKind::Xor:
                        rr = narrow ? MOpc::XOR16rr : MOpc::XOR32rr, ri = narrow ? MOpc::XOR16ri : MOpc::XOR32ri;
                        break;
                    default:
                        rr = narrow ? MOpc::IMUL16rr : MOpc::IMUL32rr, ri = narrow ? MOpc::IMUL16rri : MOpc::IMUL32rri;
                        break;
                }
                if (auto imm = constant(rhs))
                {
                    values[static_cast<IrValue const*>(inst)] = emit(ri, {MOp::from_reg(*l), MOp::from_imm(fit(*imm, bits, false, op_width(bits)))});
                    return;
                }
                auto r = value(rhs);
                if (!r)
                    return;
                values[static_cast<IrValue const*>(inst)] = emit(rr, {MOp::from_reg(*l), MOp::from_reg(*r)});
            }

            void lower_shift(IrNode const* inst, IrType const* type)
            {
                if (wide(type))
                    return wide_shift(static_cast<IrValue const*>(inst));
                auto [lhs, rhs] = binary_operands(inst);
                auto bits = value_bits(type);
                auto l = bits ? value(lhs) : std::nullopt;
                if (!l)
                    return;
                auto width = op_width(bits);
                bool const narrow = width == 16;
                VReg base = *l;
                if (inst->kind != IrNodeKind::Shl)
                    base = extend(base, bits, inst->kind == IrNodeKind::AShr, width);
                MOpc by_imm{};
                MOpc by_cl{};
                switch (inst->kind)
                {
                    case IrNodeKind::Shl:
                        by_imm = narrow ? MOpc::SHL16ri8 : MOpc::SHL32ri8, by_cl = narrow ? MOpc::SHL16rCL : MOpc::SHL32rCL;
                        break;
                    case IrNodeKind::LShr:
                        by_imm = narrow ? MOpc::SHR16ri8 : MOpc::SHR32ri8, by_cl = narrow ? MOpc::SHR16rCL : MOpc::SHR32rCL;
                        break;
                    default:
                        by_imm = narrow ? MOpc::SAR16ri8 : MOpc::SAR32ri8, by_cl = narrow ? MOpc::SAR16rCL : MOpc::SAR32rCL;
                        break;
                }
                if (auto imm = constant(rhs))
                {
                    values[static_cast<IrValue const*>(inst)] = emit(by_imm, {MOp::from_reg(base), MOp::from_imm(*imm & 31)});
                    return;
                }
                auto count = value(rhs);
                if (!count)
                    return;
                copy_to(PhysReg::RCX, *count, op_width(scalar_bits(rhs->type)));
                values[static_cast<IrValue const*>(inst)] = emit(by_cl, {MOp::from_reg(base), phys_operand(PhysReg::RCX)});
            }

            void lower_divide(IrNode const* inst, IrType const* type)
            {
                auto [lhs, rhs] = binary_operands(inst);
                auto bits = value_bits(type);
                bool signed_op = inst->kind == IrNodeKind::SDiv || inst->kind == IrNodeKind::SRem;
                auto l = bits ? value(lhs) : std::nullopt;
                auto r = l ? value(rhs) : std::nullopt;
                if (!r)
                    return;
                auto width = bits == 16 && signed_op ? 32u : op_width(bits);
                auto dividend = extend(*l, bits, signed_op, width);
                auto divisor = extend(*r, bits, signed_op, width);
                copy_to(PhysReg::RAX, dividend, width);
                std::uint64_t const ax = 1ULL << static_cast<unsigned>(PhysReg::RAX);
                std::uint64_t const dx = 1ULL << static_cast<unsigned>(PhysReg::RDX);
                if (signed_op)
                {
                    MInstr widen = make_instr(width == 16 ? MOpc::CWD : MOpc::CDQ, {}, 0);
                    widen.implicit_defs = dx;
                    widen.implicit_uses = ax;
                    append(widen);
                }
                else
                    append(make_instr(width == 16 ? MOpc::MOV16ri : MOpc::MOV32ri, {phys_operand(PhysReg::RDX), MOp::from_imm(0)}, 1));
                auto opc = width == 16 ? (signed_op ? MOpc::IDIV16r : MOpc::DIV16r) : (signed_op ? MOpc::IDIV32r : MOpc::DIV32r);
                MInstr divide = make_instr(opc, {MOp::from_reg(divisor)}, 0);
                divide.implicit_defs = ax | dx;
                divide.implicit_uses = ax | dx;
                append(divide);
                bool remainder = inst->kind == IrNodeKind::SRem || inst->kind == IrNodeKind::URem;
                values[static_cast<IrValue const*>(inst)] = copy_from(remainder ? PhysReg::RDX : PhysReg::RAX);
            }

            [[nodiscard]] bool emit_compare(IrNode const* inst, MOpc setcc)
            {
                auto [lhs, rhs] = binary_operands(inst);
                auto bits = value_bits(lhs ? lhs->type : nullptr);
                auto l = bits ? value(lhs) : std::nullopt;
                if (!l)
                    return false;
                auto width = op_width(bits);
                bool signed_compare = setcc == MOpc::SETLr || setcc == MOpc::SETLEr || setcc == MOpc::SETGr || setcc == MOpc::SETGEr;
                auto left = extend(*l, bits, signed_compare, width);
                if (auto imm = constant(rhs))
                    append(make_instr(width == 16 ? MOpc::CMP16ri : MOpc::CMP32ri, {MOp::from_reg(left), MOp::from_imm(fit(*imm, bits, signed_compare, width))}, 0));
                else
                {
                    auto r = value(rhs);
                    if (!r)
                        return false;
                    auto right = extend(*r, bits, signed_compare, width);
                    append(make_instr(width == 16 ? MOpc::CMP16rr : MOpc::CMP32rr, {MOp::from_reg(left), MOp::from_reg(right)}, 0));
                }
                return true;
            }

            void lower_compare(IrNode const* inst, MOpc setcc)
            {
                auto [lhs, rhs] = binary_operands(inst);
                if (lhs && (wide(lhs->type) || far(lhs->type)))
                {
                    auto l = pair(lhs), r = pair(rhs);
                    if (l && r)
                        values[static_cast<IrValue const*>(inst)] = compare_pair(*l, *r, setcc);
                    return;
                }
                if (!emit_compare(inst, setcc))
                    return;
                append(make_instr(setcc, {phys_operand(PhysReg::RAX)}, 1));
                values[static_cast<IrValue const*>(inst)] = emit(MOpc::MOVZX16_8rr, {phys_operand(PhysReg::RAX)});
            }

            void lower_unary(IrValue const* inst, IrValue const* operand)
            {
                if (wide(inst->type))
                {
                    auto src = pair(operand);
                    if (!src)
                        return;
                    auto high = emit(MOpc::NOT32r, {MOp::from_reg(src->high)});
                    auto low = emit(inst->kind == IrNodeKind::Neg ? MOpc::NEG32r : MOpc::NOT32r, {MOp::from_reg(src->low)});
                    if (inst->kind == IrNodeKind::Neg)
                    {
                        high = emit(MOpc::SBB32ri, {MOp::from_reg(high), MOp::from_imm(-1)});
                    }
                    pairs[inst] = {low, high};
                    return;
                }
                auto bits = value_bits(inst->type);
                auto v = bits ? value(operand) : std::nullopt;
                if (!v)
                    return;
                bool const narrow = op_width(bits) == 16;
                if (inst->kind == IrNodeKind::Neg)
                    values[inst] = emit(narrow ? MOpc::NEG16r : MOpc::NEG32r, {MOp::from_reg(*v)});
                else if (inst->type->kind == IrTypeKind::Bool)
                    values[inst] = emit(MOpc::XOR16ri, {MOp::from_reg(*v), MOp::from_imm(1)});
                else
                    values[inst] = emit(narrow ? MOpc::NOT16r : MOpc::NOT32r, {MOp::from_reg(*v)});
            }

            void lower_cast(IrValue const* inst, IrValue const* operand)
            {
                if (far(inst->type) && far(operand->type))
                {
                    if (auto source = pair(operand))
                        pairs[inst] = *source;
                    return;
                }
                if (wide(operand->type))
                {
                    if (auto src = pair(operand))
                    {
                        if (wide(inst->type))
                            pairs[inst] = *src;
                        else
                            values[inst] = src->low;
                    }
                    return;
                }
                if (wide(inst->type))
                {
                    auto src = value(operand);
                    if (!src)
                        return;
                    auto low = extend(*src, scalar_bits(operand->type), inst->kind == IrNodeKind::Sext, 32);
                    auto high = inst->kind == IrNodeKind::Sext ? emit(MOpc::SAR32ri8, {MOp::from_reg(low), MOp::from_imm(31)})
                                                               : emit(MOpc::MOV32ri, {MOp::from_imm(0)});
                    pairs[inst] = {low, high};
                    return;
                }
                auto to = value_bits(inst->type);
                auto from = to ? value_bits(operand ? operand->type : nullptr) : 0;
                auto v = from ? value(operand) : std::nullopt;
                if (!v)
                    return;
                if (inst->kind == IrNodeKind::Trunc || inst->kind == IrNodeKind::Bitcast || from >= to)
                    values[inst] = *v;
                else
                    values[inst] = extend(*v, from, inst->kind == IrNodeKind::Sext, op_width(to));
            }

            static constexpr std::array<PhysReg, 4> register_arguments{PhysReg::RAX, PhysReg::RDX, PhysReg::RBX, PhysReg::RCX};

            [[nodiscard]] static std::uint64_t bit(PhysReg reg) noexcept { return 1ULL << static_cast<unsigned>(reg); }

            void push_argument(IrValue const* arg, unsigned bits)
            {
                if (auto imm = constant(arg); imm && bits <= 16)
                {
                    append(make_instr(MOpc::PUSH16i, {MOp::from_imm(fit(*imm, bits, false, 16))}, 0));
                    return;
                }
                auto v = value(arg);
                if (!v)
                    return;
                append(make_instr(bits <= 16 ? MOpc::PUSH16r : MOpc::PUSH32r, {MOp::from_reg(*v)}, 0));
            }

            [[nodiscard]] std::vector<std::optional<PhysReg>> classify_arguments(std::span<IrValue* const> args, bool c_convention, bool sret)
            {
                std::vector<std::optional<PhysReg>> registers(args.size());
                std::size_t next = sret ? 1 : 0;
                for (std::size_t i = 0; i < args.size(); ++i)
                    if (!c_convention && !memory_type(args[i]->type) && !wide(args[i]->type))
                    {
                        auto count = far(args[i]->type) ? 2u : 1u;
                        if (next + count <= register_arguments.size())
                        {
                            registers[i] = register_arguments[next];
                            next += count;
                        }
                    }
                return registers;
            }

            [[nodiscard]] static PhysReg segment_argument_register(PhysReg offset) { return *(std::ranges::find(register_arguments, offset) + 1); }

            [[nodiscard]] static bool call_uses_c_abi(IrValue const* inst)
            {
                if (auto* call = ir_cast<IrCallInst>(inst))
                    return call->c_abi || call->cc != CallingConv::Cdecl;
                if (auto* call = ir_cast<IrCallTailInst>(inst))
                    return call->c_abi || call->cc != CallingConv::Cdecl;
                return false;
            }

            void lower_call(IrValue const* inst, IrValue const* callee, std::span<IrValue* const> args)
            {
                auto const* ref = ir_cast<IrGlobalRef>(callee);
                bool const indirect = ref == nullptr;
                if (indirect && far(callee->type))
                    return unsupported("an indirect call through a far function pointer");
                std::optional<VReg> target_reg;
                if (indirect)
                {
                    target_reg = value(callee);
                    if (!target_reg)
                        return;
                }
                if (ref && ref->function && ref->function->name == "__dcc_udivmod32" && args.size() == 3 && wide(inst->type) &&
                    std::ranges::all_of(args, [](IrValue const* arg) { return scalar_bits(arg->type) == 32; }))
                {
                    auto high = value(args[0]);
                    auto low = value(args[1]);
                    auto divisor = value(args[2]);
                    if (!high || !low || !divisor)
                        return;
                    auto input_group = mfunc.new_copy_group();
                    auto move = make_instr(MOpc::COPY, {phys_operand(PhysReg::RAX), MOp::from_reg(*low)}, 1);
                    move.copy_group = input_group;
                    append(move);
                    move = make_instr(MOpc::COPY, {phys_operand(PhysReg::RDX), MOp::from_reg(*high)}, 1);
                    move.copy_group = input_group;
                    append(move);
                    auto ax_dx = (1ULL << static_cast<unsigned>(PhysReg::RAX)) | (1ULL << static_cast<unsigned>(PhysReg::RDX));
                    auto divide = make_instr(MOpc::DIV32r, {MOp::from_reg(*divisor)}, 0);
                    divide.implicit_defs = ax_dx;
                    divide.implicit_uses = ax_dx;
                    append(divide);
                    Pair result{mfunc.new_vreg(), mfunc.new_vreg()};
                    auto output_group = mfunc.new_copy_group();
                    move = make_instr(MOpc::COPY, {MOp::from_reg(result.low), phys_operand(PhysReg::RAX)}, 1);
                    move.copy_group = output_group;
                    append(move);
                    move = make_instr(MOpc::COPY, {MOp::from_reg(result.high), phys_operand(PhysReg::RDX)}, 1);
                    move.copy_group = output_group;
                    append(move);
                    pairs[inst] = result;
                    return;
                }
                bool const c_convention = !indirect ? (ref->function ? uses_c_abi(*ref->function) : false) : call_uses_c_abi(inst);
                bool const sret = indirect_return(inst->type);
                auto registers = classify_arguments(args, c_convention, sret);
                std::optional<VReg> result;
                if (memory_type(inst->type))
                    result = temporary(inst->type);
                std::vector<VReg> inputs(args.size());
                for (std::size_t i = 0; i < args.size(); ++i)
                {
                    if (wide(args[i]->type) || far(args[i]->type))
                    {
                        if (!pair(args[i]))
                            return;
                        continue;
                    }
                    auto input = value(args[i]);
                    if (!input)
                        return;
                    inputs[i] = *input;
                }
                std::int64_t pushed = 0;
                for (std::size_t i = args.size(); i > 0; --i)
                {
                    auto* arg = args[i - 1];
                    if (registers[i - 1])
                        continue;
                    if (far(arg->type))
                    {
                        auto source = *pair(arg);
                        if (target.far_offset_bits() == 32)
                            append(make_instr(MOpc::PUSH16i, {MOp::from_imm(0)}, 0));
                        append(make_instr(MOpc::PUSH16r, {MOp::from_reg(source.high)}, 0));
                        append(make_instr(target.far_offset_bits() == 16 ? MOpc::PUSH16r : MOpc::PUSH32r, {MOp::from_reg(source.low)}, 0));
                        pushed += static_cast<std::int64_t>(arg->type->byte_size);
                        continue;
                    }
                    if (wide(arg->type))
                    {
                        auto src = *pair(arg);
                        append(make_instr(MOpc::PUSH32r, {MOp::from_reg(src.high)}, 0));
                        append(make_instr(MOpc::PUSH32r, {MOp::from_reg(src.low)}, 0));
                        pushed += 8;
                        continue;
                    }
                    auto bytes = memory_type(arg->type) ? (arg->type->byte_size + 1) & ~std::uint64_t{1} : scalar_bits(arg->type) <= 16 ? 2 : 4;
                    if (memory_type(arg->type))
                    {
                        append(make_instr(MOpc::SUB16ri,
                                          {phys_operand(PhysReg::RSP), phys_operand(PhysReg::RSP), MOp::from_imm(static_cast<std::int64_t>(bytes))}, 1));
                        auto stack = emit(MOpc::MOVZX32_16rr, {phys_operand(PhysReg::RSP)});
                        bulk_memory(stack, inputs[i - 1], arg->type->byte_size, false);
                    }
                    else
                        push_argument(arg, value_bits(arg->type));
                    pushed += bytes;
                }
                if (sret && c_convention)
                {
                    append(make_instr(target.pointer_bits == 16 ? MOpc::PUSH16r : MOpc::PUSH32r, {MOp::from_reg(*result)}, 0));
                    pushed += target.pointer_bits / 8;
                }
                std::uint64_t uses = bit(PhysReg::RSP);
                if (sret && !c_convention)
                {
                    copy_to(PhysReg::RAX, *result, target.pointer_bits);
                    uses |= bit(PhysReg::RAX);
                }
                for (std::size_t i = 0; i < args.size(); ++i)
                    if (registers[i])
                    {
                        if (far(args[i]->type))
                        {
                            auto source = *pair(args[i]);
                            auto segment = segment_argument_register(*registers[i]);
                            copy_to(*registers[i], source.low, target.far_offset_bits());
                            copy_to(segment, source.high, 16);
                            uses |= bit(segment);
                        }
                        else
                            copy_to(*registers[i], inputs[i], op_width(value_bits(args[i]->type)));
                        uses |= bit(*registers[i]);
                    }
                MInstr call;
                if (indirect)
                    call = make_instr(MOpc::CALL16r, {MOp::from_reg(*target_reg)}, 0);
                else
                    call = make_instr(MOpc::CALL, {MOp::from_symbol(ref->name)}, 0);
                call.implicit_uses = uses;
                call.implicit_defs = bit(PhysReg::RAX) | bit(PhysReg::RBX) | bit(PhysReg::RCX) | bit(PhysReg::RDX) |
                                     bit(PhysReg::ES) | bit(PhysReg::FS) | bit(PhysReg::GS);
                append(call);
                if (pushed > 0)
                    append(make_instr(MOpc::ADD16ri, {phys_operand(PhysReg::RSP), phys_operand(PhysReg::RSP), MOp::from_imm(pushed)}, 1));
                if (!inst->type || inst->type->kind == IrTypeKind::Void)
                    return;
                if (far(inst->type))
                {
                    auto offset = copy_from(PhysReg::RAX), segment = copy_from(PhysReg::RDX);
                    pairs[inst] = {extend(offset, target.far_offset_bits(), false, 32), extend(segment, 16, false, 32)};
                    return;
                }
                if (wide(inst->type))
                {
                    if (!c_convention)
                        pairs[inst] = {copy_from(PhysReg::RAX), copy_from(PhysReg::RDX)};
                    else
                    {
                        auto ax = copy_from(PhysReg::RAX), bx = copy_from(PhysReg::RBX);
                        auto cx = copy_from(PhysReg::RCX), dx = copy_from(PhysReg::RDX);
                        auto join = [&](VReg low, VReg high) {
                            auto a = emit(MOpc::MOVZX32_16rr, {MOp::from_reg(low)});
                            auto b = emit(MOpc::SHL32ri8, {MOp::from_reg(high), MOp::from_imm(16)});
                            return emit(MOpc::OR32rr, {MOp::from_reg(a), MOp::from_reg(b)});
                        };
                        pairs[inst] = {join(dx, cx), join(bx, ax)};
                    }
                    return;
                }
                if (sret)
                {
                    values[inst] = *result;
                    return;
                }
                auto bits = memory_type(inst->type) ? inst->type->byte_size * 8 : value_bits(inst->type);
                if (!bits)
                    return;
                auto returned = copy_from(PhysReg::RAX);
                if (bits == 32 && (c_convention || memory_type(inst->type)))
                {
                    auto high = copy_from(PhysReg::RDX);
                    auto shifted = emit(MOpc::SHL32ri8, {MOp::from_reg(high), MOp::from_imm(16)});
                    auto widened = emit(MOpc::MOVZX32_16rr, {MOp::from_reg(returned)});
                    returned = emit(MOpc::OR32rr, {MOp::from_reg(shifted), MOp::from_reg(widened)});
                }
                if (memory_type(inst->type))
                {
                    append(make_instr(bits == 8    ? MOpc::MOV8mr
                                      : bits == 16 ? MOpc::MOV16mr
                                                   : MOpc::MOV32mr,
                                      {MOp::from_mem(near_memory(*result)), MOp::from_reg(returned)}, 0));
                    values[inst] = *result;
                }
                else
                    values[inst] = returned;
            }

            void lower_parameters()
            {
                auto const& params = func.entry_block->params;
                auto ret = func.func_type->return_type;
                auto sret = indirect_return(ret);
                std::int32_t offset = 4;
                if (sret)
                {
                    if (c_abi)
                    {
                        MMem mem = MMem::make_base_disp(VReg::phys(PhysReg::RBP), offset);
                        sret_pointer = emit(target.pointer_bits == 16 ? MOpc::MOV16rm : MOpc::MOV32rm, {MOp::from_mem(mem)});
                        offset += target.pointer_bits / 8;
                    }
                    else
                        sret_pointer = copy_from(PhysReg::RAX);
                }
                auto registers = classify_arguments(params, c_abi, sret);
                auto parameter_uses = analysis::UseDef::build(func);
                for (std::size_t i = 0; i < params.size(); ++i)
                    if (registers[i])
                    {
                        if (far(params[i]->type))
                        {
                            auto found = parameter_uses.uses.find(params[i]);
                            bool returned = std::ranges::any_of(func.blocks, [&](IrBasicBlock const* entry) {
                                auto* ret = ir_cast<IrRetInst>(entry->terminator);
                                return ret && ret->value == params[i];
                            });
                            bool offset_used = returned || (found != parameter_uses.uses.end() && std::ranges::any_of(found->second, [](IrNode const* user) {
                                                                return user->kind != IrNodeKind::PointerSegment;
                                                            }));
                            bool segment_used = returned || (found != parameter_uses.uses.end() && std::ranges::any_of(found->second, [](IrNode const* user) {
                                                                 return user->kind != IrNodeKind::PointerOffset;
                                                             }));
                            pairs[params[i]] = {offset_used ? copy_from(*registers[i]) : VReg{},
                                                segment_used ? copy_from(segment_argument_register(*registers[i])) : VReg{}};
                        }
                        else
                            values[params[i]] = copy_from(*registers[i]);
                    }
                for (std::size_t i = 0; i < params.size(); ++i)
                {
                    auto const* param = params[i];
                    if (registers[i])
                    {
                        if (far(param->type))
                        {
                            auto source = pairs.at(param);
                            pairs[param] = {source.low.is_valid() ? extend(source.low, target.far_offset_bits(), false, 32) : source.low,
                                            source.high.is_valid() ? extend(source.high, 16, false, 32) : source.high};
                        }
                        continue;
                    }
                    MMem slot = MMem::make_base_disp(VReg::phys(PhysReg::RBP), offset);
                    if (far(param->type))
                    {
                        pairs[param] = load_far(MOp::from_mem(slot));
                        offset += static_cast<std::int32_t>(param->type->byte_size);
                    }
                    else if (wide(param->type))
                    {
                        pairs[param] = load_pair(slot);
                        offset += 8;
                    }
                    else if (memory_type(param->type))
                    {
                        auto addr = emit(MOpc::LEA16rm, {MOp::from_mem(slot)});
                        values[param] = target.pointer_bits == 32 ? emit(MOpc::MOVZX32_16rr, {MOp::from_reg(addr)}) : addr;
                        offset += (param->type->byte_size + 1) & ~std::uint64_t{1};
                    }
                    else if (auto bits = value_bits(param->type))
                    {
                        values[param] = emit(bits <= 16 ? MOpc::MOV16rm : MOpc::MOV32rm, {MOp::from_mem(slot)});
                        offset += bits <= 16 ? 2 : 4;
                    }
                }
            }

            void lower_inline_asm(IrInlineAsmInst const& assembly)
            {
                auto fail = [&](std::string const& message) {
                    diags.push_back(std::format("i8086 backend: inline assembly: {} in function `{}`", message, func.name));
                };
                for (auto const& operand : assembly.operands)
                    if (operand.placement_kind == IrAsmOperand::PlacementKind::Mem)
                        return fail("memory operands are not supported yet");
                auto plan = dcc::backend::prepare_inline_asm(assembly, target);
                if (!plan.error.empty())
                    return fail(plan.error);
                std::vector<VReg> pinned(assembly.operands.size());
                std::vector<VReg> inputs(assembly.operands.size());
                for (std::size_t i = 0; i < assembly.operands.size(); ++i)
                {
                    auto const& operand = assembly.operands[i];
                    if (operand.direction == IrAsmOperand::Direction::Out || operand.placement_kind == IrAsmOperand::PlacementKind::Imm)
                        continue;
                    auto input = value(operand.value);
                    if (!input)
                        return;
                    inputs[i] = *input;
                }

                std::uint64_t clobbers = 0;
                for (auto const& name : plan.clobbers)
                {
                    if (name == "memory" || name == "cc")
                        continue;
                    auto reg = dcc::backend::inline_asm_family_phys(target::register_family(name));
                    if (reg < PhysReg::RAX || reg > PhysReg::RDI || reg == PhysReg::RSP || reg == PhysReg::RBP)
                        return fail(std::format("clobbered register '{}' is not supported yet", name));
                    clobbers |= 1ULL << static_cast<unsigned>(reg);
                }
                for (std::size_t i = 0; i < assembly.operands.size(); ++i)
                {
                    auto const& operand = assembly.operands[i];
                    if (plan.registers[i].empty())
                        continue;
                    auto reg = dcc::backend::inline_asm_family_phys(target::register_family(plan.registers[i]));
                    if (reg < PhysReg::RAX || reg > PhysReg::RDI || reg == PhysReg::RSP || reg == PhysReg::RBP)
                        return fail(std::format("operand register '{}' is not supported yet", plan.registers[i]));
                    pinned[i] = VReg::phys(reg);
                    auto bits = value_bits(operand.type);
                    if (!bits)
                        return;
                    if (operand.direction != IrAsmOperand::Direction::Out)
                    {
                        append(make_instr(bits == 8    ? MOpc::MOV8rr
                                          : bits == 16 ? MOpc::MOV16rr
                                                       : MOpc::MOV32rr,
                                          {MOp::from_reg(pinned[i]), MOp::from_reg(inputs[i])}, 1));
                    }
                }
                for (auto instruction : plan.instructions)
                {
                    if (instruction.opc != MOpc::IN8ri && instruction.opc != MOpc::IN16ri && instruction.opc != MOpc::IN32ri &&
                        instruction.opc != MOpc::IN8rr && instruction.opc != MOpc::IN16rr && instruction.opc != MOpc::IN32rr &&
                        instruction.opc != MOpc::OUT8ir && instruction.opc != MOpc::OUT16ir && instruction.opc != MOpc::OUT32ir &&
                        instruction.opc != MOpc::OUT8rr && instruction.opc != MOpc::OUT16rr && instruction.opc != MOpc::OUT32rr)
                        return fail("only in and out instructions are supported yet");
                    instruction.implicit_defs |= clobbers;
                    append(instruction);
                }
                std::vector<VReg> outputs;
                for (std::size_t i = 0; i < assembly.operands.size(); ++i)
                {
                    auto const& operand = assembly.operands[i];
                    if (operand.direction == IrAsmOperand::Direction::In)
                        continue;
                    auto bits = value_bits(operand.type);
                    if (!bits)
                        return;
                    auto output = copy_from(pinned[i].phys_reg());
                    if (bits == 8)
                        output = emit(MOpc::AND16ri, {MOp::from_reg(output), MOp::from_imm(255)});
                    outputs.push_back(output);
                }
                if (outputs.size() == 1)
                    values[&assembly] = outputs.front();
                else if (!outputs.empty())
                    asm_results[&assembly] = std::move(outputs);
            }

            void lower_instruction(IrNode const* inst)
            {
                auto const* v = static_cast<IrValue const*>(inst);
                switch (inst->kind)
                {
                    case IrNodeKind::MakePointer: {
                        auto* make = static_cast<IrMakePointerInst const*>(inst);
                        if (far(v->type))
                        {
                            auto offset = value(make->offset), segment = value(make->segment);
                            if (offset && segment)
                                pairs[v] = {extend(*offset, target.far_offset_bits(), false, 32), extend(*segment, 16, false, 32)};
                            return;
                        }
                        if (auto offset = value(make->offset))
                            values[v] = *offset;
                        return;
                    }
                    case IrNodeKind::PointerOffset: {
                        auto* extract = static_cast<IrPointerOffsetInst const*>(inst);
                        if (far(extract->pointer->type))
                        {
                            if (auto source = pair(extract->pointer))
                                values[v] = source->low;
                            return;
                        }
                        if (auto offset = value(extract->pointer))
                            values[v] = *offset;
                        return;
                    }
                    case IrNodeKind::PointerSegment: {
                        auto* extract = static_cast<IrPointerSegmentInst const*>(inst);
                        if (auto source = pair(extract->pointer))
                            values[v] = source->high;
                        return;
                    }
                    case IrNodeKind::ReadSegment: {
                        auto* read = static_cast<IrReadSegmentInst const*>(inst);
                        auto segment = read->segment;
                        auto reg = segment == Segment::Cs   ? PhysReg::CS
                                   : segment == Segment::Ds ? PhysReg::DS
                                   : segment == Segment::Es ? PhysReg::ES
                                   : segment == Segment::Ss ? PhysReg::SS
                                   : segment == Segment::Fs ? PhysReg::FS
                                                            : PhysReg::GS;
                        values[v] = read_segment(reg);
                        return;
                    }
                    case IrNodeKind::InlineAsm:
                        return lower_inline_asm(*static_cast<IrInlineAsmInst const*>(inst));
                    case IrNodeKind::Extract: {
                        auto const* extract = static_cast<IrExtractInst const*>(inst);
                        auto found = asm_results.find(extract->aggregate);
                        if (found != asm_results.end() && extract->field_index < found->second.size())
                            values[v] = found->second[extract->field_index];
                        else if (auto aggregate = value(extract->aggregate))
                            load_at(v, offset_address(*aggregate, member_offset(extract->aggregate->type, extract->field_index)));
                        return;
                    }
                    case IrNodeKind::Aggregate:
                        std::ignore = value(v);
                        return;
                    case IrNodeKind::Gep:
                        return lower_gep(*static_cast<IrGepInst const*>(inst));
                    case IrNodeKind::Insert: {
                        auto const* insert = static_cast<IrInsertInst const*>(inst);
                        auto aggregate = value(insert->aggregate);
                        if (!aggregate)
                            return;
                        auto dst = temporary(v->type);
                        bulk_memory(dst, *aggregate, v->type->byte_size, false);
                        store_at(insert->value, offset_address(dst, member_offset(v->type, insert->field_index)));
                        values[v] = dst;
                        return;
                    }
                    case IrNodeKind::PtrToI:
                        return lower_cast(v, static_cast<IrPtrToIInst const*>(inst)->operand);
                    case IrNodeKind::IToPtr:
                        return lower_cast(v, static_cast<IrIToPtrInst const*>(inst)->operand);
                    case IrNodeKind::Alloca:
                        return lower_alloca(*static_cast<IrAllocaInst const*>(inst));
                    case IrNodeKind::Load:
                        return lower_load(v, static_cast<IrLoadInst const*>(inst)->pointer);
                    case IrNodeKind::LoadVolatile:
                        return lower_load(v, static_cast<IrLoadVolatileInst const*>(inst)->pointer);
                    case IrNodeKind::Store: {
                        auto const* store = static_cast<IrStoreInst const*>(inst);
                        return lower_store(store->value, store->pointer);
                    }
                    case IrNodeKind::StoreVolatile: {
                        auto const* store = static_cast<IrStoreVolatileInst const*>(inst);
                        return lower_store(store->value, store->pointer);
                    }
                    case IrNodeKind::Add:
                    case IrNodeKind::Sub:
                    case IrNodeKind::Mul:
                    case IrNodeKind::And:
                    case IrNodeKind::Or:
                    case IrNodeKind::Xor:
                        return lower_arithmetic(inst, v->type);
                    case IrNodeKind::Shl:
                    case IrNodeKind::LShr:
                    case IrNodeKind::AShr:
                        return lower_shift(inst, v->type);
                    case IrNodeKind::UDiv:
                    case IrNodeKind::SDiv:
                    case IrNodeKind::URem:
                    case IrNodeKind::SRem:
                        return lower_divide(inst, v->type);
                    case IrNodeKind::Neg:
                        return lower_unary(v, static_cast<IrNegInst const*>(inst)->operand);
                    case IrNodeKind::Not:
                        return lower_unary(v, static_cast<IrNotInst const*>(inst)->operand);
                    case IrNodeKind::Zext:
                        return lower_cast(v, static_cast<IrZextInst const*>(inst)->operand);
                    case IrNodeKind::Sext:
                        return lower_cast(v, static_cast<IrSextInst const*>(inst)->operand);
                    case IrNodeKind::Trunc:
                        return lower_cast(v, static_cast<IrTruncInst const*>(inst)->operand);
                    case IrNodeKind::Bitcast: {
                        auto const* operand = static_cast<IrBitcastInst const*>(inst)->operand;
                        if (v->type->byte_size != operand->type->byte_size)
                            return unsupported("a bitcast between differently sized values");
                        if (memory_type(v->type) && memory_type(operand->type))
                        {
                            if (auto addr = value(operand))
                                values[v] = *addr;
                        }
                        else if (memory_type(operand->type))
                        {
                            if (auto addr = value(operand))
                                load_at(v, *addr);
                        }
                        else if (memory_type(v->type))
                        {
                            auto addr = temporary(v->type);
                            store_at(operand, addr);
                            values[v] = addr;
                        }
                        else
                            lower_cast(v, operand);
                        return;
                    }
                    case IrNodeKind::Call: {
                        auto const* call = static_cast<IrCallInst const*>(inst);
                        return lower_call(v, call->callee, call->args);
                    }
                    case IrNodeKind::CallTail: {
                        auto const* call = static_cast<IrCallTailInst const*>(inst);
                        return lower_call(v, call->callee, call->args);
                    }
                    default:
                        if (auto setcc = setcc_for(inst->kind))
                            return lower_compare(inst, *setcc);
                        unsupported(std::format("IR instruction {}", kind_name(inst->kind)));
                }
            }

            void lower_ret(IrRetInst const& ret)
            {
                std::uint64_t uses = (1ULL << static_cast<unsigned>(PhysReg::RSP));
                if (ret.value)
                {
                    auto type = ret.value->type;
                    if (far(type))
                    {
                        if (auto source = pair(ret.value))
                        {
                            copy_to(PhysReg::RAX, source->low, target.far_offset_bits());
                            copy_to(PhysReg::RDX, source->high, 16);
                            auto mi = make_instr(MOpc::RET, {}, 0);
                            mi.implicit_uses = uses | bit(PhysReg::RAX) | bit(PhysReg::RDX);
                            append(mi);
                        }
                        return;
                    }
                    if (wide(type))
                    {
                        auto src = pair(ret.value);
                        if (!src)
                            return;
                        if (!c_abi)
                        {
                            copy_to(PhysReg::RAX, src->low, 32);
                            copy_to(PhysReg::RDX, src->high, 32);
                            uses |= bit(PhysReg::RAX) | bit(PhysReg::RDX);
                        }
                        else
                        {
                            auto high_word = emit(MOpc::SHR32ri8, {MOp::from_reg(src->high), MOp::from_imm(16)});
                            auto low_word = emit(MOpc::SHR32ri8, {MOp::from_reg(src->low), MOp::from_imm(16)});
                            copy_to(PhysReg::RAX, high_word, 16);
                            copy_to(PhysReg::RBX, src->high, 16);
                            copy_to(PhysReg::RCX, low_word, 16);
                            copy_to(PhysReg::RDX, src->low, 16);
                            uses |= bit(PhysReg::RAX) | bit(PhysReg::RBX) | bit(PhysReg::RCX) | bit(PhysReg::RDX);
                        }
                        MInstr mi = make_instr(MOpc::RET, {}, 0);
                        mi.implicit_uses = uses;
                        append(mi);
                        return;
                    }
                    auto bits = memory_type(type) ? static_cast<unsigned>(type->byte_size * 8) : value_bits(type);
                    auto v = bits || memory_type(type) ? value(ret.value) : std::nullopt;
                    if (v && indirect_return(type))
                    {
                        bulk_memory(sret_pointer, *v, type->byte_size, false);
                        copy_to(PhysReg::RAX, sret_pointer, target.pointer_bits);
                        MInstr mi = make_instr(MOpc::RET, {}, 0);
                        mi.implicit_uses = uses | bit(PhysReg::RAX);
                        append(mi);
                        return;
                    }
                    if (v && memory_type(type))
                        v = emit(bits == 8 ? MOpc::MOVZX16rm8 : bits == 16 ? MOpc::MOV16rm : MOpc::MOV32rm, {MOp::from_mem(near_memory(*v))});
                    if (!v)
                        return;
                    copy_to(PhysReg::RAX, *v, op_width(bits));
                    uses |= 1ULL << static_cast<unsigned>(PhysReg::RAX);
                    if (bits == 32 && (c_abi || memory_type(ret.value->type)))
                    {
                        copy_to(PhysReg::RDX, *v, 32);
                        append(make_instr(MOpc::SHR32ri8, {phys_operand(PhysReg::RDX), phys_operand(PhysReg::RDX), MOp::from_imm(16)}, 1));
                        uses |= 1ULL << static_cast<unsigned>(PhysReg::RDX);
                    }
                }
                MInstr mi = make_instr(MOpc::RET, {}, 0);
                mi.implicit_uses = uses;
                append(mi);
            }

            void jump(MOpc opc, IrBasicBlock const* destination) { append(make_instr(opc, {MOp::from_label(block_ids.at(destination))}, 0)); }

            void lower_branch(IrBrCondInst const& branch)
            {
                if (fused.contains(branch.condition))
                {
                    auto setcc = *setcc_for(branch.condition->kind);
                    if (!emit_compare(branch.condition, setcc))
                        return;
                    jump(jcc_for(setcc), branch.true_target);
                }
                else
                {
                    auto condition = value(branch.condition);
                    if (!condition)
                        return;
                    append(make_instr(MOpc::TEST16ri, {MOp::from_reg(*condition), MOp::from_imm(0xFF)}, 0));
                    jump(MOpc::JNE, branch.true_target);
                }
                jump(MOpc::JMP, branch.false_target);
            }

            void lower_switch(IrSwitchInst const& sw)
            {
                auto bits = value_bits(sw.value ? sw.value->type : nullptr);
                auto v = bits ? value(sw.value) : std::nullopt;
                if (!v)
                    return;
                bool const signed_value = sw.value->type->kind == IrTypeKind::Int && static_cast<IrIntType const*>(sw.value->type)->is_signed;
                auto width = op_width(bits);
                bool const narrow = width == 16;
                auto selector = extend(*v, bits, signed_value, width);
                auto normalized = [&](std::int64_t case_value) { return fit(case_value, bits, signed_value, width); };

                std::int64_t low = std::numeric_limits<std::int64_t>::max();
                std::int64_t high = std::numeric_limits<std::int64_t>::min();
                std::uint64_t count = 0;
                for (auto const& c : sw.cases)
                {
                    if (c.start > c.end)
                        return unsupported("a switch case with an empty range");
                    low = std::min(low, c.start);
                    high = std::max(high, c.end);
                    count += static_cast<std::uint64_t>(c.end - c.start) + 1;
                }
                std::uint64_t span = sw.cases.empty() ? 0 : static_cast<std::uint64_t>(high - low) + 1;
                if (count >= 6 && span <= count * 4 && span <= 4096)
                {
                    VReg index = selector;
                    if (low != 0)
                        index = emit(narrow ? MOpc::SUB16ri : MOpc::SUB32ri, {MOp::from_reg(selector), MOp::from_imm(normalized(low))});
                    append(make_instr(narrow ? MOpc::CMP16ri : MOpc::CMP32ri, {MOp::from_reg(index), MOp::from_imm(fit(static_cast<std::int64_t>(span - 1), 32, false, width))}, 0));
                    jump(MOpc::JA, sw.default_target);

                    MJumpTable table;
                    table.id = mfunc.next_jump_table_id++;
                    table.min_value = low;
                    table.max_value = high;
                    table.default_target = block_ids.at(sw.default_target);
                    table.targets.assign(span, table.default_target);
                    for (auto const& c : sw.cases)
                        for (auto entry = c.start; entry <= c.end; ++entry)
                            table.targets[static_cast<std::size_t>(entry - low)] = block_ids.at(c.target);
                    table.symbol = std::format("{}.jt{}", mfunc.owned_name, table.id);
                    auto const& symbol = *mfunc.owned_strings.emplace_back(std::make_unique<std::string>(table.symbol));
                    mfunc.jump_tables.push_back(std::move(table));
                    bool const wide_addresses = target.far_offset_bits() == 32;
                    if (wide_addresses && narrow)
                        index = extend(index, 16, false, 32);
                    MInstr dispatch = make_instr(MOpc::JUMP_TABLE, {MOp::from_reg(index), MOp::from_symbol(symbol), MOp::from_imm(wide_addresses ? 32 : 16)}, 0);
                    if (!wide_addresses)
                        dispatch.implicit_defs = 1ULL << static_cast<unsigned>(PhysReg::RDI);
                    append(dispatch);
                    jump_table_blocks[block->id] = mfunc.jump_tables.size() - 1;
                    return;
                }
                for (auto const& c : sw.cases)
                {
                    if (c.start == c.end)
                    {
                        append(make_instr(narrow ? MOpc::CMP16ri : MOpc::CMP32ri, {MOp::from_reg(selector), MOp::from_imm(normalized(c.start))}, 0));
                        jump(MOpc::JE, c.target);
                        continue;
                    }
                    auto offset = emit(narrow ? MOpc::SUB16ri : MOpc::SUB32ri, {MOp::from_reg(selector), MOp::from_imm(normalized(c.start))});
                    append(make_instr(narrow ? MOpc::CMP16ri : MOpc::CMP32ri,
                                      {MOp::from_reg(offset), MOp::from_imm(fit(c.end - c.start, 32, false, width))}, 0));
                    jump(MOpc::JBE, c.target);
                }
                jump(MOpc::JMP, sw.default_target);
            }

            void lower_terminator(IrNode const* term)
            {
                if (!term)
                    return unsupported("a block without a terminator");
                switch (term->kind)
                {
                    case IrNodeKind::Ret:
                        return lower_ret(*static_cast<IrRetInst const*>(term));
                    case IrNodeKind::Br:
                        return jump(MOpc::JMP, static_cast<IrBrInst const*>(term)->target);
                    case IrNodeKind::BrCond:
                        return lower_branch(*static_cast<IrBrCondInst const*>(term));
                    case IrNodeKind::Switch:
                        return lower_switch(*static_cast<IrSwitchInst const*>(term));
                    case IrNodeKind::Unreachable:
                        return append(make_instr(MOpc::UD2, {}, 0));
                    default:
                        return unsupported(std::format("IR terminator {}", kind_name(term->kind)));
                }
            }

            [[nodiscard]] static std::vector<IrBasicBlock const*> successors(IrNode const* term)
            {
                std::vector<IrBasicBlock const*> out;
                if (!term)
                    return out;
                switch (term->kind)
                {
                    case IrNodeKind::Br:
                        out.push_back(static_cast<IrBrInst const*>(term)->target);
                        break;
                    case IrNodeKind::BrCond:
                        out.push_back(static_cast<IrBrCondInst const*>(term)->true_target);
                        out.push_back(static_cast<IrBrCondInst const*>(term)->false_target);
                        break;
                    case IrNodeKind::Switch: {
                        auto const* sw = static_cast<IrSwitchInst const*>(term);
                        for (auto const& c : sw->cases)
                            out.push_back(c.target);
                        out.push_back(sw->default_target);
                        break;
                    }
                    default:
                        break;
                }
                std::vector<IrBasicBlock const*> unique;
                for (auto* b : out)
                    if (b && std::ranges::find(unique, b) == unique.end())
                        unique.push_back(b);
                return unique;
            }

            [[nodiscard]] std::vector<IrBasicBlock const*> reverse_post_order()
            {
                std::vector<IrBasicBlock const*> order;
                std::unordered_set<IrBasicBlock const*> visited;
                std::vector<std::pair<IrBasicBlock const*, std::size_t>> stack;
                if (!func.entry_block)
                    return order;
                stack.emplace_back(func.entry_block, 0);
                visited.insert(func.entry_block);
                while (!stack.empty())
                {
                    auto& [current, next] = stack.back();
                    auto succs = successors(current->terminator);
                    if (next < succs.size())
                    {
                        auto* succ = succs[next++];
                        if (visited.insert(succ).second)
                            stack.emplace_back(succ, 0);
                        continue;
                    }
                    order.push_back(current);
                    stack.pop_back();
                }
                std::ranges::reverse(order);
                return order;
            }

            [[nodiscard]] static bool is_branch(MOpc opc) noexcept { return opc == MOpc::JMP || opc == MOpc::JUMP_TABLE || opc == MOpc::RET || opc == MOpc::UD2 || inverse_jcc(opc); }

            [[nodiscard]] std::uint32_t edge_block(IrBasicBlock const* pred, IrBasicBlock const* succ, std::size_t succ_preds)
            {
                auto pred_id = block_tails.contains(pred) ? block_tails.at(pred) : block_ids.at(pred);
                auto succ_id = block_ids.at(succ);
                if (successors(pred->terminator).size() < 2 || succ_preds < 2)
                    return pred_id;
                auto key = std::pair{pred_id, succ_id};
                if (auto found = split_edges.find(key); found != split_edges.end())
                    return found->second;
                auto& edge = mfunc.create_block({});
                auto edge_id = edge.id;
                edge.instrs.push_back(make_instr(MOpc::JMP, {MOp::from_label(succ_id)}, 0));
                for (auto& mi : mfunc.block_by_id(pred_id)->instrs)
                    if (is_branch(mi.opc) && mi.num_ops > 0 && mi.ops[0].kind == MOpKind::Label && mi.ops[0].label == succ_id)
                        mi.ops[0].label = edge_id;
                if (auto table = jump_table_blocks.find(pred_id); table != jump_table_blocks.end())
                    for (auto& t : mfunc.jump_tables[table->second].targets)
                        if (t == succ_id)
                            t = edge_id;
                split_edges[key] = edge_id;
                return edge_id;
            }

            void insert_before_branches(std::uint32_t block_id, MInstr const& mi)
            {
                auto& instrs = mfunc.block_by_id(block_id)->instrs;
                auto at = instrs.size();
                while (at > 0 && (is_branch(instrs[at - 1].opc) || instrs[at - 1].opc == MOpc::CMP16ri || instrs[at - 1].opc == MOpc::CMP32ri ||
                                  instrs[at - 1].opc == MOpc::CMP16rr || instrs[at - 1].opc == MOpc::CMP32rr || instrs[at - 1].opc == MOpc::TEST16ri))
                    --at;
                instrs.insert(instrs.begin() + static_cast<std::ptrdiff_t>(at), mi);
            }

            void lower_phis(std::vector<IrBasicBlock const*> const& order)
            {
                std::unordered_map<IrBasicBlock const*, std::size_t> pred_counts;
                for (auto* b : order)
                    for (auto* succ : successors(b->terminator))
                        ++pred_counts[succ];
                for (auto const& [phi, owner, result] : pending_phis)
                {
                    auto bits = wide(phi->type) || far(phi->type) ? 32 : memory_type(phi->type) ? target.pointer_bits : value_bits(phi->type);
                    if (!bits)
                        return;
                    std::vector<MOp> inputs;
                    for (auto const& incoming : phi->incoming)
                    {
                        if (!block_ids.contains(incoming.block))
                            continue;
                        auto via = edge_block(incoming.block, owner, pred_counts[owner]);
                        VReg input;
                        if (wide(phi->type) || far(phi->type))
                        {
                            if (far(phi->type) && (ir_cast<IrPointerConstant>(incoming.value) || ir_cast<IrNullConstant>(incoming.value)))
                            {
                                auto* pointer = ir_cast<IrPointerConstant>(incoming.value);
                                auto half = pointer ? (result == pairs.at(phi).high ? pointer->segment : pointer->offset) : 0;
                                input = mfunc.new_vreg();
                                insert_before_branches(via,
                                                       make_instr(MOpc::MOV32ri, {MOp::from_reg(input), MOp::from_imm(static_cast<std::int32_t>(half))}, 1));
                            }
                            else if (auto imm = ir_cast<IrIntConstant>(incoming.value))
                            {
                                input = mfunc.new_vreg();
                                auto half_bits = static_cast<std::uint64_t>(imm->value) >> (result == pairs.at(phi).high ? 32 : 0);
                                insert_before_branches(
                                    via, make_instr(MOpc::MOV32ri, {MOp::from_reg(input), MOp::from_imm(static_cast<std::int32_t>(half_bits))}, 1));
                            }
                            else if (auto found = pairs.find(incoming.value); found != pairs.end())
                                input = result == pairs.at(phi).high ? found->second.high : found->second.low;
                            else
                                return unsupported("an unavailable i64 phi input");
                        }
                        else if (auto imm = constant(incoming.value))
                        {
                            input = mfunc.new_vreg();
                            auto width = op_width(bits);
                            insert_before_branches(via, make_instr(width == 16 ? MOpc::MOV16ri : MOpc::MOV32ri,
                                                                   {MOp::from_reg(input), MOp::from_imm(fit(*imm, bits, false, width))}, 1));
                        }
                        else if (auto found = values.find(incoming.value); found != values.end())
                            input = found->second;
                        else
                            return unsupported(std::format("a phi input of IR {}", kind_name(incoming.value ? incoming.value->kind : IrNodeKind::Local)));
                        inputs.push_back(MOp::from_reg(input));
                        inputs.push_back(MOp::from_label(via));
                    }
                    auto& instrs = mfunc.block_by_id(block_ids.at(owner))->instrs;
                    std::size_t at = 0;
                    while (at < instrs.size() && instrs[at].opc == MOpc::PHI)
                        ++at;
                    for (std::size_t first = 0; first < inputs.size() || first == 0; first += 30)
                    {
                        MInstr mi = make_instr(MOpc::PHI, {MOp::from_reg(result)}, 1);
                        for (std::size_t k = first; k < std::min(first + 30, inputs.size()); ++k)
                            mi.ops[mi.num_ops++] = inputs[k];
                        instrs.insert(instrs.begin() + static_cast<std::ptrdiff_t>(at++), mi);
                        if (inputs.empty())
                            break;
                    }
                }
            }

            void finish_layout()
            {
                for (auto& b : mfunc.blocks)
                {
                    for (auto const& mi : b.instrs)
                        if (is_branch(mi.opc) && mi.num_ops > 0 && mi.ops[0].kind == MOpKind::Label && std::ranges::find(b.succs, mi.ops[0].label) == b.succs.end())
                            b.succs.push_back(mi.ops[0].label);
                    if (auto table = jump_table_blocks.find(b.id); table != jump_table_blocks.end())
                        for (auto t : mfunc.jump_tables[table->second].targets)
                            if (std::ranges::find(b.succs, t) == b.succs.end())
                                b.succs.push_back(t);
                }
                for (auto& b : mfunc.blocks)
                    for (auto succ : b.succs)
                        mfunc.block_by_id(succ)->preds.push_back(b.id);
            }

            void run()
            {
                mfunc.owned_name = func.name.empty() ? "<unnamed>" : std::string{func.name};
                mfunc.src_line = static_cast<std::int32_t>(func.decl_line);
                mfunc.linkage = func.linkage;
                mfunc.conv = func.conv;
                c_abi = uses_c_abi(func);


                auto order = reverse_post_order();
                for (auto* b : order)
                {
                    for (auto* param : b->params)
                        preserve_es |= has_based_type(param->type);
                    for (auto* inst : b->instructions)
                        preserve_es |= has_based_type(static_cast<IrValue const*>(inst)->type);
                }
                for (auto* ir_block : order)
                {
                    auto& mblock = mfunc.create_block(ir_block->has_name() ? ir_block->name : std::string_view{});
                    if (ir_block == func.entry_block)
                        mfunc.entry_block_id = mblock.id;
                    block_ids[ir_block] = mblock.id;
                }

                auto use_def = analysis::UseDef::build(func);
                for (auto const& [operand, users] : use_def.uses)
                    preserve_es |= has_based_type(operand->type);
                std::unordered_map<IrValue const*, std::size_t> terminator_uses;
                for (auto* ir_block : order)
                {
                    auto const* term = ir_block->terminator;
                    if (term && term->kind == IrNodeKind::BrCond)
                        ++terminator_uses[static_cast<IrBrCondInst const*>(term)->condition];
                    else if (term && term->kind == IrNodeKind::Ret)
                        ++terminator_uses[static_cast<IrRetInst const*>(term)->value];
                    else if (term && term->kind == IrNodeKind::Switch)
                        ++terminator_uses[static_cast<IrSwitchInst const*>(term)->value];
                }
                for (auto* ir_block : order)
                {
                    auto const* term = ir_block->terminator;
                    if (!term || term->kind != IrNodeKind::BrCond)
                        continue;
                    auto* condition = static_cast<IrBrCondInst const*>(term)->condition;
                    if (condition && setcc_for(condition->kind) && !wide(binary_operands(condition).first->type) &&
                        !far(binary_operands(condition).first->type) && use_def.use_count(condition) == 0 && terminator_uses[condition] == 1 &&
                        std::ranges::find(ir_block->instructions, condition) != ir_block->instructions.end())
                        fused.insert(condition);
                }

                for (auto* ir_block : order)
                    for (std::size_t i = 0; i < ir_block->instructions.size(); ++i)
                    {
                        auto* value = static_cast<IrValue const*>(ir_block->instructions[i]);
                        if (!memory_type(value->type) || value->type->byte_size <= 65535)
                            continue;
                        if ((value->kind == IrNodeKind::Load || value->kind == IrNodeKind::Aggregate) && use_def.use_count(value) == 0 &&
                            terminator_uses[value] == 0)
                            fused.insert(value);
                        else if (value->kind == IrNodeKind::Aggregate && zero_value(value))
                            fused.insert(value);
                        else if (value->kind == IrNodeKind::Load && use_def.use_count(value) == 1 && terminator_uses[value] == 0 &&
                                 i + 1 < ir_block->instructions.size())
                            if (auto* store = ir_cast<IrStoreInst>(ir_block->instructions[i + 1]); store && store->value == value)
                                fused.insert(value);
                    }
                for (auto* ir_block : order)
                {
                    block = mfunc.block_by_id(block_ids.at(ir_block));
                    if (ir_block == func.entry_block)
                    {
                        lower_parameters();
                        if (!diags.empty())
                            return;
                    }
                    for (auto* inst : ir_block->instructions)
                    {
                        if (!inst || fused.contains(static_cast<IrValue const*>(inst)))
                            continue;
                        if (inst->kind == IrNodeKind::Phi)
                        {
                            auto const* phi = static_cast<IrPhiInst const*>(inst);
                            if (wide(phi->type) || far(phi->type))
                            {
                                Pair result{mfunc.new_vreg(), mfunc.new_vreg()};
                                pairs[phi] = result;
                                pending_phis.push_back({phi, ir_block, result.low});
                                pending_phis.push_back({phi, ir_block, result.high});
                                continue;
                            }
                            VReg result = mfunc.new_vreg();
                            values[phi] = result;
                            pending_phis.push_back({phi, ir_block, result});
                            continue;
                        }
                        lower_instruction(inst);
                        if (!diags.empty())
                            return;
                    }
                    lower_terminator(ir_block->terminator);
                    block_tails[ir_block] = block->id;
                    if (!diags.empty())
                        return;
                }
                lower_phis(order);
                if (!diags.empty())
                    return;
                finish_layout();
            }
        };
    } // namespace

    bool uses_c_abi(IrFunction const& func)
    {
        return std::ranges::any_of(func.attrs,
                                   [](IrFuncAttribute const& attr) { return attr.kind == IrFuncAttr::NoMangle || attr.kind == IrFuncAttr::CallingConv; });
    }

    void remove_fallthrough_jumps(MFunction& func)
    {
        for (std::size_t i = 0; i + 1 < func.blocks.size(); ++i)
        {
            auto& instrs = func.blocks[i].instrs;
            auto next = func.blocks[i + 1].id;
            if (instrs.empty() || instrs.back().opc != MOpc::JMP || instrs.back().ops[0].label != next)
                continue;
            instrs.pop_back();
            if (!instrs.empty() && inverse_jcc(instrs.back().opc) && instrs.back().ops[0].label == next)
                instrs.pop_back();
        }
        for (std::size_t i = 0; i + 1 < func.blocks.size(); ++i)
        {
            auto& instrs = func.blocks[i].instrs;
            auto next = func.blocks[i + 1].id;
            if (instrs.size() < 2 || instrs.back().opc != MOpc::JMP)
                continue;
            auto& conditional = instrs[instrs.size() - 2];
            auto inverse = inverse_jcc(conditional.opc);
            if (!inverse || conditional.ops[0].label != next)
                continue;
            conditional.opc = *inverse;
            conditional.ops[0].label = instrs.back().ops[0].label;
            instrs.pop_back();
        }
    }

    MFunction isel_function(IrFunction const& func, target::TargetConfig const& target, std::vector<std::string>& diags)
    {
        Isel isel{func, target, diags};
        isel.run();
        std::unordered_set<VReg> foldable_loads;
        for (auto const& [value, reg] : isel.values)
            if (value->kind == IrNodeKind::Load)
                foldable_loads.insert(reg);
        fold_operands(isel.mfunc, target.pointer_bits, foldable_loads);
        return std::move(isel.mfunc);
    }
} // namespace dcc::backend::i8086
