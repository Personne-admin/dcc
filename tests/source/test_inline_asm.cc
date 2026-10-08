#if defined(__linux__) && defined(__x86_64__)
#include <asm/prctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

import std;
import dcc.ir;
import dcc.sm;
import dcc.target;
import dcc.backend.inline_asm;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.backend.x86.prefix;

#include "harness.hh"

using namespace dcc::ir;
using namespace dcc::backend;

SECTION("x86: legacy prefixes");

TEST_CASE("operand address and segment overrides follow x86 prefix groups")
{
    using namespace dcc::backend::x86;
    std::vector<std::uint8_t> bytes;
    append_legacy_prefixes(bytes, EncodeMode::Long64, 16, 32, SegmentOverride::GS);
    CHECK(bytes == (std::vector<std::uint8_t>{0x65, 0x67, 0x66}));

    bytes.clear();
    append_legacy_prefixes(bytes, EncodeMode::Long64, 64, 64, SegmentOverride::FS);
    CHECK(bytes == (std::vector<std::uint8_t>{0x64}));

    bytes.clear();
    append_legacy_prefixes(bytes, EncodeMode::Protected32, 16, 16, SegmentOverride::SS);
    CHECK(bytes == (std::vector<std::uint8_t>{0x36, 0x67, 0x66}));

    bytes.clear();
    append_legacy_prefixes(bytes, EncodeMode::Protected32, 32, 32, SegmentOverride::None);
    CHECK(bytes.empty());

    bytes.clear();
    append_legacy_prefixes(bytes, EncodeMode::Real16, 32, 32, SegmentOverride::ES);
    CHECK(bytes == (std::vector<std::uint8_t>{0x26, 0x67, 0x66}));

    bytes.clear();
    append_legacy_prefixes(bytes, EncodeMode::Real16, 16, 16, SegmentOverride::CS);
    CHECK(bytes == (std::vector<std::uint8_t>{0x2e}));

    bytes.clear();
    append_legacy_prefixes(bytes, EncodeMode::Long64, 32, 64, SegmentOverride::DS);
    CHECK(bytes == (std::vector<std::uint8_t>{0x3e}));
}

TEST_CASE("segment-overridden memory operands encode absolute and register forms")
{
    using namespace dcc::backend::x86;
    auto load = [](MMem mem) {
        MInstr instr;
        instr.opc = MOpc::MOV64rm;
        instr.num_ops = 2;
        instr.num_defs = 1;
        instr.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RAX));
        instr.ops[1] = MOp::from_mem(mem);
        return encode_single_instruction(instr, EncodeMode::Long64);
    };
    auto fs_zero = load(MMem::make_segment_absolute(SegmentOverride::FS, 0));
    REQUIRE(fs_zero.has_value());
    CHECK(*fs_zero == (std::vector<std::uint8_t>{0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}));
    auto gs_teb = load(MMem::make_segment_absolute(SegmentOverride::GS, 0x30));
    REQUIRE(gs_teb.has_value());
    CHECK(*gs_teb == (std::vector<std::uint8_t>{0x65, 0x48, 0x8b, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00}));
    auto fs_reg = load(MMem::make_base_disp(VReg::phys(PhysReg::RCX)).with_segment(SegmentOverride::FS));
    REQUIRE(fs_reg.has_value());
    CHECK(*fs_reg == (std::vector<std::uint8_t>{0x64, 0x48, 0x8b, 0x01}));
    auto gs_indexed = load(MMem::make_indexed(VReg::phys(PhysReg::RCX), VReg::phys(PhysReg::RDX), 8, 0x10).with_segment(SegmentOverride::GS));
    REQUIRE(gs_indexed.has_value());
    CHECK(*gs_indexed == (std::vector<std::uint8_t>{0x65, 0x48, 0x8b, 0x44, 0xd1, 0x10}));
    auto gs_extended = load(MMem::make_base_disp(VReg::phys(PhysReg::R9), 8).with_segment(SegmentOverride::GS));
    REQUIRE(gs_extended.has_value());
    CHECK(*gs_extended == (std::vector<std::uint8_t>{0x65, 0x49, 0x8b, 0x41, 0x08}));

    MInstr store;
    store.opc = MOpc::MOV64mr;
    store.num_ops = 2;
    store.ops[0] = MOp::from_mem(MMem::make_base_disp(VReg::phys(PhysReg::RCX)).with_segment(SegmentOverride::FS));
    store.ops[1] = MOp::from_reg(VReg::phys(PhysReg::RAX));
    auto stored = encode_single_instruction(store, EncodeMode::Long64);
    REQUIRE(stored.has_value());
    CHECK(*stored == (std::vector<std::uint8_t>{0x64, 0x48, 0x89, 0x01}));
}

