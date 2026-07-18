export module dcc.ir.pass;

import std;
import dcc.ir;
import dcc.ir.analysis;

namespace dcc::ir::pass
{
    [[nodiscard]] IrModule* clone_module(IrModule const* src, IrContext& dst);
}

export namespace dcc::ir::pass
{
    enum class OptLevel : std::uint8_t
    {
        O0,
        O1,
        O2,
        Os,
    };

    struct FunctionPassContext
    {
    public:
        IrFunction* func{};
        IrContext* ctx{};

        std::vector<IrBasicBlock*> const& get_rpo()
        {
            if (!m_rpo_cache)
                m_rpo_cache = analysis::compute_rpo(*func);
            return *m_rpo_cache;
        }

        analysis::PredMap const& get_pred_map()
        {
            if (!m_pred_map_cache)
                m_pred_map_cache = analysis::build_pred_map(*func);
            return *m_pred_map_cache;
        }

        analysis::DomTree const& get_dom_tree()
        {
            if (!m_dom_tree_cache)
            {
                auto const& r = get_rpo();
                auto const& p = get_pred_map();
                m_dom_tree_cache = analysis::DomTree::build(*func, r, p);
            }
            return *m_dom_tree_cache;
        }

        analysis::UseDef const& get_use_def()
        {
            if (!m_use_def_cache)
                m_use_def_cache = analysis::UseDef::build(*func);
            return *m_use_def_cache;
        }

        void invalidate_cfg()
        {
            m_rpo_cache.reset();
            m_pred_map_cache.reset();
            m_dom_tree_cache.reset();
            m_use_def_cache.reset();
        }

        void invalidate_use_def() { m_use_def_cache.reset(); }

    private:
        mutable std::optional<std::vector<IrBasicBlock*>> m_rpo_cache;
        mutable std::optional<analysis::PredMap> m_pred_map_cache;
        mutable std::optional<analysis::DomTree> m_dom_tree_cache;
        mutable std::optional<analysis::UseDef> m_use_def_cache;
    };

    struct FunctionPass
    {
        std::string_view name;
        OptLevel min_level{OptLevel::O1};
        bool (*run)(FunctionPassContext& ctx) = nullptr;
    };

    struct ModulePass
    {
        std::string_view name;
        OptLevel min_level{OptLevel::O1};
        bool (*run)(IrModule& mod, IrContext& ctx, OptLevel level) = nullptr;
    };

    class PassManager
    {
    public:
        void add_function_pass(FunctionPass pass) { m_func_passes.push_back(pass); }
        void add_module_pass(ModulePass pass) { m_module_passes.push_back(pass); }

        [[nodiscard]] IrModule* run(IrModule const& input, IrContext& output_ctx, OptLevel level)
        {
            if (level == OptLevel::O0)
                return const_cast<IrModule*>(&input);

            IrModule* cloned = clone_module(&input, output_ctx);
            if (!cloned)
                return const_cast<IrModule*>(&input);

            for (auto& mp : m_module_passes)
                if (level >= mp.min_level && mp.run)
                    mp.run(*cloned, output_ctx, level);

            for (int iter = 0; iter < 8; ++iter)
            {
                bool any_changed = false;
                for (auto* f : cloned->functions)
                {
                    FunctionPassContext fctx;
                    fctx.func = f;
                    fctx.ctx = &output_ctx;

                    for (auto& fp : m_func_passes)
                        if (level >= fp.min_level && fp.run)
                            if (fp.run(fctx))
                                any_changed = true;
                }
                if (!any_changed)
                    break;
            }

            return cloned;
        }

    private:
        std::vector<FunctionPass> m_func_passes;
        std::vector<ModulePass> m_module_passes;
    };

    [[nodiscard]] PassManager& global_pass_manager()
    {
        static PassManager pm;
        return pm;
    }

} // namespace dcc::ir::pass

namespace dcc::ir::pass
{
    namespace cloner_detail
    {
        struct CloneCtx
        {
            IrContext& dst;
            std::unordered_map<IrType const*, IrType const*> type_map;
            std::unordered_map<IrValue const*, IrValue*> value_map;
            std::unordered_map<IrBasicBlock const*, IrBasicBlock*> bb_map;
            std::unordered_map<IrFunction const*, IrFunction*> func_map;
            std::unordered_map<IrGlobal const*, IrGlobal*> global_map;
            IrModule* dst_module{};
        };

