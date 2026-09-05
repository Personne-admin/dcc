import std;
import dcc.ir;
import dcc.target;
import dcc.backend.em64t.isel;
import dcc.backend.em64t.mir;

#include "harness.hh"

using namespace dcc::ir;
using namespace dcc::backend::em64t;

SECTION("em64t: branch selection");

TEST_CASE("integer predicates branch directly and preserve successors")
{
    std::array kinds{IrNodeKind::CmpEq, IrNodeKind::CmpNe, IrNodeKind::CmpLt, IrNodeKind::CmpLe, IrNodeKind::CmpGt,
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
