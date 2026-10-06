import std;
import dcc.ir;
import dcc.ir.analysis;
import dcc.ir.pipeline;

#include "harness.hh"

using namespace dcc::ir;

namespace
{
    IrFunction* find_fn(IrModule* mod, std::string_view sub)
    {
        for (auto* f : mod->functions)
            if (f && f->name.find(sub) != std::string_view::npos)
                return f;
        return nullptr;
    }

    int count_calls_to(IrFunction* caller, std::string_view sub)
    {
        int n = 0;
        for (auto* bb : caller->blocks)
        {
            if (!bb)
                continue;
            for (auto* inst : bb->instructions)
            {
                if (!inst)
                    continue;
                IrValue const* callee = nullptr;
                if (auto* c = ir_cast<IrCallInst>(inst))
                    callee = c->callee;
                else if (auto* ct = ir_cast<IrCallTailInst>(inst))
                    callee = ct->callee;
                else
                    continue;
                std::string_view nm;
                if (auto* gr = ir_cast<IrGlobalRef>(callee))
                    nm = gr->name;
                else if (auto* f = ir_cast<IrFunction>(callee))
                    nm = f->name;
                else
                    continue;
                if (nm.find(sub) != std::string_view::npos)
                    ++n;
            }
        }
        return n;
    }

    bool any_call_marked(IrFunction* caller, std::string_view sub)
    {
        for (auto* bb : caller->blocks)
        {
            if (!bb)
                continue;

            for (auto* inst : bb->instructions)
            {
                if (!inst)
                    continue;
                bool marked = false;
                IrValue const* callee = nullptr;
                if (auto* c = ir_cast<IrCallInst>(inst))
                {
                    marked = c->is_noinline;
                    callee = c->callee;
                }
                else if (auto* ct = ir_cast<IrCallTailInst>(inst))
                {
                    marked = ct->is_noinline;
                    callee = ct->callee;
                }
                else
                    continue;
                if (!marked)
                    continue;
                std::string_view nm;
                if (auto* gr = ir_cast<IrGlobalRef>(callee))
                    nm = gr->name;
                else if (auto* f = ir_cast<IrFunction>(callee))
                    nm = f->name;
                else
                    continue;
                if (nm.find(sub) != std::string_view::npos)
                    return true;
            }
        }
        return false;
    }

    IrModule* run_o1(IrModule* mod, IrContext& out)
    {
        return dcc::ir::pass::global_pass_manager().run(*mod, out, dcc::ir::pass::OptLevel::O1);
    }

    IrFunction* mk_func(IrContext& c, IrModule* m, std::string_view name, IrType const* ret, std::span<IrType const*> params)
    {
        auto* ft = ir_type_cast<IrFuncType>(c.func_t(ret, params));
        auto* f = c.function(name, ft);
        m->functions.push_back(f);
        return f;
    }

    IrBasicBlock* mk_block(IrContext& c, IrFunction* f, std::uint32_t id)
    {
        auto* bb = c.basic_block(id);
        bb->parent = f;
        f->blocks.push_back(bb);
        if (!f->entry_block)
            f->entry_block = bb;
        return bb;
    }

    IrLocal* mk_param(IrContext& c, IrBasicBlock* entry, std::string_view name, std::uint32_t id, IrType const* t)
    {
        auto* p = c.local(name, id, t);
        entry->params.push_back(p);
        return p;
    }

    IrCallInst* mk_call(IrContext& c, IrBasicBlock* bb, IrType const* ret, IrFunction* target, std::span<IrValue*> args)
    {
        auto* call = c.call(ret, c.func_ref(target));
        for (auto* a : args)
            call->args.push_back(a);
        bb->instructions.push_back(call);
        return call;
    }

    TEST_CASE("the default pass manager explicitly registers every built-in pass once")
    {
        auto const& manager = dcc::ir::pass::global_pass_manager();
        auto modules = manager.module_passes();
        auto functions = manager.function_passes();
        REQUIRE(modules.size() == 1);
        CHECK_EQ(modules[0].name, "inline");
        REQUIRE(functions.size() == 3);
        CHECK_EQ(functions[0].name, "mem2reg");
        CHECK_EQ(functions[1].name, "dce");
        CHECK_EQ(functions[2].name, "simplifycfg");
        CHECK_EQ(&manager, &dcc::ir::pass::global_pass_manager());
        CHECK_EQ(manager.module_passes().size(), 1u);
        CHECK_EQ(manager.function_passes().size(), 3u);
    }

    SECTION("ir: dominance queries");

