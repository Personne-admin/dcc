module;

#include <algorithm>

export module dcc.ir.analysis;

import std;
import dcc.ir;

export namespace dcc::ir::analysis
{
    [[nodiscard]] std::vector<IrBasicBlock*> compute_rpo(IrFunction const& func)
    {
        std::vector<IrBasicBlock*> rpo;
        std::unordered_set<IrBasicBlock const*> visited;

        struct Frame
        {
            IrBasicBlock* bb;
            std::size_t succ_idx{0};
        };

        std::vector<Frame> stack;
        if (func.entry_block)
        {
            visited.insert(func.entry_block);
            stack.push_back({func.entry_block, 0});
        }

        auto get_succ = [](IrBasicBlock const* bb, std::vector<IrBasicBlock*>& out) {
            if (!bb->terminator)
                return;
            switch (bb->terminator->kind)
            {
                case IrNodeKind::Br:
                    out.push_back(static_cast<IrBrInst*>(bb->terminator)->target);
                    break;
                case IrNodeKind::BrCond: {
                    auto* br = static_cast<IrBrCondInst*>(bb->terminator);
                    out.push_back(br->true_target);
                    out.push_back(br->false_target);
                    break;
                }
                case IrNodeKind::Switch: {
                    auto* sw = static_cast<IrSwitchInst*>(bb->terminator);
                    out.push_back(sw->default_target);
                    for (auto& c : sw->cases)
                        out.push_back(c.target);
                    break;
                }
                default:
                    break;
            }
        };

        while (!stack.empty())
        {
            auto& top = stack.back();
            auto* bb = top.bb;

            std::vector<IrBasicBlock*> succs;
            get_succ(bb, succs);

            if (top.succ_idx < succs.size())
            {
                auto* next = succs[top.succ_idx++];
                if (visited.insert(next).second)
                    stack.push_back({next, 0});
            }
            else
            {
                rpo.push_back(bb);
                stack.pop_back();
            }
        }

        std::ranges::reverse(rpo);
        return rpo;
    }

    using PredMap = std::unordered_map<IrBasicBlock const*, std::vector<IrBasicBlock*>>;

    [[nodiscard]] PredMap build_pred_map(IrFunction const& func)
    {
        PredMap pm;
        for (auto* bb : func.blocks)
            pm[bb] = {};

        for (auto* bb : func.blocks)
        {
            if (!bb->terminator)
                continue;

            auto add_succ = [&](IrBasicBlock* succ) {
                if (succ)
                    pm[succ].push_back(bb);
            };
            switch (bb->terminator->kind)
            {
                case IrNodeKind::Br:
                    add_succ(static_cast<IrBrInst*>(bb->terminator)->target);
                    break;
                case IrNodeKind::BrCond: {
                    auto* br = static_cast<IrBrCondInst*>(bb->terminator);
                    add_succ(br->true_target);
                    add_succ(br->false_target);
                    break;
                }
                case IrNodeKind::Switch: {
                    auto* sw = static_cast<IrSwitchInst*>(bb->terminator);
                    add_succ(sw->default_target);
                    for (auto& c : sw->cases)
                        add_succ(c.target);
                    break;
                }
                default:
                    break;
            }
        }
        return pm;
    }

    struct DomTree
    {
        std::unordered_map<IrBasicBlock const*, IrBasicBlock const*> idom;
        std::unordered_map<IrBasicBlock const*, std::vector<IrBasicBlock const*>> children;
        std::unordered_map<IrBasicBlock const*, std::vector<IrBasicBlock const*>> frontier;

        [[nodiscard]] bool dominates(IrBasicBlock const* a, IrBasicBlock const* b) const
        {
            if (!a || !b)
                return false;

            if (a == b)
                return true;

            auto it = idom.find(b);
            while (it != idom.end() && it->second != b)
            {
                if (it->second == a)
                    return true;

                it = idom.find(it->second);
            }

            return false;
        }

        [[nodiscard]] bool strictly_dominates(IrBasicBlock const* a, IrBasicBlock const* b) const { return a != b && dominates(a, b); }

