export module dcc.ir.pass;

import std;
import dcc.ir;
import dcc.ir.analysis;
import dcc.target;

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

    [[nodiscard]] bool pass_list_contains(char const* env_var, std::string_view name)
    {
        char const* raw = std::getenv(env_var);
        if (!raw || !*raw)
            return false;

        std::string_view rest{raw};
        while (!rest.empty())
        {
            auto comma = rest.find(',');
            std::string_view item = (comma == std::string_view::npos) ? rest : rest.substr(0, comma);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
                item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
                item.remove_suffix(1);
            if (item == name)
                return true;
            if (comma == std::string_view::npos)
                break;
            rest.remove_prefix(comma + 1);
        }
        return false;
    }

    [[nodiscard]] bool pass_disabled(std::string_view name)
    {
        return pass_list_contains("DCC_DISABLE_PASS", name);
    }

    [[nodiscard]] bool pass_print_after(std::string_view name)
    {
        return pass_list_contains("DCC_PRINT_AFTER", name);
    }

    void benchmark_stats(IrModule const& mod, std::string_view stage)
    {
        std::unordered_map<IrValue const*, std::size_t> sizes;
        std::size_t instructions = 0;
        std::size_t calls = 0;
        std::size_t small_calls = 0;
        std::size_t expansion = 0;
        std::size_t loads = 0;
        std::size_t stores = 0;
        std::size_t allocas = 0;
        std::size_t branches = 0;
        std::size_t phis = 0;
        for (auto* f : mod.functions)
        {
            std::size_t n = 0;
            for (auto* b : f->blocks)
                n += b->instructions.size() + (b->terminator != nullptr);

            sizes[f] = n;
            instructions += n;
            if (n)
                std::println(std::cerr, "DCC_BENCH function {} {} {}", stage, n, f->name);
        }

        for (auto* f : mod.functions)
            for (auto* b : f->blocks)
            {
                if (b->terminator && (b->terminator->kind == IrNodeKind::BrCond || b->terminator->kind == IrNodeKind::Switch))
                    ++branches;

                for (auto* i : b->instructions)
                {
                    loads += i->kind == IrNodeKind::Load;
                    stores += i->kind == IrNodeKind::Store;
                    allocas += i->kind == IrNodeKind::Alloca;
                    phis += i->kind == IrNodeKind::Phi;

                    IrValue const* callee = nullptr;
                    if (auto* c = ir_cast<IrCallInst>(i))
                        callee = c->callee;

                    if (auto* c = ir_cast<IrCallTailInst>(i))
                        callee = c->callee;

                    if (!callee)
                        continue;

                    ++calls;
                    if (auto* ref = ir_cast<IrGlobalRef const>(callee))
                        callee = ref->function;

                    auto it = sizes.find(callee);
                    if (it != sizes.end() && it->second > 0 && it->second <= 20)
                    {
                        ++small_calls;
                        expansion += it->second - 1;
                    }
                }
            }

        std::println(std::cerr, "DCC_BENCH static {} {} {} {} {}", stage, instructions, calls, small_calls, expansion);
        std::println(std::cerr, "DCC_BENCH memory {} {} {} {} {} {}", stage, loads, stores, allocas, branches, phis);
    }

    class PassManager
    {
    public:
        void add_function_pass(FunctionPass pass) { m_func_passes.push_back(pass); }
        void add_module_pass(ModulePass pass) { m_module_passes.push_back(pass); }
        [[nodiscard]] std::span<FunctionPass const> function_passes() const { return m_func_passes; }
        [[nodiscard]] std::span<ModulePass const> module_passes() const { return m_module_passes; }

        [[nodiscard]] IrModule* run(IrModule const& input, IrContext& output_ctx, OptLevel level)
        {
            bool const measure = std::getenv("DCC_BENCH_STATS") != nullptr;
            auto count = [](IrModule const& mod) {
                std::size_t n = 0;
                for (auto* f : mod.functions)
                    for (auto* b : f->blocks)
                        n += b->instructions.size() + (b->terminator != nullptr);

                return n;
            };

            auto start = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            std::unordered_map<std::string_view, double> times;
            if (level == OptLevel::O0)
                return const_cast<IrModule*>(&input);

            IrModule* cloned = clone_module(&input, output_ctx);
            if (!cloned)
                return const_cast<IrModule*>(&input);

            for (auto& mp : m_module_passes)
                if (level >= mp.min_level && mp.run && !pass_disabled(mp.name))
                {
                    bool const changed = mp.run(*cloned, output_ctx, level);
                    if (changed && pass_print_after(mp.name))
                        std::println(std::cerr, ";;; after {} (module)\n{}", mp.name, IrSerializer::dump(cloned));
                }

            for (int iter = 0; iter < 8; ++iter)
            {
                bool any_changed = false;
                for (auto* f : cloned->functions)
                {
                    FunctionPassContext fctx;
                    fctx.func = f;
                    fctx.ctx = &output_ctx;

                    for (auto& fp : m_func_passes)
                        if (level >= fp.min_level && fp.run && !pass_disabled(fp.name))
                        {
                            auto t = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                            bool const changed = fp.run(fctx);
                            if (changed)
                            {
                                any_changed = true;
                                if (pass_print_after(fp.name))
                                    std::println(std::cerr, ";;; after {} on {}\n{}", fp.name, fctx.func ? fctx.func->name : "?",
                                                 IrSerializer::dump(fctx.func));
                            }
                            if (measure)
                                times[fp.name] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
                        }
                }
                if (!any_changed)
                    break;
            }

            if (measure)
            {
                auto pipeline_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                benchmark_stats(*cloned, "after");
                std::println(std::cerr, "DCC_BENCH ir {} {}", count(input), count(*cloned));
                std::println(std::cerr, "DCC_BENCH phase pipeline {}", pipeline_seconds);
                for (auto const& [name, seconds] : times)
                    std::println(std::cerr, "DCC_BENCH phase {} {}", name, seconds);
            }
            return cloned;
        }

    private:
        std::vector<FunctionPass> m_func_passes;
        std::vector<ModulePass> m_module_passes;
    };

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
                    result = dst.pointer_to(clone_type_impl(pt->pointee, dst, cctx), pt->seg, pt->flavor);
                    break;
                }
                case IrTypeKind::Aggregate: {
                    auto* at = static_cast<IrAggregateType const*>(t);
                    auto* placeholder = dst.create_aggregate_placeholder(at->byte_size, at->byte_align);
                    cctx.type_map[t] = placeholder;
                    for (auto* m : at->members)
                        placeholder->members.push_back(clone_type_impl(m, dst, cctx));
                    placeholder->member_offsets.assign(at->member_offsets.begin(), at->member_offsets.end());
                    result = placeholder;
                    break;
                }
                case IrTypeKind::Array: {
                    auto* art = static_cast<IrArrayType const*>(t);
                    result = dst.array_t(clone_type_impl(art->element, dst, cctx), art->count);
                    break;
                }
                case IrTypeKind::Slice: {
                    auto* st = static_cast<IrSliceType const*>(t);
                    result = dst.slice_t(clone_type_impl(st->element, dst, cctx), st->seg, st->flavor);
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
                case IrNodeKind::PointerConstant: {
                    auto* p = static_cast<IrPointerConstant const*>(v);
                    result = dst.pointer_const(clone_type_impl(p->type, dst, cctx), p->offset, p->segment);
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
                        result = it2 != cctx.global_map.end() ? dst.global_ref(it2->second, cloned_type, gr->addend)
                                                              : dst.symbol_ref(gr->name, cloned_type, gr->addend);
                    }
                    else if (gr->function)
                    {
                        auto it2 = cctx.func_map.find(gr->function);
                        result = it2 != cctx.func_map.end() ? dst.func_ref(it2->second, gr->addend) : dst.symbol_ref(gr->name, cloned_type, gr->addend);
                    }
                    else
                        result = dst.symbol_ref(gr->name, cloned_type, gr->addend);

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
#undef CLONE_CAST

                case IrNodeKind::ReadSegment: {
                    result = dst.read_segment(static_cast<IrReadSegmentInst const*>(v)->segment);
                    break;
                }
                case IrNodeKind::MakePointer: {
                    auto* p = static_cast<IrMakePointerInst const*>(v);
                    result =
                        dst.make_pointer(clone_type_impl(p->type, dst, cctx), clone_value_impl(p->offset, dst, cctx), clone_value_impl(p->segment, dst, cctx));
                    break;
                }
                case IrNodeKind::PointerOffset: {
                    auto* p = static_cast<IrPointerOffsetInst const*>(v);
                    result = dst.pointer_offset(clone_value_impl(p->pointer, dst, cctx));
                    break;
                }
                case IrNodeKind::PointerSegment: {
                    auto* p = static_cast<IrPointerSegmentInst const*>(v);
                    result = dst.pointer_segment(clone_value_impl(p->pointer, dst, cctx));
                    break;
                }

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
                case IrNodeKind::AtomicCmpXchg: {
                    auto* x = static_cast<IrAtomicCmpXchgInst const*>(v);
                    result = dst.atomic_cmpxchg(clone_type_impl(x->type, dst, cctx), clone_value_impl(x->pointer, dst, cctx),
                                                clone_value_impl(x->expected, dst, cctx), clone_value_impl(x->desired, dst, cctx), x->success_ordering,
                                                x->failure_ordering);
                    if (result)
                        static_cast<IrAtomicCmpXchgInst*>(result)->alignment = x->alignment;

                    break;
                }
                case IrNodeKind::Fence: {
                    auto* f = static_cast<IrFenceInst const*>(v);
                    result = dst.fence(f->ordering);
                    break;
                }

                case IrNodeKind::InlineAsm: {
                    auto* ia = static_cast<IrInlineAsmInst const*>(v);
                    std::pmr::vector<IrAsmOperand> cloned_operands(dst.allocator());
                    cloned_operands.reserve(ia->operands.size());
                    for (auto const& op : ia->operands)
                    {
                        IrAsmOperand cloned_op;
                        cloned_op.direction = op.direction;
                        cloned_op.placement_kind = op.placement_kind;
                        cloned_op.reg_name = op.reg_name;
                        cloned_op.reg_name2 = op.reg_name2;
                        cloned_op.flag_cond = op.flag_cond;
                        cloned_op.placeholder = op.placeholder;
                        cloned_op.type = clone_type_impl(op.type, dst, cctx);
                        if (op.value)
                        {
                            auto it2 = cctx.value_map.find(op.value);
                            cloned_op.value = it2 != cctx.value_map.end() ? it2->second : clone_value_impl(op.value, dst, cctx);
                        }
                        cloned_operands.push_back(cloned_op);
                    }
                    std::pmr::vector<std::string_view> cloned_clobbers(dst.allocator());
                    cloned_clobbers.reserve(ia->clobbers.size());
                    for (auto const& c : ia->clobbers)
                        cloned_clobbers.push_back(c);
                    result = dst.inline_asm(std::pmr::string(ia->template_str, dst.allocator()), std::move(cloned_operands), std::move(cloned_clobbers),
                                            ia->is_volatile, ia->align_stack, ia->dialect, clone_type_impl(ia->type, dst, cctx), ia->range);
                    static_cast<IrInlineAsmInst*>(result)->template_parts = ia->template_parts;
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
                new_g->is_declaration = g->is_declaration;
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

            for (auto* a : src->module_asms)
            {
                std::pmr::string tmpl(a->template_str, dst.allocator());
                auto* fresh = dst.make<IrModuleAsm>(std::move(tmpl), a->dialect, a->range, dst.allocator());
                for (auto* g : a->globals)
                {
                    auto it = cctx.global_map.find(g);
                    fresh->globals.push_back(it != cctx.global_map.end() ? it->second : g);
                }
                for (auto* f : a->funcs)
                {
                    auto it = cctx.func_map.find(f);
                    fresh->funcs.push_back(it != cctx.func_map.end() ? it->second : f);
                }
                mod->module_asms.push_back(fresh);
            }

            return mod;
        }

    } // namespace cloner_detail

    [[nodiscard]] IrModule* clone_module(IrModule const* src, IrContext& dst)
    {
        return cloner_detail::clone_module_impl(src, dst);
    }

    export class IrVerifier
    {
    public:
        explicit IrVerifier(dcc::target::TargetConfig const& target) : m_target(target) {}

        [[nodiscard]] std::vector<std::string> verify(IrModule const& module)
        {
            for (auto* global : module.globals)
            {
                if (!global)
                    continue;

                verify_type(global->type);
                verify_value(global->init);
            }
            for (auto* function : module.functions)
            {
                if (!function)
                    continue;

                verify_type(function->func_type);
                for (auto* block : function->blocks)
                {
                    if (!block)
                        continue;

                    for (auto* param : block->params)
                        verify_value(param);
                    for (auto* inst : block->instructions)
                        verify_value(inst);
                    if (block->terminator && block->terminator->kind == IrNodeKind::Ret)
                    {
                        auto* ret = static_cast<IrRetInst const*>(block->terminator);
                        if (ret->value && (segmented(ret->value->type) || segmented(function->func_type->return_type)) &&
                            ret->value->type != function->func_type->return_type)
                            error("return changes pointer flavor or segment register");
                        verify_value(ret->value);
                    }
                }
            }
            return std::move(m_errors);
        }

    private:
        dcc::target::TargetConfig const& m_target;
        std::unordered_set<IrType const*> m_seen_types;
        std::unordered_set<IrValue const*> m_seen_values;
        std::vector<std::string> m_errors;

        void error(std::string_view message) { m_errors.emplace_back(message); }

        [[nodiscard]] bool segmented(IrType const* type) const
        {
            auto* ptr = ir_type_cast<IrPointerType>(type);
            return ptr && ptr->flavor != PointerFlavor::Near;
        }

        [[nodiscard]] static bool integer_value(IrValue const* value, std::uint8_t bits)
        {
            auto* type = value ? ir_type_cast<IrIntType>(value->type) : nullptr;
            return type && type->bits == bits && !type->is_signed;
        }

        [[nodiscard]] static bool same_pointer_flavor(IrPointerType const* lhs, IrPointerType const* rhs)
        {
            return lhs && rhs && lhs->flavor == rhs->flavor && lhs->seg == rhs->seg;
        }

        void verify_comparison(IrValue const* lhs, IrValue const* rhs)
        {
            if (lhs && rhs && (segmented(lhs->type) || segmented(rhs->type)) && lhs->type != rhs->type)
                error("comparison mixes pointer flavors or segment registers");
            verify_value(lhs);
            verify_value(rhs);
        }

        [[nodiscard]] bool valid_register(Segment seg) const
        {
            if (m_target.arch == dcc::target::Arch::X86_64)
                return seg == Segment::Fs || seg == Segment::Gs;
            if (m_target.arch == dcc::target::Arch::I8086)
                return seg == Segment::Cs || seg == Segment::Ds || seg == Segment::Es || seg == Segment::Ss;
            return seg != Segment::None;
        }

        void verify_type(IrType const* type)
        {
            if (!type || !m_seen_types.insert(type).second)
                return;

            switch (type->kind)
            {
                case IrTypeKind::Pointer: {
                    auto* ptr = static_cast<IrPointerType const*>(type);
                    if ((ptr->flavor == PointerFlavor::Based) != (ptr->seg != Segment::None))
                        error("pointer flavor and segment register disagree");
                    if (ptr->flavor == PointerFlavor::Based && !valid_register(ptr->seg))
                        error("based pointer uses a register unavailable on the target");
                    if (ptr->flavor == PointerFlavor::Far && m_target.pointer_bits == 64)
                        error("dynamic far pointer is invalid on x86-64");
                    auto const expected_size =
                        ptr->flavor == PointerFlavor::Far ? (m_target.pointer_bits == 16 ? 4u : 8u) : static_cast<unsigned>(m_target.pointer_bits / 8);
                    auto const expected_align =
                        ptr->flavor == PointerFlavor::Far ? (m_target.pointer_bits == 16 ? 2u : 4u) : static_cast<unsigned>(m_target.pointer_align);
                    if (ptr->byte_size != expected_size || ptr->byte_align != expected_align)
                        error("pointer layout does not match target flavor");
                    verify_type(ptr->pointee);
                    break;
                }
                case IrTypeKind::Slice: {
                    auto* slice = static_cast<IrSliceType const*>(type);
                    if ((slice->flavor == PointerFlavor::Based) != (slice->seg != Segment::None))
                        error("slice flavor and segment register disagree");
                    if (slice->flavor == PointerFlavor::Based && !valid_register(slice->seg))
                        error("based slice uses a register unavailable on the target");
                    if (slice->flavor == PointerFlavor::Far && m_target.pointer_bits == 64)
                        error("dynamic far slice is invalid on x86-64");
                    auto const pointer_size =
                        slice->flavor == PointerFlavor::Far ? (m_target.pointer_bits == 16 ? 4u : 8u) : static_cast<unsigned>(m_target.pointer_bits / 8);
                    auto const align =
                        slice->flavor == PointerFlavor::Far ? (m_target.pointer_bits == 16 ? 2u : 4u) : static_cast<unsigned>(m_target.pointer_align);
                    auto const unaligned = pointer_size + static_cast<unsigned>(m_target.pointer_bits / 8);
                    auto const size = (unaligned + align - 1) / align * align;
                    if (slice->byte_size != size || slice->byte_align != align)
                        error("slice layout does not match target flavor");
                    verify_type(slice->element);
                    break;
                }
                case IrTypeKind::Array:
                    verify_type(static_cast<IrArrayType const*>(type)->element);
                    break;
                case IrTypeKind::Aggregate:
                    for (auto* member : static_cast<IrAggregateType const*>(type)->members)
                        verify_type(member);
                    break;
                case IrTypeKind::Func: {
                    auto* func = static_cast<IrFuncType const*>(type);
                    verify_type(func->return_type);
                    for (auto* param : func->params)
                        verify_type(param);
                    break;
                }
                default:
                    break;
            }
        }

        void verify_value(IrValue const* value)
        {
            if (!value || !m_seen_values.insert(value).second)
                return;

            verify_type(value->type);
            switch (value->kind)
            {
                case IrNodeKind::PointerConstant: {
                    auto* constant = static_cast<IrPointerConstant const*>(value);
                    auto* ptr = ir_type_cast<IrPointerType>(value->type);
                    if (!ptr || (ptr->flavor != PointerFlavor::Far && constant->segment != 0) ||
                        (m_target.pointer_bits < 64 && constant->offset >= (std::uint64_t{1} << m_target.pointer_bits)))
                        error("pointer constant has invalid flavor or noncanonical fields");
                    break;
                }
                case IrNodeKind::NullConstant:
                    if (segmented(value->type) && static_cast<IrPointerType const*>(value->type)->flavor == PointerFlavor::Based)
                        error("based pointer cannot contain null");
                    break;
                case IrNodeKind::ReadSegment: {
                    auto* read = static_cast<IrReadSegmentInst const*>(value);
                    auto* result = ir_type_cast<IrIntType>(value->type);
                    if (!valid_register(read->segment) || !result || result->bits != 16 || result->is_signed)
                        error("read_segment has invalid register or result type");
                    break;
                }
                case IrNodeKind::MakePointer: {
                    auto* make = static_cast<IrMakePointerInst const*>(value);
                    auto* ptr = ir_type_cast<IrPointerType>(value->type);
                    if (!ptr || !integer_value(make->offset, m_target.pointer_bits) || (ptr->flavor == PointerFlavor::Far) != (make->segment != nullptr))
                        error("make_pointer has invalid flavor or offset operand");
                    if (make->segment && !integer_value(make->segment, 16))
                        error("make_pointer segment operand must be u16");
                    verify_value(make->offset);
                    verify_value(make->segment);
                    break;
                }
                case IrNodeKind::PointerOffset:
                case IrNodeKind::PointerSegment: {
                    auto* pointer = value->kind == IrNodeKind::PointerOffset ? static_cast<IrPointerOffsetInst const*>(value)->pointer
                                                                             : static_cast<IrPointerSegmentInst const*>(value)->pointer;
                    auto* ptr = pointer ? ir_type_cast<IrPointerType>(pointer->type) : nullptr;
                    if (!ptr || (value->kind == IrNodeKind::PointerSegment && ptr->flavor != PointerFlavor::Far))
                        error("pointer extraction has invalid flavor");
                    auto* result = ir_type_cast<IrIntType>(value->type);
                    auto expected_bits = value->kind == IrNodeKind::PointerSegment ? 16 : m_target.pointer_bits;
                    if (!result || result->bits != expected_bits || result->is_signed)
                        error("pointer extraction has invalid result type");
                    verify_value(pointer);
                    break;
                }
                case IrNodeKind::PtrToI:
                    if (segmented(static_cast<IrPtrToIInst const*>(value)->operand->type))
                        error("integer cast of based or dynamic far pointer");
                    break;
                case IrNodeKind::IToPtr:
                    if (segmented(value->type))
                        error("integer cast to based or dynamic far pointer");
                    break;
                case IrNodeKind::Bitcast: {
                    auto* cast = static_cast<IrBitcastInst const*>(value);
                    auto* src = cast->operand ? ir_type_cast<IrPointerType>(cast->operand->type) : nullptr;
                    auto* dst = ir_type_cast<IrPointerType>(value->type);
                    if ((src && src->flavor != PointerFlavor::Near) || (dst && dst->flavor != PointerFlavor::Near))
                        if (!src || !dst || src->flavor != dst->flavor || src->seg != dst->seg)
                            error("bitcast changes pointer flavor or segment register");
                    break;
                }
                case IrNodeKind::Gep: {
                    auto* gep = static_cast<IrGepInst const*>(value);
                    auto* src = gep->base ? ir_type_cast<IrPointerType>(gep->base->type) : nullptr;
                    auto* dst = ir_type_cast<IrPointerType>(value->type);
                    if ((src && src->flavor != PointerFlavor::Near) || (dst && dst->flavor != PointerFlavor::Near))
                        if (!same_pointer_flavor(src, dst))
                            error("GEP changes pointer flavor or segment register");
                    verify_value(gep->base);
                    for (auto const& index : gep->indices)
                        verify_value(index.dynamic_index);
                    break;
                }
                case IrNodeKind::Load:
                case IrNodeKind::LoadVolatile: {
                    auto* pointer = value->kind == IrNodeKind::Load ? static_cast<IrLoadInst const*>(value)->pointer
                                                                    : static_cast<IrLoadVolatileInst const*>(value)->pointer;
                    auto* ptr = pointer ? ir_type_cast<IrPointerType>(pointer->type) : nullptr;
                    if (ptr && (segmented(pointer->type) || segmented(ptr->pointee)) && ptr->pointee != value->type)
                        error("load changes pointer pointee type");
                    verify_value(pointer);
                    break;
                }
                case IrNodeKind::Store:
                case IrNodeKind::StoreVolatile: {
                    auto* pointer = value->kind == IrNodeKind::Store ? static_cast<IrStoreInst const*>(value)->pointer
                                                                     : static_cast<IrStoreVolatileInst const*>(value)->pointer;
                    auto* stored = value->kind == IrNodeKind::Store ? static_cast<IrStoreInst const*>(value)->value
                                                                    : static_cast<IrStoreVolatileInst const*>(value)->value;
                    auto* ptr = pointer ? ir_type_cast<IrPointerType>(pointer->type) : nullptr;
                    if (ptr && stored && (segmented(pointer->type) || segmented(stored->type) || segmented(ptr->pointee)) && ptr->pointee != stored->type)
                        error("store changes pointer pointee type");
                    verify_value(pointer);
                    verify_value(stored);
                    break;
                }
#define VERIFY_CMP(K)                                                                                                                                          \
    case IrNodeKind::K: {                                                                                                                                      \
        auto* cmp = static_cast<Ir##K##Inst const*>(value);                                                                                                    \
        verify_comparison(cmp->lhs, cmp->rhs);                                                                                                                 \
        break;                                                                                                                                                 \
    }
                    VERIFY_CMP(CmpEq)
                    VERIFY_CMP(CmpNe)
                    VERIFY_CMP(CmpLt)
                    VERIFY_CMP(CmpLe)
                    VERIFY_CMP(CmpGt)
                    VERIFY_CMP(CmpGe)
                    VERIFY_CMP(CmpOLt)
                    VERIFY_CMP(CmpOLe)
                    VERIFY_CMP(CmpOGt)
                    VERIFY_CMP(CmpOGe)
                    VERIFY_CMP(CmpULt)
                    VERIFY_CMP(CmpULe)
                    VERIFY_CMP(CmpUGt)
                    VERIFY_CMP(CmpUGe)
#undef VERIFY_CMP
                case IrNodeKind::Extract: {
                    auto* extract = static_cast<IrExtractInst const*>(value);
                    auto* slice = extract->aggregate ? ir_type_cast<IrSliceType>(extract->aggregate->type) : nullptr;
                    if (slice && slice->flavor != PointerFlavor::Near && extract->field_index == slice_data_index)
                    {
                        auto* field = ir_type_cast<IrPointerType>(value->type);
                        if (!field || field->flavor != slice->flavor || field->seg != slice->seg || field->pointee != slice->element)
                            error("slice extraction changes pointer flavor");
                    }
                    verify_value(extract->aggregate);
                    break;
                }
                case IrNodeKind::Insert: {
                    auto* insert = static_cast<IrInsertInst const*>(value);
                    auto* slice = ir_type_cast<IrSliceType>(value->type);
                    if (slice && slice->flavor != PointerFlavor::Near && insert->field_index == slice_data_index)
                    {
                        auto* field = insert->value ? ir_type_cast<IrPointerType>(insert->value->type) : nullptr;
                        if (!field || field->flavor != slice->flavor || field->seg != slice->seg || field->pointee != slice->element)
                            error("slice insertion changes pointer flavor");
                    }
                    verify_value(insert->aggregate);
                    verify_value(insert->value);
                    break;
                }
                case IrNodeKind::Aggregate: {
                    auto* aggregate = static_cast<IrAggregateInst const*>(value);
                    auto* slice = ir_type_cast<IrSliceType>(value->type);
                    if (slice && slice->flavor != PointerFlavor::Near && !aggregate->values.empty())
                    {
                        auto* field = aggregate->values[0] ? ir_type_cast<IrPointerType>(aggregate->values[0]->type) : nullptr;
                        if (!field || field->flavor != slice->flavor || field->seg != slice->seg || field->pointee != slice->element)
                            error("slice aggregate changes pointer flavor");
                    }
                    for (auto* member : aggregate->values)
                        verify_value(member);
                    break;
                }
                case IrNodeKind::Call:
                case IrNodeKind::CallTail: {
                    auto* callee =
                        value->kind == IrNodeKind::Call ? static_cast<IrCallInst const*>(value)->callee : static_cast<IrCallTailInst const*>(value)->callee;
                    auto const& args =
                        value->kind == IrNodeKind::Call ? static_cast<IrCallInst const*>(value)->args : static_cast<IrCallTailInst const*>(value)->args;
                    auto* callee_pointer = callee ? ir_type_cast<IrPointerType>(callee->type) : nullptr;
                    auto* signature = callee_pointer ? ir_type_cast<IrFuncType>(callee_pointer->pointee)
                                      : callee       ? ir_type_cast<IrFuncType>(callee->type)
                                                     : nullptr;
                    if (signature)
                    {
                        if ((segmented(value->type) || segmented(signature->return_type)) && value->type != signature->return_type)
                            error("call changes based or far return type");
                        auto count = std::min(args.size(), signature->params.size());
                        for (std::size_t i = 0; i < count; ++i)
                            if (args[i] && (segmented(args[i]->type) || segmented(signature->params[i])) && args[i]->type != signature->params[i])
                                error("call mixes pointer flavors or segment registers");
                    }
                    verify_value(callee);
                    for (auto* arg : args)
                        verify_value(arg);
                    break;
                }
                case IrNodeKind::AtomicLoad:
                case IrNodeKind::AtomicStore:
                case IrNodeKind::AtomicRmw:
                case IrNodeKind::AtomicCmpXchg: {
                    IrValue const* pointer = nullptr;
                    if (value->kind == IrNodeKind::AtomicLoad)
                        pointer = static_cast<IrAtomicLoadInst const*>(value)->pointer;
                    else if (value->kind == IrNodeKind::AtomicStore)
                        pointer = static_cast<IrAtomicStoreInst const*>(value)->pointer;
                    else if (value->kind == IrNodeKind::AtomicRmw)
                        pointer = static_cast<IrAtomicRmwInst const*>(value)->pointer;
                    else
                        pointer = static_cast<IrAtomicCmpXchgInst const*>(value)->pointer;
                    verify_value(pointer);
                    break;
                }
                default:
                    break;
            }
        }
    };

} // namespace dcc::ir::pass
