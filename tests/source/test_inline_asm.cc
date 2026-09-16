import std;
import dcc.ir;
import dcc.sm;
import dcc.target;
import dcc.backend.inline_asm;
import dcc.backend.em64t.mir;
import dcc.backend.em64t.encode;

#include "harness.hh"

using namespace dcc::ir;
using namespace dcc::backend;

namespace
{
    struct AsmFixture
    {
        IrContext ctx;
        dcc::target::TargetConfig target = dcc::target::TargetConfig::host_default();

        IrInlineAsmInst* build(std::string_view tmpl, std::vector<IrAsmOperand> operands, IrAsmDialect dialect,
                               std::vector<std::pair<std::string, std::uint32_t>> refs = {}, std::vector<std::string_view> clobbers = {})
        {
            std::pmr::vector<IrAsmOperand> ops(operands.begin(), operands.end(), ctx.allocator());
            std::pmr::vector<std::string_view> clobs(clobbers.begin(), clobbers.end(), ctx.allocator());
            auto* inst = ctx.inline_asm(std::pmr::string(tmpl, ctx.allocator()), std::move(ops), std::move(clobs), true, false, dialect, ctx.void_t(),
                                        dcc::sm::SourceRange{});
            std::string_view view(inst->template_str);
            for (auto const& [name, index] : refs)
            {
                std::size_t pos = 0;
                while ((pos = view.find(name, pos)) != std::string_view::npos)
                {
                    inst->template_parts.push_back({static_cast<std::uint32_t>(pos), static_cast<std::uint32_t>(name.size()), index});
                    pos += name.size();
                }
            }
            std::size_t pos = 0;
            while ((pos = view.find("%%", pos)) != std::string_view::npos)
            {
                bool covered = false;
                for (auto const& part : inst->template_parts)
                    if (part.offset == pos)
                        covered = true;
                if (!covered)
                    inst->template_parts.push_back({static_cast<std::uint32_t>(pos), 2, 0xFFFFFFFFU});
                pos += 2;
            }
            return inst;
        }

        IrAsmOperand reg_operand(IrAsmOperand::Direction direction, std::string_view reg, IrType const* type, IrValue* value = nullptr)
        {
            IrAsmOperand op;
            op.direction = direction;
            op.placement_kind = IrAsmOperand::PlacementKind::Reg;
            op.reg_name = reg;
            op.type = type;
            op.value = value;
            return op;
        }
    };

    std::vector<std::uint8_t> try_encode(em64t::MInstr const& instr)
    {
        auto result = em64t::encode_single_instruction(instr);
        if (!result.has_value())
            return {0xFF};
        return *result;
    }

    void require_bytes(std::vector<std::uint8_t> const& bytes, std::vector<std::uint8_t> const& expected)
    {
        CHECK(bytes.size() == expected.size());
        CHECK(bytes == expected);
    }

} // namespace

SECTION("inline-asm: shared planning");

TEST_CASE("intel mov with named operands selects registers and encodes")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* value = fx.ctx.int_const(u64, 40);
    std::vector<IrAsmOperand> operands;
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::Out, "", u64));
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "", u64, value));
    auto* inst = fx.build("mov %[dst], %[src]", std::move(operands), IrAsmDialect::Intel, {{"%[dst]", 0}, {"%[src]", 1}});
    auto plan = prepare_inline_asm(*inst, fx.target);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.registers.size() == 2);
    REQUIRE(plan.registers[0] == "rax");
    REQUIRE(plan.registers[1] == "rbx");
    REQUIRE(plan.instructions.size() == 1);
    REQUIRE(plan.instructions[0].opc == em64t::MOpc::MOV64rr);
    std::vector<std::uint8_t> bytes = try_encode(plan.instructions[0]);
    require_bytes(bytes, {0x48, 0x89, 0xD8});
}