TEST_CASE("instruction prefixes come from operand width and memory address size")
{
    using namespace dcc::backend::x86;
    CHECK_EQ(operand_bits(MOpc::MOV8rr), 8u);
    CHECK_EQ(operand_bits(MOpc::MOV16rm), 16u);
    CHECK_EQ(operand_bits(MOpc::LOCK_XOR16mr), 16u);
    CHECK_EQ(operand_bits(MOpc::MOV32rr), 32u);
    CHECK_EQ(operand_bits(MOpc::MOVZX32_16rr), 32u);
    CHECK_EQ(operand_bits(MOpc::CDQ), 32u);
    CHECK_EQ(operand_bits(MOpc::MOV64rm), 64u);
    CHECK_EQ(operand_bits(MOpc::REPSTOS), 8u);
    CHECK_EQ(operand_bits(MOpc::MOVQ64rr), 0u);
    CHECK_EQ(operand_bits(MOpc::RET), 0u);

    auto encode = [](MOpc opc, MOp dst, MOp src) {
        MInstr instr;
        instr.opc = opc;
        instr.num_ops = 2;
        instr.ops[0] = dst;
        instr.ops[1] = src;
        return encode_single_instruction(instr, EncodeMode::Long64);
    };
    auto reg = [](PhysReg r) { return MOp::from_reg(VReg::phys(r)); };
    auto mem32 = [](PhysReg base, std::int32_t disp) {
        auto mem = MMem::make_base_disp(VReg::phys(base), disp);
        mem.address_bits = 32;
        return mem;
    };

    auto segmented16 =
        encode(MOpc::MOV16rm, reg(PhysReg::RAX), MOp::from_mem(MMem::make_base_disp(VReg::phys(PhysReg::RCX)).with_segment(SegmentOverride::FS)));
    REQUIRE(segmented16.has_value());
    CHECK(*segmented16 == (std::vector<std::uint8_t>{0x64, 0x66, 0x8b, 0x01}));

    auto address32 = encode(MOpc::MOV32rm, reg(PhysReg::RAX), MOp::from_mem(mem32(PhysReg::RCX, 0)));
    REQUIRE(address32.has_value());
    CHECK(*address32 == (std::vector<std::uint8_t>{0x67, 0x40, 0x8b, 0x01}));

    auto all_prefixes = encode(MOpc::MOV16rm, reg(PhysReg::RAX), MOp::from_mem(mem32(PhysReg::RCX, 4).with_segment(SegmentOverride::GS)));
    REQUIRE(all_prefixes.has_value());
    CHECK(*all_prefixes == (std::vector<std::uint8_t>{0x65, 0x67, 0x66, 0x8b, 0x41, 0x04}));

    auto locked16 = encode(MOpc::LOCK_CMPXCHG16mr, MOp::from_mem(MMem::make_base_disp(VReg::phys(PhysReg::RCX))), reg(PhysReg::RDX));
    REQUIRE(locked16.has_value());
    CHECK(*locked16 == (std::vector<std::uint8_t>{0x66, 0xf0, 0x0f, 0xb1, 0x11}));

    CHECK(!encode(MOpc::MOV16rm, reg(PhysReg::RAX), reg(PhysReg::RCX)).has_value());
    CHECK(!encode(MOpc::MOV16mr, reg(PhysReg::RAX), reg(PhysReg::RCX)).has_value());
}