    TEST_CASE("dominance queries terminate at the root and reject blocks outside the tree")
    {
        IrContext in;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* params[] = {in.bool_t()};
        auto* func = mk_func(in, mod, "diamond", i32, params);
        auto* entry = mk_block(in, func, 0);
        auto* left = mk_block(in, func, 1);
        auto* right = mk_block(in, func, 2);
        auto* join = mk_block(in, func, 3);
        auto* unreachable = mk_block(in, func, 4);
        auto* outside = in.basic_block(5);
        auto* condition = mk_param(in, entry, "condition", 0, in.bool_t());
        entry->terminator = in.br_cond(condition, left, right);
        left->terminator = in.br(join);
        right->terminator = in.br(join);
        join->terminator = in.ret(in.int_const(i32, 0));
        unreachable->terminator = in.ret(in.int_const(i32, 1));
        auto rpo = analysis::compute_rpo(*func);
        auto dom = analysis::DomTree::build(*func, rpo, analysis::build_pred_map(*func));
        CHECK(dom.dominates(entry, entry));
        CHECK(dom.dominates(entry, left));
        CHECK(dom.dominates(entry, join));
        CHECK(dom.dominates(join, join));
        CHECK(!dom.dominates(left, join));
        CHECK(!dom.dominates(right, left));
        CHECK(!dom.dominates(join, entry));
        CHECK(!dom.dominates(entry, unreachable));
        CHECK(!dom.dominates(unreachable, join));
        CHECK(!dom.dominates(unreachable, unreachable));
        CHECK(!dom.dominates(outside, entry));
        CHECK(!dom.dominates(entry, outside));
        CHECK(!dom.dominates(outside, outside));
        CHECK(!dom.dominates(nullptr, entry));
        CHECK(!dom.dominates(entry, nullptr));
        CHECK(dom.strictly_dominates(entry, join));
        CHECK(!dom.strictly_dominates(entry, entry));
    }

    SECTION("ir: dominance order");

    TEST_CASE("dominance children are independent of allocation order")
    {
        std::array<unsigned, 4> allocation_order{0, 1, 2, 3};
        do
        {
            IrContext in;
            auto* mod = in.module("t");
            auto* i32 = in.int_t(32, true);
            IrType const* params[] = {in.bool_t()};
            auto* func = mk_func(in, mod, "diamond", i32, params);
            std::array<IrBasicBlock*, 4> blocks{};
            for (auto index : allocation_order)
            {
                blocks[index] = in.basic_block(index);
                blocks[index]->parent = func;
            }
            func->blocks.assign(blocks.begin(), blocks.end());
            func->entry_block = blocks[0];
            auto* condition = mk_param(in, blocks[0], "condition", 0, in.bool_t());
            blocks[0]->terminator = in.br_cond(condition, blocks[1], blocks[2]);
            blocks[1]->terminator = in.br(blocks[3]);
            blocks[2]->terminator = in.br(blocks[3]);
            blocks[3]->terminator = in.ret(in.int_const(i32, 0));
            auto rpo = analysis::compute_rpo(*func);
            auto dom = analysis::DomTree::build(*func, rpo, analysis::build_pred_map(*func));
            std::vector<IrBasicBlock const*> expected;
            for (auto* block : rpo)
                if (block != func->entry_block)
                    expected.push_back(block);
            CHECK(dom.children.at(func->entry_block) == expected);
            CHECK(dom.dominates(blocks[0], blocks[3]));
            CHECK(!dom.dominates(blocks[1], blocks[3]));
        } while (std::next_permutation(allocation_order.begin(), allocation_order.end()));
    }

    SECTION("inline: callee gates");

