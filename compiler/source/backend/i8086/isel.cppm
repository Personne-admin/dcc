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

        struct Isel
        {
            IrFunction const& func;
            target::TargetConfig const& target;
            std::vector<std::string>& diags;
            MFunction mfunc;
            MBlock* block{};
            bool c_abi{};

            void unsupported(std::string_view what)
            {
                diags.push_back(std::format("i8086 backend: {} in function `{}` is not supported yet", what, func.name));
            }

            void append(MInstr const& mi) { block->instrs.push_back(mi); }

            [[nodiscard]] static MInstr make(MOpc opc, std::initializer_list<MOp> ops, std::uint8_t defs)
            {
                MInstr mi;
                mi.opc = opc;
                mi.num_defs = defs;
                for (auto const& op : ops)
                    mi.ops[mi.num_ops++] = op;
                return mi;
            }

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

            [[nodiscard]] std::optional<VReg> materialize(IrValue const* value)
            {
                std::optional<std::int64_t> constant;
                if (auto* integer = ir_cast<IrIntConstant>(value))
                    constant = integer->value;
                else if (auto* boolean = ir_cast<IrBoolConstant>(value))
                    constant = boolean->value ? 1 : 0;
                if (!constant)
                {
                    unsupported(std::format("IR value kind {}", static_cast<int>(value->kind)));
                    return std::nullopt;
                }
                auto bits = scalar_bits(value->type);
                if (bits == 0 || bits > 32)
                {
                    unsupported(std::format("a {}-byte constant", value->type ? value->type->byte_size : 0));
                    return std::nullopt;
                }
                VReg v = mfunc.new_vreg();
                append(make(MOpc::MOV32ri, {MOp::from_reg(v), MOp::from_imm(static_cast<std::int64_t>(static_cast<std::uint32_t>(*constant)))}, 1));
                return v;
            }

            void lower_ret(IrRetInst const& ret)
            {
                std::uint64_t uses = (1ULL << static_cast<unsigned>(PhysReg::RSP));
                if (ret.value)
                {
                    auto bits = scalar_bits(ret.value->type);
                    if (bits == 0 || bits > 32)
                    {
                        unsupported(std::format("returning a {}-byte value", ret.value->type ? ret.value->type->byte_size : 0));
                        return;
                    }
                    auto v = materialize(ret.value);
                    if (!v)
                        return;
                    append(make(MOpc::COPY, {MOp::from_reg(VReg::phys(PhysReg::RAX)), MOp::from_reg(*v)}, 1));
                    uses |= 1ULL << static_cast<unsigned>(PhysReg::RAX);
                    if (bits == 32 && c_abi)
                    {
                        append(make(MOpc::COPY, {MOp::from_reg(VReg::phys(PhysReg::RDX)), MOp::from_reg(*v)}, 1));
                        append(make(MOpc::SHR32ri8, {MOp::from_reg(VReg::phys(PhysReg::RDX)), MOp::from_reg(VReg::phys(PhysReg::RDX)), MOp::from_imm(16)}, 1));
                        uses |= 1ULL << static_cast<unsigned>(PhysReg::RDX);
                    }
                }
                MInstr mi = make(MOpc::RET, {}, 0);
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
                unsupported(std::format("IR terminator kind {}", static_cast<int>(term->kind)));
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
                        if (!inst)
                            continue;
                        unsupported(std::format("IR instruction kind {}", static_cast<int>(inst->kind)));
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
        Isel isel{func, target, diags, {}};
        isel.run();
        return std::move(isel.mfunc);
    }

} // namespace dcc::backend::i8086