        static DomTree build(IrFunction const& /*func*/, std::vector<IrBasicBlock*> const& rpo, PredMap const& preds)
        {
            DomTree dt;
            if (rpo.empty())
                return dt;

            std::unordered_map<IrBasicBlock const*, std::size_t> rpo_idx;
            for (std::size_t i = 0; i < rpo.size(); ++i)
                rpo_idx[rpo[i]] = i;

            for (auto* bb : rpo)
                dt.idom[bb] = nullptr;
            dt.idom[rpo[0]] = rpo[0];

            auto intersect = [&](IrBasicBlock const* f1, IrBasicBlock const* f2) -> IrBasicBlock const* {
                while (f1 != f2)
                {
                    while (rpo_idx.at(f1) > rpo_idx.at(f2))
                        f1 = dt.idom.at(f1);
                    while (rpo_idx.at(f2) > rpo_idx.at(f1))
                        f2 = dt.idom.at(f2);
                }

                return f1;
            };

            bool changed = true;
            while (changed)
            {
                changed = false;
                for (std::size_t i = 1; i < rpo.size(); ++i)
                {
                    auto* bb = rpo[i];
                    auto pit = preds.find(bb);
                    if (pit == preds.end() || pit->second.empty())
                        continue;

                    auto const& bb_preds = pit->second;

                    IrBasicBlock const* new_idom = nullptr;
                    for (auto* p : bb_preds)
                    {
                        auto it = dt.idom.find(p);
                        if (it != dt.idom.end() && it->second != nullptr)
                        {
                            new_idom = p;
                            break;
                        }
                    }
                    if (!new_idom)
                        continue;

                    for (auto* p : bb_preds)
                    {
                        if (p == new_idom)
                            continue;

                        auto it = dt.idom.find(p);
                        if (it == dt.idom.end() || it->second == nullptr)
                            continue;

                        new_idom = intersect(p, new_idom);
                    }

                    if (dt.idom[bb] != new_idom)
                    {
                        dt.idom[bb] = new_idom;
                        changed = true;
                    }
                }
            }

            for (auto& [bb, parent] : dt.idom)
                if (parent && parent != bb)
                    dt.children[parent].push_back(bb);

            for (auto* bb : rpo)
            {
                auto pit = preds.find(bb);
                if (pit == preds.end() || pit->second.size() < 2)
                    continue;

                auto const& bb_preds = pit->second;
                auto* bb_idom = dt.idom[bb];
                for (auto* p : bb_preds)
                {
                    IrBasicBlock const* runner = p;
                    while (runner != bb_idom)
                    {
                        dt.frontier[runner].push_back(bb);
                        runner = dt.idom[runner];
                    }
                }
            }

            return dt;
        }
    };

    struct UseDef
    {
        std::unordered_map<IrValue const*, std::vector<IrValue*>> uses;

        [[nodiscard]] std::span<IrValue*> uses_of(IrValue const* v)
        {
            auto it = uses.find(v);
            if (it != uses.end())
                return it->second;

            return {};
        }

        [[nodiscard]] std::span<IrValue* const> uses_of(IrValue const* v) const
        {
            auto it = uses.find(v);
            if (it != uses.end())
                return it->second;

            return {};
        }

        [[nodiscard]] bool has_uses(IrValue const* v) const
        {
            auto it = uses.find(v);
            return it != uses.end() && !it->second.empty();
        }

        [[nodiscard]] std::size_t use_count(IrValue const* v) const
        {
            auto it = uses.find(v);
            return it != uses.end() ? it->second.size() : 0;
        }

