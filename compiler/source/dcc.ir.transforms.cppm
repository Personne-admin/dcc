export module dcc.ir.transforms;

import std;
import dcc.ir;
import dcc.ir.analysis;
import dcc.ir.pass;

namespace dcc::ir::pass
{
    [[nodiscard]] static bool is_side_effect_free(IrNodeKind kind)
    {
        switch (kind)
        {
            case IrNodeKind::Add:
            case IrNodeKind::Sub:
            case IrNodeKind::Mul:
            case IrNodeKind::UDiv:
            case IrNodeKind::SDiv:
            case IrNodeKind::URem:
            case IrNodeKind::SRem:
            case IrNodeKind::FDiv:
            case IrNodeKind::FRem:
            case IrNodeKind::And:
            case IrNodeKind::Or:
            case IrNodeKind::Xor:
            case IrNodeKind::Shl:
            case IrNodeKind::LShr:
            case IrNodeKind::AShr:
            case IrNodeKind::Neg:
            case IrNodeKind::Not:
            case IrNodeKind::CmpEq:
            case IrNodeKind::CmpNe:
            case IrNodeKind::CmpLt:
            case IrNodeKind::CmpLe:
            case IrNodeKind::CmpGt:
            case IrNodeKind::CmpGe:
            case IrNodeKind::CmpOLt:
            case IrNodeKind::CmpOLe:
            case IrNodeKind::CmpOGt:
            case IrNodeKind::CmpOGe:
            case IrNodeKind::CmpULt:
            case IrNodeKind::CmpULe:
            case IrNodeKind::CmpUGt:
            case IrNodeKind::CmpUGe:
            case IrNodeKind::Zext:
            case IrNodeKind::Sext:
            case IrNodeKind::Trunc:
            case IrNodeKind::FpExt:
            case IrNodeKind::FpTrunc:
            case IrNodeKind::FpToI:
            case IrNodeKind::IToFp:
            case IrNodeKind::PtrToI:
            case IrNodeKind::IToPtr:
            case IrNodeKind::Bitcast:
            case IrNodeKind::ReadSegment:
            case IrNodeKind::MakePointer:
            case IrNodeKind::PointerOffset:
            case IrNodeKind::PointerSegment:
            case IrNodeKind::Gep:
            case IrNodeKind::Extract:
            case IrNodeKind::Insert:
            case IrNodeKind::Aggregate:
            case IrNodeKind::Phi:
            case IrNodeKind::Load:
                return true;
            case IrNodeKind::Store:
            case IrNodeKind::StoreVolatile:
            case IrNodeKind::LoadVolatile:
            case IrNodeKind::AtomicLoad:
            case IrNodeKind::AtomicStore:
            case IrNodeKind::AtomicRmw:
            case IrNodeKind::AtomicCmpXchg:
            case IrNodeKind::Fence:
            case IrNodeKind::Call:
            case IrNodeKind::CallTail:
            case IrNodeKind::Alloca:
                return false;
            default:
                return false;
        }
    }

    [[nodiscard]] static IrValue* create_zero_for_type(IrType const* t, IrContext& ctx)
    {
        if (!t)
            return nullptr;

        switch (t->kind)
        {
            case IrTypeKind::Bool:
                return ctx.bool_const(false);
            case IrTypeKind::Int:
                return ctx.int_const(t, 0);
            case IrTypeKind::Float:
                return ctx.float_const(t, 0.0);
            case IrTypeKind::Pointer:
                return ctx.null_const(t);
            default:
                return nullptr;
        }
    }

    static void replace_value_uses(IrFunction& func, IrValue* old_val, IrValue* new_val)
    {
        auto replace_in = [&](IrNode* node) {
            if (!node)
                return;

            switch (node->kind)
            {

#define BINOP_CASE(k)                                                                                                                                          \
    case IrNodeKind::k: {                                                                                                                                      \
        auto* bi = static_cast<Ir##k##Inst*>(node);                                                                                                            \
        if (bi->lhs == old_val)                                                                                                                                \
            bi->lhs = new_val;                                                                                                                                 \
        if (bi->rhs == old_val)                                                                                                                                \
            bi->rhs = new_val;                                                                                                                                 \
        break;                                                                                                                                                 \
    }

                BINOP_CASE(Add);
                BINOP_CASE(Sub);
                BINOP_CASE(Mul);
                BINOP_CASE(UDiv);
                BINOP_CASE(SDiv);
                BINOP_CASE(URem);
                BINOP_CASE(SRem);
                BINOP_CASE(FDiv);
                BINOP_CASE(FRem);
                BINOP_CASE(And);
                BINOP_CASE(Or);
                BINOP_CASE(Xor);
                BINOP_CASE(Shl);
                BINOP_CASE(LShr);
                BINOP_CASE(AShr);

#undef BINOP_CASE

                case IrNodeKind::Neg:
                    if (static_cast<IrNegInst*>(node)->operand == old_val)
                        static_cast<IrNegInst*>(node)->operand = new_val;
                    break;
                case IrNodeKind::Not:
                    if (static_cast<IrNotInst*>(node)->operand == old_val)
                        static_cast<IrNotInst*>(node)->operand = new_val;
                    break;

#define CMP_CASE(k)                                                                                                                                            \
    case IrNodeKind::k: {                                                                                                                                      \
        auto* ci = static_cast<Ir##k##Inst*>(node);                                                                                                            \
        if (ci->lhs == old_val)                                                                                                                                \
            ci->lhs = new_val;                                                                                                                                 \
        if (ci->rhs == old_val)                                                                                                                                \
            ci->rhs = new_val;                                                                                                                                 \
        break;                                                                                                                                                 \
    }

                    CMP_CASE(CmpEq);
                    CMP_CASE(CmpNe);
                    CMP_CASE(CmpLt);
                    CMP_CASE(CmpLe);
                    CMP_CASE(CmpGt);
                    CMP_CASE(CmpGe);
                    CMP_CASE(CmpOLt);
                    CMP_CASE(CmpOLe);
                    CMP_CASE(CmpOGt);
                    CMP_CASE(CmpOGe);
                    CMP_CASE(CmpULt);
                    CMP_CASE(CmpULe);
                    CMP_CASE(CmpUGt);
                    CMP_CASE(CmpUGe);

#undef CMP_CASE

                case IrNodeKind::Alloca: {
                    auto& count = static_cast<IrAllocaInst*>(node)->count;
                    if (count == old_val)
                        count = new_val;
                    break;
                }
                case IrNodeKind::Load: {
                    auto& ptr = static_cast<IrLoadInst*>(node)->pointer;
                    if (ptr == old_val)
                        ptr = new_val;
                    break;
                }
                case IrNodeKind::LoadVolatile: {
                    auto& ptr = static_cast<IrLoadVolatileInst*>(node)->pointer;
                    if (ptr == old_val)
                        ptr = new_val;
                    break;
                }
                case IrNodeKind::Store: {
                    auto* s = static_cast<IrStoreInst*>(node);
                    if (s->value == old_val)
                        s->value = new_val;
                    if (s->pointer == old_val)
                        s->pointer = new_val;
                    break;
                }
                case IrNodeKind::StoreVolatile: {
                    auto* s = static_cast<IrStoreVolatileInst*>(node);
                    if (s->value == old_val)
                        s->value = new_val;
                    if (s->pointer == old_val)
                        s->pointer = new_val;
                    break;
                }
                case IrNodeKind::Gep: {
                    auto* g = static_cast<IrGepInst*>(node);
                    if (g->base == old_val)
                        g->base = new_val;
                    for (auto& idx : g->indices)
                        if (idx.kind == IrGepInst::IndexKind::Array && idx.dynamic_index == old_val)
                            idx.dynamic_index = new_val;
                    break;
                }

#define CAST_CASE(k)                                                                                                                                           \
    case IrNodeKind::k: {                                                                                                                                      \
        auto& op = static_cast<Ir##k##Inst*>(node)->operand;                                                                                                   \
        if (op == old_val)                                                                                                                                     \
            op = new_val;                                                                                                                                      \
        break;                                                                                                                                                 \
    }

                    CAST_CASE(Zext);
                    CAST_CASE(Sext);
                    CAST_CASE(Trunc);
                    CAST_CASE(FpExt);
                    CAST_CASE(FpTrunc);
                    CAST_CASE(FpToI);
                    CAST_CASE(IToFp);
                    CAST_CASE(PtrToI);
                    CAST_CASE(IToPtr);
                    CAST_CASE(Bitcast);

#undef CAST_CASE

                case IrNodeKind::MakePointer: {
                    auto* p = static_cast<IrMakePointerInst*>(node);
                    if (p->offset == old_val)
                        p->offset = new_val;
                    if (p->segment == old_val)
                        p->segment = new_val;
                    break;
                }
                case IrNodeKind::PointerOffset: {
                    auto* p = static_cast<IrPointerOffsetInst*>(node);
                    if (p->pointer == old_val)
                        p->pointer = new_val;
                    break;
                }
                case IrNodeKind::PointerSegment: {
                    auto* p = static_cast<IrPointerSegmentInst*>(node);
                    if (p->pointer == old_val)
                        p->pointer = new_val;
                    break;
                }

                case IrNodeKind::Extract: {
                    auto& agg = static_cast<IrExtractInst*>(node)->aggregate;
                    if (agg == old_val)
                        agg = new_val;
                    break;
                }
                case IrNodeKind::Insert: {
                    auto* i = static_cast<IrInsertInst*>(node);
                    if (i->aggregate == old_val)
                        i->aggregate = new_val;
                    if (i->value == old_val)
                        i->value = new_val;
                    break;
                }
                case IrNodeKind::Aggregate: {
                    for (auto& v : static_cast<IrAggregateInst*>(node)->values)
                        if (v == old_val)
                            v = new_val;
                    break;
                }
                case IrNodeKind::Phi: {
                    for (auto& inc : static_cast<IrPhiInst*>(node)->incoming)
                        if (inc.value == old_val)
                            inc.value = new_val;
                    break;
                }
                case IrNodeKind::Call: {
                    auto* c = static_cast<IrCallInst*>(node);
                    if (c->callee == old_val)
                        c->callee = new_val;
                    for (auto& arg : c->args)
                        if (arg == old_val)
                            arg = new_val;
                    break;
                }
                case IrNodeKind::InlineAsm: {
                    for (auto& op : static_cast<IrInlineAsmInst*>(node)->operands)
                        if (op.value == old_val)
                            op.value = new_val;
                    break;
                }
                case IrNodeKind::CallTail: {
                    auto* c = static_cast<IrCallTailInst*>(node);
                    if (c->callee == old_val)
                        c->callee = new_val;
                    for (auto& arg : c->args)
                        if (arg == old_val)
                            arg = new_val;
                    break;
                }
                case IrNodeKind::AtomicLoad: {
                    auto& ptr = static_cast<IrAtomicLoadInst*>(node)->pointer;
                    if (ptr == old_val)
                        ptr = new_val;
                    break;
                }
                case IrNodeKind::AtomicStore: {
                    auto* s = static_cast<IrAtomicStoreInst*>(node);
                    if (s->value == old_val)
                        s->value = new_val;
                    if (s->pointer == old_val)
                        s->pointer = new_val;
                    break;
                }
                case IrNodeKind::AtomicRmw: {
                    auto* r = static_cast<IrAtomicRmwInst*>(node);
                    if (r->pointer == old_val)
                        r->pointer = new_val;
                    if (r->value == old_val)
                        r->value = new_val;
                    break;
                }
                case IrNodeKind::AtomicCmpXchg: {
                    auto* x = static_cast<IrAtomicCmpXchgInst*>(node);
                    if (x->pointer == old_val)
                        x->pointer = new_val;
                    if (x->expected == old_val)
                        x->expected = new_val;
                    if (x->desired == old_val)
                        x->desired = new_val;
                    break;
                }
                case IrNodeKind::BrCond: {
                    auto& cond = static_cast<IrBrCondInst*>(node)->condition;
                    if (cond == old_val)
                        cond = new_val;
                    break;
                }
                case IrNodeKind::Ret: {
                    auto& val = static_cast<IrRetInst*>(node)->value;
                    if (val == old_val)
                        val = new_val;
                    break;
                }
                case IrNodeKind::Switch: {
                    auto& val = static_cast<IrSwitchInst*>(node)->value;
                    if (val == old_val)
                        val = new_val;
                    break;
                }
                case IrNodeKind::IntConstant:
                case IrNodeKind::FloatConstant:
                case IrNodeKind::BoolConstant:
                case IrNodeKind::NullConstant:
                case IrNodeKind::PointerConstant:
                case IrNodeKind::StringConstant:
                case IrNodeKind::Local:
                case IrNodeKind::GlobalRef:
                case IrNodeKind::Fence:
                case IrNodeKind::ReadSegment:
                case IrNodeKind::Br:
                case IrNodeKind::Unreachable:
                case IrNodeKind::BasicBlock:
                case IrNodeKind::Function:
                case IrNodeKind::Global:
                    break;
                default:
                    std::abort();
            }
        };

        for (auto* bb : func.blocks)
        {
            for (auto* inst : bb->instructions)
                if (inst)
                    replace_in(inst);

            if (bb->terminator)
                replace_in(bb->terminator);
        }
    }

    [[nodiscard]] static bool value_used_by_terminator(IrFunction const& func, IrValue const* val)
    {
        for (auto* bb : func.blocks)
        {
            if (!bb->terminator)
                continue;

            switch (bb->terminator->kind)
            {
                case IrNodeKind::BrCond: {
                    if (static_cast<IrBrCondInst const*>(bb->terminator)->condition == val)
                        return true;
                    break;
                }
                case IrNodeKind::Ret: {
                    if (static_cast<IrRetInst const*>(bb->terminator)->value == val)
                        return true;
                    break;
                }
                case IrNodeKind::Switch: {
                    if (static_cast<IrSwitchInst const*>(bb->terminator)->value == val)
                        return true;
                    break;
                }
                default:
                    break;
            }
        }
        return false;
    }

    [[nodiscard]] static std::vector<IrType const*> sroa_members(IrType const* type, IrContext& ctx)
    {
        if (auto* aggregate = ir_type_cast<IrAggregateType>(type))
        {
            if (aggregate->has_trailing_fam || aggregate->bitfield_is_union ||
                std::ranges::any_of(aggregate->bitfield_debug, [](auto const& field) { return field.is_bitfield; }))
                return {};
            std::uint64_t end = 0;
            if (aggregate->member_offsets.size() != aggregate->members.size())
                return {};
            for (std::size_t i = 0; i < aggregate->members.size(); ++i)
            {
                auto* member = aggregate->members[i];
                auto offset = aggregate->member_offsets[i];
                if (!member || offset < end || offset > type->byte_size || member->byte_size > type->byte_size - offset)
                    return {};
                end = offset + member->byte_size;
            }
            return {aggregate->members.begin(), aggregate->members.end()};
        }
        if (auto* array = ir_type_cast<IrArrayType>(type); array && array->count <= 16)
            return std::vector<IrType const*>(array->count, array->element);
        if (auto* slice = ir_type_cast<IrSliceType>(type))
            return {ctx.pointer_to(slice->element, slice->seg, slice->flavor), ctx.usize_t()};
        return {};
    }

