export module dcc.backend.i8086.isel;

import std;
import dcc.ir;
import dcc.target;
import dcc.backend.x86.mir;

using namespace dcc::backend::x86;

export namespace dcc::backend::i8086
{
    [[nodiscard]] bool uses_c_abi(dcc::ir::IrFunction const& func);

    [[nodiscard]] MFunction isel_function(dcc::ir::IrFunction const& func, dcc::target::TargetConfig const& target, std::vector<std::string>& diags);

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
            std::unordered_map<IrValue const*, VReg> values;
            std::unordered_map<IrValue const*, std::uint32_t> slots;

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
                    default:
                        return 0;
                }
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

            [[nodiscard]] static std::optional<std::int64_t> constant(IrValue const* value)
            {
                if (auto* integer = ir_cast<IrIntConstant>(value))
                    return static_cast<std::int32_t>(static_cast<std::uint32_t>(integer->value));
                if (auto* boolean = ir_cast<IrBoolConstant>(value))
                    return boolean->value ? 1 : 0;
                return std::nullopt;
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
                if (auto imm = constant(v))
                {
                    if (!value_bits(v->type))
                        return std::nullopt;
                    VReg r = mfunc.new_vreg();
                    append(make_instr(MOpc::MOV32ri, {MOp::from_reg(r), MOp::from_imm(*imm)}, 1));
                    return r;
                }
                unsupported(std::format("an operand of IR {}", kind_name(v->kind)));
                return std::nullopt;
            }

            [[nodiscard]] std::optional<MOp> source(IrValue const* v)
            {
                if (auto imm = constant(v))
                    return MOp::from_imm(*imm);
                auto r = value(v);
                if (!r)
                    return std::nullopt;
                return MOp::from_reg(*r);
            }

            [[nodiscard]] std::optional<MOp> address(IrValue const* pointer)
            {
                if (auto found = slots.find(pointer); found != slots.end())
                    return MOp::from_frame_slot(found->second);
                if (auto* ref = ir_cast<IrGlobalRef>(pointer); ref && ref->global)
                {
                    MMem mem{};
                    mem.symbol = ref->global->name;
                    mem.disp = static_cast<std::int32_t>(ref->addend);
                    mem.address_bits = static_cast<std::uint8_t>(target.pointer_bits == 32 ? 32 : 16);
                    return MOp::from_mem(mem);
                }
                unsupported(std::format("memory access through an IR {} pointer", kind_name(pointer ? pointer->kind : IrNodeKind::Local)));
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

            [[nodiscard]] VReg zero_extend(VReg v, unsigned bits)
            {
                if (bits >= 32)
                    return v;
                if (bits == 16)
                    return emit(MOpc::MOVZX32_16rr, {MOp::from_reg(v)});
                return emit(MOpc::AND32ri, {MOp::from_reg(v), MOp::from_imm(0xFF)});
            }

            [[nodiscard]] VReg sign_extend(VReg v, unsigned bits)
            {
                if (bits >= 32)
                    return v;
                if (bits == 16)
                    return emit(MOpc::MOVSX32_16rr, {MOp::from_reg(v)});
                auto shifted = emit(MOpc::SHL32ri8, {MOp::from_reg(v), MOp::from_imm(24)});
                return emit(MOpc::SAR32ri8, {MOp::from_reg(shifted), MOp::from_imm(24)});
            }

            [[nodiscard]] VReg extend(VReg v, unsigned bits, bool signed_value) { return signed_value ? sign_extend(v, bits) : zero_extend(v, bits); }

            void copy_to(PhysReg reg, VReg v) { append(make_instr(MOpc::COPY, {phys_operand(reg), MOp::from_reg(v)}, 1)); }

            [[nodiscard]] VReg copy_from(PhysReg reg)
            {
                VReg d = mfunc.new_vreg();
                append(make_instr(MOpc::COPY, {MOp::from_reg(d), phys_operand(reg)}, 1));
                return d;
            }

            void lower_alloca(IrAllocaInst const& alloca)
            {
                auto* type = alloca.allocated_type;
                if (alloca.count || !scalar_bits(type) || scalar_bits(type) > 32)
                {
                    unsupported("a local that is not an 8-, 16- or 32-bit scalar");
                    return;
                }
                slots[&alloca] =
                    mfunc.new_frame_slot(static_cast<std::uint32_t>(type->byte_size), std::min<std::uint32_t>(static_cast<std::uint32_t>(type->byte_align), 2));
            }

            void lower_load(IrValue const* inst, IrValue const* pointer)
            {
                auto bits = value_bits(inst->type);
                auto mem = bits ? address(pointer) : std::nullopt;
                if (!mem)
                    return;
                auto opc = bits == 8 ? MOpc::MOVZX32rm8 : bits == 16 ? MOpc::MOVZX32rm16 : MOpc::MOV32rm;
                values[inst] = emit(opc, {*mem});
            }

            void lower_store(IrValue const* stored, IrValue const* pointer)
            {
                auto bits = value_bits(stored ? stored->type : nullptr);
                auto mem = bits ? address(pointer) : std::nullopt;
                if (!mem)
                    return;
                if (auto imm = constant(stored))
                {
                    auto opc = bits == 8 ? MOpc::MOV8mi : bits == 16 ? MOpc::MOV16mi : MOpc::MOV32mi;
                    auto masked = bits == 32 ? *imm : *imm & ((std::int64_t{1} << bits) - 1);
                    append(make_instr(opc, {*mem, MOp::from_imm(masked)}, 0));
                    return;
                }
                auto v = value(stored);
                if (!v)
                    return;
                if (bits == 8)
                {
                    copy_to(PhysReg::RAX, *v);
                    append(make_instr(MOpc::MOV8mr, {*mem, phys_operand(PhysReg::RAX)}, 0));
                    return;
                }
                append(make_instr(bits == 16 ? MOpc::MOV16mr : MOpc::MOV32mr, {*mem, MOp::from_reg(*v)}, 0));
            }

            void lower_arithmetic(IrNode const* inst, IrType const* type)
            {
                auto [lhs, rhs] = binary_operands(inst);
                auto bits = value_bits(type);
                auto l = bits ? value(lhs) : std::nullopt;
                if (!l)
                    return;
                MOpc rr{};
                MOpc ri{};
                switch (inst->kind)
                {
                    case IrNodeKind::Add:
                        rr = MOpc::ADD32rr, ri = MOpc::ADD32ri;
                        break;
                    case IrNodeKind::Sub:
                        rr = MOpc::SUB32rr, ri = MOpc::SUB32ri;
                        break;
                    case IrNodeKind::And:
                        rr = MOpc::AND32rr, ri = MOpc::AND32ri;
                        break;
                    case IrNodeKind::Or:
                        rr = MOpc::OR32rr, ri = MOpc::OR32ri;
                        break;
                    case IrNodeKind::Xor:
                        rr = MOpc::XOR32rr, ri = MOpc::XOR32ri;
                        break;
                    default:
                        rr = MOpc::IMUL32rr, ri = MOpc::IMUL32rri;
                        break;
                }
                auto r = source(rhs);
                if (!r)
                    return;
                values[static_cast<IrValue const*>(inst)] = emit(r->kind == MOpKind::Imm64 ? ri : rr, {MOp::from_reg(*l), *r});
            }

            void lower_shift(IrNode const* inst, IrType const* type)
            {
                auto [lhs, rhs] = binary_operands(inst);
                auto bits = value_bits(type);
                auto l = bits ? value(lhs) : std::nullopt;
                if (!l)
                    return;
                VReg base = *l;
                if (inst->kind == IrNodeKind::LShr)
                    base = zero_extend(base, bits);
                else if (inst->kind == IrNodeKind::AShr)
                    base = sign_extend(base, bits);
                auto by_imm = inst->kind == IrNodeKind::Shl ? MOpc::SHL32ri8 : inst->kind == IrNodeKind::LShr ? MOpc::SHR32ri8 : MOpc::SAR32ri8;
                auto by_cl = inst->kind == IrNodeKind::Shl ? MOpc::SHL32rCL : inst->kind == IrNodeKind::LShr ? MOpc::SHR32rCL : MOpc::SAR32rCL;
                if (auto imm = constant(rhs))
                {
                    values[static_cast<IrValue const*>(inst)] = emit(by_imm, {MOp::from_reg(base), MOp::from_imm(*imm & 31)});
                    return;
                }
                auto count = value(rhs);
                if (!count)
                    return;
                copy_to(PhysReg::RCX, *count);
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
                auto dividend = extend(*l, bits, signed_op);
                auto divisor = extend(*r, bits, signed_op);
                copy_to(PhysReg::RAX, dividend);
                std::uint64_t const ax = 1ULL << static_cast<unsigned>(PhysReg::RAX);
                std::uint64_t const dx = 1ULL << static_cast<unsigned>(PhysReg::RDX);
                if (signed_op)
                {
                    MInstr cdq = make_instr(MOpc::CDQ, {}, 0);
                    cdq.implicit_defs = dx;
                    cdq.implicit_uses = ax;
                    append(cdq);
                }
                else
                    append(make_instr(MOpc::MOV32ri, {phys_operand(PhysReg::RDX), MOp::from_imm(0)}, 1));
                MInstr divide = make_instr(signed_op ? MOpc::IDIV32r : MOpc::DIV32r, {MOp::from_reg(divisor)}, 0);
                divide.implicit_defs = ax | dx;
                divide.implicit_uses = ax | dx;
                append(divide);
                bool remainder = inst->kind == IrNodeKind::SRem || inst->kind == IrNodeKind::URem;
                values[static_cast<IrValue const*>(inst)] = copy_from(remainder ? PhysReg::RDX : PhysReg::RAX);
            }

            void lower_compare(IrNode const* inst, MOpc setcc)
            {
                auto [lhs, rhs] = binary_operands(inst);
                auto bits = value_bits(lhs ? lhs->type : nullptr);
                auto l = bits ? value(lhs) : std::nullopt;
                if (!l)
                    return;
                bool signed_compare = setcc == MOpc::SETLr || setcc == MOpc::SETLEr || setcc == MOpc::SETGr || setcc == MOpc::SETGEr;
                auto left = extend(*l, bits, signed_compare);
                std::optional<MOp> right;
                if (auto imm = constant(rhs); imm && bits == 32)
                    right = MOp::from_imm(*imm);
                else
                {
                    auto r = value(rhs);
                    if (!r)
                        return;
                    right = MOp::from_reg(extend(*r, bits, signed_compare));
                }
                append(make_instr(right->kind == MOpKind::Imm64 ? MOpc::CMP32ri : MOpc::CMP32rr, {MOp::from_reg(left), *right}, 0));
                append(make_instr(setcc, {phys_operand(PhysReg::RAX)}, 1));
                values[static_cast<IrValue const*>(inst)] = emit(MOpc::MOVZX32_8rr, {phys_operand(PhysReg::RAX)});
            }

            void lower_unary(IrValue const* inst, IrValue const* operand)
            {
                auto bits = value_bits(inst->type);
                auto v = bits ? value(operand) : std::nullopt;
                if (!v)
                    return;
                if (inst->kind == IrNodeKind::Neg)
                    values[inst] = emit(MOpc::NEG32r, {MOp::from_reg(*v)});
                else if (inst->type->kind == IrTypeKind::Bool)
                    values[inst] = emit(MOpc::XOR32ri, {MOp::from_reg(*v), MOp::from_imm(1)});
                else
                    values[inst] = emit(MOpc::NOT32r, {MOp::from_reg(*v)});
            }

            void lower_cast(IrValue const* inst, IrValue const* operand)
            {
                auto to = value_bits(inst->type);
                auto from = to ? value_bits(operand ? operand->type : nullptr) : 0;
                auto v = from ? value(operand) : std::nullopt;
                if (!v)
                    return;
                if (inst->kind == IrNodeKind::Trunc || from >= to)
                    values[inst] = *v;
                else if (inst->kind == IrNodeKind::Zext)
                    values[inst] = zero_extend(*v, from);
                else
                    values[inst] = sign_extend(*v, from);
            }

            void lower_instruction(IrNode const* inst)
            {
                auto const* v = static_cast<IrValue const*>(inst);
                switch (inst->kind)
                {
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
                        if (scalar_bits(v->type) != scalar_bits(operand ? operand->type : nullptr))
                            return unsupported("a bitcast between differently sized values");
                        return lower_cast(v, operand);
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
                    auto bits = value_bits(ret.value->type);
                    auto v = bits ? value(ret.value) : std::nullopt;
                    if (!v)
                        return;
                    copy_to(PhysReg::RAX, *v);
                    uses |= 1ULL << static_cast<unsigned>(PhysReg::RAX);
                    if (bits == 32 && c_abi)
                    {
                        copy_to(PhysReg::RDX, *v);
                        append(make_instr(MOpc::SHR32ri8, {phys_operand(PhysReg::RDX), phys_operand(PhysReg::RDX), MOp::from_imm(16)}, 1));
                        uses |= 1ULL << static_cast<unsigned>(PhysReg::RDX);
                    }
                }
                MInstr mi = make_instr(MOpc::RET, {}, 0);
                mi.implicit_uses = uses;
                append(mi);
            }

            void lower_terminator(IrNode const* term)
            {
                if (!term)
                {
                    unsupported("a block without a terminator");
                    return;
                }
                if (term->kind == IrNodeKind::Ret)
                {
                    lower_ret(*static_cast<IrRetInst const*>(term));
                    return;
                }
                unsupported(std::format("IR terminator {}", kind_name(term->kind)));
            }

            void run()
            {
                mfunc.owned_name = func.name.empty() ? "<unnamed>" : std::string{func.name};
                mfunc.src_line = static_cast<std::int32_t>(func.decl_line);
                mfunc.linkage = func.linkage;
                mfunc.conv = func.conv;
                c_abi = uses_c_abi(func);

                if (func.entry_block && !func.entry_block->params.empty())
                {
                    unsupported("a function with parameters");
                    return;
                }

                std::vector<std::pair<IrBasicBlock const*, std::uint32_t>> blocks;
                for (auto* ir_block : func.blocks)
                {
                    if (!ir_block)
                        continue;
                    auto& mblock = mfunc.create_block(ir_block->has_name() ? ir_block->name : std::string_view{});
                    if (ir_block == func.entry_block)
                        mfunc.entry_block_id = mblock.id;
                    blocks.emplace_back(ir_block, mblock.id);
                }
                for (auto const& [ir_block, id] : blocks)
                {
                    block = mfunc.block_by_id(id);
                    for (auto* inst : ir_block->instructions)
                    {
                        if (inst)
                            lower_instruction(inst);
                        if (!diags.empty())
                            return;
                    }
                    lower_terminator(ir_block->terminator);
                    if (!diags.empty())
                        return;
                }
            }
        };
    } // namespace

    bool uses_c_abi(IrFunction const& func)
    {
        return std::ranges::any_of(func.attrs,
                                   [](IrFuncAttribute const& attr) { return attr.kind == IrFuncAttr::NoMangle || attr.kind == IrFuncAttr::CallingConv; });
    }

    MFunction isel_function(IrFunction const& func, target::TargetConfig const& target, std::vector<std::string>& diags)
    {
        Isel isel{func, target, diags};
        isel.run();
        return std::move(isel.mfunc);
    }
} // namespace dcc::backend::i8086