TEST_CASE("register destination alu operations with memory sources load from memory")
{
    using namespace dcc::backend::x86;
    auto encode = [](MOpc opc, PhysReg reg) {
        MInstr instr;
        instr.opc = opc;
        instr.num_ops = 3;
        instr.num_defs = 1;
        instr.ops[0] = MOp::from_reg(VReg::phys(reg));
        instr.ops[1] = MOp::from_reg(VReg::phys(reg));
        instr.ops[2] = MOp::from_mem(MMem::make_base_disp(VReg::phys(PhysReg::RCX)));
        auto bytes = encode_single_instruction(instr, EncodeMode::Long64);
        return bytes.value_or(std::vector<std::uint8_t>{});
    };
    CHECK(encode(MOpc::ADD64rm, PhysReg::RAX) == (std::vector<std::uint8_t>{0x48, 0x03, 0x01}));
    CHECK(encode(MOpc::SUB64rm, PhysReg::RAX) == (std::vector<std::uint8_t>{0x48, 0x2b, 0x01}));
    CHECK(encode(MOpc::AND64rm, PhysReg::RAX) == (std::vector<std::uint8_t>{0x48, 0x23, 0x01}));
    CHECK(encode(MOpc::OR64rm, PhysReg::RAX) == (std::vector<std::uint8_t>{0x48, 0x0b, 0x01}));
    CHECK(encode(MOpc::XOR64rm, PhysReg::RAX) == (std::vector<std::uint8_t>{0x48, 0x33, 0x01}));
    CHECK(encode(MOpc::CMP64rm, PhysReg::RAX) == (std::vector<std::uint8_t>{0x48, 0x3b, 0x01}));
    CHECK(encode(MOpc::ADD32rm, PhysReg::RDX) == (std::vector<std::uint8_t>{0x40, 0x03, 0x11}));
    CHECK(encode(MOpc::SUB32rm, PhysReg::RDX) == (std::vector<std::uint8_t>{0x40, 0x2b, 0x11}));
    CHECK(encode(MOpc::CMP32rm, PhysReg::RDX) == (std::vector<std::uint8_t>{0x40, 0x3b, 0x11}));
}

TEST_CASE("three-address memory alu operations keep the segment override on the memory instruction")
{
    using namespace dcc::backend::x86;
    auto encode = [](MOpc opc, PhysReg dst, PhysReg left, MMem mem) {
        MInstr instr;
        instr.opc = opc;
        instr.num_ops = 3;
        instr.num_defs = 1;
        instr.ops[0] = MOp::from_reg(VReg::phys(dst));
        instr.ops[1] = MOp::from_reg(VReg::phys(left));
        instr.ops[2] = MOp::from_mem(mem);
        return encode_single_instruction(instr, EncodeMode::Long64).value_or(std::vector<std::uint8_t>{});
    };
    auto fs = [](PhysReg base, std::int32_t disp = 0) { return MMem::make_base_disp(VReg::phys(base), disp).with_segment(SegmentOverride::FS); };
    auto gs = [](PhysReg base, std::int32_t disp = 0) { return MMem::make_base_disp(VReg::phys(base), disp).with_segment(SegmentOverride::GS); };
    CHECK(encode(MOpc::ADD64rm, PhysReg::RAX, PhysReg::RDI, fs(PhysReg::RSI)) == (std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x64, 0x48, 0x03, 0x06}));
    CHECK(encode(MOpc::SUB32rm, PhysReg::RDX, PhysReg::RCX, gs(PhysReg::RBX, 8)) == (std::vector<std::uint8_t>{0x48, 0x89, 0xca, 0x65, 0x40, 0x2b, 0x53, 0x08}));
    CHECK(encode(MOpc::CMP64rm, PhysReg::RAX, PhysReg::RCX, fs(PhysReg::RDX)) == (std::vector<std::uint8_t>{0x48, 0x89, 0xc8, 0x64, 0x48, 0x3b, 0x02}));
    CHECK(encode(MOpc::XOR64rm, PhysReg::R9, PhysReg::R10, gs(PhysReg::R11, 0x30)) == (std::vector<std::uint8_t>{0x4d, 0x89, 0xd1, 0x65, 0x4d, 0x33, 0x4b, 0x30}));
    CHECK(encode(MOpc::ADD64rm, PhysReg::RAX, PhysReg::RAX, fs(PhysReg::RSI)) == (std::vector<std::uint8_t>{0x64, 0x48, 0x03, 0x06}));
    CHECK(encode(MOpc::ADD64rm, PhysReg::RAX, PhysReg::RDI, MMem::make_base_disp(VReg::phys(PhysReg::RSI))) == (std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x03, 0x06}));