    static bool sroa_impl(FunctionPassContext& ctx)
    {
        using Path = std::vector<std::uint32_t>;
        struct Leaf
        {
            Path path;
            IrType const* type;
            IrAllocaInst* slot{};
        };
        std::vector<IrAllocaInst*> candidates;
        if (!ctx.func->entry_block)
            return false;
        for (auto* inst : ctx.func->entry_block->instructions)
            if (auto* root = ir_cast<IrAllocaInst>(inst); root && !root->count && root->allocated_type && root->allocated_type->byte_size > 0 &&
                                                          root->allocated_type->byte_size <= 64 &&
                                                          (root->allocated_type->kind == IrTypeKind::Aggregate ||
                                                           root->allocated_type->kind == IrTypeKind::Array || root->allocated_type->kind == IrTypeKind::Slice))
                candidates.push_back(root);
        std::vector<IrAggregateInst*> aggregate_nodes;
        for (auto const& [value, users] : ctx.get_use_def().uses)
            if (auto* aggregate = ir_cast<IrAggregateInst>(const_cast<IrValue*>(value)))
                aggregate_nodes.push_back(aggregate);
        for (auto* bb : ctx.func->blocks)
            if (auto* ret = ir_cast<IrRetInst>(bb->terminator))
                if (auto* aggregate = ir_cast<IrAggregateInst>(ret->value))
                    aggregate_nodes.push_back(aggregate);
        std::unordered_set<IrAggregateInst*> seen_aggregates;
        for (std::size_t i = 0; i < aggregate_nodes.size(); ++i)
            if (seen_aggregates.insert(aggregate_nodes[i]).second)
                for (auto* member : aggregate_nodes[i]->values)
                    if (auto* aggregate = ir_cast<IrAggregateInst>(member); aggregate && !seen_aggregates.contains(aggregate))
                        aggregate_nodes.push_back(aggregate);
        auto replace = [&](IrValue* old_value, IrValue* new_value) {
            replace_value_uses(*ctx.func, old_value, new_value);
            for (auto* aggregate : aggregate_nodes)
                for (auto*& member : aggregate->values)
                    if (member == old_value)
                        member = new_value;
        };
        bool changed = false;
        for (auto* root : candidates)
        {
            std::vector<Leaf> leaves;
            std::map<Path, IrType const*> types;
            std::map<Path, std::vector<IrType const*>> children;
            auto flatten = [&](auto&& self, IrType const* type, Path path) -> bool {
                if (!type || path.size() > 8 || leaves.size() >= 16 || type->byte_size == 0)
                    return false;
                types[path] = type;
                if (type->kind == IrTypeKind::Aggregate || type->kind == IrTypeKind::Array || type->kind == IrTypeKind::Slice)
                {
                    auto members = sroa_members(type, *ctx.ctx);
                    if (members.empty())
                        return false;
                    children[path] = members;
                    for (std::uint32_t i = 0; i < members.size(); ++i)
                    {
                        auto child = path;
                        child.push_back(i);
                        if (!self(self, members[i], std::move(child)))
                            return false;
                    }
                    return true;
                }
                if (!create_zero_for_type(type, *ctx.ctx))
                    return false;
                leaves.push_back({std::move(path), type});
                return true;
            };
            if (!flatten(flatten, root->allocated_type, {}))
                continue;
            auto uses = ctx.get_use_def();
            std::unordered_set<IrValue const*> in_block;
            std::vector<IrAggregateInst*> aggregates;
            for (auto* bb : ctx.func->blocks)
            {
                for (auto* inst : bb->instructions)
                    in_block.insert(inst);
                if (auto* ret = ir_cast<IrRetInst>(bb->terminator))
                    if (auto* aggregate = ir_cast<IrAggregateInst>(ret->value))
                        aggregates.push_back(aggregate);
            }
            for (auto const& [value, users] : uses.uses)
                if (auto* aggregate = ir_cast<IrAggregateInst>(const_cast<IrValue*>(value)))
                    aggregates.push_back(aggregate);
            std::unordered_set<IrAggregateInst*> visited;
            for (std::size_t i = 0; i < aggregates.size(); ++i)
            {
                auto* aggregate = aggregates[i];
                if (!visited.insert(aggregate).second)
                    continue;
                for (auto* member : aggregate->values)
                {
                    uses.uses[member].push_back(aggregate);
                    if (auto* nested = ir_cast<IrAggregateInst>(member))
                        aggregates.push_back(nested);
                }
            }
            std::unordered_map<IrValue const*, Path> addresses;
            addresses.emplace(root, Path{});
            std::vector<IrValue*> pending{root};
            bool eligible = true;
            bool accessed = false;
            auto reachable = ctx.get_rpo();
            std::unordered_set<IrBasicBlock const*> reachable_set(reachable.begin(), reachable.end());
            for (auto* bb : ctx.func->blocks)
                if (!reachable_set.contains(bb))
                    for (auto* inst : bb->instructions)
                        if (inst == root)
                            eligible = false;
            for (std::size_t i = 0; eligible && i < pending.size(); ++i)
            {
                auto* address = pending[i];
                auto path = addresses.at(address);
                if (value_used_by_terminator(*ctx.func, address))
                {
                    eligible = false;
                    break;
                }
                for (auto* user : uses.uses_of(address))
                {
                    if (auto* gep = ir_cast<IrGepInst>(user); gep && gep->base == address)
                    {
                        auto subpath = path;
                        for (auto const& index : gep->indices)
                        {
                            auto* type = types.at(subpath);
                            if (index.kind == IrGepInst::IndexKind::Array && type->kind != IrTypeKind::Array)
                            {
                                auto* constant = ir_cast<IrIntConstant>(index.dynamic_index);
                                if (!constant || constant->value != 0)
                                    eligible = false;
                                continue;
                            }
                            auto found = children.find(subpath);
                            std::int64_t field = index.field_index;
                            if (index.kind == IrGepInst::IndexKind::Array)
                            {
                                auto* constant = ir_cast<IrIntConstant>(index.dynamic_index);
                                if (!constant)
                                {
                                    eligible = false;
                                    break;
                                }
                                field = constant->value;
                            }
                            if (found == children.end() || field < 0 || static_cast<std::uint64_t>(field) >= found->second.size())
                            {
                                eligible = false;
                                break;
                            }
                            subpath.push_back(static_cast<std::uint32_t>(field));
                        }
                        if (!eligible)
                            break;
                        if (addresses.emplace(gep, subpath).second)
                            pending.push_back(gep);
                    }
                    else if (auto* load = ir_cast<IrLoadInst>(user);
                             load && load->pointer == address && load->type->kind == types.at(path)->kind && load->type->byte_size == types.at(path)->byte_size)
                    {
                        accessed = true;
                        for (auto* consumer : uses.uses_of(load))
                            if (consumer->kind == IrNodeKind::Aggregate && !in_block.contains(consumer))
                                eligible = false;
                    }
                    else if (auto* store = ir_cast<IrStoreInst>(user); store && store->pointer == address && store->value != address &&
                                                                       store->value->type->kind == types.at(path)->kind &&
                                                                       store->value->type->byte_size == types.at(path)->byte_size)
                        accessed = true;
                    else
                    {
                        eligible = false;
                        break;
                    }
                }
            }
            if (!eligible || !accessed)
                continue;
            std::map<Path, IrAllocaInst*> slots;
            for (auto& leaf : leaves)
            {
                leaf.slot = ctx.ctx->alloca(ctx.ctx->pointer_to(leaf.type), leaf.type);
                leaf.slot->debug_loc = root->debug_loc;
                slots[leaf.path] = leaf.slot;
            }
            for (auto* bb : ctx.func->blocks)
            {
                std::vector<IrValue*> rewritten;
                auto read = [&](auto&& self, Path const& path) -> IrValue* {
                    if (auto found = slots.find(path); found != slots.end())
                    {
                        auto* load = ctx.ctx->load(types.at(path), found->second);
                        rewritten.push_back(load);
                        return load;
                    }
                    auto* aggregate = ctx.ctx->aggregate(types.at(path));
                    auto const& members = children.at(path);
                    for (std::uint32_t i = 0; i < members.size(); ++i)
                    {
                        auto child = path;
                        child.push_back(i);
                        aggregate->values.push_back(self(self, child));
                    }
                    rewritten.push_back(aggregate);
                    return aggregate;
                };
                auto write = [&](auto&& self, Path const& path, IrValue* value) -> void {
                    if (auto found = slots.find(path); found != slots.end())
                    {
                        rewritten.push_back(ctx.ctx->store(value, found->second));
                        return;
                    }
                    auto const& members = children.at(path);
                    for (std::uint32_t i = 0; i < members.size(); ++i)
                    {
                        auto child = path;
                        child.push_back(i);
                        IrValue* member{};
                        if (auto* aggregate = ir_cast<IrAggregateInst>(value); aggregate && i < aggregate->values.size())
                            member = aggregate->values[i];
                        else
                        {
                            member = ctx.ctx->extract(members[i], value, i);
                            rewritten.push_back(member);
                        }
                        self(self, child, member);
                    }
                };
                for (auto* inst : bb->instructions)
                {
                    if (inst == root)
                    {
                        for (auto const& leaf : leaves)
                            rewritten.push_back(leaf.slot);
                    }
                    else if (inst->kind == IrNodeKind::Gep && addresses.contains(inst))
                        continue;
                    else if (auto* load = ir_cast<IrLoadInst>(inst); load && addresses.contains(load->pointer))
                        replace(load, read(read, addresses.at(load->pointer)));
                    else if (auto* store = ir_cast<IrStoreInst>(inst); store && addresses.contains(store->pointer))
                        write(write, addresses.at(store->pointer), store->value);
                    else
                        rewritten.push_back(inst);
                }
                bb->instructions.assign(rewritten.begin(), rewritten.end());
            }
            changed = true;
            ctx.invalidate_use_def();
        }
        std::unordered_set<IrValue const*> owned;
        for (auto* bb : ctx.func->blocks)
            for (auto* inst : bb->instructions)
                owned.insert(inst);
        auto external_aggregate_use = [&](IrValue const* value) {
            return std::ranges::any_of(aggregate_nodes, [&](auto* aggregate) {
                return !owned.contains(aggregate) && std::ranges::find(aggregate->values, value) != aggregate->values.end();
            });
        };
        for (auto* bb : ctx.func->blocks)
            for (auto it = bb->instructions.begin(); it != bb->instructions.end();)
                if (auto* aggregate = ir_cast<IrAggregateInst>(*it); aggregate && !aggregate->values.empty())
                {
                    auto* first = ir_cast<IrExtractInst>(aggregate->values.front());
                    IrValue* original = first ? first->aggregate : nullptr;
                    auto* source = original;
                    while (auto* part = ir_cast<IrExtractInst>(source))
                        source = part->aggregate;
                    bool identity = ir_cast<IrLocal>(source) && original->type == aggregate->type &&
                                    sroa_members(aggregate->type, *ctx.ctx).size() == aggregate->values.size();
                    for (std::uint32_t i = 0; identity && i < aggregate->values.size(); ++i)
                    {
                        auto* part = ir_cast<IrExtractInst>(aggregate->values[i]);
                        identity = part && part->aggregate == original && part->field_index == i;
                    }
                    if (identity && !ir_cast<IrLocal>(original) && external_aggregate_use(aggregate))
                        identity = false;
                    if (identity)
                    {
                        replace(aggregate, original);
                        it = bb->instructions.erase(it);
                        changed = true;
                    }
                    else
                        ++it;
                }
                else if (auto* extract = ir_cast<IrExtractInst>(*it))
                {
                    IrValue* value{};
                    if (auto* source = ir_cast<IrAggregateInst>(extract->aggregate); source && extract->field_index < source->values.size())
                        value = source->values[extract->field_index];
                    if (auto* insert = ir_cast<IrInsertInst>(extract->aggregate); insert && insert->field_index == extract->field_index)
                        value = insert->value;
                    if (value && value->kind > IrNodeKind::GlobalRef && external_aggregate_use(extract))
                        value = nullptr;
                    if (value)
                    {
                        replace(extract, value);
                        it = bb->instructions.erase(it);
                        changed = true;
                    }
                    else
                        ++it;
                }
                else
                    ++it;
        if (changed)
            ctx.invalidate_use_def();
        return changed;
    }

    [[nodiscard]] static bool promote_alloca(IrAllocaInst* alloca, FunctionPassContext& ctx, analysis::UseDef const& ud,
                                             std::unordered_map<IrValue const*, IrBasicBlock*> const& home);
    [[nodiscard]] static bool merge_blocks(FunctionPassContext& ctx);
    [[nodiscard]] static bool remove_unreachable(FunctionPassContext& ctx);

    static bool mem2reg_impl(FunctionPassContext& ctx)
    {
        std::vector<IrAllocaInst*> all_allocas;
        for (auto* bb : ctx.func->blocks)
            for (auto* inst : bb->instructions)
                if (auto* a = ir_cast<IrAllocaInst>(inst))
                    all_allocas.push_back(a);

        auto rebuild_home = [&] {
            std::unordered_map<IrValue const*, IrBasicBlock*> home;
            for (auto* bb : ctx.func->blocks)
            {
                if (!bb)
                    continue;
                for (auto* inst : bb->instructions)
                    if (inst)
                        home[inst] = bb;
            }
            return home;
        };
        std::unordered_map<IrValue const*, IrBasicBlock*> home = rebuild_home();
        bool promoted = false;
        for (auto* alloca : all_allocas)
        {
            if (promote_alloca(alloca, ctx, ctx.get_use_def(), home))
            {
                promoted = true;
                ctx.invalidate_use_def();
                home = rebuild_home();
            }
        }

        if (promoted)
            ctx.invalidate_cfg();

        return promoted;
    }

    [[nodiscard]] static bool promote_alloca(IrAllocaInst* alloca, FunctionPassContext& ctx, analysis::UseDef const& ud,
                                             std::unordered_map<IrValue const*, IrBasicBlock*> const& home)
    {
        if (alloca->count != nullptr)
            return false;

        auto home_block = [&](IrValue const* v) {
            auto it = home.find(v);
            return it != home.end() ? it->second : nullptr;
        };
        auto uses = ud.uses_of(alloca);
        if (uses.empty())
            return false;

        std::vector<IrLoadInst*> loads;
        std::vector<IrStoreInst*> stores;
        bool promotable = true;

        for (auto* user : uses)
        {
            if (!user)
                continue;

            if (auto* load = ir_cast<IrLoadInst>(user))
            {
                if (load->pointer == alloca)
                    loads.push_back(load);
                else
                {
                    promotable = false;
                    break;
                }
            }
            else if (auto* store = ir_cast<IrStoreInst>(user))
            {
                if (store->pointer == alloca)
                    stores.push_back(store);
                else
                {
                    promotable = false;
                    break;
                }
            }
            else
            {
                promotable = false;
                break;
            }
        }

        if (promotable && value_used_by_terminator(*ctx.func, alloca))
            promotable = false;

        if (promotable)
        {
            auto rpo = ctx.get_rpo();
            std::unordered_set<IrBasicBlock const*> reachable(rpo.begin(), rpo.end());
            for (auto* user : uses)
            {
                if (!user)
                    continue;

                auto* bb = home_block(user);
                if (bb && !reachable.contains(bb))
                {
                    promotable = false;
                    break;
                }
            }
        }

        if (!promotable)
            return false;

        auto* val_type = alloca->allocated_type;
        if (!val_type)
            return false;

        if (!create_zero_for_type(val_type, *ctx.ctx))
            return false;

        std::unordered_set<IrBasicBlock*> def_blocks;
        for (auto* st : stores)
        {
            auto* bb = home_block(st);
            if (bb)
                def_blocks.insert(bb);
        }

        auto const& dom = ctx.get_dom_tree();
        auto rpo = ctx.get_rpo();

        std::unordered_set<IrBasicBlock*> idf;
        if (!def_blocks.empty())
        {
            std::vector<IrBasicBlock*> worklist;
            for (auto* bb : rpo)
                if (def_blocks.contains(bb))
                    worklist.push_back(bb);
            std::unordered_set<IrBasicBlock*> in_wl(worklist.begin(), worklist.end());

            while (!worklist.empty())
            {
                auto* b = worklist.back();
                worklist.pop_back();
                in_wl.erase(b);

                auto fit = dom.frontier.find(b);
                if (fit == dom.frontier.end())
                    continue;

                for (auto* d : fit->second)
                {
                    auto* df_block = const_cast<IrBasicBlock*>(d);
                    if (idf.insert(df_block).second)
                    {
                        if (!def_blocks.contains(df_block) && !in_wl.contains(df_block))
                        {
                            worklist.push_back(df_block);
                            in_wl.insert(df_block);
                        }
                    }
                }
            }
        }

        std::unordered_map<IrBasicBlock*, IrPhiInst*> block_phi;
        for (auto* dfb : rpo)
        {
            if (!idf.contains(dfb))
                continue;
            auto* phi = ctx.ctx->phi(val_type);
            phi->debug_loc = alloca->debug_loc;
            dfb->instructions.insert(dfb->instructions.begin(), phi);
            block_phi[dfb] = phi;
        }

        std::unordered_map<IrBasicBlock*, IrValue*> block_end_val;

        IrValue* zero_val = create_zero_for_type(val_type, *ctx.ctx);

        std::unordered_map<IrBasicBlock*, std::vector<IrBasicBlock*>> dom_children;
        for (auto* parent : rpo)
        {
            auto it = dom.children.find(parent);
            if (it != dom.children.end())
                for (auto* child : it->second)
                    dom_children[parent].push_back(const_cast<IrBasicBlock*>(child));
        }

        std::vector<IrBasicBlock*> root_children;
        {
            auto it = dom_children.find(ctx.func->entry_block);
            if (it != dom_children.end())
                root_children = it->second;
        }

        auto* entry_bb = ctx.func->entry_block;
        {
            IrValue* current = zero_val;

            auto pit = block_phi.find(entry_bb);
            if (pit != block_phi.end())
                current = pit->second;

            for (auto it = entry_bb->instructions.begin(); it != entry_bb->instructions.end();)
            {
                auto* inst = *it;
                if (!inst)
                {
                    ++it;
                    continue;
                }

                bool erased = false;

                if (auto* load = ir_cast<IrLoadInst>(inst))
                {
                    if (load->pointer == alloca)
                    {
                        replace_value_uses(*ctx.func, load, current);
                        it = entry_bb->instructions.erase(it);
                        erased = true;
                    }
                }
                if (!erased)
                {
                    if (auto* store = ir_cast<IrStoreInst>(inst))
                    {
                        if (store->pointer == alloca)
                        {
                            current = store->value;
                            it = entry_bb->instructions.erase(it);
                            erased = true;
                        }
                    }
                }
                if (!erased)
                    ++it;
            }

            block_end_val[entry_bb] = current;
        }

        struct RenameFrame
        {
            IrBasicBlock* block;
            std::size_t child_idx{0};
            IrValue* entry_val;
        };

        std::vector<RenameFrame> stack;
        stack.reserve(root_children.size());
        for (auto* child : root_children)
            stack.push_back({.block = child, .child_idx = 0, .entry_val = block_end_val[entry_bb]});

        while (!stack.empty())
        {
            auto& f = stack.back();
            auto* bb = f.block;

            if (f.child_idx == 0)
            {
                IrValue* current = f.entry_val;

                auto pit = block_phi.find(bb);
                if (pit != block_phi.end())
                    current = pit->second;

                for (auto it = bb->instructions.begin(); it != bb->instructions.end();)
                {
                    auto* inst = *it;
                    if (!inst)
                    {
                        ++it;
                        continue;
                    }

                    bool erased = false;
                    if (auto* load = ir_cast<IrLoadInst>(inst))
                    {
                        if (load->pointer == alloca)
                        {
                            replace_value_uses(*ctx.func, load, current);
                            it = bb->instructions.erase(it);
                            erased = true;
                        }
                    }
                    if (!erased)
                    {
                        if (auto* store = ir_cast<IrStoreInst>(inst))
                        {
                            if (store->pointer == alloca)
                            {
                                current = store->value;
                                it = bb->instructions.erase(it);
                                erased = true;
                            }
                        }
                    }
                    if (!erased)
                        ++it;
                }

                block_end_val[bb] = current;
            }

            static const std::vector<IrBasicBlock*> empty_vec;
            auto cit = dom_children.find(bb);
            auto const& kids = (cit != dom_children.end()) ? cit->second : empty_vec;

            if (f.child_idx < kids.size())
            {
                auto* child = kids[f.child_idx++];
                stack.push_back({.block = child, .child_idx = 0, .entry_val = block_end_val[bb]});
            }
            else
                stack.pop_back();
        }

        auto const& pm = ctx.get_pred_map();
        for (auto* phi_bb : rpo)
        {
            auto phi_it = block_phi.find(phi_bb);
            if (phi_it == block_phi.end())
                continue;
            auto* phi = phi_it->second;
            auto pit = pm.find(phi_bb);
            if (pit == pm.end())
                continue;

            for (auto* pred : pit->second)
            {
                auto vit = block_end_val.find(pred);
                IrValue* inc_val = (vit != block_end_val.end()) ? vit->second : zero_val;
                phi->incoming.push_back({inc_val, pred});
            }
        }

        for (auto* bb : ctx.func->blocks)
        {
            auto& insts = bb->instructions;
            for (auto it = insts.begin(); it != insts.end();)
            {
                if (*it == alloca)
                {
                    it = insts.erase(it);
                    break;
                }
                else
                    ++it;
            }
        }

        return true;
    }

