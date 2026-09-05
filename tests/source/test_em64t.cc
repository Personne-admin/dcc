import std;
import dcc.ir;
import dcc.target;
import dcc.backend.em64t.isel;
import dcc.backend.em64t.mir;
import dcc.backend.em64t.regalloc;

#include "harness.hh"

using namespace dcc::ir;
using namespace dcc::backend::em64t;

SECTION("em64t: branch selection");

TEST_CASE("integer predicates branch directly and preserve successors")
{
    std::array kinds{IrNodeKind::CmpEq, IrNodeKind::CmpNe,  IrNodeKind::CmpLt,  IrNodeKind::CmpLe,  IrNodeKind::CmpGt,
                     IrNodeKind::CmpGe, IrNodeKind::CmpULt, IrNodeKind::CmpULe, IrNodeKind::CmpUGt, IrNodeKind::CmpUGe};
    std::array opcodes{MOpc::JE, MOpc::JNE, MOpc::JL, MOpc::JLE, MOpc::JG, MOpc::JGE, MOpc::JB, MOpc::JBE, MOpc::JA, MOpc::JAE};
    for (std::size_t i = 0; i < kinds.size(); ++i)
    {
        for (unsigned multiple : {0u, 1u, 2u})
        {
            IrContext ctx;
            auto* type = ctx.int_t(64, true);
            IrType const* params[] = {type, type};
            auto* func = ctx.function("compare", ir_type_cast<IrFuncType>(ctx.func_t(ctx.bool_t(), params)));
            auto* entry = ctx.basic_block("entry", 0);
            auto* yes = ctx.basic_block("yes", 1);
            auto* no = ctx.basic_block("no", 2);
            func->blocks = {entry, yes, no};
            func->entry_block = entry;
            auto* a = ctx.local("a", 0, type);
            auto* b = ctx.local("b", 1, type);
            entry->params = {a, b};
            auto* cmp = ctx.cmp_eq(a, b);
            cmp->kind = kinds[i];
            entry->instructions.push_back(cmp);
            if (multiple == 2)
            {
                auto* slot = ctx.alloca(ctx.pointer_to(ctx.bool_t()), ctx.bool_t());
                entry->instructions.push_back(slot);
                entry->instructions.push_back(ctx.store(cmp, slot));
            }
            entry->terminator = ctx.br_cond(cmp, yes, no);
            yes->terminator = ctx.ret(multiple == 1 ? static_cast<IrValue*>(cmp) : ctx.bool_const(true));
            no->terminator = ctx.ret(ctx.bool_const(false));
            auto machine = isel_function(*func, dcc::target::TargetConfig::host_default());
            auto const& instructions = machine.blocks.front().instrs;
            REQUIRE(instructions.size() >= 3);
            auto const& branch = instructions[instructions.size() - 2];
            CHECK(branch.opc == (multiple ? MOpc::JNE : opcodes[i]));
            CHECK_EQ(branch.ops[0].label, machine.blocks[1].id);
            CHECK(instructions.back().opc == MOpc::JMP);
            CHECK_EQ(instructions.back().ops[0].label, machine.blocks[2].id);
            CHECK(instructions[instructions.size() - 3].opc == (multiple ? MOpc::TEST64rr : MOpc::CMP64rr));
            unsigned sets = 0;
            for (auto const& inst : instructions)
                if (inst.opc >= MOpc::SETEr && inst.opc <= MOpc::SETAr)
                    ++sets;
            CHECK_EQ(sets, multiple ? 1u : 0u);
        }
    }
}

SECTION("em64t: address selection");

TEST_CASE("byte address arithmetic folds only legal scales and displacements")
{
    for (std::int64_t scale : {1, 2, 3, 4, 8, 16})
        for (std::int64_t displacement : {0LL, 8LL, -16LL, 2147483647LL, -2147483648LL, 2147483648LL})
            for (bool store : {false, true})
            {
                IrContext ctx;
                auto* type = ctx.int_t(64, true);
                auto* pointer = ctx.pointer_to(type);
                IrType const* params[] = {pointer, type};
                auto* func = ctx.function("address", ir_type_cast<IrFuncType>(ctx.func_t(type, params)));
                auto* entry = ctx.basic_block("entry", 0);
                func->blocks.push_back(entry);
                func->entry_block = entry;
                auto* base = ctx.local("base", 0, pointer);
                auto* index = ctx.local("index", 1, type);
                entry->params = {base, index};
                auto* scaled = ctx.mul(type, index, ctx.int_const(type, scale));
                auto* address = ctx.add(pointer, base, scaled);
                auto* displaced = ctx.add(pointer, address, ctx.int_const(type, displacement));
                auto* load = ctx.load(type, displaced);
                entry->instructions = {scaled, address, displaced};
                if (store)
                    entry->instructions.push_back(ctx.store(index, displaced));
                else
                    entry->instructions.push_back(load);
                entry->terminator = ctx.ret(store ? static_cast<IrValue*>(index) : load);
                auto machine = isel_function(*func, dcc::target::TargetConfig::host_default());
                MMem const* memory = nullptr;
                unsigned multiplies = 0;
                for (auto const& inst : machine.blocks.front().instrs)
                {
                    if (inst.opc == MOpc::IMUL64rr)
                        ++multiplies;
                    if (inst.opc == (store ? MOpc::MOV64mr : MOpc::MOV64rm))
                        memory = &inst.ops[store ? 0 : 1].mem;
                }
                REQUIRE(memory != nullptr);
                bool legal = scale == 1 || scale == 2 || scale == 4 || scale == 8;
                if (displacement <= 2147483647LL)
                {
                    CHECK_EQ(memory->scale, legal ? scale : 1);
                    CHECK_EQ(memory->disp, displacement);
                    CHECK(memory->base.is_valid());
                    CHECK(memory->index.is_valid());
                    CHECK_EQ(multiplies, legal ? 0u : 1u);
                }
                else
                    CHECK(memory->disp != static_cast<std::int32_t>(displacement));
            }
}