        IrType const* clone_type_impl(IrType const* t, IrContext& dst, CloneCtx& cctx)
        {
            if (!t)
                return nullptr;

            auto it = cctx.type_map.find(t);
            if (it != cctx.type_map.end())
                return it->second;

            IrType const* result = nullptr;

            switch (t->kind)
            {
                case IrTypeKind::Void:
                    result = dst.void_t();
                    break;
                case IrTypeKind::Bool:
                    result = dst.bool_t();
                    break;
                case IrTypeKind::Int: {
                    auto* it_t = static_cast<IrIntType const*>(t);
                    result = dst.int_t(it_t->bits, it_t->is_signed, it_t->is_pointer_sized);
                    break;
                }
                case IrTypeKind::Float: {
                    auto* ft = static_cast<IrFloatType const*>(t);
                    result = dst.float_t(ft->bits);
                    break;
                }
                case IrTypeKind::Pointer: {
                    auto* pt = static_cast<IrPointerType const*>(t);
                    result = dst.pointer_to(clone_type_impl(pt->pointee, dst, cctx), pt->seg);
                    break;
                }
                case IrTypeKind::Aggregate: {
                    auto* at = static_cast<IrAggregateType const*>(t);
                    std::vector<IrType const*> members;
                    for (auto* m : at->members)
                        members.push_back(clone_type_impl(m, dst, cctx));

                    result = dst.aggregate_t(members, at->member_offsets, at->byte_size, at->byte_align, false);
                    break;
                }
                case IrTypeKind::Array: {
                    auto* art = static_cast<IrArrayType const*>(t);
                    result = dst.array_t(clone_type_impl(art->element, dst, cctx), art->count);
                    break;
                }
                case IrTypeKind::Slice: {
                    auto* st = static_cast<IrSliceType const*>(t);
                    result = dst.slice_t(clone_type_impl(st->element, dst, cctx), st->seg);
                    break;
                }
                case IrTypeKind::Func: {
                    auto* ft = static_cast<IrFuncType const*>(t);
                    std::vector<IrType const*> params;
                    for (auto* p : ft->params)
                        params.push_back(clone_type_impl(p, dst, cctx));

                    result = dst.func_t(clone_type_impl(ft->return_type, dst, cctx), params);
                    break;
                }
            }

            if (result)
                cctx.type_map[t] = result;
            return result;
        }

        IrValue* clone_value_impl(IrValue const* v, IrContext& dst, CloneCtx& cctx);