    static bool dce_impl(FunctionPassContext& ctx)
    {
        bool changed = false;
        bool iter = true;

        while (iter)
        {
            iter = false;
            auto ud = analysis::UseDef::build(*ctx.func);

            for (auto* bb : ctx.func->blocks)
            {
                auto& insts = bb->instructions;
                for (auto it = insts.begin(); it != insts.end();)
                {
                    auto* inst = *it;
                    if (!inst)
                    {
                        ++it;
                        continue;
                    }

                    if (!is_side_effect_free(inst->kind))
                    {
                        ++it;
                        continue;
                    }

                    bool live = ud.has_uses(inst) || value_used_by_terminator(*ctx.func, inst);

                    if (live && inst->kind == IrNodeKind::Phi)
                    {
                        bool self_only = true;
                        for (auto* u : ud.uses_of(inst))
                        {
                            if (u && u != inst)
                            {
                                self_only = false;
                                break;
                            }
                        }
                        if (self_only && !value_used_by_terminator(*ctx.func, inst))
                            live = false;
                    }

                    if (!live)
                    {
                        it = insts.erase(it);
                        iter = true;
                        changed = true;
                    }
                    else
                        ++it;
                }
            }
        }

        if (changed)
            ctx.invalidate_use_def();

        return changed;
    }

    static bool simplifycfg_impl(FunctionPassContext& ctx)
    {
        bool changed = false;
        changed |= merge_blocks(ctx);
        changed |= remove_unreachable(ctx);
        if (changed)
            ctx.invalidate_cfg();

        return changed;
    }

    [[nodiscard]] static bool merge_blocks(FunctionPassContext& ctx)
    {
        bool changed = false;
        auto pm = analysis::build_pred_map(*ctx.func);
        std::unordered_set<IrBasicBlock*> dead_blocks;
        auto blocks_copy = ctx.func->blocks;

        for (auto* bb : blocks_copy)
        {
            if (bb == ctx.func->entry_block || dead_blocks.contains(bb))
                continue;

            auto pit = pm.find(bb);
            if (pit == pm.end() || pit->second.size() != 1)
                continue;

            auto* pred = pit->second[0];

            if (!pred->terminator || pred->terminator->kind != IrNodeKind::Br)
                continue;
            if (static_cast<IrBrInst*>(pred->terminator)->target != bb)
                continue;

            if (pred == bb)
                continue;

            {
                auto it = bb->instructions.begin();
                while (it != bb->instructions.end())
                {
                    auto* phi = ir_cast<IrPhiInst>(*it);
                    if (!phi)
                        break;

                    IrValue* inc_val = phi->incoming.empty() ? nullptr : phi->incoming[0].value;
                    if (inc_val)
                        replace_value_uses(*ctx.func, phi, inc_val);

                    it = bb->instructions.erase(it);
                }
            }

            for (auto* inst : bb->instructions)
                if (inst)
                    pred->instructions.push_back(inst);

            bb->instructions.clear();

            pred->terminator = bb->terminator;
            bb->terminator = nullptr;

            auto update_succ_phis = [&](IrBasicBlock* old_block, IrBasicBlock* new_block) {
                std::vector<IrBasicBlock*> succs;
                if (!new_block->terminator)
                    return;

                switch (new_block->terminator->kind)
                {
                    case IrNodeKind::Br:
                        succs.push_back(static_cast<IrBrInst*>(new_block->terminator)->target);
                        break;
                    case IrNodeKind::BrCond: {
                        auto* br = static_cast<IrBrCondInst*>(new_block->terminator);
                        succs.push_back(br->true_target);
                        succs.push_back(br->false_target);
                        break;
                    }
                    case IrNodeKind::Switch: {
                        auto* sw = static_cast<IrSwitchInst*>(new_block->terminator);
                        succs.push_back(sw->default_target);
                        for (auto& c : sw->cases)
                            succs.push_back(c.target);
                        break;
                    }
                    default:
                        break;
                }
                for (auto* succ : succs)
                {
                    for (auto* inst : succ->instructions)
                    {
                        auto* phi = ir_cast<IrPhiInst>(inst);
                        if (!phi)
                            continue;

                        for (auto& inc : phi->incoming)
                            if (inc.block == old_block)
                                inc.block = new_block;
                    }
                }
            };
            update_succ_phis(bb, pred);
            {
                std::vector<IrBasicBlock*> succs;
                if (pred->terminator)
                {
                    switch (pred->terminator->kind)
                    {
                        case IrNodeKind::Br:
                            succs.push_back(static_cast<IrBrInst*>(pred->terminator)->target);
                            break;
                        case IrNodeKind::BrCond: {
                            auto* br = static_cast<IrBrCondInst*>(pred->terminator);
                            succs.push_back(br->true_target);
                            succs.push_back(br->false_target);
                            break;
                        }
                        case IrNodeKind::Switch: {
                            auto* sw = static_cast<IrSwitchInst*>(pred->terminator);
                            succs.push_back(sw->default_target);
                            for (auto& c : sw->cases)
                                succs.push_back(c.target);
                            break;
                        }
                        default:
                            break;
                    }
                }
                pm.erase(bb);
                for (auto* succ : succs)
                {
                    if (!succ)
                        continue;
                    auto sit = pm.find(succ);
                    if (sit == pm.end())
                        continue;
                    bool has_pred = false;
                    for (auto* p : sit->second)
                        if (p == pred)
                        {
                            has_pred = true;
                            break;
                        }
                    std::vector<IrBasicBlock*> updated;
                    for (auto* p : sit->second)
                    {
                        if (p == bb)
                        {
                            if (!has_pred)
                            {
                                updated.push_back(pred);
                                has_pred = true;
                            }
                        }
                        else
                            updated.push_back(p);
                    }
                    sit->second = std::move(updated);
                }
            }
            dead_blocks.insert(bb);
            changed = true;
        }
        if (changed)
        {
            auto& blocks = ctx.func->blocks;
            for (auto it = blocks.begin(); it != blocks.end();)
            {
                if (dead_blocks.contains(*it))
                    it = blocks.erase(it);
                else
                    ++it;
            }
        }

        return changed;
    }

    [[nodiscard]] static bool remove_unreachable(FunctionPassContext& ctx)
    {
        std::unordered_set<IrBasicBlock*> reachable;
        std::vector<IrBasicBlock*> worklist;
        if (ctx.func->entry_block)
        {
            reachable.insert(ctx.func->entry_block);
            worklist.push_back(ctx.func->entry_block);
        }

        while (!worklist.empty())
        {
            auto* bb = worklist.back();
            worklist.pop_back();

            auto push_succ = [&](IrBasicBlock const* b) {
                if (!b->terminator)
                    return;

                switch (b->terminator->kind)
                {
                    case IrNodeKind::Br:
                        if (reachable.insert(static_cast<IrBrInst*>(b->terminator)->target).second)
                            worklist.push_back(static_cast<IrBrInst*>(b->terminator)->target);
                        break;
                    case IrNodeKind::BrCond: {
                        auto* br = static_cast<IrBrCondInst*>(b->terminator);
                        if (reachable.insert(br->true_target).second)
                            worklist.push_back(br->true_target);
                        if (reachable.insert(br->false_target).second)
                            worklist.push_back(br->false_target);
                        break;
                    }
                    case IrNodeKind::Switch: {
                        auto* sw = static_cast<IrSwitchInst*>(b->terminator);
                        if (reachable.insert(sw->default_target).second)
                            worklist.push_back(sw->default_target);
                        for (auto& c : sw->cases)
                            if (reachable.insert(c.target).second)
                                worklist.push_back(c.target);
                        break;
                    }
                    default:
                        break;
                }
            };

            push_succ(bb);
        }

        bool changed = false;
        auto& blocks = ctx.func->blocks;

        for (auto it = blocks.begin(); it != blocks.end();)
        {
            auto* bb = *it;
            if (!reachable.contains(bb) && bb != ctx.func->entry_block)
            {
                if (bb->terminator)
                {
                    auto clean_succ_phis = [&](IrBasicBlock const* b) {
                        std::vector<IrBasicBlock*> succs;
                        switch (b->terminator->kind)
                        {
                            case IrNodeKind::Br:
                                succs.push_back(static_cast<IrBrInst*>(b->terminator)->target);
                                break;
                            case IrNodeKind::BrCond: {
                                auto* br = static_cast<IrBrCondInst*>(b->terminator);
                                succs.push_back(br->true_target);
                                succs.push_back(br->false_target);
                                break;
                            }
                            case IrNodeKind::Switch: {
                                auto* sw = static_cast<IrSwitchInst*>(b->terminator);
                                succs.push_back(sw->default_target);
                                for (auto& c : sw->cases)
                                    succs.push_back(c.target);
                                break;
                            }
                            default:
                                break;
                        }
                        for (auto* succ : succs)
                        {
                            for (auto* inst : succ->instructions)
                            {
                                auto* phi = ir_cast<IrPhiInst>(inst);
                                if (!phi)
                                    continue;

                                for (auto pi = phi->incoming.begin(); pi != phi->incoming.end();)
                                    if (pi->block == bb)
                                        pi = phi->incoming.erase(pi);
                                    else
                                        ++pi;
                            }
                        }
                    };
                    clean_succ_phis(bb);
                }
                it = blocks.erase(it);
                changed = true;
            }
            else
            {
                ++it;
            }
        }

        return changed;
    }

    static unsigned integer_width(IrType const* type)
    {
        if (auto* integer = ir_type_cast<IrIntType>(type))
            return integer->bits <= 64 ? integer->bits : 0;
        return type && type->kind == IrTypeKind::Bool ? 1 : 0;
    }

    static std::uint64_t integer_mask(unsigned width)
    {
        return width == 64 ? ~std::uint64_t{} : (std::uint64_t{1} << width) - 1;
    }

    static std::int64_t signed_integer(std::uint64_t value, unsigned width)
    {
        value &= integer_mask(width);
        if (value & (std::uint64_t{1} << (width - 1)))
            value |= ~integer_mask(width);
        return std::bit_cast<std::int64_t>(value);
    }

    static std::optional<std::uint64_t> integer_literal(IrValue const* value)
    {
        if (auto* integer = ir_cast<IrIntConstant>(value); integer && integer_width(integer->type))
            return static_cast<std::uint64_t>(integer->value) & integer_mask(integer_width(integer->type));
        if (auto* boolean = ir_cast<IrBoolConstant>(value))
            return boolean->value;
        return {};
    }

    static IrValue* integer_constant(IrContext& ctx, IrType const* type, std::uint64_t value)
    {
        if (type->kind == IrTypeKind::Bool)
            return ctx.bool_const((value & 1) != 0);
        auto* integer = ir_type_cast<IrIntType>(type);
        value &= integer_mask(integer->bits);
        return ctx.int_const(type, integer->is_signed ? signed_integer(value, integer->bits) : std::bit_cast<std::int64_t>(value));
    }

    struct ConstantState
    {
        unsigned level{};
        std::uint64_t value{};
        bool operator==(ConstantState const&) const = default;
    };

    template <typename Lookup> static ConstantState evaluate_integer(IrValue* inst, Lookup lookup)
    {
        auto width = integer_width(inst->type);
        if (!width)
            return {2};
        auto unary = [&](IrValue* operand, auto operation) -> ConstantState {
            auto state = lookup(operand);
            if (state.level == 1)
                state.value = operation(state.value) & integer_mask(width);
            return state;
        };
        switch (inst->kind)
        {
            case IrNodeKind::Zext:
                return unary(static_cast<IrZextInst*>(inst)->operand, [](auto v) { return v; });
            case IrNodeKind::Trunc:
                return unary(static_cast<IrTruncInst*>(inst)->operand, [](auto v) { return v; });
            case IrNodeKind::Sext: {
                auto* operand = static_cast<IrSextInst*>(inst)->operand;
                auto bits = integer_width(operand->type);
                return bits ? unary(operand, [&](auto v) { return static_cast<std::uint64_t>(signed_integer(v, bits)); }) : ConstantState{2};
            }
            case IrNodeKind::Bitcast: {
                auto* operand = static_cast<IrBitcastInst*>(inst)->operand;
                return integer_width(operand->type) == width ? unary(operand, [](auto v) { return v; }) : ConstantState{2};
            }
            case IrNodeKind::Neg:
                return unary(static_cast<IrNegInst*>(inst)->operand, [](auto v) { return std::uint64_t{} - v; });
            case IrNodeKind::Not:
                return unary(static_cast<IrNotInst*>(inst)->operand, [](auto v) { return ~v; });
            case IrNodeKind::Extract: {
                auto* extract = static_cast<IrExtractInst*>(inst);
                auto* aggregate = ir_cast<IrAggregateInst>(extract->aggregate);
                return aggregate && extract->field_index < aggregate->values.size() ? lookup(aggregate->values[extract->field_index]) : ConstantState{2};
            }
            default:
                break;
        }
        IrValue* lhs{};
        IrValue* rhs{};
        switch (inst->kind)
        {
#define INTEGER_OPERANDS(kind)                                                                                                                                 \
    case IrNodeKind::kind:                                                                                                                                     \
        lhs = static_cast<Ir##kind##Inst*>(inst)->lhs;                                                                                                         \
        rhs = static_cast<Ir##kind##Inst*>(inst)->rhs;                                                                                                         \
        break
            INTEGER_OPERANDS(Add);
            INTEGER_OPERANDS(Sub);
            INTEGER_OPERANDS(Mul);
            INTEGER_OPERANDS(UDiv);
            INTEGER_OPERANDS(SDiv);
            INTEGER_OPERANDS(URem);
            INTEGER_OPERANDS(SRem);
            INTEGER_OPERANDS(And);
            INTEGER_OPERANDS(Or);
            INTEGER_OPERANDS(Xor);
            INTEGER_OPERANDS(Shl);
            INTEGER_OPERANDS(LShr);
            INTEGER_OPERANDS(AShr);
            INTEGER_OPERANDS(CmpEq);
            INTEGER_OPERANDS(CmpNe);
            INTEGER_OPERANDS(CmpLt);
            INTEGER_OPERANDS(CmpLe);
            INTEGER_OPERANDS(CmpGt);
            INTEGER_OPERANDS(CmpGe);
            INTEGER_OPERANDS(CmpULt);
            INTEGER_OPERANDS(CmpULe);
            INTEGER_OPERANDS(CmpUGt);
            INTEGER_OPERANDS(CmpUGe);
#undef INTEGER_OPERANDS
            default:
                return {2};
        }
        auto bits = integer_width(lhs->type);
        if (!bits || !integer_width(rhs->type))
            return {2};
        auto a = lookup(lhs);
        auto b = lookup(rhs);
        if ((inst->kind == IrNodeKind::And || inst->kind == IrNodeKind::Mul) && ((a.level == 1 && a.value == 0) || (b.level == 1 && b.value == 0)))
            return {1, 0};
        if (a.level == 2 || b.level == 2)
            return {2};
        if (a.level == 0 || b.level == 0)
            return {};
        std::uint64_t result{};
        auto sa = signed_integer(a.value, bits);
        auto sb = signed_integer(b.value, integer_width(rhs->type));
        switch (inst->kind)
        {
            case IrNodeKind::Add:
                result = a.value + b.value;
                break;
            case IrNodeKind::Sub:
                result = a.value - b.value;
                break;
            case IrNodeKind::Mul:
                result = a.value * b.value;
                break;
            case IrNodeKind::And:
                result = a.value & b.value;
                break;
            case IrNodeKind::Or:
                result = a.value | b.value;
                break;
            case IrNodeKind::Xor:
                result = a.value ^ b.value;
                break;
            case IrNodeKind::Shl:
            case IrNodeKind::LShr:
            case IrNodeKind::AShr:
                if (b.value >= bits)
                    return {2};
                if (inst->kind == IrNodeKind::Shl)
                    result = a.value << b.value;
                else if (inst->kind == IrNodeKind::LShr)
                    result = a.value >> b.value;
                else
                {
                    result = a.value >> b.value;
                    if (sa < 0 && b.value)
                        result |= integer_mask(bits) ^ (integer_mask(bits) >> b.value);
                }
                break;
            case IrNodeKind::UDiv:
            case IrNodeKind::URem:
                if (!b.value)
                    return {2};
                result = inst->kind == IrNodeKind::UDiv ? a.value / b.value : a.value % b.value;
                break;
            case IrNodeKind::SDiv:
            case IrNodeKind::SRem:
                if (!sb || (a.value == (std::uint64_t{1} << (bits - 1)) && sb == -1))
                    return {2};
                result = static_cast<std::uint64_t>(inst->kind == IrNodeKind::SDiv ? sa / sb : sa % sb);
                break;
            case IrNodeKind::CmpEq:
                result = a.value == b.value;
                break;
            case IrNodeKind::CmpNe:
                result = a.value != b.value;
                break;
            case IrNodeKind::CmpLt:
                result = sa < sb;
                break;
            case IrNodeKind::CmpLe:
                result = sa <= sb;
                break;
            case IrNodeKind::CmpGt:
                result = sa > sb;
                break;
            case IrNodeKind::CmpGe:
                result = sa >= sb;
                break;
            case IrNodeKind::CmpULt:
                result = a.value < b.value;
                break;
            case IrNodeKind::CmpULe:
                result = a.value <= b.value;
                break;
            case IrNodeKind::CmpUGt:
                result = a.value > b.value;
                break;
            case IrNodeKind::CmpUGe:
                result = a.value >= b.value;
                break;
            default:
                return {2};
        }
        return {1, result & integer_mask(width)};
    }

    struct IntegerRewriter
    {
        IrFunction& function;
        std::unordered_set<IrValue const*> external;
        std::vector<IrAggregateInst*> aggregates;
        explicit IntegerRewriter(FunctionPassContext& ctx) : function(*ctx.func)
        {
            std::unordered_set<IrValue const*> owned;
            for (auto* block : function.blocks)
            {
                for (auto* inst : block->instructions)
                    owned.insert(inst);
                if (auto* ret = ir_cast<IrRetInst>(block->terminator))
                    if (auto* aggregate = ir_cast<IrAggregateInst>(ret->value))
                        aggregates.push_back(aggregate);
            }
            for (auto const& [value, users] : ctx.get_use_def().uses)
                if (auto* aggregate = ir_cast<IrAggregateInst>(const_cast<IrValue*>(value)))
                    aggregates.push_back(aggregate);
            std::unordered_set<IrAggregateInst*> seen;
            for (std::size_t i = 0; i < aggregates.size(); ++i)
                if (seen.insert(aggregates[i]).second)
                    for (auto* member : aggregates[i]->values)
                    {
                        if (!owned.contains(aggregates[i]))
                            external.insert(member);
                        if (auto* nested = ir_cast<IrAggregateInst>(member); nested && !seen.contains(nested))
                            aggregates.push_back(nested);
                    }
        }
        bool replace(IrValue* from, IrValue* to)
        {
            if (!to || from == to || (external.contains(from) && to->kind > IrNodeKind::GlobalRef))
                return false;
            replace_value_uses(function, from, to);
            for (auto* aggregate : aggregates)
                for (auto*& member : aggregate->values)
                    if (member == from)
                        member = to;
            return true;
        }
    };