        static UseDef build(IrFunction const& func)
        {
            UseDef ud;

            auto record_use = [&](IrValue const* val, IrValue* user) {
                if (val)
                    ud.uses[val].push_back(user);
            };

            auto collect_inst_operands = [&](IrValue const* inst) {
                auto kind = inst->kind;
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
                    case IrNodeKind::AShr: {
                        auto get_lhs = [](IrValue const* v) -> IrValue* {
                            switch (v->kind)
                            {
#define BINOP_CASE(k)                                                                                                                                          \
    case IrNodeKind::k:                                                                                                                                        \
        return static_cast<Ir##k##Inst const*>(v)->lhs

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
                                default:
                                    return nullptr;
                            }
                        };
                        auto get_rhs = [](IrValue const* v) -> IrValue* {
                            switch (v->kind)
                            {
#define BINOP_CASE(k)                                                                                                                                          \
    case IrNodeKind::k:                                                                                                                                        \
        return static_cast<Ir##k##Inst const*>(v)->rhs

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
                                default:
                                    return nullptr;
                            }
                        };
                        record_use(get_lhs(inst), const_cast<IrValue*>(inst));
                        record_use(get_rhs(inst), const_cast<IrValue*>(inst));
                        break;
                    }

                    case IrNodeKind::Neg:
                    case IrNodeKind::Not: {
                        auto get_op = [](IrValue const* v) -> IrValue* {
                            switch (v->kind)
                            {
                                case IrNodeKind::Neg:
                                    return static_cast<IrNegInst const*>(v)->operand;
                                case IrNodeKind::Not:
                                    return static_cast<IrNotInst const*>(v)->operand;
                                default:
                                    return nullptr;
                            }
                        };
                        record_use(get_op(inst), const_cast<IrValue*>(inst));
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
                    case IrNodeKind::CmpUGe: {
                        auto* rep = static_cast<IrCmpEqInst const*>(inst);
                        record_use(rep->lhs, const_cast<IrValue*>(inst));
                        record_use(rep->rhs, const_cast<IrValue*>(inst));
                        break;
                    }

                    case IrNodeKind::Alloca: {
                        auto* a = static_cast<IrAllocaInst const*>(inst);
                        record_use(a->count, const_cast<IrValue*>(inst));
                        break;
                    }

                    case IrNodeKind::Load: {
                        auto* l = static_cast<IrLoadInst const*>(inst);
                        record_use(l->pointer, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::LoadVolatile: {
                        auto* l = static_cast<IrLoadVolatileInst const*>(inst);
                        record_use(l->pointer, const_cast<IrValue*>(inst));
                        break;
                    }

                    case IrNodeKind::Store: {
                        auto* s = static_cast<IrStoreInst const*>(inst);
                        record_use(s->value, const_cast<IrValue*>(inst));
                        record_use(s->pointer, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::StoreVolatile: {
                        auto* s = static_cast<IrStoreVolatileInst const*>(inst);
                        record_use(s->value, const_cast<IrValue*>(inst));
                        record_use(s->pointer, const_cast<IrValue*>(inst));
                        break;
                    }

                    case IrNodeKind::Gep: {
                        auto* g = static_cast<IrGepInst const*>(inst);
                        record_use(g->base, const_cast<IrValue*>(inst));
                        for (auto const& idx : g->indices)
                            if (idx.kind == IrGepInst::IndexKind::Array)
                                record_use(idx.dynamic_index, const_cast<IrValue*>(inst));

                        break;
                    }

                    case IrNodeKind::Zext: {
                        record_use(static_cast<IrZextInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::Sext: {
                        record_use(static_cast<IrSextInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::Trunc: {
                        record_use(static_cast<IrTruncInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::FpExt: {
                        record_use(static_cast<IrFpExtInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::FpTrunc: {
                        record_use(static_cast<IrFpTruncInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::FpToI: {
                        record_use(static_cast<IrFpToIInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::IToFp: {
                        record_use(static_cast<IrIToFpInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::PtrToI: {
                        record_use(static_cast<IrPtrToIInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::IToPtr: {
                        record_use(static_cast<IrIToPtrInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::Bitcast: {
                        record_use(static_cast<IrBitcastInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::Segcast: {
                        record_use(static_cast<IrSegcastInst const*>(inst)->operand, const_cast<IrValue*>(inst));
                        break;
                    }

                    case IrNodeKind::Extract: {
                        auto* e = static_cast<IrExtractInst const*>(inst);
                        record_use(e->aggregate, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::Insert: {
                        auto* i = static_cast<IrInsertInst const*>(inst);
                        record_use(i->aggregate, const_cast<IrValue*>(inst));
                        record_use(i->value, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::Aggregate: {
                        auto* a = static_cast<IrAggregateInst const*>(inst);
                        for (auto* v : a->values)
                            record_use(v, const_cast<IrValue*>(inst));
                        break;
                    }

                    case IrNodeKind::Phi: {
                        auto* p = static_cast<IrPhiInst const*>(inst);
                        for (auto const& inc : p->incoming)
                            record_use(inc.value, const_cast<IrValue*>(inst));

                        break;
                    }

                    case IrNodeKind::Call: {
                        auto* c = static_cast<IrCallInst const*>(inst);
                        record_use(c->callee, const_cast<IrValue*>(inst));
                        for (auto* arg : c->args)
                            record_use(arg, const_cast<IrValue*>(inst));

                        break;
                    }
                    case IrNodeKind::CallTail: {
                        auto* c = static_cast<IrCallTailInst const*>(inst);
                        record_use(c->callee, const_cast<IrValue*>(inst));
                        for (auto* arg : c->args)
                            record_use(arg, const_cast<IrValue*>(inst));

                        break;
                    }

                    case IrNodeKind::AtomicLoad: {
                        record_use(static_cast<IrAtomicLoadInst const*>(inst)->pointer, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::AtomicStore: {
                        auto* s = static_cast<IrAtomicStoreInst const*>(inst);
                        record_use(s->value, const_cast<IrValue*>(inst));
                        record_use(s->pointer, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::AtomicRmw: {
                        auto* r = static_cast<IrAtomicRmwInst const*>(inst);
                        record_use(r->pointer, const_cast<IrValue*>(inst));
                        record_use(r->value, const_cast<IrValue*>(inst));
                        break;
                    }
                    case IrNodeKind::Fence:
                        break;

                    case IrNodeKind::Br:
                    case IrNodeKind::BrCond:
                    case IrNodeKind::Ret:
                    case IrNodeKind::Unreachable:
                    case IrNodeKind::Switch:
                        break;

                    default:
                        break;
                }
            };

            for (auto* bb : func.blocks)
            {
                for (auto* inst : bb->instructions)
                {
                    if (!inst)
                        continue;

                    collect_inst_operands(inst);
                }
            }

            return ud;
        }
    };

    struct LivenessInfo
    {
        std::unordered_map<IrBasicBlock const*, std::unordered_set<IrValue const*>> live_in;
        std::unordered_map<IrBasicBlock const*, std::unordered_set<IrValue const*>> live_out;

        static LivenessInfo compute(IrFunction const& func, UseDef const& /*ud*/, std::vector<IrBasicBlock*> const& rpo)
        {
            LivenessInfo li;

            for (auto* bb : func.blocks)
            {
                li.live_in[bb] = {};
                li.live_out[bb] = {};
            }

            if (rpo.empty())
                return li;

            auto preds = build_pred_map(func);
            std::unordered_map<IrBasicBlock const*, std::vector<IrBasicBlock*>> succs;
            for (auto* bb : func.blocks)
            {
                if (!bb->terminator)
                    continue;

                switch (bb->terminator->kind)
                {
                    case IrNodeKind::Br:
                        succs[bb].push_back(static_cast<IrBrInst*>(bb->terminator)->target);
                        break;
                    case IrNodeKind::BrCond: {
                        auto* br = static_cast<IrBrCondInst*>(bb->terminator);
                        succs[bb].push_back(br->true_target);
                        succs[bb].push_back(br->false_target);
                        break;
                    }
                    case IrNodeKind::Switch: {
                        auto* sw = static_cast<IrSwitchInst*>(bb->terminator);
                        succs[bb].push_back(sw->default_target);
                        for (auto& c : sw->cases)
                            succs[bb].push_back(c.target);
                        break;
                    }
                    default:
                        break;
                }
            }

            std::unordered_map<IrBasicBlock const*, std::unordered_set<IrValue const*>> gen;
            std::unordered_map<IrBasicBlock const*, std::unordered_set<IrValue const*>> kill;

            for (auto* bb : func.blocks)
            {
                std::unordered_set<IrValue const*>& kill_set = kill[bb];
                std::unordered_set<IrValue const*>& gen_set = gen[bb];

                for (auto* inst : bb->instructions)
                    if (inst && inst->type)
                        kill_set.insert(inst);

                for (auto* inst : bb->instructions)
                {
                    if (!inst)
                        continue;

                    auto collect_operands_into = [&](IrNode const* n) {
                        switch (n->kind)
                        {
#define HANDLE_BINOP(k)                                                                                                                                        \
    case IrNodeKind::k: {                                                                                                                                      \
        auto* bi = static_cast<Ir##k##Inst const*>(n);                                                                                                         \
        if (!kill_set.contains(bi->lhs))                                                                                                                       \
            gen_set.insert(bi->lhs);                                                                                                                           \
        if (!kill_set.contains(bi->rhs))                                                                                                                       \
            gen_set.insert(bi->rhs);                                                                                                                           \
        break;                                                                                                                                                 \
    }

                            HANDLE_BINOP(Add);
                            HANDLE_BINOP(Sub);
                            HANDLE_BINOP(Mul);
                            HANDLE_BINOP(UDiv);
                            HANDLE_BINOP(SDiv);
                            HANDLE_BINOP(URem);
                            HANDLE_BINOP(SRem);
                            HANDLE_BINOP(FDiv);
                            HANDLE_BINOP(FRem);
                            HANDLE_BINOP(And);
                            HANDLE_BINOP(Or);
                            HANDLE_BINOP(Xor);
                            HANDLE_BINOP(Shl);
                            HANDLE_BINOP(LShr);
                            HANDLE_BINOP(AShr);
#undef HANDLE_BINOP
                            case IrNodeKind::Neg: {
                                auto* u = static_cast<IrNegInst const*>(n);
                                if (!kill_set.contains(u->operand))
                                    gen_set.insert(u->operand);
                                break;
                            }
                            case IrNodeKind::Not: {
                                auto* u = static_cast<IrNotInst const*>(n);
                                if (!kill_set.contains(u->operand))
                                    gen_set.insert(u->operand);
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
                            case IrNodeKind::CmpUGe: {
                                auto* ci = static_cast<IrCmpEqInst const*>(n);
                                if (!kill_set.contains(ci->lhs))
                                    gen_set.insert(ci->lhs);
                                if (!kill_set.contains(ci->rhs))
                                    gen_set.insert(ci->rhs);
                                break;
                            }
                            case IrNodeKind::Alloca: {
                                auto* a = static_cast<IrAllocaInst const*>(n);
                                if (a->count && !kill_set.contains(a->count))
                                    gen_set.insert(a->count);
                                break;
                            }
                            case IrNodeKind::Load: {
                                auto* l = static_cast<IrLoadInst const*>(n);
                                if (!kill_set.contains(l->pointer))
                                    gen_set.insert(l->pointer);
                                break;
                            }
                            case IrNodeKind::LoadVolatile: {
                                auto* l = static_cast<IrLoadVolatileInst const*>(n);
                                if (!kill_set.contains(l->pointer))
                                    gen_set.insert(l->pointer);
                                break;
                            }
                            case IrNodeKind::Store: {
                                auto* s = static_cast<IrStoreInst const*>(n);
                                if (!kill_set.contains(s->value))
                                    gen_set.insert(s->value);
                                if (!kill_set.contains(s->pointer))
                                    gen_set.insert(s->pointer);
                                break;
                            }
                            case IrNodeKind::StoreVolatile: {
                                auto* s = static_cast<IrStoreVolatileInst const*>(n);
                                if (!kill_set.contains(s->value))
                                    gen_set.insert(s->value);
                                if (!kill_set.contains(s->pointer))
                                    gen_set.insert(s->pointer);
                                break;
                            }
                            case IrNodeKind::Gep: {
                                auto* g = static_cast<IrGepInst const*>(n);
                                if (!kill_set.contains(g->base))
                                    gen_set.insert(g->base);
                                for (auto const& idx : g->indices)
                                    if (idx.kind == IrGepInst::IndexKind::Array && idx.dynamic_index)
                                        if (!kill_set.contains(idx.dynamic_index))
                                            gen_set.insert(idx.dynamic_index);

                                break;
                            }
                            case IrNodeKind::Zext: {
                                auto* c = static_cast<IrZextInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::Sext: {
                                auto* c = static_cast<IrSextInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::Trunc: {
                                auto* c = static_cast<IrTruncInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::FpExt: {
                                auto* c = static_cast<IrFpExtInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::FpTrunc: {
                                auto* c = static_cast<IrFpTruncInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::FpToI: {
                                auto* c = static_cast<IrFpToIInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::IToFp: {
                                auto* c = static_cast<IrIToFpInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::PtrToI: {
                                auto* c = static_cast<IrPtrToIInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::IToPtr: {
                                auto* c = static_cast<IrIToPtrInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::Bitcast: {
                                auto* c = static_cast<IrBitcastInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::Segcast: {
                                auto* c = static_cast<IrSegcastInst const*>(n);
                                if (!kill_set.contains(c->operand))
                                    gen_set.insert(c->operand);
                                break;
                            }
                            case IrNodeKind::Extract: {
                                auto* e = static_cast<IrExtractInst const*>(n);
                                if (!kill_set.contains(e->aggregate))
                                    gen_set.insert(e->aggregate);
                                break;
                            }
                            case IrNodeKind::Insert: {
                                auto* i = static_cast<IrInsertInst const*>(n);
                                if (!kill_set.contains(i->aggregate))
                                    gen_set.insert(i->aggregate);
                                if (!kill_set.contains(i->value))
                                    gen_set.insert(i->value);
                                break;
                            }
                            case IrNodeKind::Aggregate: {
                                auto* a = static_cast<IrAggregateInst const*>(n);
                                for (auto* v : a->values)
                                    if (!kill_set.contains(v))
                                        gen_set.insert(v);
                                break;
                            }
                            case IrNodeKind::Phi:
                                break;
                            case IrNodeKind::Call: {
                                auto* c = static_cast<IrCallInst const*>(n);
                                if (!kill_set.contains(c->callee))
                                    gen_set.insert(c->callee);
                                for (auto* arg : c->args)
                                    if (!kill_set.contains(arg))
                                        gen_set.insert(arg);
                                break;
                            }
                            case IrNodeKind::CallTail: {
                                auto* c = static_cast<IrCallTailInst const*>(n);
                                if (!kill_set.contains(c->callee))
                                    gen_set.insert(c->callee);
                                for (auto* arg : c->args)
                                    if (!kill_set.contains(arg))
                                        gen_set.insert(arg);
                                break;
                            }
                            case IrNodeKind::AtomicLoad: {
                                auto* a = static_cast<IrAtomicLoadInst const*>(n);
                                if (!kill_set.contains(a->pointer))
                                    gen_set.insert(a->pointer);
                                break;
                            }
                            case IrNodeKind::AtomicStore: {
                                auto* s = static_cast<IrAtomicStoreInst const*>(n);
                                if (!kill_set.contains(s->value))
                                    gen_set.insert(s->value);
                                if (!kill_set.contains(s->pointer))
                                    gen_set.insert(s->pointer);
                                break;
                            }
                            case IrNodeKind::AtomicRmw: {
                                auto* r = static_cast<IrAtomicRmwInst const*>(n);
                                if (!kill_set.contains(r->pointer))
                                    gen_set.insert(r->pointer);
                                if (!kill_set.contains(r->value))
                                    gen_set.insert(r->value);
                                break;
                            }
                            case IrNodeKind::Fence:
                                break;
                            default:
                                break;
                        }
                    };

                    collect_operands_into(inst);
                }

                if (bb->terminator)
                {
                    switch (bb->terminator->kind)
                    {
                        case IrNodeKind::BrCond: {
                            auto* br = static_cast<IrBrCondInst*>(bb->terminator);
                            if (!kill_set.contains(br->condition))
                                gen_set.insert(br->condition);
                            break;
                        }
                        case IrNodeKind::Ret: {
                            auto* ret = static_cast<IrRetInst*>(bb->terminator);
                            if (ret->value && !kill_set.contains(ret->value))
                                gen_set.insert(ret->value);
                            break;
                        }
                        case IrNodeKind::Switch: {
                            auto* sw = static_cast<IrSwitchInst*>(bb->terminator);
                            if (!kill_set.contains(sw->value))
                                gen_set.insert(sw->value);
                            break;
                        }
                        default:
                            break;
                    }
                }
            }

            auto rev_rpo = rpo;
            std::ranges::reverse(rev_rpo);

            bool changed = true;
            while (changed)
            {
                changed = false;
                for (auto* bb : rev_rpo)
                {
                    std::unordered_set<IrValue const*> new_out;
                    for (auto* succ : succs[bb])
                    {
                        auto it = li.live_in.find(succ);
                        if (it != li.live_in.end())
                            for (auto* v : it->second)
                                new_out.insert(v);

                        if (succ)
                        {
                            for (auto* inst : succ->instructions)
                            {
                                if (!inst)
                                    continue;

                                if (auto* phi = ir_cast<IrPhiInst>(inst))
                                    for (auto const& inc : phi->incoming)
                                        if (inc.block == bb)
                                            new_out.insert(inc.value);
                            }
                        }
                    }

                    if (new_out != li.live_out[bb])
                    {
                        li.live_out[bb] = std::move(new_out);
                        changed = true;
                    }

                    std::unordered_set<IrValue const*> new_in = gen[bb];
                    for (auto* v : li.live_out[bb])
                    {
                        if (!kill[bb].contains(v))
                            new_in.insert(v);
                    }

                    if (new_in != li.live_in[bb])
                    {
                        li.live_in[bb] = std::move(new_in);
                        changed = true;
                    }
                }
            }

            return li;
        }
    };

} // namespace dcc::ir::analysis