#if defined(__linux__) && defined(__x86_64__)
    std::uint64_t fs_base = 0;
    REQUIRE(syscall(SYS_arch_prctl, ARCH_GET_FS, &fs_base) == 0);
    auto run = [&](MOpc opc, std::uint64_t value) -> std::uint64_t {
        MFunction func;
        auto& block = func.create_block("entry");
        MInstr alu;
        alu.opc = opc;
        alu.num_ops = 3;
        alu.num_defs = 1;
        alu.ops[0] = MOp::from_reg(VReg::phys(PhysReg::RAX));
        alu.ops[1] = MOp::from_reg(VReg::phys(PhysReg::RDI));
        alu.ops[2] = MOp::from_mem(fs(PhysReg::RSI));
        block.instrs.push_back(alu);
        MInstr ret;
        ret.opc = MOpc::RET;
        block.instrs.push_back(ret);
        auto encoded = encode_function(func, EncodeMode::Long64);
        if (!encoded.warnings.empty() || encoded.bytes.empty())
            return 0;
        auto size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        void* page = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            return 0;
        std::memcpy(page, encoded.bytes.data(), encoded.bytes.size());
        std::uint64_t result = 0;
        if (mprotect(page, size, PROT_READ | PROT_EXEC) == 0)
        {
            auto* code = reinterpret_cast<std::uint64_t (*)(std::uint64_t, std::uint64_t)>(page);
            result = code(value, 0);
        }
        munmap(page, size);
        return result;
    };
    CHECK(run(MOpc::ADD64rm, 0x1234) == 0x1234 + fs_base);
    CHECK(run(MOpc::SUB64rm, 0x1234) == 0x1234 - fs_base);
    CHECK(run(MOpc::ADD32rm, 0x1234) == static_cast<std::uint32_t>(0x1234 + fs_base));
#endif
}