    static std::uint64_t possible_integer_bits(IrValue* value, unsigned depth = 8)
    {
        auto width = integer_width(value->type);
        if (!width)
            return ~std::uint64_t{};
        if (auto constant = integer_literal(value))
            return *constant;
        auto mask = integer_mask(width);
        if (!depth)
            return mask;
        if (auto* extension = ir_cast<IrZextInst>(value))
            return possible_integer_bits(extension->operand, depth - 1) & mask;
        if (auto* truncation = ir_cast<IrTruncInst>(value))
            return possible_integer_bits(truncation->operand, depth - 1) & mask;
        if (auto* conjunction = ir_cast<IrAndInst>(value))
            return possible_integer_bits(conjunction->lhs, depth - 1) & possible_integer_bits(conjunction->rhs, depth - 1);
        if (auto* disjunction = ir_cast<IrOrInst>(value))
            return possible_integer_bits(disjunction->lhs, depth - 1) | possible_integer_bits(disjunction->rhs, depth - 1);
        return mask;
    }

    static bool combine_impl(FunctionPassContext& ctx)
    {
        IntegerRewriter rewriter(ctx);
        bool changed = false;
        auto resize = [&](IrValue* value, IrType const* type) -> IrValue* {
            if (value->type == type)
                return value;
            auto from = integer_width(value->type);
            auto to = integer_width(type);
            return from == to  ? static_cast<IrValue*>(ctx.ctx->bitcast(type, value))
                   : from < to ? static_cast<IrValue*>(ctx.ctx->zext(type, value))
                               : ctx.ctx->trunc(type, value);
        };
        auto lookup = [](IrValue* value) -> ConstantState {
            if (auto constant = integer_literal(value))
                return {1, *constant};
            return {2};
        };
        for (auto* block : ctx.func->blocks)
            for (auto it = block->instructions.begin(); it != block->instructions.end();)
            {
                auto* inst = *it;
                IrValue* replacement{};
                bool fresh = false;
                auto width = integer_width(inst->type);
                if (width)
                {
                    auto folded = evaluate_integer(inst, lookup);
                    if (folded.level == 1)
                        replacement = integer_constant(*ctx.ctx, inst->type, folded.value);
                    auto binary = [&](IrValue* lhs, IrValue* rhs) {
                        auto constant = integer_literal(rhs);
                        if (!constant && (inst->kind == IrNodeKind::Add || inst->kind == IrNodeKind::Mul || inst->kind == IrNodeKind::And ||
                                          inst->kind == IrNodeKind::Or || inst->kind == IrNodeKind::Xor))
                        {
                            std::swap(lhs, rhs);
                            constant = integer_literal(rhs);
                        }
                        if (lhs == rhs)
                        {
                            if (inst->kind == IrNodeKind::Sub || inst->kind == IrNodeKind::Xor)
                                replacement = integer_constant(*ctx.ctx, inst->type, 0);
                            if (inst->kind == IrNodeKind::And || inst->kind == IrNodeKind::Or)
                                replacement = lhs;
                        }
                        if (!constant)
                            return;
                        if ((*constant == 0 &&
                             (inst->kind == IrNodeKind::Add || inst->kind == IrNodeKind::Sub || inst->kind == IrNodeKind::Or || inst->kind == IrNodeKind::Xor ||
                              inst->kind == IrNodeKind::Shl || inst->kind == IrNodeKind::LShr || inst->kind == IrNodeKind::AShr)) ||
                            (*constant == 1 && inst->kind == IrNodeKind::Mul))
                            replacement = lhs;
                        if (inst->kind == IrNodeKind::And)
                        {
                            if ((possible_integer_bits(lhs) & ~*constant) == 0)
                                replacement = lhs;
                            else if (auto* previous = ir_cast<IrAndInst>(lhs))
                            {
                                auto first = integer_literal(previous->rhs);
                                auto* source = previous->lhs;
                                if (!first)
                                {
                                    first = integer_literal(previous->lhs);
                                    source = previous->rhs;
                                }
                                if (first)
                                {
                                    auto mask = *first & *constant;
                                    replacement = ctx.ctx->and_(inst->type, source, integer_constant(*ctx.ctx, inst->type, mask));
                                    fresh = true;
                                }
                            }
                        }
                    };
                    switch (inst->kind)
                    {
#define COMBINE_BINARY(kind)                                                                                                                                   \
    case IrNodeKind::kind:                                                                                                                                     \
        binary(static_cast<Ir##kind##Inst*>(inst)->lhs, static_cast<Ir##kind##Inst*>(inst)->rhs);                                                              \
        break
                        COMBINE_BINARY(Add);
                        COMBINE_BINARY(Sub);
                        COMBINE_BINARY(Mul);
                        COMBINE_BINARY(And);
                        COMBINE_BINARY(Or);
                        COMBINE_BINARY(Xor);
                        COMBINE_BINARY(Shl);
                        COMBINE_BINARY(LShr);
                        COMBINE_BINARY(AShr);
#undef COMBINE_BINARY
                        case IrNodeKind::Neg:
                            if (auto* inner = ir_cast<IrNegInst>(static_cast<IrNegInst*>(inst)->operand))
                                replacement = inner->operand;
                            break;
                        case IrNodeKind::Not:
                            if (auto* inner = ir_cast<IrNotInst>(static_cast<IrNotInst*>(inst)->operand))
                                replacement = inner->operand;
                            break;
                        case IrNodeKind::Zext: {
                            auto* operand = static_cast<IrZextInst*>(inst)->operand;
                            if (auto* inner = ir_cast<IrZextInst>(operand))
                            {
                                replacement = ctx.ctx->zext(inst->type, inner->operand);
                                fresh = true;
                            }
                            if (auto* inner = ir_cast<IrTruncInst>(operand);
                                inner && (possible_integer_bits(inner->operand) & ~integer_mask(integer_width(operand->type))) == 0)
                            {
                                replacement = resize(inner->operand, inst->type);
                                fresh = replacement != inner->operand;
                            }
                            break;
                        }
                        case IrNodeKind::Trunc: {
                            auto* operand = static_cast<IrTruncInst*>(inst)->operand;
                            if (auto* inner = ir_cast<IrZextInst>(operand))
                            {
                                replacement = resize(inner->operand, inst->type);
                                fresh = replacement != inner->operand;
                            }
                            else if (auto* truncated = ir_cast<IrTruncInst>(operand))
                            {
                                replacement = ctx.ctx->trunc(inst->type, truncated->operand);
                                fresh = true;
                            }
                            else if (auto* masked = ir_cast<IrAndInst>(operand))
                            {
                                auto mask = integer_literal(masked->rhs);
                                auto* source = masked->lhs;
                                if (!mask)
                                {
                                    mask = integer_literal(masked->lhs);
                                    source = masked->rhs;
                                }
                                if (mask && (*mask & integer_mask(width)) == integer_mask(width))
                                {
                                    replacement = ctx.ctx->trunc(inst->type, source);
                                    fresh = true;
                                }
                            }
                            break;
                        }
                        default:
                            break;
                    }
                    if (!replacement && inst->kind >= IrNodeKind::CmpEq && inst->kind <= IrNodeKind::CmpUGe)
                    {
                        IrValue* lhs{};
                        IrValue* rhs{};
                        switch (inst->kind)
                        {
#define SELF_COMPARE(kind)                                                                                                                                     \
    case IrNodeKind::kind:                                                                                                                                     \
        lhs = static_cast<Ir##kind##Inst*>(inst)->lhs;                                                                                                         \
        rhs = static_cast<Ir##kind##Inst*>(inst)->rhs;                                                                                                         \
        break
                            SELF_COMPARE(CmpEq);
                            SELF_COMPARE(CmpNe);
                            SELF_COMPARE(CmpLt);
                            SELF_COMPARE(CmpLe);
                            SELF_COMPARE(CmpGt);
                            SELF_COMPARE(CmpGe);
                            SELF_COMPARE(CmpULt);
                            SELF_COMPARE(CmpULe);
                            SELF_COMPARE(CmpUGt);
                            SELF_COMPARE(CmpUGe);
#undef SELF_COMPARE
                            default:
                                break;
                        }
                        if (lhs && lhs == rhs && integer_width(lhs->type))
                            replacement =
                                ctx.ctx->bool_const(inst->kind == IrNodeKind::CmpEq || inst->kind == IrNodeKind::CmpLe || inst->kind == IrNodeKind::CmpGe ||
                                                    inst->kind == IrNodeKind::CmpULe || inst->kind == IrNodeKind::CmpUGe);
                    }
                }
                if (rewriter.replace(inst, replacement))
                {
                    if (fresh)
                        *it++ = replacement;
                    else
                        it = block->instructions.erase(it);
                    changed = true;
                }
                else
                    ++it;
            }
        if (changed)
            ctx.invalidate_use_def();
        return changed;
    }

    static bool sccp_impl(FunctionPassContext& ctx)
    {
        if (!ctx.func->entry_block)
            return false;
        auto const& uses = ctx.get_use_def();
        IntegerRewriter rewriter(ctx);
        std::unordered_map<IrValue const*, ConstantState> states;
        std::unordered_map<IrValue const*, IrBasicBlock*> homes;
        std::unordered_map<IrValue const*, std::vector<IrBasicBlock*>> conditions;
        std::unordered_map<IrBasicBlock*, std::unordered_set<IrBasicBlock*>> edges;
        std::unordered_set<IrBasicBlock*> executable;
        std::vector<std::pair<IrBasicBlock*, IrValue*>> work;
        std::unordered_set<IrNode*> queued;
        auto enqueue = [&](IrBasicBlock* block, IrValue* value) {
            IrNode* key = value ? static_cast<IrNode*>(value) : block->terminator;
            if (key && queued.insert(key).second)
                work.emplace_back(block, value);
        };
        for (auto* block : ctx.func->blocks)
        {
            for (auto* parameter : block->params)
            {
                homes[parameter] = block;
                states[parameter] = {2};
            }
            for (auto* inst : block->instructions)
                homes[inst] = block;
            if (auto* branch = ir_cast<IrBrCondInst>(block->terminator))
                conditions[branch->condition].push_back(block);
            if (auto* branch = ir_cast<IrSwitchInst>(block->terminator))
                conditions[branch->value].push_back(block);
        }
        auto lookup = [&](IrValue* value) -> ConstantState {
            if (auto constant = integer_literal(value))
                return {1, *constant};
            if (auto found = states.find(value); found != states.end())
                return found->second;
            return homes.contains(value) ? ConstantState{} : ConstantState{2};
        };
        auto update = [&](IrValue* value, ConstantState next) {
            auto& current = states[value];
            if (current.level == 2 || next.level == 0)
                return;
            if (current.level == 1 && next.level == 1 && current.value != next.value)
                next = {2};
            if (next == current)
                return;
            current = next;
            for (auto* user : uses.uses_of(value))
                if (auto found = homes.find(user); found != homes.end() && executable.contains(found->second))
                    enqueue(found->second, user);
            if (auto found = conditions.find(value); found != conditions.end())
                for (auto* block : found->second)
                    if (executable.contains(block))
                        enqueue(block, nullptr);
        };
        auto mark_edge = [&](IrBasicBlock* from, IrBasicBlock* to) {
            if (!to || !edges[to].insert(from).second)
                return;
            if (executable.insert(to).second)
            {
                for (auto* inst : to->instructions)
                    enqueue(to, inst);
                enqueue(to, nullptr);
            }
            else
                for (auto* inst : to->instructions)
                    if (inst->kind == IrNodeKind::Phi)
                        enqueue(to, inst);
        };
        auto switch_target = [&](IrSwitchInst* branch, std::uint64_t value) {
            auto* integer = ir_type_cast<IrIntType>(branch->value->type);
            for (auto const& item : branch->cases)
            {
                bool match = integer && integer->is_signed
                                 ? signed_integer(value, integer->bits) >= item.start && signed_integer(value, integer->bits) <= item.end
                                 : value >= static_cast<std::uint64_t>(item.start) && value <= static_cast<std::uint64_t>(item.end);
                if (match)
                    return item.target;
            }
            return branch->default_target;
        };
        mark_edge(nullptr, ctx.func->entry_block);
        bool force_unknown = true;
        while (force_unknown)
        {
            for (std::size_t cursor = 0; cursor < work.size(); ++cursor)
            {
                auto [block, inst] = work[cursor];
                queued.erase(inst ? static_cast<IrNode*>(inst) : block->terminator);
                if (!inst)
                {
                    if (auto* branch = ir_cast<IrBrInst>(block->terminator))
                        mark_edge(block, branch->target);
                    else if (auto* conditional = ir_cast<IrBrCondInst>(block->terminator))
                    {
                        auto condition = lookup(conditional->condition);
                        if (condition.level == 1)
                            mark_edge(block, condition.value ? conditional->true_target : conditional->false_target);
                        else if (condition.level == 2)
                        {
                            mark_edge(block, conditional->true_target);
                            mark_edge(block, conditional->false_target);
                        }
                    }
                    else if (auto* selection = ir_cast<IrSwitchInst>(block->terminator))
                    {
                        auto condition = lookup(selection->value);
                        if (condition.level == 1)
                            mark_edge(block, switch_target(selection, condition.value));
                        else if (condition.level == 2)
                        {
                            mark_edge(block, selection->default_target);
                            for (auto const& item : selection->cases)
                                mark_edge(block, item.target);
                        }
                    }
                    continue;
                }
                if (auto* phi = ir_cast<IrPhiInst>(inst))
                {
                    ConstantState next;
                    for (auto const& incoming : phi->incoming)
                        if (edges[block].contains(incoming.block))
                        {
                            auto state = lookup(incoming.value);
                            if (next.level == 0)
                                next = state;
                            else if (state.level == 2 || (state.level == 1 && next.level == 1 && state.value != next.value))
                                next = {2};
                        }
                    update(phi, integer_width(phi->type) ? next : ConstantState{2});
                }
                else
                    update(inst, evaluate_integer(inst, lookup));
            }
            work.clear();
            force_unknown = false;
            for (auto* block : ctx.func->blocks)
                if (executable.contains(block))
                    for (auto* inst : block->instructions)
                        if (states[inst].level == 0)
                        {
                            update(inst, {2});
                            force_unknown = true;
                        }
        }
        bool changed = false;
        bool cfg_changed = false;
        for (auto* block : ctx.func->blocks)
            if (executable.contains(block))
            {
                for (auto* inst : block->instructions)
                {
                    auto state = lookup(inst);
                    if (state.level == 1 && integer_width(inst->type))
                        changed |= rewriter.replace(inst, integer_constant(*ctx.ctx, inst->type, state.value));
                }
                IrBasicBlock* target{};
                if (auto* branch = ir_cast<IrBrCondInst>(block->terminator))
                {
                    auto condition = lookup(branch->condition);
                    if (condition.level == 1)
                        target = condition.value ? branch->true_target : branch->false_target;
                }
                if (auto* branch = ir_cast<IrSwitchInst>(block->terminator))
                {
                    auto condition = lookup(branch->value);
                    if (condition.level == 1)
                        target = switch_target(branch, condition.value);
                }
                if (target)
                {
                    block->terminator = ctx.ctx->br(target);
                    cfg_changed = true;
                }
            }
        if (cfg_changed)
        {
            std::ignore = remove_unreachable(ctx);
            auto predecessors = analysis::build_pred_map(*ctx.func);
            for (auto* block : ctx.func->blocks)
                for (auto* inst : block->instructions)
                    if (auto* phi = ir_cast<IrPhiInst>(inst))
                        std::erase_if(phi->incoming, [&](auto const& incoming) {
                            auto const& pred = predecessors[block];
                            return std::ranges::find(pred, incoming.block) == pred.end();
                        });
            ctx.invalidate_cfg();
        }
        else if (changed)
            ctx.invalidate_use_def();
        return changed || cfg_changed;
    }

} // namespace dcc::ir::pass

namespace dcc::ir::pass
{
    namespace inline_detail
    {
        [[nodiscard]] static std::size_t inline_count_instructions(IrFunction const& func)
        {
            std::size_t n = 0;
            for (auto* bb : func.blocks)
                n += bb->instructions.size() + (bb->terminator != nullptr);
            return n;
        }

        [[nodiscard]] static bool inline_has_attr(IrFunction const& func, IrFuncAttr attr)
        {
            for (auto const& a : func.attrs)
                if (a.kind == attr)
                    return true;

            return false;
        }

        [[nodiscard]] static IrFunction const* inline_resolve_callee(IrValue const* callee)
        {
            if (!callee)
                return nullptr;

            if (auto* f = ir_cast<IrFunction const>(callee))
                return f;

            if (auto* gr = ir_cast<IrGlobalRef const>(callee))
                return gr->function;

            return nullptr;
        }