    TEST_CASE("small leaf inlines away")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* two[] = {i32, i32};
        auto* small = mk_func(in, mod, "small_add", i32, two);
        auto* se = mk_block(in, small, 0);
        auto* pa = mk_param(in, se, "a", 0, i32);
        auto* pb = mk_param(in, se, "b", 1, i32);
        auto* sum = in.add(i32, pa, pb);
        se->instructions.push_back(sum);
        se->terminator = in.ret(sum);
        IrType const* one[] = {i32};
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* px = mk_param(in, te, "x", 0, i32);
        IrValue* args[] = {px, px};
        auto* call = mk_call(in, te, i32, small, args);
        te->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "small_add"), 0);
    }

    TEST_CASE("oversize callee refused")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* big = mk_func(in, mod, "big_body", i32, one);
        auto* be = mk_block(in, big, 0);
        auto* pb = mk_param(in, be, "x", 0, i32);
        IrValue* acc = pb;
        for (int i = 0; i < 21; ++i)
        {
            auto* nxt = in.add(i32, acc, pb);
            be->instructions.push_back(nxt);
            acc = nxt;
        }
        be->terminator = in.ret(acc);
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* px = mk_param(in, te, "x", 0, i32);
        IrValue* args[] = {px};
        auto* call = mk_call(in, te, i32, big, args);
        te->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "big_body"), 1);
    }

    TEST_CASE("multi exit refused")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* tw = mk_func(in, mod, "two_exit", i32, one);
        auto* e = mk_block(in, tw, 0);
        auto* px = mk_param(in, e, "x", 0, i32);
        auto* t = mk_block(in, tw, 1);
        auto* f = mk_block(in, tw, 2);
        auto* z = in.int_const(i32, 0);
        auto* c = in.cmp_gt(px, z);
        e->instructions.push_back(c);
        e->terminator = in.br_cond(c, t, f);
        t->terminator = in.ret(in.int_const(i32, 1));
        f->terminator = in.ret(in.int_const(i32, 2));
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* qx = mk_param(in, te, "x", 0, i32);
        IrValue* args[] = {qx};
        auto* call = mk_call(in, te, i32, tw, args);
        te->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "two_exit"), 1);
    }

    TEST_CASE("aggregate return refused")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* mem[] = {i32};
        std::uint64_t off[] = {0};
        auto* agg = in.aggregate_t(mem, off, 4, 4, false);
        auto* mk = mk_func(in, mod, "mk_agg", agg, std::span<IrType const*>{});
        auto* e = mk_block(in, mk, 0);
        auto* av = in.aggregate(agg);
        av->values.push_back(in.int_const(i32, 7));
        e->instructions.push_back(av);
        e->terminator = in.ret(av);
        IrType const* one[] = {i32};
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* qx = mk_param(in, te, "x", 0, i32);
        (void)qx;
        auto* call = in.call(agg, in.func_ref(mk));
        te->instructions.push_back(call);
        auto* ex = in.extract(i32, call, 0);
        te->instructions.push_back(ex);
        te->terminator = in.ret(ex);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "mk_agg"), 1);
    }

    SECTION("inline: safety gates");

    TEST_CASE("self recursive refused")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* rec = mk_func(in, mod, "self_rec", i32, one);
        auto* e = mk_block(in, rec, 0);
        auto* px = mk_param(in, e, "x", 0, i32);
        IrValue* args[] = {px};
        auto* call = mk_call(in, e, i32, rec, args);
        e->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_rec = find_fn(res, "self_rec");
        REQUIRE(out_rec != nullptr);
        CHECK_EQ(count_calls_to(out_rec, "self_rec"), 1);
    }

    TEST_CASE("mutual cycle refused")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* fa = mk_func(in, mod, "cyc_a", i32, one);
        auto* fb = mk_func(in, mod, "cyc_b", i32, one);
        auto* ea = mk_block(in, fa, 0);
        auto* pa = mk_param(in, ea, "x", 0, i32);
        IrValue* aa[] = {pa};
        auto* ca = mk_call(in, ea, i32, fb, aa);
        ea->terminator = in.ret(ca);
        auto* eb = mk_block(in, fb, 0);
        auto* pb = mk_param(in, eb, "x", 0, i32);
        IrValue* ab[] = {pb};
        auto* cb = mk_call(in, eb, i32, fa, ab);
        eb->terminator = in.ret(cb);
        auto* res = run_o1(mod, out);
        auto* out_a = find_fn(res, "cyc_a");
        auto* out_b = find_fn(res, "cyc_b");
        REQUIRE(out_a != nullptr);
        REQUIRE(out_b != nullptr);
        CHECK_EQ(count_calls_to(out_a, "cyc_b"), 1);
        CHECK_EQ(count_calls_to(out_b, "cyc_a"), 1);
    }

    TEST_CASE("dynamic alloca refused")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* dy = mk_func(in, mod, "dyn_slot", i32, one);
        auto* e = mk_block(in, dy, 0);
        auto* px = mk_param(in, e, "x", 0, i32);
        auto* slot = in.alloca(in.pointer_to(i32), i32, px);
        e->instructions.push_back(slot);
        e->terminator = in.ret(in.int_const(i32, 0));
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* qx = mk_param(in, te, "x", 0, i32);
        IrValue* args[] = {qx};
        auto* call = mk_call(in, te, i32, dy, args);
        te->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "dyn_slot"), 1);
    }

    TEST_CASE("noinline refused")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* ni = mk_func(in, mod, "stay_out", i32, one);
        ni->attrs.push_back({IrFuncAttr::NoInline, {}});
        auto* e = mk_block(in, ni, 0);
        auto* px = mk_param(in, e, "x", 0, i32);
        e->terminator = in.ret(px);
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* qx = mk_param(in, te, "x", 0, i32);
        IrValue* args[] = {qx};
        auto* call = mk_call(in, te, i32, ni, args);
        te->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "stay_out"), 1);
    }

    TEST_CASE("inline overrides size")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* big = mk_func(in, mod, "forced_big", i32, one);
        big->attrs.push_back({IrFuncAttr::Inline, {}});
        auto* e = mk_block(in, big, 0);
        auto* px = mk_param(in, e, "x", 0, i32);
        IrValue* acc = px;
        for (int i = 0; i < 25; ++i)
        {
            auto* nxt = in.add(i32, acc, px);
            e->instructions.push_back(nxt);
            acc = nxt;
        }
        e->terminator = in.ret(acc);
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* qx = mk_param(in, te, "x", 0, i32);
        IrValue* args[] = {qx};
        auto* call = mk_call(in, te, i32, big, args);
        te->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "forced_big"), 0);
    }

    TEST_CASE("void callee inlines")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* vv = mk_func(in, mod, "do_void", in.void_t(), std::span<IrType const*>{});
        auto* e = mk_block(in, vv, 0);
        e->terminator = in.ret(nullptr);
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* qx = mk_param(in, te, "x", 0, i32);
        (void)qx;
        auto* call = in.call(in.void_t(), in.func_ref(vv));
        te->instructions.push_back(call);
        te->terminator = in.ret(in.int_const(i32, 3));
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "do_void"), 0);
    }

    TEST_CASE("calltail site inlines")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* small = mk_func(in, mod, "tail_small", i32, one);
        auto* se = mk_block(in, small, 0);
        auto* ps = mk_param(in, se, "x", 0, i32);
        auto* sum = in.add(i32, ps, ps);
        se->instructions.push_back(sum);
        se->terminator = in.ret(sum);
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* qx = mk_param(in, te, "x", 0, i32);
        auto* call = in.call_tail(i32, in.func_ref(small));
        call->args.push_back(qx);
        te->instructions.push_back(call);
        te->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "tail_small"), 0);
    }

    SECTION("inline: site gate");

    static IrFunction* mk_tag_query(IrContext& c, IrModule* m, std::string_view name)
    {
        auto* u8 = c.int_t(8, false);
        IrType const* mem[] = {u8};
        std::uint64_t off[] = {0};
        auto* agg = c.aggregate_t(mem, off, 1, 1, false);
        auto* ptr = c.pointer_to(agg);
        IrType const* one[] = {ptr};
        auto* f = mk_func(c, m, name, c.bool_t(), one);
        auto* entry = mk_block(c, f, 0);
        auto* self = mk_param(c, entry, "self", 0, ptr);
        auto* test = mk_block(c, f, 1);
        auto* bt = mk_block(c, f, 2);
        auto* bf = mk_block(c, f, 3);
        auto* merge = mk_block(c, f, 4);
        auto* v = c.load(agg, self);
        entry->instructions.push_back(v);
        entry->terminator = c.br(test);
        auto* t = c.extract(u8, v, 0);
        test->instructions.push_back(t);
        auto* e = c.cmp_eq(t, c.int_const(u8, 0));
        test->instructions.push_back(e);
        test->terminator = c.br_cond(e, bt, bf);
        bt->terminator = c.br(merge);
        bf->terminator = c.br(merge);
        auto* phi = c.phi(c.bool_t());
        phi->incoming.push_back({c.bool_const(true), bt});
        phi->incoming.push_back({c.bool_const(false), bf});
        merge->instructions.push_back(phi);
        merge->terminator = c.ret(phi);
        return f;
    }

    static IrType const* tag_agg(IrContext& c)
    {
        auto* u8 = c.int_t(8, false);
        IrType const* mem[] = {u8};
        std::uint64_t off[] = {0};
        return c.aggregate_t(mem, off, 1, 1, false);
    }

    TEST_CASE("const tag inlines")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* tq = mk_tag_query(in, mod, "tag_q");
        auto* agg = tag_agg(in);
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* e = mk_block(in, top, 0);
        auto* t = mk_block(in, top, 1);
        auto* f = mk_block(in, top, 2);
        auto* slot = in.alloca(in.pointer_to(agg), agg);
        e->instructions.push_back(slot);
        auto* lit = in.aggregate(agg);
        lit->values.push_back(in.int_const(in.int_t(8, false), 0));
        e->instructions.push_back(lit);
        auto* st = in.store(lit, slot);
        e->instructions.push_back(st);
        IrValue* args[] = {slot};
        auto* call = mk_call(in, e, in.bool_t(), tq, args);
        e->terminator = in.br_cond(call, t, f);
        t->terminator = in.ret(in.int_const(i32, 1));
        f->terminator = in.ret(in.int_const(i32, 2));
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "tag_q"), 0);
    }

    TEST_CASE("data tag refused and marked")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* tq = mk_tag_query(in, mod, "tag_q");
        auto* agg = tag_agg(in);
        auto* i32 = in.int_t(32, true);
        auto* opq = mk_func(in, mod, "opaque_src", agg, std::span<IrType const*>{});
        opq->attrs.push_back({IrFuncAttr::NoInline, {}});
        auto* oe = mk_block(in, opq, 0);
        auto* ov = in.aggregate(agg);
        ov->values.push_back(in.int_const(in.int_t(8, false), 9));
        oe->instructions.push_back(ov);
        oe->terminator = in.ret(ov);
        IrType const* one[] = {i32};
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* e = mk_block(in, top, 0);
        auto* t = mk_block(in, top, 1);
        auto* f = mk_block(in, top, 2);
        auto* slot = in.alloca(in.pointer_to(agg), agg);
        e->instructions.push_back(slot);
        auto* g = in.call(agg, in.func_ref(opq));
        e->instructions.push_back(g);
        auto* st = in.store(g, slot);
        e->instructions.push_back(st);
        IrValue* args[] = {slot};
        auto* call = mk_call(in, e, in.bool_t(), tq, args);
        e->terminator = in.br_cond(call, t, f);
        t->terminator = in.ret(in.int_const(i32, 1));
        f->terminator = in.ret(in.int_const(i32, 2));
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "tag_q"), 1);
        CHECK(any_call_marked(out_top, "tag_q"));
    }

    TEST_CASE("forwarder collapses")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* leaf = mk_func(in, mod, "leaf_add", i32, one);
        auto* le = mk_block(in, leaf, 0);
        auto* lp = mk_param(in, le, "x", 0, i32);
        auto* ls = in.add(i32, lp, lp);
        le->instructions.push_back(ls);
        le->terminator = in.ret(ls);
        auto* mid = mk_func(in, mod, "mid_fwd", i32, one);
        auto* me = mk_block(in, mid, 0);
        auto* mp = mk_param(in, me, "x", 0, i32);
        IrValue* ma[] = {mp};
        auto* mc = mk_call(in, me, i32, leaf, ma);
        me->terminator = in.ret(mc);
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* tp = mk_param(in, te, "x", 0, i32);
        IrValue* ta[] = {tp};
        auto* tc = mk_call(in, te, i32, mid, ta);
        te->terminator = in.ret(tc);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "mid_fwd"), 0);
        CHECK_EQ(count_calls_to(out_top, "leaf_add"), 0);
    }

    TEST_CASE("forced multi exit inlines")
    {
        IrContext in, out;
        auto* mod = in.module("t");
        auto* i32 = in.int_t(32, true);
        IrType const* one[] = {i32};
        auto* tw = mk_func(in, mod, "forced_two", i32, one);
        tw->attrs.push_back({IrFuncAttr::Inline, {}});
        auto* e = mk_block(in, tw, 0);
        auto* px = mk_param(in, e, "x", 0, i32);
        auto* t = mk_block(in, tw, 1);
        auto* f = mk_block(in, tw, 2);
        auto* c = in.cmp_gt(px, in.int_const(i32, 0));
        e->instructions.push_back(c);
        e->terminator = in.br_cond(c, t, f);
        t->terminator = in.ret(in.int_const(i32, 1));
        f->terminator = in.ret(in.int_const(i32, 2));
        auto* top = mk_func(in, mod, "top_main", i32, one);
        auto* te = mk_block(in, top, 0);
        auto* qx = mk_param(in, te, "x", 0, i32);
        IrValue* args[] = {qx};
        auto* call = mk_call(in, te, i32, tw, args);
        te->terminator = in.ret(call);
        auto* res = run_o1(mod, out);
        auto* out_top = find_fn(res, "top_main");
        REQUIRE(out_top != nullptr);
        CHECK_EQ(count_calls_to(out_top, "forced_two"), 0);
    }

} // anonymous namespace