        IrValue* clone_value_impl(IrValue const* v, IrContext& dst, CloneCtx& cctx)
        {
            if (!v)
                return nullptr;

            auto it = cctx.value_map.find(v);
            if (it != cctx.value_map.end())
                return it->second;

            IrValue* result = nullptr;

            switch (v->kind)
            {
                case IrNodeKind::IntConstant: {
                    auto* c = static_cast<IrIntConstant const*>(v);
                    result = dst.int_const(clone_type_impl(c->type, dst, cctx), c->value);
                    break;
                }
                case IrNodeKind::FloatConstant: {
                    auto* c = static_cast<IrFloatConstant const*>(v);
                    result = dst.float_const(clone_type_impl(c->type, dst, cctx), c->value);
                    break;
                }
                case IrNodeKind::BoolConstant: {
                    auto* c = static_cast<IrBoolConstant const*>(v);
                    result = dst.bool_const(c->value);
                    break;
                }
                case IrNodeKind::NullConstant: {
                    auto* c = static_cast<IrNullConstant const*>(v);
                    result = dst.null_const(clone_type_impl(c->type, dst, cctx));
                    break;
                }
                case IrNodeKind::StringConstant: {
                    auto* c = static_cast<IrStringConstant const*>(v);
                    result = dst.string_const(clone_type_impl(c->type, dst, cctx), c->value);
                    break;
                }
                case IrNodeKind::Local: {
                    auto* l = static_cast<IrLocal const*>(v);
                    result = dst.local(l->name, l->id, clone_type_impl(l->type, dst, cctx));
                    break;
                }
                case IrNodeKind::GlobalRef: {
                    auto* gr = static_cast<IrGlobalRef const*>(v);
                    auto* cloned_type = clone_type_impl(gr->type, dst, cctx);
                    if (gr->global)
                    {
                        auto it2 = cctx.global_map.find(gr->global);
                        result = it2 != cctx.global_map.end() ? dst.global_ref(it2->second, cloned_type) : dst.symbol_ref(gr->name, cloned_type);
                    }
                    else if (gr->function)
                    {
                        auto it2 = cctx.func_map.find(gr->function);
                        result = it2 != cctx.func_map.end() ? dst.func_ref(it2->second) : dst.symbol_ref(gr->name, cloned_type);
                    }
                    else
                        result = dst.symbol_ref(gr->name, cloned_type);

                    break;
                }

#define CLONE_BINOP(k, factory)                                                                                                                                \
    case IrNodeKind::k: {                                                                                                                                      \
        auto* bi = static_cast<Ir##k##Inst const*>(v);                                                                                                         \
        result = dst.factory(clone_type_impl(bi->type, dst, cctx), clone_value_impl(bi->lhs, dst, cctx), clone_value_impl(bi->rhs, dst, cctx));                \
        break;                                                                                                                                                 \
    }

                    CLONE_BINOP(Add, add);
                    CLONE_BINOP(Sub, sub);
                    CLONE_BINOP(Mul, mul);
                    CLONE_BINOP(UDiv, udiv);
                    CLONE_BINOP(SDiv, sdiv);
                    CLONE_BINOP(URem, urem);
                    CLONE_BINOP(SRem, srem);
                    CLONE_BINOP(FDiv, fdiv);
                    CLONE_BINOP(FRem, frem);
                    CLONE_BINOP(And, and_);
                    CLONE_BINOP(Or, or_);
                    CLONE_BINOP(Xor, xor_);
                    CLONE_BINOP(Shl, shl);
                    CLONE_BINOP(LShr, lshr);
                    CLONE_BINOP(AShr, ashr);
#undef CLONE_BINOP

                case IrNodeKind::Neg: {
                    auto* u = static_cast<IrNegInst const*>(v);
                    result = dst.neg(clone_type_impl(u->type, dst, cctx), clone_value_impl(u->operand, dst, cctx));
                    break;
                }
                case IrNodeKind::Not: {
                    auto* u = static_cast<IrNotInst const*>(v);
                    result = dst.not_(clone_type_impl(u->type, dst, cctx), clone_value_impl(u->operand, dst, cctx));
                    break;
                }

                case IrNodeKind::CmpEq: {
                    auto* ci = static_cast<IrCmpEqInst const*>(v);
                    result = dst.cmp_eq(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpNe: {
                    auto* ci = static_cast<IrCmpNeInst const*>(v);
                    result = dst.cmp_ne(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpLt: {
                    auto* ci = static_cast<IrCmpLtInst const*>(v);
                    result = dst.cmp_lt(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpLe: {
                    auto* ci = static_cast<IrCmpLeInst const*>(v);
                    result = dst.cmp_le(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpGt: {
                    auto* ci = static_cast<IrCmpGtInst const*>(v);
                    result = dst.cmp_gt(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpGe: {
                    auto* ci = static_cast<IrCmpGeInst const*>(v);
                    result = dst.cmp_ge(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpOLt: {
                    auto* ci = static_cast<IrCmpOLtInst const*>(v);
                    result = dst.cmp_olt(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpOLe: {
                    auto* ci = static_cast<IrCmpOLeInst const*>(v);
                    result = dst.cmp_ole(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpOGt: {
                    auto* ci = static_cast<IrCmpOGtInst const*>(v);
                    result = dst.cmp_ogt(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpOGe: {
                    auto* ci = static_cast<IrCmpOGeInst const*>(v);
                    result = dst.cmp_oge(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpULt: {
                    auto* ci = static_cast<IrCmpULtInst const*>(v);
                    result = dst.cmp_ult(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpULe: {
                    auto* ci = static_cast<IrCmpULeInst const*>(v);
                    result = dst.cmp_ule(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpUGt: {
                    auto* ci = static_cast<IrCmpUGtInst const*>(v);
                    result = dst.cmp_ugt(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }
                case IrNodeKind::CmpUGe: {
                    auto* ci = static_cast<IrCmpUGeInst const*>(v);
                    result = dst.cmp_uge(clone_value_impl(ci->lhs, dst, cctx), clone_value_impl(ci->rhs, dst, cctx));
                    break;
                }

                case IrNodeKind::Alloca: {
                    auto* a = static_cast<IrAllocaInst const*>(v);
                    result =
                        dst.alloca(clone_type_impl(a->type, dst, cctx), clone_type_impl(a->allocated_type, dst, cctx), clone_value_impl(a->count, dst, cctx));

                    if (result)
                        static_cast<IrAllocaInst*>(result)->alignment = a->alignment;

                    break;
                }

                case IrNodeKind::Load: {
                    auto* l = static_cast<IrLoadInst const*>(v);
                    result = dst.load(clone_type_impl(l->type, dst, cctx), clone_value_impl(l->pointer, dst, cctx));
                    if (result)
                        static_cast<IrLoadInst*>(result)->alignment = l->alignment;

                    break;
                }
                case IrNodeKind::LoadVolatile: {
                    auto* l = static_cast<IrLoadVolatileInst const*>(v);
                    result = dst.load_volatile(clone_type_impl(l->type, dst, cctx), clone_value_impl(l->pointer, dst, cctx));
                    if (result)
                        static_cast<IrLoadVolatileInst*>(result)->alignment = l->alignment;

                    break;
                }

                case IrNodeKind::Store: {
                    auto* s = static_cast<IrStoreInst const*>(v);
                    result = dst.store(clone_value_impl(s->value, dst, cctx), clone_value_impl(s->pointer, dst, cctx));
                    if (result)
                        static_cast<IrStoreInst*>(result)->alignment = s->alignment;

                    break;
                }
                case IrNodeKind::StoreVolatile: {
                    auto* s = static_cast<IrStoreVolatileInst const*>(v);
                    result = dst.store_volatile(clone_value_impl(s->value, dst, cctx), clone_value_impl(s->pointer, dst, cctx));
                    if (result)
                        static_cast<IrStoreVolatileInst*>(result)->alignment = s->alignment;

                    break;
                }

                case IrNodeKind::Gep: {
                    auto* g = static_cast<IrGepInst const*>(v);
                    auto* new_gep = dst.gep(clone_type_impl(g->type, dst, cctx), clone_value_impl(g->base, dst, cctx));
                    for (auto const& idx : g->indices)
                    {
                        IrGepInst::Index new_idx;
                        new_idx.kind = idx.kind;
                        new_idx.dynamic_index = clone_value_impl(idx.dynamic_index, dst, cctx);
                        new_idx.field_index = idx.field_index;
                        new_gep->indices.push_back(new_idx);
                    }
                    result = new_gep;
                    break;
                }

#define CLONE_CAST(k, factory)                                                                                                                                 \
    case IrNodeKind::k: {                                                                                                                                      \
        auto* ci = static_cast<Ir##k##Inst const*>(v);                                                                                                         \
        result = dst.factory(clone_type_impl(ci->type, dst, cctx), clone_value_impl(ci->operand, dst, cctx));                                                  \
        break;                                                                                                                                                 \
    }

                    CLONE_CAST(Zext, zext);
                    CLONE_CAST(Sext, sext);
                    CLONE_CAST(Trunc, trunc);
                    CLONE_CAST(FpExt, fpext);
                    CLONE_CAST(FpTrunc, fptrunc);
                    CLONE_CAST(FpToI, fptoi);
                    CLONE_CAST(IToFp, itofp);
                    CLONE_CAST(PtrToI, ptrtoi);
                    CLONE_CAST(IToPtr, itoptr);
                    CLONE_CAST(Bitcast, bitcast);
                    CLONE_CAST(Segcast, segcast);
#undef CLONE_CAST

                case IrNodeKind::Extract: {
                    auto* e = static_cast<IrExtractInst const*>(v);
                    result = dst.extract(clone_type_impl(e->type, dst, cctx), clone_value_impl(e->aggregate, dst, cctx), e->field_index);
                    break;
                }
                case IrNodeKind::Insert: {
                    auto* i = static_cast<IrInsertInst const*>(v);
                    result = dst.insert(clone_type_impl(i->type, dst, cctx), clone_value_impl(i->aggregate, dst, cctx), i->field_index,
                                        clone_value_impl(i->value, dst, cctx));
                    break;
                }
                case IrNodeKind::Aggregate: {
                    auto* a = static_cast<IrAggregateInst const*>(v);
                    auto* new_agg = dst.aggregate(clone_type_impl(a->type, dst, cctx));
                    for (auto* val : a->values)
                        new_agg->values.push_back(clone_value_impl(val, dst, cctx));

                    result = new_agg;
                    break;
                }

                case IrNodeKind::Phi: {
                    auto* p = static_cast<IrPhiInst const*>(v);
                    auto* new_phi = dst.phi(clone_type_impl(p->type, dst, cctx));
                    for (auto const& inc : p->incoming)
                    {
                        auto* cloned_block = cctx.bb_map[inc.block];
                        new_phi->incoming.push_back({clone_value_impl(inc.value, dst, cctx), cloned_block});
                    }

                    result = new_phi;
                    break;
                }

                case IrNodeKind::Call: {
                    auto* c = static_cast<IrCallInst const*>(v);
                    auto* new_call = dst.call(clone_type_impl(c->type, dst, cctx), clone_value_impl(c->callee, dst, cctx));
                    new_call->cc = c->cc;
                    for (auto* arg : c->args)
                        new_call->args.push_back(clone_value_impl(arg, dst, cctx));

                    result = new_call;
                    break;
                }
                case IrNodeKind::CallTail: {
                    auto* c = static_cast<IrCallTailInst const*>(v);
                    auto* new_call = dst.call_tail(clone_type_impl(c->type, dst, cctx), clone_value_impl(c->callee, dst, cctx));
                    new_call->cc = c->cc;
                    for (auto* arg : c->args)
                        new_call->args.push_back(clone_value_impl(arg, dst, cctx));

                    result = new_call;
                    break;
                }

                case IrNodeKind::AtomicLoad: {
                    auto* a = static_cast<IrAtomicLoadInst const*>(v);
                    result = dst.atomic_load(clone_type_impl(a->type, dst, cctx), clone_value_impl(a->pointer, dst, cctx), a->ordering);
                    if (result)
                        static_cast<IrAtomicLoadInst*>(result)->alignment = a->alignment;

                    break;
                }
                case IrNodeKind::AtomicStore: {
                    auto* s = static_cast<IrAtomicStoreInst const*>(v);
                    result = dst.atomic_store(clone_value_impl(s->value, dst, cctx), clone_value_impl(s->pointer, dst, cctx), s->ordering);
                    if (result)
                        static_cast<IrAtomicStoreInst*>(result)->alignment = s->alignment;

                    break;
                }
                case IrNodeKind::AtomicRmw: {
                    auto* r = static_cast<IrAtomicRmwInst const*>(v);
                    result = dst.atomic_rmw(clone_type_impl(r->type, dst, cctx), r->op, clone_value_impl(r->pointer, dst, cctx),
                                            clone_value_impl(r->value, dst, cctx), r->ordering);
                    if (result)
                        static_cast<IrAtomicRmwInst*>(result)->alignment = r->alignment;

                    break;
                }
                case IrNodeKind::Fence: {
                    auto* f = static_cast<IrFenceInst const*>(v);
                    result = dst.fence(f->ordering);
                    break;
                }

                case IrNodeKind::BasicBlock: {
                    auto* bb = static_cast<IrBasicBlock const*>(v);
                    auto it2 = cctx.bb_map.find(bb);
                    result = it2 != cctx.bb_map.end() ? it2->second : nullptr;
                    break;
                }

                case IrNodeKind::Br:
                case IrNodeKind::BrCond:
                case IrNodeKind::Ret:
                case IrNodeKind::Unreachable:
                case IrNodeKind::Switch:
                    break;

                case IrNodeKind::Function:
                case IrNodeKind::Global:
                    break;
            }

            if (result)
            {
                result->name = v->name;
                result->range = v->range;
                cctx.value_map[v] = result;
            }
            return result;
        }

        IrNode* clone_terminator_impl(IrNode const* term, IrContext& dst, CloneCtx& cctx)
        {
            if (!term)
                return nullptr;

            switch (term->kind)
            {
                case IrNodeKind::Br: {
                    auto* br = static_cast<IrBrInst const*>(term);
                    auto it = cctx.bb_map.find(br->target);
                    return it != cctx.bb_map.end() ? dst.br(it->second) : nullptr;
                }
                case IrNodeKind::BrCond: {
                    auto* br = static_cast<IrBrCondInst const*>(term);
                    auto* cond = clone_value_impl(br->condition, dst, cctx);
                    auto* tt = cctx.bb_map[br->true_target];
                    auto* ft = cctx.bb_map[br->false_target];
                    return dst.br_cond(cond, tt, ft);
                }
                case IrNodeKind::Ret: {
                    auto* ret = static_cast<IrRetInst const*>(term);
                    return dst.ret(ret->value ? clone_value_impl(ret->value, dst, cctx) : nullptr);
                }
                case IrNodeKind::Unreachable:
                    return dst.unreachable();
                case IrNodeKind::Switch: {
                    auto* sw = static_cast<IrSwitchInst const*>(term);
                    auto* val = clone_value_impl(sw->value, dst, cctx);
                    auto* def = cctx.bb_map[sw->default_target];
                    auto* new_sw = dst.switch_(val, def);
                    for (auto const& c : sw->cases)
                    {
                        auto* ct = cctx.bb_map[c.target];
                        new_sw->cases.push_back({c.start, c.end, ct});
                    }

                    return new_sw;
                }
                default:
                    return nullptr;
            }
        }

        [[nodiscard]] IrModule* clone_module_impl(IrModule const* src, IrContext& dst)
        {
            if (!src)
                return nullptr;

            cloner_detail::CloneCtx cctx{
                .dst = dst,
                .type_map = {},
                .value_map = {},
                .bb_map = {},
                .func_map = {},
                .global_map = {},
                .dst_module = nullptr,
            };

            auto* mod = dst.module(src->name);
            mod->source_file_id = src->source_file_id;
            cctx.dst_module = mod;

            for (auto* g : src->globals)
            {
                auto* new_g = dst.global(g->name, cloner_detail::clone_type_impl(g->type, dst, cctx), nullptr, g->is_constant);
                new_g->is_dll_import = g->is_dll_import;
                new_g->is_dll_export = g->is_dll_export;
                new_g->linkage = g->linkage;
                new_g->alignment = g->alignment;
                new_g->section = g->section;
                cctx.global_map[g] = new_g;
                mod->globals.push_back(new_g);
            }

            for (auto* f : src->functions)
            {
                auto* ft = static_cast<IrFuncType const*>(cloner_detail::clone_type_impl(f->func_type, dst, cctx));
                auto* new_f = dst.function(f->name, ft);
                new_f->attrs.assign(f->attrs.begin(), f->attrs.end());
                new_f->source_name = f->source_name;
                new_f->decl_file_id = f->decl_file_id;
                new_f->decl_line = f->decl_line;
                new_f->is_dll_import = f->is_dll_import;
                new_f->is_dll_export = f->is_dll_export;
                new_f->linkage = f->linkage;
                new_f->alignment = f->alignment;
                new_f->conv = f->conv;
                new_f->debug_locations.assign(f->debug_locations.begin(), f->debug_locations.end());
                cctx.func_map[f] = new_f;
                mod->functions.push_back(new_f);

                for (auto* bb : f->blocks)
                {
                    auto* new_bb = dst.basic_block(bb->name, bb->id);
                    new_bb->parent = new_f;

                    for (auto* p : bb->params)
                    {
                        auto* local_p = ir_cast<IrLocal const>(p);
                        IrValue* new_p;
                        if (local_p)
                            new_p = dst.local(local_p->name, local_p->id, cloner_detail::clone_type_impl(local_p->type, dst, cctx));
                        else
                            new_p = dst.local(p->name, 0, cloner_detail::clone_type_impl(p->type, dst, cctx));

                        new_bb->params.push_back(new_p);
                        cctx.value_map[p] = new_p;
                    }
                    cctx.bb_map[bb] = new_bb;
                    new_f->blocks.push_back(new_bb);
                }

                if (f->entry_block)
                    new_f->entry_block = cctx.bb_map[f->entry_block];
            }

            for (auto* g : src->globals)
            {
                auto* new_g = cctx.global_map[g];
                if (g->init)
                    new_g->init = cloner_detail::clone_value_impl(g->init, dst, cctx);
            }

            for (auto* f : src->functions)
            {
                for (auto* bb : f->blocks)
                {
                    auto* new_bb = cctx.bb_map[bb];
                    for (auto* inst : bb->instructions)
                    {
                        if (!inst)
                            continue;

                        auto* new_inst = cloner_detail::clone_value_impl(inst, dst, cctx);
                        if (new_inst)
                        {
                            new_inst->name = inst->name;
                            new_bb->instructions.push_back(new_inst);
                        }
                    }

                    new_bb->terminator = cloner_detail::clone_terminator_impl(bb->terminator, dst, cctx);
                }
            }

            return mod;
        }

    } // namespace cloner_detail

    [[nodiscard]] IrModule* clone_module(IrModule const* src, IrContext& dst)
    {
        return cloner_detail::clone_module_impl(src, dst);
    }

} // namespace dcc::ir::pass