TEST_CASE("instruction encoding requires an implemented x86 mode")
{
    using namespace dcc::backend::x86;
    MInstr instr;
    instr.opc = MOpc::RET;
    auto long_mode = encode_single_instruction(instr, EncodeMode::Long64);
    REQUIRE(long_mode.has_value());
    CHECK(*long_mode == (std::vector<std::uint8_t>{0xc3}));
    auto result = encode_single_instruction(instr, EncodeMode::Protected32);
    CHECK(!result.has_value());
    MFunction func;
    auto function = encode_function(func, EncodeMode::Protected32);
    CHECK(function.bytes.empty());
    CHECK_EQ(function.warnings.size(), 1u);
    auto real_mode = encode_single_instruction(instr, EncodeMode::Real16);
    REQUIRE(real_mode.has_value());
    CHECK(*real_mode == (std::vector<std::uint8_t>{0xc3}));
}

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

    std::vector<std::uint8_t> try_encode(x86::MInstr const& instr)
    {
        auto result = x86::encode_single_instruction(instr, x86::EncodeMode::Long64);
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
    REQUIRE(plan.instructions[0].opc == x86::MOpc::MOV64rr);
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
    REQUIRE(plan.instructions[0].opc == x86::MOpc::ADD64ri32);
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
    REQUIRE(plan.instructions[0].opc == x86::MOpc::XOR32rr);
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
        REQUIRE(plan.instructions[0].opc == x86::MOpc::MOV64rm);
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
    REQUIRE(plan.instructions[0].opc == x86::MOpc::MOV64rm);
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

TEST_CASE("llvm lowering braces every flag output constraint")
{
    struct FlagCase
    {
        std::string_view cond;
        std::string_view code;
    };
    FlagCase cases[] = {{"zero", "z"}, {"equal", "z"}, {"not_zero", "nz"}, {"not_equal", "nz"}, {"carry", "c"}, {"below", "c"},
                        {"not_carry", "nc"}, {"above_equal", "nc"}, {"above", "a"}, {"below_equal", "be"}, {"sign", "s"},
                        {"not_sign", "ns"}, {"overflow", "o"}, {"not_overflow", "no"}, {"parity_even", "p"}, {"parity_odd", "np"},
                        {"less", "l"}, {"less_equal", "le"}, {"greater", "g"}, {"greater_equal", "ge"}};
    for (auto const& [cond, code] : cases)
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
        flag.flag_cond = cond;
        flag.type = boolean;
        operands.push_back(flag);
        operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rax", u64, av));
        operands.push_back(fx.reg_operand(IrAsmOperand::Direction::In, "rbx", u64, bv));
        auto* inst = fx.build("cmp %[x], %[y]", std::move(operands), IrAsmDialect::Intel, {{"%[x]", 1}, {"%[y]", 2}});
        auto lowered = prepare_llvm_asm(*inst);
        REQUIRE(lowered.error.empty());
        CHECK_EQ(lowered.constraints, "={@cc" + std::string(code) + "},{rax},{rbx}");
    }
}

TEST_CASE("llvm lowering emits any immediate and symbolic constraints")
{
    AsmFixture fx;
    auto* u64 = fx.ctx.int_t(64, false);
    auto* value = fx.ctx.int_const(u64, 7);
    std::vector<IrAsmOperand> operands;
    IrAsmOperand any_out;
    any_out.direction = IrAsmOperand::Direction::Out;
    any_out.placement_kind = IrAsmOperand::PlacementKind::Any;
    any_out.type = u64;
    operands.push_back(any_out);
    IrAsmOperand any_in;
    any_in.direction = IrAsmOperand::Direction::In;
    any_in.placement_kind = IrAsmOperand::PlacementKind::Any;
    any_in.type = u64;
    any_in.value = value;
    operands.push_back(any_in);
    IrAsmOperand imm;
    imm.direction = IrAsmOperand::Direction::In;
    imm.placement_kind = IrAsmOperand::PlacementKind::Imm;
    imm.type = u64;
    imm.value = value;
    operands.push_back(imm);
    IrAsmOperand sym;
    sym.direction = IrAsmOperand::Direction::In;
    sym.placement_kind = IrAsmOperand::PlacementKind::Sym;
    sym.type = fx.ctx.pointer_to(u64);
    sym.value = fx.ctx.symbol_ref("table", sym.type);
    operands.push_back(sym);
    auto* inst = fx.build("op %0, %1, %2, %3", std::move(operands), IrAsmDialect::Att, {{"%0", 0}, {"%1", 1}, {"%2", 2}, {"%3", 3}});
    auto lowered = prepare_llvm_asm(*inst);
    REQUIRE(lowered.error.empty());
    CHECK_EQ(lowered.constraints, "=&rm,rm,i,s");
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
