export module dcc.backend.i8086.isel;

import std;
import dcc.ir;
import dcc.ir.analysis;
import dcc.target;
import dcc.backend.x86.mir;

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
            std::unordered_map<IrValue const*, VReg> values;
            std::unordered_map<IrValue const*, std::uint32_t> slots;
            std::unordered_map<IrBasicBlock const*, std::uint32_t> block_ids;
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

            [[nodiscard]] static unsigned op_width(unsigned bits) noexcept { return bits <= 16 ? 16 : 32; }

            [[nodiscard]] static std::optional<std::int64_t> constant(IrValue const* value)
            {
                if (auto* integer = ir_cast<IrIntConstant>(value))
                    return static_cast<std::int32_t>(static_cast<std::uint32_t>(integer->value));
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
                auto opc = bits == 8 ? MOpc::MOVZX16rm8 : bits == 16 ? MOpc::MOV16rm : MOpc::MOV32rm;
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
                    append(make_instr(opc, {*mem, MOp::from_imm(fit(*imm, bits, false, op_width(bits)))}, 0));
                    return;
                }
                auto v = value(stored);
                if (!v)
                    return;
                if (bits == 8)
                {
                    copy_to(PhysReg::RAX, *v, 16);
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
                if (!emit_compare(inst, setcc))
                    return;
                append(make_instr(setcc, {phys_operand(PhysReg::RAX)}, 1));
                values[static_cast<IrValue const*>(inst)] = emit(MOpc::MOVZX16_8rr, {phys_operand(PhysReg::RAX)});
            }

            void lower_unary(IrValue const* inst, IrValue const* operand)
            {
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

            void lower_call(IrValue const* inst, IrValue const* callee, std::span<IrValue* const> args)
            {
                auto const* ref = ir_cast<IrGlobalRef>(callee);
                if (!ref || !ref->function)
                    return unsupported("an indirect call");
                bool const c_convention = uses_c_abi(*ref->function);
                std::vector<unsigned> widths;
                for (auto* arg : args)
                {
                    auto bits = value_bits(arg ? arg->type : nullptr);
                    if (!bits)
                        return;
                    widths.push_back(bits);
                }
                std::size_t const in_registers = c_convention ? 0 : std::min(args.size(), register_arguments.size());
                std::int64_t pushed = 0;
                for (auto i = args.size(); i > in_registers; --i)
                {
                    push_argument(args[i - 1], widths[i - 1]);
                    pushed += widths[i - 1] <= 16 ? 2 : 4;
                }
                std::uint64_t uses = bit(PhysReg::RSP);
                for (std::size_t i = 0; i < in_registers; ++i)
                {
                    auto reg = register_arguments[i];
                    auto width = op_width(widths[i]);
                    if (auto imm = constant(args[i]))
                        append(make_instr(width == 16 ? MOpc::MOV16ri : MOpc::MOV32ri, {phys_operand(reg), MOp::from_imm(fit(*imm, widths[i], false, width))}, 1));
                    else if (auto v = value(args[i]))
                        copy_to(reg, *v, width);
                    else
                        return;
                    uses |= bit(reg);
                }
                MInstr call = make_instr(MOpc::CALL, {MOp::from_symbol(ref->function->name)}, 0);
                call.implicit_uses = uses;
                call.implicit_defs = bit(PhysReg::RAX) | bit(PhysReg::RBX) | bit(PhysReg::RCX) | bit(PhysReg::RDX);
                append(call);
                if (pushed > 0)
                    append(make_instr(MOpc::ADD16ri, {phys_operand(PhysReg::RSP), phys_operand(PhysReg::RSP), MOp::from_imm(pushed)}, 1));
                if (!inst->type || inst->type->kind == IrTypeKind::Void)
                    return;
                auto bits = value_bits(inst->type);
                if (!bits)
                    return;
                if (bits == 32 && c_convention)
                {
                    auto low = copy_from(PhysReg::RAX);
                    auto high = copy_from(PhysReg::RDX);
                    auto shifted = emit(MOpc::SHL32ri8, {MOp::from_reg(high), MOp::from_imm(16)});
                    auto widened = emit(MOpc::MOVZX32_16rr, {MOp::from_reg(low)});
                    values[inst] = emit(MOpc::OR32rr, {MOp::from_reg(shifted), MOp::from_reg(widened)});
                    return;
                }
                values[inst] = copy_from(PhysReg::RAX);
            }

            void lower_parameters()
            {
                auto const& params = func.entry_block->params;
                std::size_t const in_registers = c_abi ? 0 : std::min(params.size(), register_arguments.size());
                std::int32_t offset = 4;
                for (std::size_t i = 0; i < params.size(); ++i)
                {
                    auto const* param = params[i];
                    auto bits = value_bits(param ? param->type : nullptr);
                    if (!bits)
                        return;
                    if (i < in_registers)
                    {
                        values[param] = copy_from(register_arguments[i]);
                        continue;
                    }
                    MMem slot{};
                    slot.base = VReg::phys(PhysReg::RBP);
                    slot.disp = offset;
                    values[param] = emit(bits <= 16 ? MOpc::MOV16rm : MOpc::MOV32rm, {MOp::from_mem(slot)});
                    offset += bits <= 16 ? 2 : 4;
                }
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
                    auto bits = value_bits(ret.value->type);
                    auto v = bits ? value(ret.value) : std::nullopt;
                    if (!v)
                        return;
                    copy_to(PhysReg::RAX, *v, op_width(bits));
                    uses |= 1ULL << static_cast<unsigned>(PhysReg::RAX);
                    if (bits == 32 && c_abi)
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
                    bool const wide_addresses = target.pointer_bits == 32;
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
                auto pred_id = block_ids.at(pred);
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
                    auto bits = value_bits(phi->type);
                    if (!bits)
                        return;
                    std::vector<MOp> inputs;
                    for (auto const& incoming : phi->incoming)
                    {
                        if (!block_ids.contains(incoming.block))
                            continue;
                        auto via = edge_block(incoming.block, owner, pred_counts[owner]);
                        VReg input;
                        if (auto imm = constant(incoming.value))
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
                for (auto* ir_block : order)
                {
                    auto& mblock = mfunc.create_block(ir_block->has_name() ? ir_block->name : std::string_view{});
                    if (ir_block == func.entry_block)
                        mfunc.entry_block_id = mblock.id;
                    block_ids[ir_block] = mblock.id;
                }

                auto use_def = analysis::UseDef::build(func);
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
                    if (condition && setcc_for(condition->kind) && use_def.use_count(condition) == 0 && terminator_uses[condition] == 1 &&
                        std::ranges::find(ir_block->instructions, condition) != ir_block->instructions.end())
                        fused.insert(condition);
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
        return std::move(isel.mfunc);
    }
} // namespace dcc::backend::i8086