TEST_CASE("att add with immediate encodes through the shared encoder")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* value = fx.ctx.int_const(u64, 10);
    std::vector<IrAsmOperand> operands;
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::Out, "rax", u64));
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rbx", u64, value));
    auto* inst = fx.build("add $5, %0", std::move(operands), IrAsmDialect::Att, {{"%0", 0}});
    auto plan = prepare_inline_asm(*inst, fx.target);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.instructions.size() == 1);
    REQUIRE(plan.instructions[0].opc == em64t::MOpc::ADD64ri32);
    std::vector<std::uint8_t> bytes = try_encode(plan.instructions[0]);
    require_bytes(bytes, {0x48, 0x83, 0xC0, 0x05});
}

TEST_CASE("explicit registers are honored without consuming the pool")
{
    AsmFixture fx;
    auto* u32 = fx.ctx.int_t(32, false);
    auto* value = fx.ctx.int_const(u32, 7);
    std::vector<IrAsmOperand> operands;
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::Out, "eax", u32));
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "ecx", u32, value));
    auto* inst = fx.build("xor %0, %0", std::move(operands), IrAsmDialect::Intel, {{"%0", 0}});
    auto plan = prepare_inline_asm(*inst, fx.target);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.registers[0] == "eax");
    REQUIRE(plan.registers[1] == "ecx");
    REQUIRE(plan.instructions[0].opc == em64t::MOpc::XOR32rr);
    std::vector<std::uint8_t> bytes = try_encode(plan.instructions[0]);
    require_bytes(bytes, {0x31, 0xC0});
}

TEST_CASE("memory addressing forms parse to memory operands")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* value = fx.ctx.int_const(u64, 0);
    std::vector<std::string> mem_forms = {"mov %0, [%[p]]",        "mov %0, [%[p] + 8]",      "mov %0, [%[p] - 16]",
                                          "mov %0, [%[p] + %[q]]", "mov %0, [%[p] + %[q]*4]", "mov %0, [%[p] + %[q]*8 + 32]"};
    for (std::string tmpl : mem_forms)
    {
        std::vector<IrAsmOperand> operands;
        operands.push_back(fx.reg_operand(IrAsmOperand::Direction::Out, "rax", u64));
        operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rbx", u64, value));
        operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rcx", u64, value));
        auto* inst = fx.build(tmpl, std::move(operands), IrAsmDialect::Intel, {{"%0", 0}, {"%[p]", 1}, {"%[q]", 2}});
        auto plan = prepare_inline_asm(*inst, fx.target);
        REQUIRE(plan.error.empty());
        REQUIRE(plan.instructions.size() == 1);
        REQUIRE(plan.instructions[0].opc == em64t::MOpc::MOV64rm);
        std::vector<std::uint8_t> bytes = try_encode(plan.instructions[0]);
        REQUIRE(!bytes.empty());
    }
}

TEST_CASE("att memory and suffix forms normalize to the same instructions")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* value = fx.ctx.int_const(u64, 0);
    std::vector<IrAsmOperand> operands;
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::Out, "rax", u64));
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rbx", u64, value));
    auto* inst = fx.build("movq 8(%[p]), %0", std::move(operands), IrAsmDialect::Att, {{"%[p]", 1}, {"%0", 0}});
    auto plan = prepare_inline_asm(*inst, fx.target);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.instructions.size() == 1);
    REQUIRE(plan.instructions[0].opc == em64t::MOpc::MOV64rm);
    try_encode(plan.instructions[0]);
}

TEST_CASE("unsupported native forms produce diagnostics, not crashes")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* value = fx.ctx.int_const(u64, 1);
    std::vector<std::pair<std::string, std::string>> cases = {
        {"mvo %0, %0", "Intel"}, {"mov %0", "Intel"},     {".section .text", "Intel"},     {"lock incq %0", "Att"},     {"call %0", "Intel"},
        {"rdtsc", "Intel"},      {"out %0, %0", "Intel"}, {"add %0, 9999999999", "Intel"}, {"shl %0, %1, %0", "Intel"}, {"mov [%0 + %1*3], %0", "Intel"},
        {"ret", "Intel"},
    };
    for (auto& [tmpl, dialect_name] : cases)
    {
        std::vector<IrAsmOperand> operands;
        operands.push_back(fx.reg_operand(IrAsmOperand::Direction::InOut, "rax", u64, value));
        operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rbx", u64, value));
        IrAsmDialect dialect = dialect_name == "Intel" ? IrAsmDialect::Intel : IrAsmDialect::Att;
        auto* inst = fx.build(tmpl, std::move(operands), dialect, {{"%0", 0}, {"%1", 1}});
        auto plan = prepare_inline_asm(*inst, fx.target);
        REQUIRE(!plan.error.empty());
    }
}

