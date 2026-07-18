export module dcc.ir.transforms;

import std;
import dcc.ir;
import dcc.ir.analysis;
import dcc.ir.pass;

namespace dcc::ir::pass
{
    [[nodiscard]] static IrBasicBlock* find_block(IrFunction const& func, IrValue const* inst)
    {
        for (auto* bb : func.blocks)
            for (auto* i : bb->instructions)
                if (i == inst)
                    return bb;

        return nullptr;
    }

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
            case IrNodeKind::Segcast:
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
                    CAST_CASE(Segcast);

#undef CAST_CASE

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
                default:
                    break;
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

    [[nodiscard]] static bool promote_alloca(IrAllocaInst* alloca, FunctionPassContext& ctx);
    [[nodiscard]] static bool merge_blocks(FunctionPassContext& ctx);
    [[nodiscard]] static bool remove_unreachable(FunctionPassContext& ctx);

    static bool mem2reg_impl(FunctionPassContext& ctx)
    {
        std::vector<IrAllocaInst*> all_allocas;
        for (auto* bb : ctx.func->blocks)
            for (auto* inst : bb->instructions)
                if (auto* a = ir_cast<IrAllocaInst>(inst))
                    all_allocas.push_back(a);

        bool promoted = false;
        for (auto* alloca : all_allocas)
            if (promote_alloca(alloca, ctx))
                promoted = true;

        if (promoted)
            ctx.invalidate_cfg();

        return promoted;
    }

    [[nodiscard]] static bool promote_alloca(IrAllocaInst* alloca, FunctionPassContext& ctx)
    {
        if (alloca->count != nullptr)
            return false;

        auto ud = analysis::UseDef::build(*ctx.func);
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

                auto* bb = find_block(*ctx.func, user);
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
            auto* bb = find_block(*ctx.func, st);
            if (bb)
                def_blocks.insert(bb);
        }

        auto const& dom = ctx.get_dom_tree();
        auto rpo = ctx.get_rpo();

        std::unordered_map<IrBasicBlock const*, std::size_t> rpo_index;
        for (std::size_t i = 0; i < rpo.size(); ++i)
            rpo_index[rpo[i]] = i;

        std::unordered_set<IrBasicBlock*> idf;
        if (!def_blocks.empty())
        {
            std::vector<IrBasicBlock*> worklist(def_blocks.begin(), def_blocks.end());
            std::unordered_set<IrBasicBlock*> in_wl(def_blocks.begin(), def_blocks.end());

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
        for (auto* dfb : idf)
        {
            auto* phi = ctx.ctx->phi(val_type);
            dfb->instructions.insert(dfb->instructions.begin(), phi);
            block_phi[dfb] = phi;
        }

        std::unordered_map<IrBasicBlock*, IrValue*> block_end_val;

        IrValue* zero_val = create_zero_for_type(val_type, *ctx.ctx);

        std::unordered_map<IrBasicBlock*, std::vector<IrBasicBlock*>> dom_children;
        for (auto& [parent, kids] : dom.children)
            for (auto* k : kids)
                dom_children[const_cast<IrBasicBlock*>(parent)].push_back(const_cast<IrBasicBlock*>(k));

        std::vector<IrBasicBlock*> root_children;
        {
            auto it = dom_children.find(ctx.func->entry_block);
            if (it != dom_children.end())
                root_children = it->second;
        }

        std::unordered_map<IrLoadInst*, IrValue*> load_replacements;

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
                        load_replacements[load] = current;
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
                            load_replacements[load] = current;
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

        for (auto& [phi_bb, phi] : block_phi)
        {
            auto pm = analysis::build_pred_map(*ctx.func);
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

        for (auto& [load, new_val] : load_replacements)
            replace_value_uses(*ctx.func, load, new_val);

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

        auto blocks_copy = ctx.func->blocks;

        for (auto* bb : blocks_copy)
        {
            if (bb == ctx.func->entry_block)
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

            auto& blocks = ctx.func->blocks;
            for (auto it = blocks.begin(); it != blocks.end(); ++it)
            {
                if (*it == bb)
                {
                    blocks.erase(it);
                    break;
                }
            }

            changed = true;
            pm = analysis::build_pred_map(*ctx.func);
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

} // namespace dcc::ir::pass

namespace
{

    struct AutoRegister
    {
        AutoRegister()
        {
            auto& pm = dcc::ir::pass::global_pass_manager();
            pm.add_function_pass({.name="mem2reg", .min_level=dcc::ir::pass::OptLevel::O1, .run=dcc::ir::pass::mem2reg_impl});
            pm.add_function_pass({.name="dce", .min_level=dcc::ir::pass::OptLevel::O1, .run=dcc::ir::pass::dce_impl});
            pm.add_function_pass({.name="simplifycfg", .min_level=dcc::ir::pass::OptLevel::O1, .run=dcc::ir::pass::simplifycfg_impl});
        }
    };

    AutoRegister auto_reg;

} // anonymous namespace