        [[nodiscard]] static bool inline_pure_flow_kind(IrNodeKind kind)
        {
            switch (kind)
            {
                case IrNodeKind::Add:
                case IrNodeKind::Sub:
                case IrNodeKind::Mul:
                case IrNodeKind::UDiv:
                case IrNodeKind::SDiv:
                case IrNodeKind::URem:
                case IrNodeKind::SRem:
                case IrNodeKind::FDiv:
                case IrNodeKind::FRem:
                case IrNodeKind::And:
                case IrNodeKind::Or:
                case IrNodeKind::Xor:
                case IrNodeKind::Shl:
                case IrNodeKind::LShr:
                case IrNodeKind::AShr:
                case IrNodeKind::Neg:
                case IrNodeKind::Not:
                case IrNodeKind::CmpEq:
                case IrNodeKind::CmpNe:
                case IrNodeKind::CmpLt:
                case IrNodeKind::CmpLe:
                case IrNodeKind::CmpGt:
                case IrNodeKind::CmpGe:
                case IrNodeKind::CmpOLt:
                case IrNodeKind::CmpOLe:
                case IrNodeKind::CmpOGt:
                case IrNodeKind::CmpOGe:
                case IrNodeKind::CmpULt:
                case IrNodeKind::CmpULe:
                case IrNodeKind::CmpUGt:
                case IrNodeKind::CmpUGe:
                case IrNodeKind::Zext:
                case IrNodeKind::Sext:
                case IrNodeKind::Trunc:
                case IrNodeKind::FpExt:
                case IrNodeKind::FpTrunc:
                case IrNodeKind::FpToI:
                case IrNodeKind::IToFp:
                case IrNodeKind::PtrToI:
                case IrNodeKind::IToPtr:
                case IrNodeKind::Bitcast:
            case IrNodeKind::ReadSegment:
            case IrNodeKind::MakePointer:
            case IrNodeKind::PointerOffset:
            case IrNodeKind::PointerSegment:
            case IrNodeKind::Extract:
                case IrNodeKind::Insert:
                case IrNodeKind::Aggregate:
                case IrNodeKind::Gep:
                case IrNodeKind::Phi:
                    return true;
                default:
                    return false;
            }
        }

        struct InlineCallGraph
        {
            std::unordered_map<IrFunction const*, std::vector<IrFunction const*>> edges;
        };

        [[nodiscard]] static InlineCallGraph inline_build_call_graph(IrModule const& mod)
        {
            InlineCallGraph cg;
            for (auto* f : mod.functions)
            {
                if (!f)
                    continue;

                auto& out = cg.edges[f];
                for (auto* bb : f->blocks)
                {
                    if (!bb)
                        continue;
                    for (auto* inst : bb->instructions)
                    {
                        if (!inst)
                            continue;

                        IrValue const* callee = nullptr;
                        if (auto* c = ir_cast<IrCallInst const>(inst))
                            callee = c->callee;

                        else if (auto* ct = ir_cast<IrCallTailInst const>(inst))
                            callee = ct->callee;

                        if (!callee)
                            continue;

                        if (auto* target = inline_resolve_callee(callee))
                            out.push_back(target);
                    }
                }
            }
            return cg;
        }

        struct InlineScc
        {
            std::unordered_map<IrFunction const*, std::size_t> comp;
            std::vector<std::size_t> comp_size;
        };

        [[nodiscard]] static InlineScc inline_compute_scc(IrModule const& mod, InlineCallGraph const& cg)
        {
            InlineScc scc;
            std::unordered_map<IrFunction const*, std::size_t> index;
            std::unordered_map<IrFunction const*, std::size_t> low;
            std::unordered_map<IrFunction const*, bool> on_stack;
            std::vector<IrFunction const*> stack;
            std::size_t next_index = 0;

            struct Frame
            {
                IrFunction const* func;
                std::size_t child;
            };

            for (auto* root : mod.functions)
            {
                if (!root || index.contains(root))
                    continue;

                std::vector<Frame> work;
                work.push_back({root, 0});
                index[root] = next_index;
                low[root] = next_index;
                ++next_index;
                stack.push_back(root);
                on_stack[root] = true;

                while (!work.empty())
                {
                    auto& top = work.back();
                    auto eit = cg.edges.find(top.func);
                    std::vector<IrFunction const*> const* succs = eit != cg.edges.end() ? &eit->second : nullptr;

                    std::size_t nsucc = succs ? succs->size() : 0;
                    if (top.child < nsucc)
                    {
                        auto* w = (*succs)[top.child];
                        ++top.child;
                        if (!w || !cg.edges.contains(w))
                            continue;

                        if (!index.contains(w))
                        {
                            index[w] = next_index;
                            low[w] = next_index;
                            ++next_index;
                            stack.push_back(w);
                            on_stack[w] = true;
                            work.push_back({w, 0});
                        }
                        else if (on_stack[w])
                            low[top.func] = std::min(low[top.func], index[w]);

                        continue;
                    }

                    if (low[top.func] == index[top.func])
                    {
                        std::size_t id = scc.comp_size.size();
                        std::size_t size = 0;
                        while (true)
                        {
                            auto* w = stack.back();
                            stack.pop_back();
                            on_stack[w] = false;
                            scc.comp[w] = id;
                            ++size;
                            if (w == top.func)
                                break;
                        }

                        scc.comp_size.push_back(size);
                    }

                    work.pop_back();
                    if (!work.empty())
                    {
                        auto* parent = work.back().func;
                        low[parent] = std::min(low[parent], low[top.func]);
                    }
                }
            }
            return scc;
        }

        [[nodiscard]] static std::vector<IrFunction*> inline_bottom_up_order(IrModule& mod, InlineScc const& scc)
        {
            std::vector<std::size_t> indeg(scc.comp_size.size());
            std::vector<std::vector<std::size_t>> dag(scc.comp_size.size());
            for (auto* f : mod.functions)
            {
                if (!f)
                    continue;

                auto fit = scc.comp.find(f);
                if (fit == scc.comp.end())
                    continue;

                for (auto* bb : f->blocks)
                {
                    if (!bb)
                        continue;

                    for (auto* inst : bb->instructions)
                    {
                        if (!inst)
                            continue;

                        IrValue const* callee = nullptr;
                        if (auto* c = ir_cast<IrCallInst const>(inst))
                            callee = c->callee;

                        else if (auto* ct = ir_cast<IrCallTailInst const>(inst))
                            callee = ct->callee;

                        auto* target = inline_resolve_callee(callee);
                        if (!target)
                            continue;

                        auto tit = scc.comp.find(target);
                        if (tit == scc.comp.end() || tit->second == fit->second)
                            continue;

                        auto& vec = dag[fit->second];
                        if (std::find(vec.begin(), vec.end(), tit->second) == vec.end())
                        {
                            vec.push_back(tit->second);
                            ++indeg[tit->second];
                        }
                    }
                }
            }

            std::vector<std::size_t> stack;
            for (std::size_t id = 0; id < indeg.size(); ++id)
                if (indeg[id] == 0)
                    stack.push_back(id);

            std::vector<std::size_t> topo;
            while (!stack.empty())
            {
                auto id = stack.back();
                stack.pop_back();
                topo.push_back(id);
                for (auto dep : dag[id])
                    if (--indeg[dep] == 0)
                        stack.push_back(dep);
            }

            std::vector<std::size_t> rank(scc.comp_size.size());
            for (std::size_t i = 0; i < topo.size(); ++i)
                rank[topo[i]] = i;

            std::vector<IrFunction*> order;
            for (auto* f : mod.functions)
            {
                if (!f)
                    continue;

                order.push_back(f);
            }

            std::ranges::stable_sort(order, [&](IrFunction* a, IrFunction* b) {
                auto ait = scc.comp.find(a);
                auto bit = scc.comp.find(b);
                std::size_t ar = ait != scc.comp.end() ? rank[ait->second] : 0;
                std::size_t br = bit != scc.comp.end() ? rank[bit->second] : 0;
                return ar > br;
            });
            return order;
        }

        struct InlineCalleeGate
        {
            bool eligible = false;
            bool forced = false;
            bool has_branch = false;
            std::size_t size = 0;
        };

        [[nodiscard]] static InlineCalleeGate inline_classify_callee(IrFunction const* callee, InlineScc const& scc)
        {
            InlineCalleeGate gate;
            if (!callee || callee->blocks.empty() || !callee->entry_block)
                return gate;
            bool forced = inline_has_attr(*callee, IrFuncAttr::Inline);
            if (inline_has_attr(*callee, IrFuncAttr::NoInline))
                return gate;

            auto cit = scc.comp.find(callee);
            if (cit != scc.comp.end() && cit->second < scc.comp_size.size() && scc.comp_size[cit->second] > 1)
                return gate;

            std::size_t rets = 0;

            bool branches = false;
            for (auto* bb : callee->blocks)
            {
                if (!bb || !bb->terminator)
                    continue;

                if (bb->terminator->kind == IrNodeKind::Ret)
                    ++rets;

                if (bb->terminator->kind == IrNodeKind::BrCond || bb->terminator->kind == IrNodeKind::Switch)
                    branches = true;
            }

            for (auto* bb : callee->blocks)
            {
                if (!bb)
                    continue;

                for (auto* inst : bb->instructions)
                {
                    if (!inst)
                        continue;

                    if (inst->kind == IrNodeKind::CallTail)
                        return gate;

                    if (auto* a = ir_cast<IrAllocaInst const>(inst))
                        if (a->count != nullptr)
                            return gate;

                    if (inst->kind == IrNodeKind::Call)
                        if (inline_resolve_callee(static_cast<IrCallInst const*>(inst)->callee) == callee)
                            return gate;
                }
            }

            if (rets != 1)
                if (!forced || rets == 0)
                    return gate;

            if (!forced)
            {
                std::size_t n = inline_count_instructions(*callee);
                gate.size = n;
                if (n < 1 || n > 20)
                    return gate;

                if (!callee->func_type || !callee->func_type->return_type)
                    return gate;

                auto rk = callee->func_type->return_type->kind;
                if (rk == IrTypeKind::Aggregate || rk == IrTypeKind::Array)
                    return gate;
            }

            gate.eligible = true;
            gate.forced = forced;
            gate.has_branch = branches;
            return gate;
        }

