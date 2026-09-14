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
                case IrNodeKind::StringConstant:
                case IrNodeKind::Local:
                case IrNodeKind::GlobalRef:
                case IrNodeKind::Fence:
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

        auto const& pm = ctx.get_pred_map();
        for (auto& [phi_bb, phi] : block_phi)
        {
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
                case IrNodeKind::Segcast:
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
            std::unordered_map<std::size_t, std::vector<std::size_t>> succ;
            std::unordered_map<std::size_t, std::size_t> indeg;
            for (auto& [f, id] : scc.comp)
                indeg[id] = 0;

            std::unordered_map<std::size_t, std::vector<std::size_t>> dag;
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
            for (auto& [id, d] : indeg)
                if (d == 0)
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

            std::unordered_map<std::size_t, std::size_t> rank;
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

        [[nodiscard]] static bool inline_backward_flows_to(IrValue const* v, IrValue const* target, std::vector<IrValue const*>& seen,
                                                          std::size_t& steps)
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
                    return inline_backward_flows_to(i->aggregate, target, seen, steps) ||
                           inline_backward_flows_to(i->value, target, seen, steps);
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
                case IrNodeKind::Segcast: {
                    auto* c = static_cast<IrSegcastInst const*>(v);
                    return inline_backward_flows_to(c->operand, target, seen, steps);
                }
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
                            inst->kind == IrNodeKind::AtomicStore || inst->kind == IrNodeKind::AtomicRmw || inst->kind == IrNodeKind::PtrToI ||
                            inst->kind == IrNodeKind::InlineAsm)
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
                default: {
                    auto* ci = static_cast<IrSegcastInst const*>(v);
                    return clone1(ci->operand, ci->type, &IrContext::segcast);
                }
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
                case IrNodeKind::Segcast:
                    result = inline_clone_cast(v, ic);
                    break;
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
                                if (rcit != refuse_cache.end() && rcit->second.ver_caller == func_ver[caller] &&
                                    rcit->second.ver_callee == func_ver[callee])
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

namespace
{

    struct AutoRegister
    {
        AutoRegister()
        {
            auto& pm = dcc::ir::pass::global_pass_manager();
            pm.add_module_pass({.name = "inline", .min_level = dcc::ir::pass::OptLevel::O1, .run = dcc::ir::pass::inline_detail::inline_impl});
            pm.add_function_pass({.name = "mem2reg", .min_level = dcc::ir::pass::OptLevel::O1, .run = dcc::ir::pass::mem2reg_impl});
            pm.add_function_pass({.name = "dce", .min_level = dcc::ir::pass::OptLevel::O1, .run = dcc::ir::pass::dce_impl});
            pm.add_function_pass({.name = "simplifycfg", .min_level = dcc::ir::pass::OptLevel::O1, .run = dcc::ir::pass::simplifycfg_impl});
        }
    };

    AutoRegister auto_reg;

} // anonymous namespace