TEST_CASE("spilled memory base index and value use distinct scratch registers")
{
    MFunction func;
    auto& block = func.create_block("entry");
    func.entry_block_id = block.id;
    auto base = func.new_vreg();
    auto index = func.new_vreg();
    auto value = func.new_vreg();
    block.instrs.push_back(make_mov_ri(base, 4096, 64));
    block.instrs.push_back(make_mov_ri(index, 2, 64));
    block.instrs.push_back(make_mov_ri(value, 73, 64));
    MInstr clobber;
    clobber.opc = MOpc::NOP;
    clobber.implicit_defs = 0xffff;
    block.instrs.push_back(clobber);
    MInstr store;
    store.opc = MOpc::MOV64mr;
    store.num_ops = 2;
    store.ops[0] = MOp::from_mem(MMem::make_indexed(base, index, 8));
    store.ops[1] = MOp::from_reg(value);
    block.instrs.push_back(store);
    MInstr ret;
    ret.opc = MOpc::RET;
    block.instrs.push_back(ret);
    regalloc(func, dcc::target::TargetConfig::host_default());
    bool found = false;
    for (auto const& inst : func.blocks.front().instrs)
        if (inst.opc == MOpc::MOV64mr && inst.ops[0].kind == MOpKind::Mem && inst.ops[0].mem.index.is_valid())
        {
            found = true;
            CHECK(inst.ops[0].mem.base.is_physical());
            CHECK(inst.ops[0].mem.index.is_physical());
            CHECK(inst.ops[0].mem.base != inst.ops[0].mem.index);
            CHECK(inst.ops[0].mem.base != inst.ops[1].reg);
            CHECK(inst.ops[0].mem.index != inst.ops[1].reg);
        }
    CHECK(found);
}

SECTION("em64t: peepholes");

TEST_CASE("redundant self moves are removed after allocation")
{
    MFunction func;
    auto& block = func.create_block("entry");
    func.entry_block_id = block.id;

    MInstr self_mov;
    self_mov.opc = MOpc::MOV64rr;
    self_mov.num_ops = 2;
    self_mov.num_defs = 1;
    self_mov.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RAX));
    self_mov.ops[1] = MOp::from_reg(VReg::phys(PhysReg::RAX));
    block.instrs.push_back(self_mov);

    MInstr self_copy = make_copy(VReg::phys(PhysReg::RCX), VReg::phys(PhysReg::RCX));
    block.instrs.push_back(self_copy);

    MInstr real_copy = make_copy(VReg::phys(PhysReg::RDX), VReg::phys(PhysReg::RSI));
    block.instrs.push_back(real_copy);

    MInstr ret;
    ret.opc = MOpc::RET;
    block.instrs.push_back(ret);

    regalloc(func, dcc::target::TargetConfig::host_default());

    bool has_self_mov = false, has_self_copy = false, has_real_copy = false;
    for (auto const& inst : func.blocks.front().instrs)
    {
        if (inst.opc == MOpc::MOV64rr && inst.ops[0].reg == inst.ops[1].reg)
            has_self_mov = true;
        if (inst.opc == MOpc::COPY && inst.ops[0].kind == MOpKind::Reg && inst.ops[1].kind == MOpKind::Reg && inst.ops[0].reg == inst.ops[1].reg)
            has_self_copy = true;
        if (inst.opc == MOpc::COPY && inst.ops[0].kind == MOpKind::Reg && inst.ops[0].reg == VReg::phys(PhysReg::RDX))
            has_real_copy = true;
    }
    CHECK(!has_self_mov);
    CHECK(!has_self_copy);
    CHECK(has_real_copy);
}