TEST_CASE("llvm lowering rewrites numbering and constraints")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* value = fx.ctx.int_const(u64, 40);
    std::vector<IrAsmOperand> operands;
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::Out, "rax", u64));
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rbx", u64, value));
    auto* inst = fx.build("mov %[v], %0", std::move(operands), IrAsmDialect::Att, {{"%[v]", 1}, {"%0", 0}});
    auto lowered = prepare_llvm_asm(*inst);
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.template_str == "mov $1, $0");
    REQUIRE(lowered.constraints == "=&{rax},{rbx}");
    REQUIRE(lowered.input_operand_indices.size() == 1);
    REQUIRE(lowered.input_operand_indices[0] == 1);
}

TEST_CASE("llvm lowering emits att literal registers with a single percent")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    std::vector<IrAsmOperand> operands;
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::Out, "rax", u64));
    auto* inst = fx.build("mov %%rbx, %0", std::move(operands), IrAsmDialect::Att, {{"%0", 0}});
    auto lowered = prepare_llvm_asm(*inst);
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.template_str == "mov %rbx, $0");
    REQUIRE(lowered.constraints == "=&{rax}");
}

TEST_CASE("llvm lowering ties inout and passes memory addresses twice")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* ptr_ty = fx.ctx.pointer_to(u64);
    auto* acc = fx.ctx.int_const(u64, 1);
    std::vector<IrAsmOperand> operands;
    IrAsmOperand mem;
    mem.direction = IrAsmOperand::Direction::InOut;
    mem.placement_kind = IrAsmOperand::PlacementKind::Mem;
    mem.type = ptr_ty;
    mem.value = fx.ctx.int_const(ptr_ty, 0);
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::InOut, "rax", u64, acc));
    operands.push_back(mem);
    auto* inst = fx.build("add %[slot], %[a]", std::move(operands), IrAsmDialect::Att, {{"%[slot]", 1}, {"%[a]", 0}});
    auto lowered = prepare_llvm_asm(*inst);
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.template_str == "add $1, $0");
    REQUIRE(lowered.constraints == "={rax},=*m,0,*m");
    REQUIRE(lowered.output_address_indices.size() == 1);
    REQUIRE(lowered.output_address_indices[0] == 1);
}

TEST_CASE("llvm lowering emits braced flag output constraint")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* boolean = fx.ctx.bool_t();
    auto* av = fx.ctx.int_const(u64, 1);
    auto* bv = fx.ctx.int_const(u64, 2);
    std::vector<IrAsmOperand> operands;
    IrAsmOperand flag;
    flag.direction = IrAsmOperand::Direction::Out;
    flag.placement_kind = IrAsmOperand::PlacementKind::Flag;
    flag.flag_cond = "equal";
    flag.type = boolean;
    operands.push_back(flag);
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rax", u64, av));
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rbx", u64, bv));
    auto* inst = fx.build("cmp %[x], %[y]", std::move(operands), IrAsmDialect::Intel, {{"%[x]", 1}, {"%[y]", 2}});
    auto lowered = prepare_llvm_asm(*inst);
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.constraints == "={@ccz},{rax},{rbx}");
}

TEST_CASE("clobbers and literal registers are tracked")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* value = fx.ctx.int_const(u64, 3);
    std::vector<IrAsmOperand> operands;
    operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "", u64, value));
    auto* inst = fx.build("mov %0, %%rcx", std::move(operands), IrAsmDialect::Intel, {{"%0", 0}}, {"rcx", "memory"});
    auto plan = prepare_inline_asm(*inst, fx.target);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.clobbers.size() == 2);
    REQUIRE(plan.literal_registers.size() == 1);
    REQUIRE(plan.literal_registers[0] == "rcx");
    REQUIRE(plan.registers[0] != "rcx");
}