        [[nodiscard]] static bool inline_backward_flows_to(IrValue const* v, IrValue const* target, std::vector<IrValue const*>& seen, std::size_t& steps)
        {
            if (!v || !target)
                return false;
            if (v == target)
                return true;
            if (++steps > 1024)
                return true;
            for (auto* s : seen)
                if (s == v)
                    return false;
            seen.push_back(v);
            if (!inline_pure_flow_kind(v->kind))
                return false;
            switch (v->kind)
            {
                case IrNodeKind::Phi: {
                    auto* phi = static_cast<IrPhiInst const*>(v);
                    for (auto const& inc : phi->incoming)
                        if (inline_backward_flows_to(inc.value, target, seen, steps))
                            return true;
                    return false;
                }
                case IrNodeKind::Extract: {
                    auto* e = static_cast<IrExtractInst const*>(v);
                    return inline_backward_flows_to(e->aggregate, target, seen, steps);
                }
                case IrNodeKind::Insert: {
                    auto* i = static_cast<IrInsertInst const*>(v);
                    return inline_backward_flows_to(i->aggregate, target, seen, steps) || inline_backward_flows_to(i->value, target, seen, steps);
                }
                case IrNodeKind::Aggregate: {
                    auto* a = static_cast<IrAggregateInst const*>(v);
                    for (auto* m : a->values)
                        if (inline_backward_flows_to(m, target, seen, steps))
                            return true;
                    return false;
                }
                case IrNodeKind::Gep: {
                    auto* g = static_cast<IrGepInst const*>(v);
                    if (inline_backward_flows_to(g->base, target, seen, steps))
                        return true;
                    for (auto const& idx : g->indices)
                        if (inline_backward_flows_to(idx.dynamic_index, target, seen, steps))
                            return true;
                    return false;
                }
                case IrNodeKind::Bitcast: {
                    auto* b = static_cast<IrBitcastInst const*>(v);
                    return inline_backward_flows_to(b->operand, target, seen, steps);
                }
                case IrNodeKind::PtrToI: {
                    auto* c = static_cast<IrPtrToIInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::IToPtr: {
                    auto* c = static_cast<IrIToPtrInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::MakePointer: {
                    auto* p = static_cast<IrMakePointerInst const*>(v);
                    return inline_backward_flows_to(p->offset, target, seen, steps) || inline_backward_flows_to(p->segment, target, seen, steps);
                }
                case IrNodeKind::PointerOffset:
                    return inline_backward_flows_to(static_cast<IrPointerOffsetInst const*>(v)->pointer, target, seen, steps);
                case IrNodeKind::PointerSegment:
                    return inline_backward_flows_to(static_cast<IrPointerSegmentInst const*>(v)->pointer, target, seen, steps);
                case IrNodeKind::Zext: {
                    auto* c = static_cast<IrZextInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::Sext: {
                    auto* c = static_cast<IrSextInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::Trunc: {
                    auto* c = static_cast<IrTruncInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::FpExt: {
                    auto* c = static_cast<IrFpExtInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::FpTrunc: {
                    auto* c = static_cast<IrFpTruncInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::FpToI: {
                    auto* c = static_cast<IrFpToIInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::IToFp: {
                    auto* c = static_cast<IrIToFpInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::Neg: {
                    auto* c = static_cast<IrNegInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                case IrNodeKind::Not: {
                    auto* c = static_cast<IrNotInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
                default:
                    break;
            }
            auto binop_operands = [&](IrValue const*& lhs, IrValue const*& rhs) {
                switch (v->kind)
                {
                    case IrNodeKind::Add: {
                        auto* c = static_cast<IrAddInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::Sub: {
                        auto* c = static_cast<IrSubInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::Mul: {
                        auto* c = static_cast<IrMulInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::UDiv: {
                        auto* c = static_cast<IrUDivInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::SDiv: {
                        auto* c = static_cast<IrSDivInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::URem: {
                        auto* c = static_cast<IrURemInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::SRem: {
                        auto* c = static_cast<IrSRemInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::FDiv: {
                        auto* c = static_cast<IrFDivInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::FRem: {
                        auto* c = static_cast<IrFRemInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::And: {
                        auto* c = static_cast<IrAndInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::Or: {
                        auto* c = static_cast<IrOrInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::Xor: {
                        auto* c = static_cast<IrXorInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::Shl: {
                        auto* c = static_cast<IrShlInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::LShr: {
                        auto* c = static_cast<IrLShrInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::AShr: {
                        auto* c = static_cast<IrAShrInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpEq: {
                        auto* c = static_cast<IrCmpEqInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpNe: {
                        auto* c = static_cast<IrCmpNeInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpLt: {
                        auto* c = static_cast<IrCmpLtInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpLe: {
                        auto* c = static_cast<IrCmpLeInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpGt: {
                        auto* c = static_cast<IrCmpGtInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpGe: {
                        auto* c = static_cast<IrCmpGeInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpOLt: {
                        auto* c = static_cast<IrCmpOLtInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpOLe: {
                        auto* c = static_cast<IrCmpOLeInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpOGt: {
                        auto* c = static_cast<IrCmpOGtInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpOGe: {
                        auto* c = static_cast<IrCmpOGeInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpULt: {
                        auto* c = static_cast<IrCmpULtInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpULe: {
                        auto* c = static_cast<IrCmpULeInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpUGt: {
                        auto* c = static_cast<IrCmpUGtInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    case IrNodeKind::CmpUGe: {
                        auto* c = static_cast<IrCmpUGeInst const*>(v);
                        lhs = c->lhs;
                        rhs = c->rhs;
                        return true;
                    }
                    default:
                        return false;
                }
            };
            IrValue const* lhs = nullptr;
            IrValue const* rhs = nullptr;
            if (!binop_operands(lhs, rhs))
                return true;
            return inline_backward_flows_to(lhs, target, seen, steps) || inline_backward_flows_to(rhs, target, seen, steps);
        }

        [[nodiscard]] static bool inline_result_reaches_branch(IrFunction& caller, IrValue const* result)
        {
            if (!result)
                return false;
            std::vector<IrValue const*> seen;
            for (auto* bb : caller.blocks)
            {
                if (!bb || !bb->terminator)
                    continue;
                IrValue const* cond = nullptr;
                if (bb->terminator->kind == IrNodeKind::BrCond)
                    cond = static_cast<IrBrCondInst const*>(bb->terminator)->condition;
                else if (bb->terminator->kind == IrNodeKind::Switch)
                    cond = static_cast<IrSwitchInst const*>(bb->terminator)->value;
                if (!cond)
                    continue;
                seen.clear();
                std::size_t steps = 0;
                if (inline_backward_flows_to(cond, result, seen, steps))
                    return true;
            }
            return false;
        }

        struct InlineFoldCtx
        {
            IrFunction* caller = nullptr;
            IrFunction const* callee = nullptr;
            std::vector<std::pair<IrValue const*, IrValue*>> const* argmap = nullptr;
        };

        [[nodiscard]] static IrValue const* inline_subst_param(IrValue const* v, InlineFoldCtx const& fc)
        {
            if (!v || !fc.argmap)
                return v;

            if (v->kind != IrNodeKind::Local)
                return v;

            for (auto const& [p, a] : *fc.argmap)
                if (p == v)
                    return a;
            return v;
        }

        [[nodiscard]] static bool inline_foldable_value(IrValue const* v, InlineFoldCtx const& fc, int depth, std::size_t& budget);
        [[nodiscard]] static bool inline_field_const(IrValue const* v, std::uint32_t field, InlineFoldCtx const& fc, int depth, std::size_t& budget);
        [[nodiscard]] static bool inline_alloca_field_const(IrAllocaInst const* a, std::uint32_t field, IrFunction const& owner, InlineFoldCtx const& fc,
                                                            int depth, std::size_t& budget);
        [[nodiscard]] static bool inline_alloca_scalar_const(IrAllocaInst const* a, IrFunction const& owner, InlineFoldCtx const& fc, int depth,
                                                             std::size_t& budget);

        [[nodiscard]] static bool inline_owner_contains_alloca(IrFunction const& func, IrAllocaInst const* a)
        {
            for (auto* bb : func.blocks)
            {
                if (!bb)
                    continue;
                for (auto* inst : bb->instructions)
                    if (inst == a)
                        return true;
            }
            return false;
        }

        [[nodiscard]] static IrFunction const* inline_alloca_owner(IrAllocaInst const* a, InlineFoldCtx const& fc)
        {
            if (fc.caller && inline_owner_contains_alloca(*fc.caller, a))
                return fc.caller;
            if (fc.callee && inline_owner_contains_alloca(*fc.callee, a))
                return fc.callee;
            return nullptr;
        }

        [[nodiscard]] static bool inline_foldable_value(IrValue const* v, InlineFoldCtx const& fc, int depth, std::size_t& budget)
        {
            if (!v || budget == 0)
                return false;

            if (--budget == 0)
                return false;

            switch (v->kind)
            {
                case IrNodeKind::IntConstant:
                case IrNodeKind::FloatConstant:
                case IrNodeKind::BoolConstant:
                case IrNodeKind::NullConstant:
                case IrNodeKind::PointerConstant:
                case IrNodeKind::StringConstant:
                    return true;
                default:
                    break;
            }

            if (depth <= 0)
                return false;

            if (v->kind == IrNodeKind::Local)
            {
                auto* m = inline_subst_param(v, fc);
                return m != v && inline_foldable_value(m, fc, depth - 1, budget);
            }

            if (v->kind == IrNodeKind::Extract)
            {
                auto* e = static_cast<IrExtractInst const*>(v);
                return inline_field_const(e->aggregate, e->field_index, fc, depth - 1, budget);
            }

            if (v->kind == IrNodeKind::Load)
            {
                auto* p = inline_subst_param(static_cast<IrLoadInst const*>(v)->pointer, fc);
                auto* a = ir_cast<IrAllocaInst const>(p);
                if (!a)
                    return false;

                if (auto const* owner = inline_alloca_owner(a, fc))
                    return inline_alloca_scalar_const(a, *owner, fc, depth - 1, budget);

                return false;
            }

            if (v->kind == IrNodeKind::Aggregate)
            {
                auto* agg = static_cast<IrAggregateInst const*>(v);
                for (auto* e : agg->values)
                    if (!inline_foldable_value(e, fc, depth - 1, budget))
                        return false;

                return true;
            }

            if (v->kind == IrNodeKind::Insert)
            {
                auto* i = static_cast<IrInsertInst const*>(v);
                return inline_foldable_value(i->aggregate, fc, depth - 1, budget) && inline_foldable_value(i->value, fc, depth - 1, budget);
            }

            if (v->kind == IrNodeKind::Phi)
            {
                auto* phi = static_cast<IrPhiInst const*>(v);
                if (phi->incoming.empty())
                    return false;

                for (auto const& inc : phi->incoming)
                    if (!inline_foldable_value(inc.value, fc, depth - 1, budget))
                        return false;

                return true;
            }

            if (inline_pure_flow_kind(v->kind) && v->kind != IrNodeKind::Gep)
            {
                std::vector<IrValue const*> ops;
                auto push2 = [&](IrValue const* l, IrValue const* r) {
                    ops.push_back(l);
                    ops.push_back(r);
                };

                switch (v->kind)
                {
                    case IrNodeKind::Add:
                        push2(static_cast<IrAddInst const*>(v)->lhs, static_cast<IrAddInst const*>(v)->rhs);
                        break;
                    case IrNodeKind::Sub:
                        push2(static_cast<IrSubInst const*>(v)->lhs, static_cast<IrSubInst const*>(v)->rhs);
                        break;
                    case IrNodeKind::Mul:
                        push2(static_cast<IrMulInst const*>(v)->lhs, static_cast<IrMulInst const*>(v)->rhs);
                        break;
                    case IrNodeKind::And:
                        push2(static_cast<IrAndInst const*>(v)->lhs, static_cast<IrAndInst const*>(v)->rhs);
                        break;
                    case IrNodeKind::Or:
                        push2(static_cast<IrOrInst const*>(v)->lhs, static_cast<IrOrInst const*>(v)->rhs);
                        break;
                    case IrNodeKind::Xor:
                        push2(static_cast<IrXorInst const*>(v)->lhs, static_cast<IrXorInst const*>(v)->rhs);
                        break;
                    default:
                        return false;
                }

                for (auto* o : ops)
                    if (!inline_foldable_value(o, fc, depth - 1, budget))
                        return false;

                return true;
            }
            return false;
        }

        [[nodiscard]] static bool inline_field_const(IrValue const* v, std::uint32_t field, InlineFoldCtx const& fc, int depth, std::size_t& budget)
        {
            if (!v || budget == 0 || depth <= 0)
                return false;

            if (--budget == 0)
                return false;

            if (v->kind == IrNodeKind::Local)
            {
                auto* m = inline_subst_param(v, fc);
                return m != v && inline_field_const(m, field, fc, depth - 1, budget);
            }

            if (v->kind == IrNodeKind::Aggregate)
            {
                auto* agg = static_cast<IrAggregateInst const*>(v);
                if (field >= agg->values.size())
                    return false;

                return inline_foldable_value(agg->values[field], fc, depth - 1, budget);
            }

            if (v->kind == IrNodeKind::Insert)
            {
                auto* i = static_cast<IrInsertInst const*>(v);
                if (i->field_index == field)
                    return inline_foldable_value(i->value, fc, depth - 1, budget);

                return inline_field_const(i->aggregate, field, fc, depth - 1, budget);
            }

            if (v->kind == IrNodeKind::Load)
            {
                auto* p = inline_subst_param(static_cast<IrLoadInst const*>(v)->pointer, fc);
                auto* a = ir_cast<IrAllocaInst const>(p);
                if (!a)
                    return false;

                if (auto const* owner = inline_alloca_owner(a, fc))
                    return inline_alloca_field_const(a, field, *owner, fc, depth - 1, budget);

                return false;
            }

            return false;
        }

        [[nodiscard]] static bool inline_alloca_stores_field_const(IrAllocaInst const* a, IrFunction const& owner, std::uint32_t field, bool scalar,
                                                                   InlineFoldCtx const& fc, int depth, std::size_t& budget)
        {
            struct InlineDerivedAddr
            {
                IrValue const* value;
                std::uint32_t field;
            };
            std::uint32_t const kWholeAddr = 0xFFFFFFFEu;
            std::uint32_t const kUnknownAddr = 0xFFFFFFFFu;
            std::vector<InlineDerivedAddr> derived;
            derived.push_back({a, kWholeAddr});
            auto derived_field = [&](IrValue const* op) {
                for (auto const& d : derived)
                    if (d.value == op)
                        return d.field;
                return kUnknownAddr + 1u;
            };
            for (int pass = 0; pass < 8; ++pass)
            {
                bool grew = false;
                for (auto* bb : owner.blocks)
                {
                    if (!bb)
                        continue;

                    for (auto* inst : bb->instructions)
                    {
                        if (!inst || inst == a)
                            continue;

                        if (inst->kind == IrNodeKind::Bitcast || inst->kind == IrNodeKind::Gep)
                        {
                            std::uint32_t base_field = kUnknownAddr + 1u;
                            if (inst->kind == IrNodeKind::Bitcast)
                                base_field = derived_field(static_cast<IrBitcastInst const*>(inst)->operand);
                            else
                            {
                                auto* g = static_cast<IrGepInst const*>(inst);
                                std::uint32_t bf = derived_field(g->base);
                                if (bf != kUnknownAddr + 1u && g->indices.size() == 1 && g->indices[0].kind == IrGepInst::IndexKind::Field)
                                    base_field = (bf == kWholeAddr) ? g->indices[0].field_index : kUnknownAddr;
                                else if (bf != kUnknownAddr + 1u)
                                    base_field = kUnknownAddr;
                            }
                            if (base_field != kUnknownAddr + 1u && derived_field(inst) == kUnknownAddr + 1u)
                            {
                                derived.push_back({inst, base_field});
                                grew = true;
                            }

                            continue;
                        }
                        if (inst->kind == IrNodeKind::LoadVolatile || inst->kind == IrNodeKind::StoreVolatile || inst->kind == IrNodeKind::AtomicLoad ||
                            inst->kind == IrNodeKind::AtomicStore || inst->kind == IrNodeKind::AtomicRmw || inst->kind == IrNodeKind::AtomicCmpXchg ||
                            inst->kind == IrNodeKind::PtrToI || inst->kind == IrNodeKind::InlineAsm)
                        {
                            auto touches = [&](IrValue const* op) {
                                std::uint32_t f = derived_field(op);
                                return f == kWholeAddr || f == kUnknownAddr || f == field;
                            };

                            bool hit = false;
                            if (auto* lv = ir_cast<IrLoadVolatileInst const>(inst))
                                hit = touches(lv->pointer);
                            else if (auto* sv = ir_cast<IrStoreVolatileInst const>(inst))
                                hit = touches(sv->pointer) || touches(sv->value);
                            else if (auto* al = ir_cast<IrAtomicLoadInst const>(inst))
                                hit = touches(al->pointer);
                            else if (auto* as = ir_cast<IrAtomicStoreInst const>(inst))
                                hit = touches(as->pointer) || touches(as->value);
                            else if (auto* r = ir_cast<IrAtomicRmwInst const>(inst))
                                hit = touches(r->pointer);
                            else if (auto* cx = ir_cast<IrAtomicCmpXchgInst const>(inst))
                                hit = touches(cx->pointer);
                            else if (auto* c = ir_cast<IrPtrToIInst const>(inst))
                                hit = touches(c->operand);
                            else if (auto* ia = ir_cast<IrInlineAsmInst const>(inst))
                            {
                                for (auto const& op : ia->operands)
                                    hit = hit || touches(op.value);
                            }

                            if (hit)
                                return false;
                        }
                    }
                }

                if (!grew)
                    break;
            }

            std::size_t stores = 0;
            for (auto* bb : owner.blocks)
            {
                if (!bb)
                    continue;

                for (auto* inst : bb->instructions)
                {
                    auto* s = ir_cast<IrStoreInst const>(inst);
                    if (!s)
                        continue;

                    std::uint32_t pf = derived_field(s->pointer);
                    if (pf == kUnknownAddr + 1u)
                        continue;
                    if (pf == kUnknownAddr)
                        return false;
                    if (pf != kWholeAddr && pf != field)
                        continue;

                    if (++stores > 128)
                        return false;

                    bool ok = false;
                    if (scalar || pf != kWholeAddr)
                        ok = inline_foldable_value(s->value, fc, depth - 1, budget);
                    else
                        ok = inline_field_const(s->value, field, fc, depth - 1, budget);
                    if (!ok)
                        return false;
                }
            }
            return stores > 0;
        }

        [[nodiscard]] static bool inline_alloca_field_const(IrAllocaInst const* a, std::uint32_t field, IrFunction const& owner, InlineFoldCtx const& fc,
                                                            int depth, std::size_t& budget)
        {
            return inline_alloca_stores_field_const(a, owner, field, false, fc, depth, budget);
        }

        [[nodiscard]] static bool inline_alloca_scalar_const(IrAllocaInst const* a, IrFunction const& owner, InlineFoldCtx const& fc, int depth,
                                                             std::size_t& budget)
        {
            return inline_alloca_stores_field_const(a, owner, 0, true, fc, depth, budget);
        }

        [[nodiscard]] static bool inline_cond_foldable(IrValue const* cond, InlineFoldCtx const& fc, int depth, std::size_t& budget)
        {
            if (!cond || budget == 0)
                return false;

            switch (cond->kind)
            {
                case IrNodeKind::IntConstant:
                case IrNodeKind::BoolConstant:
                    return true;
                default:
                    break;
            }

            if (depth <= 0)
                return false;

            if (cond->kind == IrNodeKind::Local)
            {
                auto* m = inline_subst_param(cond, fc);
                return m != cond && inline_foldable_value(m, fc, depth - 1, budget);
            }

            if (cond->kind == IrNodeKind::Extract)
            {
                auto* e = static_cast<IrExtractInst const*>(cond);
                return inline_field_const(e->aggregate, e->field_index, fc, depth - 1, budget);
            }

            if (cond->kind == IrNodeKind::Load)
            {
                auto* p = inline_subst_param(static_cast<IrLoadInst const*>(cond)->pointer, fc);
                auto* a = ir_cast<IrAllocaInst const>(p);
                if (!a)
                    return false;

                if (auto const* owner = inline_alloca_owner(a, fc))
                    return inline_alloca_scalar_const(a, *owner, fc, depth - 1, budget);

                return false;
            }

            switch (cond->kind)
            {
                case IrNodeKind::CmpEq:
                case IrNodeKind::CmpNe:
                case IrNodeKind::CmpLt:
                case IrNodeKind::CmpLe:
                case IrNodeKind::CmpGt:
                case IrNodeKind::CmpGe:
                case IrNodeKind::CmpOLt:
                case IrNodeKind::CmpOLe:
                case IrNodeKind::CmpOGt:
                case IrNodeKind::CmpOGe:
                case IrNodeKind::CmpULt:
                case IrNodeKind::CmpULe:
                case IrNodeKind::CmpUGt:
                case IrNodeKind::CmpUGe:
                    break;
                default:
                    return false;
            }

            auto fold_operands = [&](IrValue const* l, IrValue const* r) {
                return inline_foldable_value(l, fc, depth - 1, budget) && inline_foldable_value(r, fc, depth - 1, budget);
            };

            switch (cond->kind)
            {
                case IrNodeKind::CmpEq:
                    return fold_operands(static_cast<IrCmpEqInst const*>(cond)->lhs, static_cast<IrCmpEqInst const*>(cond)->rhs);
                case IrNodeKind::CmpNe:
                    return fold_operands(static_cast<IrCmpNeInst const*>(cond)->lhs, static_cast<IrCmpNeInst const*>(cond)->rhs);
                case IrNodeKind::CmpLt:
                    return fold_operands(static_cast<IrCmpLtInst const*>(cond)->lhs, static_cast<IrCmpLtInst const*>(cond)->rhs);
                case IrNodeKind::CmpLe:
                    return fold_operands(static_cast<IrCmpLeInst const*>(cond)->lhs, static_cast<IrCmpLeInst const*>(cond)->rhs);
                case IrNodeKind::CmpGt:
                    return fold_operands(static_cast<IrCmpGtInst const*>(cond)->lhs, static_cast<IrCmpGtInst const*>(cond)->rhs);
                case IrNodeKind::CmpGe:
                    return fold_operands(static_cast<IrCmpGeInst const*>(cond)->lhs, static_cast<IrCmpGeInst const*>(cond)->rhs);
                case IrNodeKind::CmpOLt:
                    return fold_operands(static_cast<IrCmpOLtInst const*>(cond)->lhs, static_cast<IrCmpOLtInst const*>(cond)->rhs);
                case IrNodeKind::CmpOLe:
                    return fold_operands(static_cast<IrCmpOLeInst const*>(cond)->lhs, static_cast<IrCmpOLeInst const*>(cond)->rhs);
                case IrNodeKind::CmpOGt:
                    return fold_operands(static_cast<IrCmpOGtInst const*>(cond)->lhs, static_cast<IrCmpOGtInst const*>(cond)->rhs);
                case IrNodeKind::CmpOGe:
                    return fold_operands(static_cast<IrCmpOGeInst const*>(cond)->lhs, static_cast<IrCmpOGeInst const*>(cond)->rhs);
                case IrNodeKind::CmpULt:
                    return fold_operands(static_cast<IrCmpULtInst const*>(cond)->lhs, static_cast<IrCmpULtInst const*>(cond)->rhs);
                case IrNodeKind::CmpULe:
                    return fold_operands(static_cast<IrCmpULeInst const*>(cond)->lhs, static_cast<IrCmpULeInst const*>(cond)->rhs);
                case IrNodeKind::CmpUGt:
                    return fold_operands(static_cast<IrCmpUGtInst const*>(cond)->lhs, static_cast<IrCmpUGtInst const*>(cond)->rhs);
                default:
                    return fold_operands(static_cast<IrCmpUGeInst const*>(cond)->lhs, static_cast<IrCmpUGeInst const*>(cond)->rhs);
            }
        }

        struct InlineSite
        {
            IrFunction* caller = nullptr;
            IrBasicBlock* block = nullptr;
            std::size_t index = 0;
            IrValue* inst = nullptr;
        };

        [[nodiscard]] static IrValue const* inline_site_callee_value(IrValue const* inst)
        {
            if (auto* c = ir_cast<IrCallInst const>(inst))
                return c->callee;

            if (auto* c = ir_cast<IrCallTailInst const>(inst))
                return c->callee;

            return nullptr;
        }

        [[nodiscard]] static std::pmr::vector<IrValue*> const& inline_site_args(IrValue const* inst)
        {
            if (auto* c = ir_cast<IrCallInst const>(inst))
                return c->args;

            return static_cast<IrCallTailInst const*>(inst)->args;
        }

        static void inline_site_set_noinline(IrValue* inst)
        {
            if (auto* c = ir_cast<IrCallInst>(inst))
                c->is_noinline = true;
            else if (auto* ct = ir_cast<IrCallTailInst>(inst))
                ct->is_noinline = true;
        }

        enum class InlineSiteVerdict
        {
            Inline,
            RefuseMark,
            RefuseQuiet,
        };

        [[nodiscard]] static InlineSiteVerdict inline_gate_site(InlineSite const& site, IrFunction const* callee, InlineCalleeGate const& gate,
                                                                std::vector<std::pair<IrValue const*, IrValue*>> const& argmap)
        {
            if (!gate.has_branch)
                return InlineSiteVerdict::Inline;

            if (!site.inst || !site.inst->type || site.inst->type->kind == IrTypeKind::Void)
                return InlineSiteVerdict::Inline;

            if (!inline_result_reaches_branch(*site.caller, site.inst))
                return InlineSiteVerdict::Inline;

            InlineFoldCtx fc;
            fc.caller = site.caller;
            fc.callee = callee;
            fc.argmap = &argmap;

            for (auto* bb : callee->blocks)
            {
                if (!bb || !bb->terminator)
                    continue;

                IrValue const* cond = nullptr;
                if (bb->terminator->kind == IrNodeKind::BrCond)
                    cond = static_cast<IrBrCondInst const*>(bb->terminator)->condition;

                else if (bb->terminator->kind == IrNodeKind::Switch)
                    cond = static_cast<IrSwitchInst const*>(bb->terminator)->value;

                if (!cond)
                    continue;

                std::size_t budget = 512;
                if (!inline_cond_foldable(cond, fc, 8, budget))
                {
                    inline_site_set_noinline(site.inst);
                    return InlineSiteVerdict::RefuseMark;
                }
            }

            return InlineSiteVerdict::Inline;
        }

        struct InlineClone
        {
            IrContext* ctx = nullptr;
            std::unordered_map<IrValue const*, IrValue*> values;
            std::unordered_map<IrBasicBlock const*, IrBasicBlock*> blocks;
            std::vector<IrBasicBlock*> new_blocks;
            std::vector<IrAllocaInst*> new_allocas;
            std::size_t nesting = 0;
            bool failed = false;
        };

        [[nodiscard]] static IrValue* inline_clone_value(IrValue const* v, InlineClone& ic);

        [[nodiscard]] static IrValue* inline_clone_operand(IrValue const* v, InlineClone& ic)
        {
            IrValue* out = inline_clone_value(v, ic);
            if (!out)
                ic.failed = true;

            return out;
        }

        [[nodiscard]] static IrValue* inline_clone_binop(IrValue const* v, InlineClone& ic)
        {
            auto clone2 = [&](auto* bi, auto factory) -> IrValue* {
                auto* l = inline_clone_operand(bi->lhs, ic);
                auto* r = inline_clone_operand(bi->rhs, ic);
                if (!l || !r)
                    return nullptr;

                return (ic.ctx->*factory)(bi->type, l, r);
            };

            switch (v->kind)
            {
                case IrNodeKind::Add: {
                    auto* bi = static_cast<IrAddInst const*>(v);
                    return clone2(bi, &IrContext::add);
                }
                case IrNodeKind::Sub: {
                    auto* bi = static_cast<IrSubInst const*>(v);
                    return clone2(bi, &IrContext::sub);
                }
                case IrNodeKind::Mul: {
                    auto* bi = static_cast<IrMulInst const*>(v);
                    return clone2(bi, &IrContext::mul);
                }
                case IrNodeKind::UDiv: {
                    auto* bi = static_cast<IrUDivInst const*>(v);
                    return clone2(bi, &IrContext::udiv);
                }
                case IrNodeKind::SDiv: {
                    auto* bi = static_cast<IrSDivInst const*>(v);
                    return clone2(bi, &IrContext::sdiv);
                }
                case IrNodeKind::URem: {
                    auto* bi = static_cast<IrURemInst const*>(v);
                    return clone2(bi, &IrContext::urem);
                }
                case IrNodeKind::SRem: {
                    auto* bi = static_cast<IrSRemInst const*>(v);
                    return clone2(bi, &IrContext::srem);
                }
                case IrNodeKind::FDiv: {
                    auto* bi = static_cast<IrFDivInst const*>(v);
                    return clone2(bi, &IrContext::fdiv);
                }
                case IrNodeKind::FRem: {
                    auto* bi = static_cast<IrFRemInst const*>(v);
                    return clone2(bi, &IrContext::frem);
                }
                case IrNodeKind::And: {
                    auto* bi = static_cast<IrAndInst const*>(v);
                    return clone2(bi, &IrContext::and_);
                }
                case IrNodeKind::Or: {
                    auto* bi = static_cast<IrOrInst const*>(v);
                    return clone2(bi, &IrContext::or_);
                }
                case IrNodeKind::Xor: {
                    auto* bi = static_cast<IrXorInst const*>(v);
                    return clone2(bi, &IrContext::xor_);
                }
                case IrNodeKind::Shl: {
                    auto* bi = static_cast<IrShlInst const*>(v);
                    return clone2(bi, &IrContext::shl);
                }
                case IrNodeKind::LShr: {
                    auto* bi = static_cast<IrLShrInst const*>(v);
                    return clone2(bi, &IrContext::lshr);
                }
                default: {
                    auto* bi = static_cast<IrAShrInst const*>(v);
                    return clone2(bi, &IrContext::ashr);
                }
            }
        }

        [[nodiscard]] static IrValue* inline_clone_cmp(IrValue const* v, InlineClone& ic)
        {
            auto clone2 = [&](IrValue const* l, IrValue const* r, auto factory) -> IrValue* {
                auto* cl = inline_clone_operand(l, ic);
                auto* cr = inline_clone_operand(r, ic);
                if (!cl || !cr)
                    return nullptr;

                return (ic.ctx->*factory)(cl, cr);
            };

            switch (v->kind)
            {
                case IrNodeKind::CmpEq: {
                    auto* ci = static_cast<IrCmpEqInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_eq);
                }
                case IrNodeKind::CmpNe: {
                    auto* ci = static_cast<IrCmpNeInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_ne);
                }
                case IrNodeKind::CmpLt: {
                    auto* ci = static_cast<IrCmpLtInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_lt);
                }
                case IrNodeKind::CmpLe: {
                    auto* ci = static_cast<IrCmpLeInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_le);
                }
                case IrNodeKind::CmpGt: {
                    auto* ci = static_cast<IrCmpGtInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_gt);
                }
                case IrNodeKind::CmpGe: {
                    auto* ci = static_cast<IrCmpGeInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_ge);
                }
                case IrNodeKind::CmpOLt: {
                    auto* ci = static_cast<IrCmpOLtInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_olt);
                }
                case IrNodeKind::CmpOLe: {
                    auto* ci = static_cast<IrCmpOLeInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_ole);
                }
                case IrNodeKind::CmpOGt: {
                    auto* ci = static_cast<IrCmpOGtInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_ogt);
                }
                case IrNodeKind::CmpOGe: {
                    auto* ci = static_cast<IrCmpOGeInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_oge);
                }
                case IrNodeKind::CmpULt: {
                    auto* ci = static_cast<IrCmpULtInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_ult);
                }
                case IrNodeKind::CmpULe: {
                    auto* ci = static_cast<IrCmpULeInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_ule);
                }
                case IrNodeKind::CmpUGt: {
                    auto* ci = static_cast<IrCmpUGtInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_ugt);
                }
                default: {
                    auto* ci = static_cast<IrCmpUGeInst const*>(v);
                    return clone2(ci->lhs, ci->rhs, &IrContext::cmp_uge);
                }
            }
        }

        [[nodiscard]] static IrValue* inline_clone_cast(IrValue const* v, InlineClone& ic)
        {
            auto clone1 = [&](IrValue const* o, IrType const* t, auto factory) -> IrValue* {
                auto* co = inline_clone_operand(o, ic);
                if (!co)
                    return nullptr;

                return (ic.ctx->*factory)(t, co);
            };

            switch (v->kind)
            {
                case IrNodeKind::Zext: {
                    auto* ci = static_cast<IrZextInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::zext);
                }
                case IrNodeKind::Sext: {
                    auto* ci = static_cast<IrSextInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::sext);
                }
                case IrNodeKind::Trunc: {
                    auto* ci = static_cast<IrTruncInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::trunc);
                }
                case IrNodeKind::FpExt: {
                    auto* ci = static_cast<IrFpExtInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::fpext);
                }
                case IrNodeKind::FpTrunc: {
                    auto* ci = static_cast<IrFpTruncInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::fptrunc);
                }
                case IrNodeKind::FpToI: {
                    auto* ci = static_cast<IrFpToIInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::fptoi);
                }
                case IrNodeKind::IToFp: {
                    auto* ci = static_cast<IrIToFpInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::itofp);
                }
                case IrNodeKind::PtrToI: {
                    auto* ci = static_cast<IrPtrToIInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::ptrtoi);
                }
                case IrNodeKind::IToPtr: {
                    auto* ci = static_cast<IrIToPtrInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::itoptr);
                }
                case IrNodeKind::Bitcast: {
                    auto* ci = static_cast<IrBitcastInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::bitcast);
                }
                default:
                    return nullptr;
            }
        }
        [[nodiscard]] static IrValue* inline_clone_value_impl(IrValue const* v, InlineClone& ic);

        [[nodiscard]] static IrValue* inline_clone_value(IrValue const* v, InlineClone& ic)
        {
            if (!v || !ic.ctx || ic.failed)
                return nullptr;

            if (++ic.nesting > 512)
            {
                ic.failed = true;
                return nullptr;
            }

            IrValue* out = inline_clone_value_impl(v, ic);
            --ic.nesting;
            return out;
        }

        [[nodiscard]] static IrValue* inline_clone_value_impl(IrValue const* v, InlineClone& ic)
        {
            auto it = ic.values.find(v);
            if (it != ic.values.end())
                return it->second;

            switch (v->kind)
            {
                case IrNodeKind::IntConstant:
                case IrNodeKind::FloatConstant:
                case IrNodeKind::BoolConstant:
                case IrNodeKind::NullConstant:
                case IrNodeKind::PointerConstant:
                case IrNodeKind::StringConstant:
                case IrNodeKind::GlobalRef:
                case IrNodeKind::Function:
                case IrNodeKind::Global:
                    ic.values[v] = const_cast<IrValue*>(v);
                    return const_cast<IrValue*>(v);
                case IrNodeKind::Local:
                    return nullptr;
                default:
                    break;
            }

            IrValue* result = nullptr;
            switch (v->kind)
            {
                case IrNodeKind::Add:
                case IrNodeKind::Sub:
                case IrNodeKind::Mul:
                case IrNodeKind::UDiv:
                case IrNodeKind::SDiv:
                case IrNodeKind::URem:
                case IrNodeKind::SRem:
                case IrNodeKind::FDiv:
                case IrNodeKind::FRem:
                case IrNodeKind::And:
                case IrNodeKind::Or:
                case IrNodeKind::Xor:
                case IrNodeKind::Shl:
                case IrNodeKind::LShr:
                case IrNodeKind::AShr:
                    result = inline_clone_binop(v, ic);
                    break;
                case IrNodeKind::Neg: {
                    auto* u = static_cast<IrNegInst const*>(v);
                    auto* op = inline_clone_operand(u->operand, ic);
                    if (op)
                        result = ic.ctx->neg(u->type, op);
                    break;
                }
                case IrNodeKind::Not: {
                    auto* u = static_cast<IrNotInst const*>(v);
                    auto* op = inline_clone_operand(u->operand, ic);
                    if (op)
                        result = ic.ctx->not_(u->type, op);
                    break;
                }
                case IrNodeKind::CmpEq:
                case IrNodeKind::CmpNe:
                case IrNodeKind::CmpLt:
                case IrNodeKind::CmpLe:
                case IrNodeKind::CmpGt:
                case IrNodeKind::CmpGe:
                case IrNodeKind::CmpOLt:
                case IrNodeKind::CmpOLe:
                case IrNodeKind::CmpOGt:
                case IrNodeKind::CmpOGe:
                case IrNodeKind::CmpULt:
                case IrNodeKind::CmpULe:
                case IrNodeKind::CmpUGt:
                case IrNodeKind::CmpUGe:
                    result = inline_clone_cmp(v, ic);
                    break;
                case IrNodeKind::Zext:
                case IrNodeKind::Sext:
                case IrNodeKind::Trunc:
                case IrNodeKind::FpExt:
                case IrNodeKind::FpTrunc:
                case IrNodeKind::FpToI:
                case IrNodeKind::IToFp:
                case IrNodeKind::PtrToI:
                case IrNodeKind::IToPtr:
                case IrNodeKind::Bitcast:
                    result = inline_clone_cast(v, ic);
                    break;
                case IrNodeKind::ReadSegment: {
                    result = ic.ctx->read_segment(static_cast<IrReadSegmentInst const*>(v)->segment);
                    break;
                }
                case IrNodeKind::MakePointer: {
                    auto* p = static_cast<IrMakePointerInst const*>(v);
                    result = ic.ctx->make_pointer(p->type, inline_clone_value(p->offset, ic), inline_clone_value(p->segment, ic));
                    break;
                }
                case IrNodeKind::PointerOffset: {
                    auto* p = static_cast<IrPointerOffsetInst const*>(v);
                    result = ic.ctx->pointer_offset(inline_clone_value(p->pointer, ic));
                    break;
                }
                case IrNodeKind::PointerSegment: {
                    auto* p = static_cast<IrPointerSegmentInst const*>(v);
                    result = ic.ctx->pointer_segment(inline_clone_value(p->pointer, ic));
                    break;
                }
                case IrNodeKind::Alloca: {
                    auto* a = static_cast<IrAllocaInst const*>(v);
                    if (a->count != nullptr)
                        break;
                    auto* fresh = ic.ctx->alloca(a->type, a->allocated_type, nullptr);
                    fresh->alignment = a->alignment;
                    fresh->name = a->name;
                    fresh->range = a->range;
                    ic.new_allocas.push_back(fresh);
                    result = fresh;
                    break;
                }
                case IrNodeKind::Load: {
                    auto* l = static_cast<IrLoadInst const*>(v);
                    auto* ptr = inline_clone_operand(l->pointer, ic);
                    if (ptr)
                    {
                        auto* fresh = ic.ctx->load(l->type, ptr);
                        fresh->alignment = l->alignment;
                        result = fresh;
                    }
                    break;
                }
                case IrNodeKind::LoadVolatile: {
                    auto* l = static_cast<IrLoadVolatileInst const*>(v);
                    auto* ptr = inline_clone_operand(l->pointer, ic);
                    if (ptr)
                    {
                        auto* fresh = ic.ctx->load_volatile(l->type, ptr);
                        fresh->alignment = l->alignment;
                        result = fresh;
                    }
                    break;
                }
                case IrNodeKind::Store: {
                    auto* s = static_cast<IrStoreInst const*>(v);
                    auto* val = inline_clone_operand(s->value, ic);
                    auto* ptr = inline_clone_operand(s->pointer, ic);
                    if (val && ptr)
                    {
                        auto* fresh = ic.ctx->store(val, ptr);
                        fresh->alignment = s->alignment;
                        result = fresh;
                    }
                    break;
                }
                case IrNodeKind::StoreVolatile: {
                    auto* s = static_cast<IrStoreVolatileInst const*>(v);
                    auto* val = inline_clone_operand(s->value, ic);
                    auto* ptr = inline_clone_operand(s->pointer, ic);
                    if (val && ptr)
                    {
                        auto* fresh = ic.ctx->store_volatile(val, ptr);
                        fresh->alignment = s->alignment;
                        result = fresh;
                    }
                    break;
                }
                case IrNodeKind::Gep: {
                    auto* g = static_cast<IrGepInst const*>(v);
                    auto* base = inline_clone_operand(g->base, ic);
                    if (!base)
                        break;
                    auto* fresh = ic.ctx->gep(g->type, base);
                    for (auto const& idx : g->indices)
                    {
                        IrGepInst::Index fresh_idx;
                        fresh_idx.kind = idx.kind;
                        fresh_idx.field_index = idx.field_index;
                        fresh_idx.dynamic_index = idx.dynamic_index ? inline_clone_operand(idx.dynamic_index, ic) : nullptr;
                        if (idx.dynamic_index && !fresh_idx.dynamic_index)
                        {
                            fresh = nullptr;
                            break;
                        }
                        fresh->indices.push_back(fresh_idx);
                    }
                    result = fresh;
                    break;
                }
                case IrNodeKind::Extract: {
                    auto* e = static_cast<IrExtractInst const*>(v);
                    auto* agg = inline_clone_operand(e->aggregate, ic);
                    if (agg)
                        result = ic.ctx->extract(e->type, agg, e->field_index);
                    break;
                }
                case IrNodeKind::Insert: {
                    auto* i = static_cast<IrInsertInst const*>(v);
                    auto* agg = inline_clone_operand(i->aggregate, ic);
                    auto* val = inline_clone_operand(i->value, ic);
                    if (agg && val)
                        result = ic.ctx->insert(i->type, agg, i->field_index, val);
                    break;
                }
                case IrNodeKind::Aggregate: {
                    auto* a = static_cast<IrAggregateInst const*>(v);
                    auto* fresh = ic.ctx->aggregate(a->type);
                    for (auto* e : a->values)
                    {
                        auto* ce = inline_clone_operand(e, ic);
                        if (!ce)
                        {
                            fresh = nullptr;
                            break;
                        }
                        fresh->values.push_back(ce);
                    }
                    result = fresh;
                    break;
                }
                case IrNodeKind::Phi: {
                    auto* p = static_cast<IrPhiInst const*>(v);
                    auto* fresh = ic.ctx->phi(p->type);
                    ic.values[v] = fresh;
                    for (auto const& inc : p->incoming)
                    {
                        auto bit = ic.blocks.find(inc.block);
                        auto* cval = inline_clone_operand(inc.value, ic);
                        if (bit == ic.blocks.end() || !cval)
                        {
                            fresh = nullptr;
                            break;
                        }
                        fresh->incoming.push_back({cval, bit->second});
                    }
                    result = fresh;
                    break;
                }
                case IrNodeKind::Call: {
                    auto* c = static_cast<IrCallInst const*>(v);
                    auto* callee = inline_clone_operand(c->callee, ic);
                    if (!callee)
                        break;
                    auto* fresh = ic.ctx->call(c->type, callee);
                    fresh->cc = c->cc;
                    for (auto* arg : c->args)
                    {
                        auto* carg = inline_clone_operand(arg, ic);
                        if (!carg)
                        {
                            fresh = nullptr;
                            break;
                        }
                        fresh->args.push_back(carg);
                    }
                    result = fresh;
                    break;
                }
                case IrNodeKind::CallTail: {
                    auto* c = static_cast<IrCallTailInst const*>(v);
                    auto* callee = inline_clone_operand(c->callee, ic);
                    if (!callee)
                        break;
                    auto* fresh = ic.ctx->call_tail(c->type, callee);
                    fresh->cc = c->cc;
                    for (auto* arg : c->args)
                    {
                        auto* carg = inline_clone_operand(arg, ic);
                        if (!carg)
                        {
                            fresh = nullptr;
                            break;
                        }
                        fresh->args.push_back(carg);
                    }
                    result = fresh;
                    break;
                }
                case IrNodeKind::AtomicLoad: {
                    auto* a = static_cast<IrAtomicLoadInst const*>(v);
                    auto* ptr = inline_clone_operand(a->pointer, ic);
                    if (ptr)
                    {
                        auto* fresh = ic.ctx->atomic_load(a->type, ptr, a->ordering);
                        fresh->alignment = a->alignment;
                        result = fresh;
                    }
                    break;
                }
                case IrNodeKind::AtomicStore: {
                    auto* s = static_cast<IrAtomicStoreInst const*>(v);
                    auto* val = inline_clone_operand(s->value, ic);
                    auto* ptr = inline_clone_operand(s->pointer, ic);
                    if (val && ptr)
                    {
                        auto* fresh = ic.ctx->atomic_store(val, ptr, s->ordering);
                        fresh->alignment = s->alignment;
                        result = fresh;
                    }
                    break;
                }
                case IrNodeKind::AtomicRmw: {
                    auto* r = static_cast<IrAtomicRmwInst const*>(v);
                    auto* ptr = inline_clone_operand(r->pointer, ic);
                    auto* val = inline_clone_operand(r->value, ic);
                    if (ptr && val)
                    {
                        auto* fresh = ic.ctx->atomic_rmw(r->type, r->op, ptr, val, r->ordering);
                        fresh->alignment = r->alignment;
                        result = fresh;
                    }
                    break;
                }
                case IrNodeKind::AtomicCmpXchg: {
                    auto* x = static_cast<IrAtomicCmpXchgInst const*>(v);
                    auto* ptr = inline_clone_operand(x->pointer, ic);
                    auto* exp = inline_clone_operand(x->expected, ic);
                    auto* des = inline_clone_operand(x->desired, ic);
                    if (ptr && exp && des)
                    {
                        auto* fresh = ic.ctx->atomic_cmpxchg(x->type, ptr, exp, des, x->success_ordering, x->failure_ordering);
                        fresh->alignment = x->alignment;
                        result = fresh;
                    }
                    break;
                }
                case IrNodeKind::Fence: {
                    auto* f = static_cast<IrFenceInst const*>(v);
                    result = ic.ctx->fence(f->ordering);
                    break;
                }
                case IrNodeKind::InlineAsm: {
                    auto* ia = static_cast<IrInlineAsmInst const*>(v);
                    std::pmr::vector<IrAsmOperand> ops(ic.ctx->allocator());
                    bool ok = true;
                    for (auto const& op : ia->operands)
                    {
                        IrAsmOperand fresh_op;
                        fresh_op.direction = op.direction;
                        fresh_op.placement_kind = op.placement_kind;
                        fresh_op.reg_name = op.reg_name;
                        fresh_op.reg_name2 = op.reg_name2;
                        fresh_op.flag_cond = op.flag_cond;
                        fresh_op.placeholder = op.placeholder;
                        fresh_op.type = op.type;
                        if (op.value)
                        {
                            fresh_op.value = inline_clone_operand(op.value, ic);
                            if (!fresh_op.value)
                                ok = false;
                        }
                        ops.push_back(fresh_op);
                    }
                    if (!ok)
                        break;

                    std::pmr::vector<std::string_view> clobbers(ic.ctx->allocator());
                    for (auto const& c : ia->clobbers)
                        clobbers.push_back(c);
                    std::pmr::string tmpl(ia->template_str, ic.ctx->allocator());
                    auto* fresh = ic.ctx->inline_asm(std::move(tmpl), std::move(ops), std::move(clobbers), ia->is_volatile, ia->align_stack, ia->dialect,
                                                     ia->type, ia->range);
                    fresh->template_parts = ia->template_parts;
                    result = fresh;
                    break;
                }
                default:
                    break;
            }

            if (!result || ic.failed)
            {
                ic.failed = true;
                return nullptr;
            }

            result->name = v->name;
            result->range = v->range;
            ic.values[v] = result;
            return result;
        }

        struct InlineRetCapture
        {
            IrBasicBlock* block = nullptr;
            IrValue* value = nullptr;
        };

        [[nodiscard]] static IrNode* inline_clone_terminator(IrNode const* term, IrBasicBlock* new_home, InlineClone& ic, std::vector<InlineRetCapture>& rets)
        {
            if (!term)
                return nullptr;

            switch (term->kind)
            {
                case IrNodeKind::Br: {
                    auto* br = static_cast<IrBrInst const*>(term);
                    auto it = ic.blocks.find(br->target);
                    if (it == ic.blocks.end())
                    {
                        ic.failed = true;
                        return nullptr;
                    }
                    return ic.ctx->br(it->second);
                }
                case IrNodeKind::BrCond: {
                    auto* br = static_cast<IrBrCondInst const*>(term);
                    auto* cond = inline_clone_operand(br->condition, ic);
                    auto tt = ic.blocks.find(br->true_target);
                    auto ft = ic.blocks.find(br->false_target);
                    if (!cond || tt == ic.blocks.end() || ft == ic.blocks.end())
                    {
                        ic.failed = true;
                        return nullptr;
                    }
                    return ic.ctx->br_cond(cond, tt->second, ft->second);
                }
                case IrNodeKind::Ret: {
                    auto* r = static_cast<IrRetInst const*>(term);
                    IrValue* val = nullptr;
                    if (r->value)
                    {
                        val = inline_clone_operand(r->value, ic);
                        if (!val)
                        {
                            ic.failed = true;
                            return nullptr;
                        }
                    }
                    rets.push_back({new_home, val});
                    return nullptr;
                }
                case IrNodeKind::Unreachable:
                    return ic.ctx->unreachable();
                case IrNodeKind::Switch: {
                    auto* sw = static_cast<IrSwitchInst const*>(term);
                    auto* val = inline_clone_operand(sw->value, ic);
                    auto dt = ic.blocks.find(sw->default_target);
                    if (!val || dt == ic.blocks.end())
                    {
                        ic.failed = true;
                        return nullptr;
                    }
                    auto* fresh = ic.ctx->switch_(val, dt->second);
                    for (auto const& c : sw->cases)
                    {
                        auto ct = ic.blocks.find(c.target);
                        if (ct == ic.blocks.end())
                        {
                            ic.failed = true;
                            return nullptr;
                        }
                        fresh->cases.push_back({c.start, c.end, ct->second});
                    }
                    return fresh;
                }
                default:
                    ic.failed = true;
                    return nullptr;
            }
        }

        [[nodiscard]] static std::string_view inline_fresh_name(IrContext& ctx, char const* prefix, std::uint32_t n)
        {
            char buf[64];
            int len = std::snprintf(buf, sizeof(buf), "%s%u", prefix, n);
            if (len <= 0)
                return {};

            void* mem = ctx.allocator().resource()->allocate(static_cast<std::size_t>(len) + 1, 1);
            std::memcpy(mem, buf, static_cast<std::size_t>(len) + 1);
            return {static_cast<char const*>(mem), static_cast<std::size_t>(len)};
        }
        [[nodiscard]] static bool inline_prepare_site(InlineSite const& site, IrFunction const* callee,
                                                      std::vector<std::pair<IrValue const*, IrValue*>>& argmap)
        {
            argmap.clear();
            if (!site.caller || !site.block || !site.inst || !callee || !callee->entry_block)
                return false;

            if (callee == site.caller)
                return false;

            if (!site.inst->type)
                return false;

            auto const& params = callee->entry_block->params;
            auto const& args = inline_site_args(site.inst);
            if (params.size() != args.size())
                return false;

            for (std::size_t i = 0; i < params.size(); ++i)
            {
                if (!ir_cast<IrLocal const>(params[i]))
                    return false;

                if (!args[i] || !params[i]->type || !args[i]->type)
                    return false;

                if (params[i]->type != args[i]->type)
                    return false;

                argmap.emplace_back(params[i], args[i]);
            }

            bool needs_entry = false;
            for (auto* bb : callee->blocks)
            {
                if (!bb)
                    return false;

                if (bb != callee->entry_block && !bb->params.empty())
                    return false;

                for (auto* inst : bb->instructions)
                    if (ir_cast<IrAllocaInst const>(inst))
                        needs_entry = true;
            }

            if (needs_entry && !site.caller->entry_block)
                return false;

            for (auto* inst : callee->entry_block->instructions)
                if (ir_cast<IrPhiInst const>(inst))
                    return false;

            bool call_void = site.inst->type->kind == IrTypeKind::Void;
            if (callee->func_type && callee->func_type->return_type && callee->func_type->return_type != site.inst->type)
                return false;

            for (auto* bb : callee->blocks)
            {
                if (!bb || !bb->terminator || bb->terminator->kind != IrNodeKind::Ret)
                    continue;

                auto* r = static_cast<IrRetInst const*>(bb->terminator);
                if (call_void)
                {
                    if (r->value != nullptr)
                        return false;
                }
                else if (!r->value || !r->value->type || r->value->type != site.inst->type)
                    return false;
            }

            return true;
        }
        [[nodiscard]] static bool inline_execute_site(InlineSite const& site, IrFunction const* callee,
                                                      std::vector<std::pair<IrValue const*, IrValue*>> const& argmap, IrContext& ctx)
        {
            auto& insts = site.block->instructions;
            if (site.index >= insts.size() || insts[site.index] != site.inst)
                return false;

            for (std::size_t i = site.index + 1; i < insts.size(); ++i)
                if (ir_cast<IrPhiInst const>(insts[i]))
                    return false;

            std::uint32_t next_id = 0;
            for (auto* bb : site.caller->blocks)
            {
                if (!bb)
                    return false;
                if (bb->id == 0xFFFFFFFFU)
                    return false;
                if (bb->id >= next_id)
                    next_id = bb->id + 1;
            }

            InlineClone ic;
            ic.ctx = &ctx;
            for (auto const& [p, a] : argmap)
                ic.values[p] = a;

            for (auto* bb : callee->blocks)
            {
                auto* nb = ctx.basic_block(inline_fresh_name(ctx, "inl.", next_id), next_id);
                ++next_id;
                ic.blocks[bb] = nb;
                ic.new_blocks.push_back(nb);
            }

            std::unordered_set<IrValue*> emitted;
            for (auto* bb : callee->blocks)
            {
                auto* home = ic.blocks[bb];
                for (auto* inst : bb->instructions)
                {
                    if (!inst)
                        continue;

                    IrValue* c = inline_clone_value(inst, ic);
                    if (!c || ic.failed)
                        return false;

                    if (ir_cast<IrAllocaInst const>(inst))
                        continue;

                    if (emitted.insert(c).second)
                        home->instructions.push_back(c);
                }
            }

            std::vector<InlineRetCapture> rets;
            for (auto* bb : callee->blocks)
            {
                if (!bb->terminator)
                    return false;

                auto* home = ic.blocks[bb];
                IrNode* t = inline_clone_terminator(bb->terminator, home, ic, rets);
                if (ic.failed)
                    return false;

                if (t)
                    home->terminator = t;
            }

            if (rets.empty())
                return false;

            IrBasicBlock* cont = ctx.basic_block(inline_fresh_name(ctx, "inl.cont.", next_id), next_id);
            ++next_id;
            for (std::size_t i = site.index + 1; i < insts.size(); ++i)
                cont->instructions.push_back(insts[i]);

            insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(site.index + 1), insts.end());
            cont->terminator = site.block->terminator;
            site.block->terminator = nullptr;
            for (auto* nb : ic.new_blocks)
            {
                nb->parent = site.caller;
                site.caller->blocks.push_back(nb);
            }

            cont->parent = site.caller;
            site.caller->blocks.push_back(cont);
            if (!ic.new_allocas.empty())
            {
                if (!site.caller->entry_block)
                    return false;

                auto* entry = site.caller->entry_block;
                std::size_t pos = 0;
                while (pos < entry->instructions.size() && ir_cast<IrPhiInst const>(entry->instructions[pos]))
                    ++pos;

                entry->instructions.insert(entry->instructions.begin() + static_cast<std::ptrdiff_t>(pos), ic.new_allocas.begin(), ic.new_allocas.end());
            }

            auto entry_it = ic.blocks.find(callee->entry_block);
            site.block->terminator = ctx.br(entry_it->second);
            for (auto* bb : site.caller->blocks)
            {
                if (!bb)
                    continue;

                for (auto* inst : bb->instructions)
                {
                    auto* phi = ir_cast<IrPhiInst>(inst);
                    if (!phi)
                        continue;

                    for (auto& inc : phi->incoming)
                        if (inc.block == site.block)
                            inc.block = cont;
                }
            }

            bool call_void = site.inst->type->kind == IrTypeKind::Void;
            IrValue* ret_val = nullptr;
            if (rets.size() == 1)
            {
                if (!call_void)
                    ret_val = rets[0].value;

                rets[0].block->terminator = ctx.br(cont);
            }
            else
            {
                IrBasicBlock* join = ctx.basic_block(inline_fresh_name(ctx, "inl.join.", next_id), next_id);
                ++next_id;
                join->parent = site.caller;
                site.caller->blocks.push_back(join);
                IrPhiInst* phi = nullptr;
                if (!call_void)
                {
                    phi = ctx.phi(site.inst->type);
                    ret_val = phi;
                    join->instructions.push_back(phi);
                }

                for (auto& r : rets)
                {
                    r.block->terminator = ctx.br(join);
                    if (phi)
                        phi->incoming.push_back({r.value, r.block});
                }

                join->terminator = ctx.br(cont);
            }
            if (!call_void)
                replace_value_uses(*site.caller, site.inst, ret_val);

            auto const call_loc = site.inst->debug_loc;
            for (auto* nb : ic.new_blocks)
            {
                for (auto* inst : nb->instructions)
                    inst->debug_loc = call_loc;
                if (nb->terminator)
                    nb->terminator->debug_loc = call_loc;
            }
            for (auto* a : ic.new_allocas)
                a->debug_loc = call_loc;
            site.block->terminator->debug_loc = call_loc;
            if (rets.size() > 1)
            {
                auto* join = site.caller->blocks.back();
                for (auto* inst : join->instructions)
                    inst->debug_loc = call_loc;
                join->terminator->debug_loc = call_loc;
            }

            auto& own = site.block->instructions;
            if (!own.empty() && own.back() == site.inst)
                own.pop_back();
            else
            {
                for (auto it = own.begin(); it != own.end(); ++it)
                    if (*it == site.inst)
                    {
                        own.erase(it);
                        break;
                    }
            }

            return true;
        }

        static bool inline_impl(IrModule& mod, IrContext& ctx, OptLevel level)
        {
            std::ignore = level;
            if (std::getenv("DCC_NO_INLINE") != nullptr)
                return false;
            auto start = std::chrono::steady_clock::now();
            std::size_t inlined = 0;
            std::set<std::pair<std::string_view, std::string_view>> refused_s6;
            std::unordered_map<IrFunction const*, int> func_ver;
            struct RefuseKey
            {
                IrFunction* caller;
                IrValue* inst;
                IrFunction const* callee;
                bool operator==(RefuseKey const&) const = default;
            };
            struct RefuseKeyHash
            {
                std::size_t operator()(RefuseKey const& k) const noexcept
                {
                    std::size_t h = std::hash<void const*>{}(k.caller);
                    h = h * 31 + std::hash<void const*>{}(k.inst);
                    return h * 31 + std::hash<void const*>{}(k.callee);
                }
            };
            struct RefuseEntry
            {
                int ver_caller;
                int ver_callee;
            };
            std::unordered_map<RefuseKey, RefuseEntry, RefuseKeyHash> refuse_cache;
            InlineCallGraph cg = inline_build_call_graph(mod);
            InlineScc scc = inline_compute_scc(mod, cg);
            std::vector<IrFunction*> order = inline_bottom_up_order(mod, scc);
            for (int iter = 0; iter < 8; ++iter)
            {
                bool changed = false;
                std::unordered_map<IrFunction const*, InlineCalleeGate> gate_cache;
                auto cached_gate = [&](IrFunction const* f) {
                    auto it = gate_cache.find(f);
                    if (it != gate_cache.end())
                        return it->second;
                    InlineCalleeGate gate = inline_classify_callee(f, scc);
                    gate_cache[f] = gate;
                    return gate;
                };
                for (auto* caller : order)
                {
                    if (!caller)
                        continue;

                    std::size_t sites_done = 0;
                    while (sites_done < 4096)
                    {
                        bool found = false;
                        InlineSite chosen;
                        IrFunction const* chosen_callee = nullptr;
                        std::vector<std::pair<IrValue const*, IrValue*>> argmap;
                        for (auto* bb : caller->blocks)
                        {
                            if (!bb)
                                continue;

                            for (std::size_t i = 0; i < bb->instructions.size(); ++i)
                            {
                                auto* inst = bb->instructions[i];
                                if (!inst || (inst->kind != IrNodeKind::Call && inst->kind != IrNodeKind::CallTail))
                                    continue;

                                IrFunction const* callee = inline_resolve_callee(inline_site_callee_value(inst));
                                if (!callee || callee == caller)
                                    continue;

                                InlineCalleeGate gate = cached_gate(callee);
                                if (!gate.eligible)
                                    continue;

                                RefuseKey rk{caller, inst, callee};
                                auto rcit = refuse_cache.find(rk);
                                if (rcit != refuse_cache.end() && rcit->second.ver_caller == func_ver[caller] && rcit->second.ver_callee == func_ver[callee])
                                {
                                    refused_s6.insert({caller->name, callee->name});
                                    continue;
                                }

                                InlineSite site{caller, bb, i, inst};
                                std::vector<std::pair<IrValue const*, IrValue*>> trial;
                                if (!inline_prepare_site(site, callee, trial))
                                    continue;

                                InlineSiteVerdict verdict = inline_gate_site(site, callee, gate, trial);
                                if (verdict == InlineSiteVerdict::RefuseQuiet)
                                    continue;

                                if (verdict == InlineSiteVerdict::RefuseMark)
                                {
                                    refused_s6.insert({caller->name, callee->name});
                                    refuse_cache[rk] = {func_ver[caller], func_ver[callee]};
                                    continue;
                                }

                                if (!gate.forced)
                                {
                                    InlineCalleeGate caller_gate = cached_gate(caller);
                                    if (caller_gate.eligible && !caller_gate.forced && caller_gate.size + gate.size > 20)
                                        continue;
                                }

                                chosen = site;
                                chosen_callee = callee;
                                argmap = std::move(trial);
                                found = true;
                                break;
                            }

                            if (found)
                                break;
                        }

                        if (!found)
                            break;

                        if (!inline_execute_site(chosen, chosen_callee, argmap, ctx))
                            break;

                        ++inlined;
                        ++sites_done;
                        changed = true;
                        gate_cache.erase(caller);
                        ++func_ver[caller];
                    }
                }
                if (!changed)
                    break;
            }
            if (std::getenv("DCC_BENCH_STATS") != nullptr)
            {
                std::println(std::cerr, "DCC_BENCH inline {} {}", inlined, refused_s6.size());
                auto inline_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                std::println(std::cerr, "DCC_BENCH phase inline {}", inline_seconds);
            }

            return inlined > 0;
        }

    } // namespace inline_detail

} // namespace dcc::ir::pass

export namespace dcc::ir::pass
{
    void register_builtin_passes(PassManager& pm)
    {
        pm.add_module_pass({.name = "inline", .min_level = OptLevel::O1, .run = inline_detail::inline_impl});
        pm.add_function_pass({.name = "sroa", .min_level = OptLevel::O1, .run = sroa_impl});
        pm.add_function_pass({.name = "mem2reg", .min_level = OptLevel::O1, .run = mem2reg_impl});
        pm.add_function_pass({.name = "combine", .min_level = OptLevel::O2, .run = combine_impl});
        pm.add_function_pass({.name = "sccp", .min_level = OptLevel::O2, .run = sccp_impl});
        pm.add_function_pass({.name = "dce", .min_level = OptLevel::O1, .run = dce_impl});
        pm.add_function_pass({.name = "simplifycfg", .min_level = OptLevel::O1, .run = simplifycfg_impl});
    }
}
