import std;

import dcc.ir;
import dcc.ir.pipeline;
import dcc.types;
import dcc.ir.pass;
import dcc.target;
import dcc.backend;
import dcc.backend.llvm;
import dcc.backend.em64t;
import dcc.backend.i8086;
import dcc.backend.i8086.registers;
import dcc.backend.x86.mir;
import dcc.backend.object.layout;

#include "harness.hh"

namespace
{
    using namespace dcc::ir;
    using namespace dcc::target;
    using namespace dcc::backend;

    IrModule* build_add_module(IrContext& ir_ctx)
    {
        auto* mod = ir_ctx.module("test_backend");
        auto* i32t = ir_ctx.int_t(32, true);
        IrType const* param_arr[] = {i32t, i32t};
        auto* func_ty = ir_ctx.func_t(i32t, param_arr);
        auto* func_ft = ir_type_cast<IrFuncType>(func_ty);
        auto* func = ir_ctx.function("add", func_ft);

        auto* entry_bb = ir_ctx.basic_block("entry", 0);
        func->blocks.push_back(entry_bb);
        func->entry_block = entry_bb;

        auto* param_a = ir_ctx.local("a", 0, i32t);
        auto* param_b = ir_ctx.local("b", 1, i32t);
        entry_bb->params.push_back(param_a);
        entry_bb->params.push_back(param_b);

        auto* add_inst = ir_ctx.add(i32t, param_a, param_b);
        entry_bb->instructions.push_back(add_inst);

        entry_bb->terminator = ir_ctx.ret(add_inst);

        mod->functions.push_back(func);
        return mod;
    }

} // anonymous namespace

SECTION("backend-interface");

TEST_CASE("i8086 spill slots follow transfer width")
{
    using namespace dcc::backend::x86;
    for (auto bits : {8u, 16u, 32u})
    {
        MFunction function;
        auto& block = function.create_block();
        function.entry_block_id = block.id;
        std::vector<VReg> values;
        for (unsigned i = 0; i < 12; ++i)
        {
            auto value = function.new_vreg();
            block.instrs.push_back(bits == 8 ? make_instr(MOpc::MOVZX16rm8, {MOp::from_reg(value), MOp::from_mem(MMem::make_sym_reloc("source"))}, 1)
                                             : make_instr(bits == 16 ? MOpc::MOV16ri : MOpc::MOV32ri, {MOp::from_reg(value), MOp::from_imm(i + 1)}, 1));
            values.push_back(value);
        }
        for (auto value : values)
        {
            auto copy = function.new_vreg();
            block.instrs.push_back(make_copy(copy, value));
            block.instrs.push_back(
                make_instr(bits <= 16 ? MOpc::MOV16mr : MOpc::MOV32mr, {MOp::from_mem(MMem::make_sym_reloc("sink")), MOp::from_reg(copy)}, 0));
        }
        block.instrs.push_back(make_instr(MOpc::RET, {}, 0));
        dcc::backend::i8086::regalloc(function, *TargetConfig::parse_triple("i8086-binary"));
        REQUIRE(!function.frame_slots.empty());
        for (auto const& slot : function.frame_slots)
            if (slot.is_spill)
            {
                CHECK(slot.size == std::max(16u, bits) / 8);
                CHECK(slot.align == 2);
            }
        for (auto const& instruction : function.blocks.front().instrs)
            if (bits <= 16)
            {
                CHECK(instruction.opc != MOpc::COPY);
                CHECK(instruction.opc != MOpc::MOV32rm);
                CHECK(instruction.opc != MOpc::MOV32mr);
            }
    }
}

TEST_CASE("i8086 parallel copy cycles preserve mixed widths")
{
    using namespace dcc::backend::x86;
    using dcc::backend::x86::PhysReg;
    for (bool mixed : {false, true})
    {
        MFunction function;
        auto& block = function.create_block();
        function.entry_block_id = block.id;
        auto first = make_instr(MOpc::MOV16rr, {phys_operand(PhysReg::RAX), phys_operand(PhysReg::RCX)}, 1);
        auto second = make_instr(mixed ? MOpc::COPY : MOpc::MOV16rr, {phys_operand(PhysReg::RCX), phys_operand(PhysReg::RAX)}, 1);
        first.copy_group = second.copy_group = function.new_copy_group();
        block.instrs = {first, second, make_instr(MOpc::RET, {}, 0)};
        dcc::backend::i8086::regalloc(function, *TargetConfig::parse_triple("i8086-binary"));
        std::array<std::uint32_t, 32> registers{};
        registers[static_cast<unsigned>(PhysReg::RAX)] = 0x12345678;
        registers[static_cast<unsigned>(PhysReg::RCX)] = 0x87654321;
        for (auto const& instruction : function.blocks.front().instrs)
            if (instruction.opc == MOpc::COPY || instruction.opc == MOpc::MOV16rr)
            {
                auto& destination = registers[static_cast<unsigned>(instruction.ops[0].reg.phys_reg())];
                auto source = registers[static_cast<unsigned>(instruction.ops[1].reg.phys_reg())];
                destination = instruction.opc == MOpc::MOV16rr ? (destination & 0xffff0000) | (source & 0xffff) : source;
            }
        CHECK(registers[static_cast<unsigned>(PhysReg::RAX)] == 0x12344321);
        CHECK(registers[static_cast<unsigned>(PhysReg::RCX)] == (mixed ? 0x12345678u : 0x87655678u));
    }
}

TEST_CASE("llvm-ir-text-contains-define-add-ret")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("define") != std::string::npos);
    CHECK(ir.find("add") != std::string::npos);
    CHECK(ir.find("ret") != std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("llvm-asm-text-nonempty")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-elf";
    target.arch = Arch::X86_64;
    target.os = Os::Linux;
    target.object_format = ObjectFormat::Elf;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::AsmText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    if (!artifact.diagnostics.empty())
        ;
    else
    {
        REQUIRE(artifact.asm_text.has_value());
        CHECK(!artifact.asm_text->empty());
        CHECK(artifact.asm_text->find("add") != std::string::npos);
    }
}

TEST_CASE("llvm-object-bytes-elf-header")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-elf";
    target.arch = Arch::X86_64;
    target.os = Os::Linux;
    target.object_format = ObjectFormat::Elf;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::ObjectBytes};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    if (!artifact.diagnostics.empty())
        ;
    else
    {
        REQUIRE(artifact.object_bytes.has_value());
        auto const& obj = *artifact.object_bytes;
        CHECK(obj.size() >= 4);
        if (obj.size() >= 4)
        {
            CHECK_EQ(static_cast<int>(obj[0]), 0x7f);
            CHECK_EQ(static_cast<int>(obj[1]), 'E');
            CHECK_EQ(static_cast<int>(obj[2]), 'L');
            CHECK_EQ(static_cast<int>(obj[3]), 'F');
        }
    }
}

TEST_CASE("llvm-executable-x86-64-elf-valid")
{
    IrContext ir_ctx;
    auto* mod = ir_ctx.module("test_exe");
    auto* void_t = ir_ctx.void_t();
    auto* func_ty = ir_ctx.func_t(void_t, {});
    auto* func_ft = ir_type_cast<IrFuncType>(func_ty);
    auto* start_fn = ir_ctx.function("_start", func_ft);

    auto* entry_bb = ir_ctx.basic_block("entry", 0);
    start_fn->blocks.push_back(entry_bb);
    start_fn->entry_block = entry_bb;
    entry_bb->terminator = ir_ctx.unreachable();

    mod->functions.push_back(start_fn);

    TargetConfig target;
    target.triple = "x86_64-elf";
    target.arch = Arch::X86_64;
    target.os = Os::Linux;
    target.object_format = ObjectFormat::Elf;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::ExecutableBytes};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    if (!artifact.diagnostics.empty())
        return;

    REQUIRE(artifact.executable_bytes.has_value());
    auto const& exe = *artifact.executable_bytes;

    CHECK(exe.size() >= 64);
    if (exe.size() < 64)
        return;

    CHECK_EQ(static_cast<int>(exe[0]), 0x7f);
    CHECK_EQ(static_cast<int>(exe[1]), 'E');
    CHECK_EQ(static_cast<int>(exe[2]), 'L');
    CHECK_EQ(static_cast<int>(exe[3]), 'F');

    CHECK_EQ(static_cast<int>(exe[4]), 2);

    auto e_type = static_cast<std::uint16_t>(static_cast<unsigned char>(exe[16])) | (static_cast<std::uint16_t>(static_cast<unsigned char>(exe[17])) << 8);
    CHECK_EQ(e_type, 2);

    auto e_machine = static_cast<std::uint16_t>(static_cast<unsigned char>(exe[18])) | (static_cast<std::uint16_t>(static_cast<unsigned char>(exe[19])) << 8);
    CHECK_EQ(e_machine, 0x3E);
}

TEST_CASE("llvm-executable-non-x86-64-elf-rejected")
{
    IrContext ir_ctx;
    auto* mod = ir_ctx.module("test_exe");
    auto* void_t = ir_ctx.void_t();
    auto* func_ty = ir_ctx.func_t(void_t, {});
    auto* func_ft = ir_type_cast<IrFuncType>(func_ty);
    auto* start_fn = ir_ctx.function("_start", func_ft);

    auto* entry_bb = ir_ctx.basic_block("entry", 0);
    start_fn->blocks.push_back(entry_bb);
    start_fn->entry_block = entry_bb;
    entry_bb->terminator = ir_ctx.unreachable();

    mod->functions.push_back(start_fn);

    TargetConfig target;
    target.triple = "x86_64-coff";
    target.arch = Arch::X86_64;
    target.os = Os::Windows;
    target.object_format = ObjectFormat::Coff;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::ExecutableBytes};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    CHECK(!artifact.diagnostics.empty());
    CHECK(!artifact.executable_bytes.has_value());
}

TEST_CASE("requested backend artifacts are validated at the backend boundary")
{
    BackendOptions opts;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText,  ArtifactKind::MirText,         ArtifactKind::AsmText,
                                ArtifactKind::ObjectBytes, ArtifactKind::ExecutableBytes, ArtifactKind::SharedLibraryBytes,
                                ArtifactKind::ArchiveBytes};
    BackendArtifact artifact;

    CHECK(!validate_requested_artifacts(opts.requested_artifacts, artifact));
    CHECK_EQ(artifact.diagnostics.size(), opts.requested_artifacts.size());

    bool found_object = false;
    for (auto const& diagnostic : artifact.diagnostics)
        if (diagnostic.message == "backend did not produce requested object artifact")
            found_object = true;
    CHECK(found_object);
}

TEST_CASE("LLVM precheck returns a diagnostic for a malformed store")
{
    IrContext ir_ctx;
    auto* mod = ir_ctx.module("bad_store");
    auto* i32t = ir_ctx.int_t(32, true);
    auto* fn_type = ir_type_cast<IrFuncType>(ir_ctx.func_t(ir_ctx.void_t(), {}));
    auto* fn = ir_ctx.function("bad_store", fn_type);
    auto* entry = ir_ctx.basic_block("entry", 0);
    fn->entry_block = entry;
    fn->blocks.push_back(entry);
    auto* slot = ir_ctx.alloca(ir_ctx.pointer_to(i32t), i32t);
    entry->instructions.push_back(slot);
    entry->instructions.push_back(ir_ctx.store(nullptr, slot));
    entry->terminator = ir_ctx.ret();
    mod->functions.push_back(fn);

    BackendOptions opts;
    opts.target = TargetConfig::host_default();
    opts.requested_artifacts = {ArtifactKind::ObjectBytes};
    auto artifact = make_llvm_backend()->emit(*mod, opts);

    CHECK(!artifact.object_bytes.has_value());
    REQUIRE(!artifact.diagnostics.empty());
    CHECK(artifact.diagnostics.front().message.find("store has no value operand") != std::string::npos);
}

TEST_CASE("llvm-codegen-flags-no-red-zone")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    target.no_red_zone = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("attributes #") != std::string::npos);
    CHECK(ir.find("noredzone") != std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("llvm-codegen-flags-pic")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    target.position_independent_code = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("llvm-codegen-flags-mcmodel-kernel")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    target.code_model = CodeModel::Kernel;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    if (!artifact.diagnostics.empty())
        return;

    REQUIRE(artifact.llvm_ir_text.has_value());
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("llvm-codegen-flags-mcmodel-large")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    target.code_model = CodeModel::Large;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    if (!artifact.diagnostics.empty())
        return;

    REQUIRE(artifact.llvm_ir_text.has_value());
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("llvm-codegen-flags-no-simd")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    target.no_simd = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("llvm-codegen-flags-no-x87")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    target.no_x87 = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("llvm-codegen-flags-combined")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    target.no_red_zone = true;
    target.no_simd = true;
    target.no_x87 = true;
    target.position_independent_code = true;
    target.code_model = CodeModel::Medium;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("noredzone") != std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("llvm-codegen-flags-default-no-flags-preserved")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("noredzone") == std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("debug-elf-auto-has-dwarf-version")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-elf";
    target.arch = Arch::X86_64;
    target.os = Os::Linux;
    target.object_format = ObjectFormat::Elf;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.emit_debug_info = true;
    opts.debug_format = DebugFormat::Auto;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(artifact.diagnostics.empty());

    CHECK(ir.find("Dwarf Version") != std::string::npos);
    CHECK(ir.find("CodeView") == std::string::npos);
    CHECK(ir.find("Debug Info Version") != std::string::npos);
    CHECK(ir.find("!llvm.module.flags") != std::string::npos);
}

TEST_CASE("debug-coff-auto-has-codeview")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-coff";
    target.arch = Arch::X86_64;
    target.os = Os::Windows;
    target.object_format = ObjectFormat::Coff;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.emit_debug_info = true;
    opts.debug_format = DebugFormat::Auto;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(artifact.diagnostics.empty());

    CHECK(ir.find("CodeView") != std::string::npos);
    CHECK(ir.find("Dwarf Version") == std::string::npos);
    CHECK(ir.find("Debug Info Version") != std::string::npos);
}

TEST_CASE("debug-coff-explicit-dwarf")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-coff";
    target.arch = Arch::X86_64;
    target.os = Os::Windows;
    target.object_format = ObjectFormat::Coff;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.emit_debug_info = true;
    opts.debug_format = DebugFormat::Dwarf;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(artifact.diagnostics.empty());

    CHECK(ir.find("Dwarf Version") != std::string::npos);
    CHECK(ir.find("CodeView") == std::string::npos);
    CHECK(ir.find("Debug Info Version") != std::string::npos);
}

TEST_CASE("debug-coff-explicit-pdb")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-coff";
    target.arch = Arch::X86_64;
    target.os = Os::Windows;
    target.object_format = ObjectFormat::Coff;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.emit_debug_info = true;
    opts.debug_format = DebugFormat::Pdb;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(artifact.diagnostics.empty());

    CHECK(ir.find("CodeView") != std::string::npos);
    CHECK(ir.find("Dwarf Version") == std::string::npos);
    CHECK(ir.find("Debug Info Version") != std::string::npos);
}

TEST_CASE("debug-none-no-flags")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.emit_debug_info = false;
    opts.debug_format = DebugFormat::None;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(artifact.diagnostics.empty());

    CHECK(ir.find("Debug Info Version") == std::string::npos);
    CHECK(ir.find("Dwarf Version") == std::string::npos);
    CHECK(ir.find("CodeView") == std::string::npos);
}

TEST_CASE("debug-pdb-on-elf-rejected")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-elf";
    target.arch = Arch::X86_64;
    target.os = Os::Linux;
    target.object_format = ObjectFormat::Elf;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.emit_debug_info = true;
    opts.debug_format = DebugFormat::Pdb;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    CHECK(!artifact.diagnostics.empty());
    CHECK(!artifact.llvm_ir_text.has_value());
}

TEST_CASE("debug-coff-exe-rejected-with-clear-diagnostic")
{
    IrContext ir_ctx;
    auto* mod = ir_ctx.module("test_exe");
    auto* void_t = ir_ctx.void_t();
    auto* func_ty = ir_ctx.func_t(void_t, {});
    auto* func_ft = ir_type_cast<IrFuncType>(func_ty);
    auto* start_fn = ir_ctx.function("_start", func_ft);

    auto* entry_bb = ir_ctx.basic_block("entry", 0);
    start_fn->blocks.push_back(entry_bb);
    start_fn->entry_block = entry_bb;
    entry_bb->terminator = ir_ctx.unreachable();

    mod->functions.push_back(start_fn);

    TargetConfig target;
    target.triple = "x86_64-coff";
    target.arch = Arch::X86_64;
    target.os = Os::Windows;
    target.object_format = ObjectFormat::Coff;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::ExecutableBytes};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    CHECK(!artifact.diagnostics.empty());
    CHECK(!artifact.executable_bytes.has_value());

    bool has_helpful_msg = false;
    for (auto const& d : artifact.diagnostics)
        if (d.message.contains("COFF") || d.message.contains("lld-link") || d.message.contains("PE"))
            has_helpful_msg = true;

    CHECK(has_helpful_msg);
}

TEST_CASE("omit-frame-pointer-false-adds-frame-pointer-attribute")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.omit_frame_pointer = false;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("\"frame-pointer\"") != std::string::npos);
    CHECK(ir.find("attributes #") != std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("omit-frame-pointer-true-omits-frame-pointer-attribute")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.omit_frame_pointer = true;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("\"frame-pointer\"") == std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("omit-frame-pointer-default-omits-attribute")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("\"frame-pointer\"") == std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("omit-frame-pointer-false-coff-target")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-coff";
    target.arch = Arch::X86_64;
    target.os = Os::Windows;
    target.object_format = ObjectFormat::Coff;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.omit_frame_pointer = false;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("\"frame-pointer\"") != std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("omit-frame-pointer-false-kernel-code-model")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target = TargetConfig::host_default();
    target.code_model = CodeModel::Kernel;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.omit_frame_pointer = false;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    if (!artifact.diagnostics.empty())
        return;

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("\"frame-pointer\"") != std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("omit-frame-pointer-false-with-debug-info-elf")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-elf";
    target.arch = Arch::X86_64;
    target.os = Os::Linux;
    target.object_format = ObjectFormat::Elf;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::LlvmIrText};
    opts.emit_debug_info = true;
    opts.debug_format = DebugFormat::Dwarf;
    opts.omit_frame_pointer = false;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    REQUIRE(artifact.llvm_ir_text.has_value());
    auto const& ir = *artifact.llvm_ir_text;
    CHECK(ir.find("Dwarf Version") != std::string::npos);
    CHECK(ir.find("\"frame-pointer\"") != std::string::npos);
    CHECK(artifact.diagnostics.empty());
}

TEST_CASE("omit-frame-pointer-false-asm-has-frame-pointer-prologue")
{
    IrContext ir_ctx;
    auto* mod = build_add_module(ir_ctx);

    TargetConfig target;
    target.triple = "x86_64-elf";
    target.arch = Arch::X86_64;
    target.os = Os::Linux;
    target.object_format = ObjectFormat::Elf;
    target.pointer_bits = 64;
    target.pointer_align = 8;
    target.little_endian = true;

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::AsmText};
    opts.omit_frame_pointer = false;

    auto backend = make_llvm_backend();
    auto artifact = backend->emit(*mod, opts);

    if (!artifact.diagnostics.empty())
        return;

    REQUIRE(artifact.asm_text.has_value());
    auto const& asm_text = *artifact.asm_text;
    CHECK(!asm_text.empty());

    bool has_frame_prologue = (asm_text.find("pushq") != std::string::npos && asm_text.find("%rbp") != std::string::npos) ||
                              (asm_text.find("push") != std::string::npos && asm_text.find("rbp") != std::string::npos);

    CHECK(has_frame_prologue);
}

TEST_CASE("ir-verifier-rejects-segmented-pointer-mismatches")
{
    auto target = TargetConfig::host_default();
    IrContext ctx{256 * 1024, &target};
    auto* mod = ctx.module("segmented_verify");
    auto* i8 = ctx.int_t(8, false);
    auto* i32 = ctx.int_t(32, false);
    auto* fs = ctx.pointer_to(i8, Segment::Fs, PointerFlavor::Based);
    auto* gs = ctx.pointer_to(i8, Segment::Gs, PointerFlavor::Based);
    auto* ss = ctx.pointer_to(i8, Segment::Ss, PointerFlavor::Based);
    IrType const* params[] = {gs};
    auto* signature = ir_type_cast<IrFuncType>(ctx.func_t(fs, params));
    auto* function = ctx.function("bad", signature);
    auto* block = ctx.basic_block("entry", 0);
    auto* arg = ctx.local("arg", 0, gs);
    auto* invalid_reg = ctx.local("ss", 1, ss);
    block->params.push_back(arg);
    block->params.push_back(invalid_reg);
    block->instructions.push_back(ctx.make_pointer(fs, ctx.int_const(i32, 4), ctx.int_const(ctx.int_t(16, false), 1)));
    block->instructions.push_back(ctx.ptrtoi(ctx.usize_t(), arg));
    block->instructions.push_back(ctx.bitcast(fs, arg));
    block->instructions.push_back(ctx.gep(fs, arg));
    block->instructions.push_back(ctx.cmp_eq(arg, ctx.pointer_const(fs, 0, 1)));
    block->instructions.push_back(ctx.cmp_eq(arg, ctx.null_const(gs)));
    block->terminator = ctx.ret(arg);
    function->blocks.push_back(block);
    function->entry_block = block;
    mod->functions.push_back(function);
    const_cast<IrPointerType*>(static_cast<IrPointerType const*>(fs))->byte_size = 4;

    dcc::ir::pass::IrVerifier verifier{target};
    auto errors = verifier.verify(*mod);
    auto has = [&](std::string_view needle) {
        return std::ranges::any_of(errors, [&](std::string const& error) { return error.find(needle) != std::string::npos; });
    };
    CHECK(has("pointer layout"));
    CHECK(has("register unavailable"));
    CHECK(has("make_pointer"));
    CHECK(has("integer cast"));
    CHECK(has("bitcast"));
    CHECK(has("GEP"));
    CHECK(has("comparison"));
    CHECK(has("based pointer cannot contain null"));
    CHECK(has("pointer constant"));
    CHECK(has("return"));
}

TEST_CASE("ir-verifier-allows-fs-gs-based-pointers-only-on-i386-class-i8086")
{
    auto verify_for = [](std::string_view cpu) -> std::vector<std::string> {
        auto target = *TargetConfig::parse_triple("i8086-binary");
        if (auto error = target.configure(dcc::target::CodeModel::Default, cpu))
            return {*error};
        IrContext ctx{256 * 1024, &target};
        auto* mod = ctx.module("i8086_segments");
        auto* u16 = ctx.int_t(16, false);
        auto* fs = ctx.pointer_to(u16, Segment::Fs, PointerFlavor::Based);
        auto* gs = ctx.pointer_to(u16, Segment::Gs, PointerFlavor::Based);
        IrType const* params[] = {fs, gs};
        auto* signature = ir_type_cast<IrFuncType>(ctx.func_t(u16, params));
        auto* function = ctx.function("segments", signature);
        auto* block = ctx.basic_block("entry", 0);
        auto* f = ctx.local("f", 0, fs);
        auto* g = ctx.local("g", 1, gs);
        block->params.push_back(f);
        block->params.push_back(g);
        auto* value = ctx.load(u16, f);
        block->instructions.push_back(value);
        block->instructions.push_back(ctx.store(value, g));
        auto* segment = ctx.read_segment(Segment::Gs);
        block->instructions.push_back(segment);
        block->terminator = ctx.ret(segment);
        function->blocks.push_back(block);
        function->entry_block = block;
        mod->functions.push_back(function);
        dcc::ir::pass::IrVerifier verifier{target};
        return verifier.verify(*mod);
    };

    CHECK(verify_for("i386").empty());
    CHECK(verify_for("i486").empty());
    auto errors = verify_for("8086");
    CHECK(std::ranges::any_of(errors, [](std::string const& error) { return error.find("register unavailable") != std::string::npos; }));
    CHECK(std::ranges::any_of(errors, [](std::string const& error) { return error.find("read_segment") != std::string::npos; }));
    CHECK(!verify_for("i286").empty());
}

TEST_CASE("ir-verifier-checks-pointer-constants-against-flavor-offset-widths")
{
    for (auto model : {dcc::target::CodeModel::Small, dcc::target::CodeModel::Unreal, dcc::target::CodeModel::Unreal32})
        for (auto flavor : {PointerFlavor::Near, PointerFlavor::Based, PointerFlavor::Far})
        {
            auto target = *TargetConfig::parse_triple("i8086-binary");
            REQUIRE(!target.configure(model, "i386"));
            auto verify = [&](std::uint64_t offset, std::uint16_t segment, bool invalid_type = false, bool null = false) {
                IrContext ctx{256 * 1024, &target};
                auto* mod = ctx.module("pointer_constants");
                auto* pointer = ctx.pointer_to(ctx.int_t(8, false), flavor == PointerFlavor::Based ? Segment::Es : Segment::None, flavor);
                auto* signature = ir_type_cast<IrFuncType>(ctx.func_t(pointer, {}));
                auto* function = ctx.function("constant", signature);
                auto* block = ctx.basic_block("entry", 0);
                auto* constant = ctx.pointer_const(pointer, offset, segment);
                if (invalid_type)
                    constant->type = ctx.int_t(16, false);
                block->terminator = ctx.ret(null ? static_cast<IrValue*>(ctx.null_const(pointer)) : static_cast<IrValue*>(constant));
                function->blocks.push_back(block);
                function->entry_block = block;
                mod->functions.push_back(function);
                return dcc::ir::pass::IrVerifier{target}.verify(*mod);
            };
            auto invalid_constant = [&](std::uint64_t offset, std::uint16_t segment, bool invalid_type = false) {
                auto errors = verify(offset, segment, invalid_type);
                return std::ranges::any_of(errors, [](std::string const& error) {
                    return error.find("pointer constant has invalid flavor or noncanonical fields") != std::string::npos;
                });
            };
            auto const bits = flavor == PointerFlavor::Near ? target.pointer_bits : target.far_offset_bits();
            auto const limit = std::uint64_t{1} << bits;
            CHECK(verify(0, 0).empty());
            CHECK(verify(limit - 1, flavor == PointerFlavor::Far ? 65535 : 0).empty());
            CHECK(invalid_constant(limit, 0));
            CHECK(invalid_constant(~std::uint64_t{0}, 0));
            CHECK(invalid_constant(0, 0, true));
            CHECK(verify(1, 1).empty() == (flavor == PointerFlavor::Far));
            CHECK(verify(0, 0, false, true).empty() == (flavor != PointerFlavor::Based));
        }
}

TEST_CASE("custom-backend-rejects-far-and-unavailable-based-ir")
{
    auto target = TargetConfig::host_default();
    IrContext ctx{256 * 1024, &target};
    auto* i8 = ctx.int_t(8, false);
    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::ObjectBytes};
    auto backend = make_em64t_backend();
    std::array<IrType const*, 4> types = {ctx.pointer_to(i8, Segment::None, PointerFlavor::Far), ctx.slice_t(i8, Segment::None, PointerFlavor::Far),
                                          ctx.pointer_to(i8, Segment::Ss, PointerFlavor::Based), ctx.slice_t(i8, Segment::Ds, PointerFlavor::Based)};
    for (auto* type : types)
    {
        auto* mod = ctx.module("segmented_custom");
        IrType const* params[] = {type};
        mod->functions.push_back(ctx.function("segmented", ir_type_cast<IrFuncType>(ctx.func_t(type, params))));
        auto artifact = backend->emit(*mod, opts);
        REQUIRE(!artifact.diagnostics.empty());
        CHECK(artifact.diagnostics[0].message == "far and based pointers are not supported by this backend yet");
    }
}

TEST_CASE("custom-backend-accepts-fs-gs-based-ir-and-rejects-based-atomics")
{
    auto target = TargetConfig::host_default();
    IrContext ctx{256 * 1024, &target};
    auto* i64 = ctx.int_t(64, false);
    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::ObjectBytes};
    auto backend = make_em64t_backend();
    for (auto segment : {Segment::Fs, Segment::Gs})
        for (bool atomic : {false, true})
        {
            auto* based = ctx.pointer_to(i64, segment, PointerFlavor::Based);
            auto* mod = ctx.module("based_custom");
            IrType const* params[] = {based};
            auto* function = ctx.function("based", ir_type_cast<IrFuncType>(ctx.func_t(i64, params)));
            auto* block = ctx.basic_block("entry", 0);
            auto* arg = ctx.local("p", 0, based);
            block->params.push_back(arg);
            IrValue* value = atomic ? static_cast<IrValue*>(ctx.atomic_load(i64, arg, IrMemoryOrdering::SeqCst)) : static_cast<IrValue*>(ctx.load(i64, arg));
            block->instructions.push_back(value);
            block->terminator = ctx.ret(value);
            function->blocks.push_back(block);
            function->entry_block = block;
            mod->functions.push_back(function);
            auto artifact = backend->emit(*mod, opts);
            if (atomic)
            {
                REQUIRE(!artifact.diagnostics.empty());
                CHECK(artifact.diagnostics[0].message.find("atomic operations through based pointers") != std::string::npos);
            }
            else
            {
                CHECK(artifact.diagnostics.empty());
                CHECK(artifact.object_bytes.has_value());
            }
        }
}

TEST_CASE("llvm-x86-based-pointer-asm-uses-segment-overrides")
{
    TargetConfig target;
    target.triple = "x86-elf";
    target.arch = Arch::X86;
    target.os = Os::Linux;
    target.object_format = ObjectFormat::Elf;
    target.pointer_bits = 32;
    target.pointer_align = 4;
    target.little_endian = true;

    IrContext ctx{256 * 1024, &target};
    auto* mod = ctx.module("based_x86");
    auto* i8 = ctx.int_t(8, false);
    for (auto segment : {Segment::Fs, Segment::Gs, Segment::Ss})
    {
        auto* pointer = ctx.pointer_to(i8, segment, PointerFlavor::Based);
        IrType const* params[] = {pointer, i8};
        auto* function = ctx.function(segment == Segment::Fs ? "use_fs" : segment == Segment::Gs ? "use_gs" : "use_ss",
                                      ir_type_cast<IrFuncType>(ctx.func_t(i8, params)));
        auto* block = ctx.basic_block("entry", 0);
        auto* address = ctx.local("address", 0, pointer);
        auto* value = ctx.local("value", 1, i8);
        block->params.push_back(address);
        block->params.push_back(value);
        auto* loaded = ctx.load(i8, address);
        block->instructions.push_back(loaded);
        block->instructions.push_back(ctx.store(value, address));
        block->terminator = ctx.ret(loaded);
        function->blocks.push_back(block);
        function->entry_block = block;
        mod->functions.push_back(function);
    }

    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::AsmText};
    auto artifact = make_llvm_backend()->emit(*mod, opts);
    REQUIRE(artifact.diagnostics.empty());
    REQUIRE(artifact.asm_text.has_value());
    for (auto override : {"%fs:", "%gs:", "%ss:"})
        CHECK(artifact.asm_text->find(override) != std::string::npos);
}

TEST_CASE("llvm masks integer constants before constructing apint values")
{
    for (auto bits : {8, 16, 32})
        for (auto is_signed : {false, true})
        {
            IrContext ctx;
            auto* module = ctx.module("integer_constants");
            auto* type = ctx.int_t(static_cast<std::uint8_t>(bits), is_signed);
            auto* function = ctx.function("canonical", ir_type_cast<IrFuncType>(ctx.func_t(ctx.bool_t(), {})));
            auto* block = ctx.basic_block("entry", 0);
            function->entry_block = block;
            function->blocks.push_back(block);
            auto* comparison = ctx.cmp_eq(ctx.int_const(type, (std::int64_t{1} << bits) | 3), ctx.int_const(type, 3));
            block->instructions.push_back(comparison);
            block->terminator = ctx.ret(comparison);
            module->functions.push_back(function);
            BackendOptions options;
            options.target = TargetConfig::host_default();
            options.requested_artifacts = {ArtifactKind::LlvmIrText};
            auto artifact = make_llvm_backend()->emit(*module, options);
            REQUIRE(artifact.llvm_ir_text.has_value());
            CHECK(artifact.diagnostics.empty());
            CHECK(artifact.llvm_ir_text->find("ret i1 true") != std::string::npos);
        }
}

TEST_CASE("i8086 code models agree across sema and ir layouts")
{
    for (auto model : {dcc::target::CodeModel::Small, dcc::target::CodeModel::Unreal, dcc::target::CodeModel::Unreal32})
    {
        auto target = *dcc::target::TargetConfig::parse_triple("i8086-binary");
        REQUIRE(!target.configure(model, "i386"));
        dcc::types::TypeContext types(32768, &target);
        dcc::ir::IrContext ir(262144, &target);
        auto element = types.int_t(8, false);
        auto ir_element = ir.int_t(8, false);
        for (auto flavor : {dcc::types::PointerFlavor::Near, dcc::types::PointerFlavor::Based, dcc::types::PointerFlavor::Far})
        {
            auto sema_pointer = types.pointer_with_flavor(element, dcc::types::Qual::None, flavor, dcc::types::SegReg::DS);
            auto sema_slice = types.slice_t(element, dcc::types::Qual::None, flavor, dcc::types::SegReg::DS);
            auto ir_flavor = flavor == dcc::types::PointerFlavor::Near ? dcc::ir::PointerFlavor::Near :
                             flavor == dcc::types::PointerFlavor::Based ? dcc::ir::PointerFlavor::Based : dcc::ir::PointerFlavor::Far;
            auto ir_pointer = ir.pointer_to(ir_element, dcc::ir::Segment::Ds, ir_flavor);
            auto ir_slice = ir.slice_t(ir_element, dcc::ir::Segment::Ds, ir_flavor);
            CHECK(sema_pointer->byte_size == ir_pointer->byte_size);
            CHECK(sema_pointer->byte_align == ir_pointer->byte_align);
            CHECK(sema_slice->byte_size == ir_slice->byte_size);
            CHECK(sema_slice->byte_align == ir_slice->byte_align);
        }
    }
}

TEST_CASE("far pointer and slice initializer fields use canonical layouts")
{
    for (auto [triple, model] : {std::pair{"i8086-binary", CodeModel::Small}, {"i8086-binary", CodeModel::Unreal},
                               {"i8086-binary", CodeModel::Unreal32}, {"x86-elf", CodeModel::Default}, {"x86-coff", CodeModel::Default}})
    {
        auto target = *TargetConfig::parse_triple(triple);
        REQUIRE(!target.configure(model, "i386"));
        IrContext ctx(262144, &target);
        auto* pointer = ctx.pointer_to(ctx.int_t(8, false), Segment::None, PointerFlavor::Far);
        auto* constant = ctx.pointer_const(pointer, 0x1234, 0x5678);
        std::vector<std::uint8_t> bytes(pointer->byte_size, 0xFF);
        std::vector<dcc::backend::object::InitReloc> relocs;
        dcc::backend::object::serialize_init_memory(bytes, relocs, constant, pointer, 0, 2);
        auto width = static_cast<std::size_t>(target.far_offset_bits() / 8);
        CHECK(bytes[0] == 0x34);
        CHECK(bytes[1] == 0x12);
        CHECK(bytes[width] == 0x78);
        CHECK(bytes[width + 1] == 0x56);
        CHECK(std::ranges::all_of(std::span(bytes).subspan(width + 2), [](auto byte) { return byte == 0; }));
        CHECK(relocs.empty());
        auto* slice = ctx.slice_t(ctx.int_t(8, false), Segment::None, PointerFlavor::Far);
        auto* aggregate = ctx.aggregate(slice);
        aggregate->values.push_back(constant);
        aggregate->values.push_back(ctx.int_const(ctx.usize_t(), 7));
        bytes.assign(slice->byte_size, 0xFF);
        dcc::backend::object::serialize_init_memory(bytes, relocs, aggregate, slice, 0, 2);
        CHECK(bytes[width] == 0x78);
        CHECK(bytes[width + 1] == 0x56);
        CHECK(bytes[pointer->byte_size] == 7);
        CHECK(bytes[pointer->byte_size + 1] == 0);
    }
}

TEST_CASE("i8086 static far addresses relocate offset and segment separately")
{
    for (auto model : {CodeModel::Small, CodeModel::Unreal, CodeModel::Unreal32})
    {
        auto target = *TargetConfig::parse_triple("i8086-binary");
        REQUIRE(!target.configure(model, "i386"));
        IrContext ctx(262144, &target);
        auto* pointer = ctx.pointer_to(ctx.int_t(8, false), Segment::None, PointerFlavor::Far);
        auto* global = ctx.global("target", ctx.int_t(8, false));
        auto* reference = ctx.global_ref(global, pointer, 3);
        std::vector<std::uint8_t> bytes(pointer->byte_size, 0xFF);
        std::vector<dcc::backend::object::InitReloc> relocs;
        dcc::backend::object::serialize_init_memory(bytes, relocs, reference, pointer, 0, 2);
        REQUIRE(relocs.size() == 2);
        CHECK(relocs[0].offset == 0);
        CHECK(relocs[0].size == static_cast<std::uint64_t>(target.far_offset_bits() / 8));
        CHECK(relocs[0].name == "target");
        CHECK(relocs[0].addend == 3);
        CHECK(relocs[1].offset == static_cast<std::uint64_t>(target.far_offset_bits() / 8));
        CHECK(relocs[1].size == 2);
        CHECK(relocs[1].name == "__dcc_dgroup_segment");
        CHECK(relocs[1].addend == 0);
        CHECK(bytes[0] == 3);
        CHECK(std::ranges::all_of(std::span(bytes).subspan(1), [](auto byte) { return byte == 0; }));
    }
}

TEST_CASE("i8086 materializes far global address pairs in both optimization modes")
{
    for (auto model : {CodeModel::Small, CodeModel::Unreal, CodeModel::Unreal32})
        for (auto level : {dcc::ir::pass::OptLevel::O0, dcc::ir::pass::OptLevel::O2})
        {
            auto target = *TargetConfig::parse_triple("i8086-binary");
            REQUIRE(!target.configure(model, "i386"));
            IrContext ctx(262144, &target);
            auto* module = ctx.module("far_global_address");
            auto* u16 = ctx.int_t(16, false);
            auto* global = ctx.global("word_value", u16, ctx.int_const(u16, 19));
            module->globals.push_back(global);
            auto* pointer = ctx.pointer_to(u16, Segment::None, PointerFlavor::Far);
            auto* function = ctx.function("address", ir_type_cast<IrFuncType>(ctx.func_t(pointer, {})));
            auto* block = ctx.basic_block("entry", 0);
            block->terminator = ctx.ret(ctx.global_ref(global, pointer));
            function->entry_block = block;
            function->blocks.push_back(block);
            module->functions.push_back(function);
            BackendOptions options;
            options.target = target;
            options.opt_level = level;
            options.requested_artifacts = {ArtifactKind::ObjectBytes, ArtifactKind::AsmText};
            auto result = make_i8086_backend()->emit(*module, options);
            REQUIRE(result.diagnostics.empty());
            REQUIRE(result.object_bytes);
            REQUIRE(result.asm_text);
            CHECK(result.asm_text->find("__dcc_dgroup_segment") != std::string::npos);
        }
}

TEST_CASE("mem2reg preserves segment reads while replacing promoted loads")
{
    auto target = *TargetConfig::parse_triple("x86-elf");
    IrContext input(262144, &target);
    auto* module = input.module("segment_promotion");
    auto* u16 = input.int_t(16, false);
    auto* function = input.function("read_ds", ir_type_cast<IrFuncType>(input.func_t(u16, {})));
    auto* block = input.basic_block("entry", 0);
    auto* segment = input.read_segment(Segment::Ds);
    auto* local = input.alloca(input.pointer_to(u16), u16);
    auto* load = input.load(u16, local);
    block->instructions.push_back(segment);
    block->instructions.push_back(local);
    block->instructions.push_back(input.store(segment, local));
    block->instructions.push_back(load);
    block->terminator = input.ret(load);
    function->entry_block = block;
    function->blocks.push_back(block);
    module->functions.push_back(function);
    IrContext output(262144, &target);
    auto* optimized = dcc::ir::pass::global_pass_manager().run(*module, output, dcc::ir::pass::OptLevel::O2);
    REQUIRE(optimized);
    REQUIRE(optimized->functions.size() == 1);
    auto* result = optimized->functions.front();
    REQUIRE(result->entry_block);
    auto* returned = ir_cast<IrRetInst>(result->entry_block->terminator);
    REQUIRE(returned);
    auto* read = ir_cast<IrReadSegmentInst>(returned->value);
    REQUIRE(read);
    CHECK(read->segment == Segment::Ds);
    dcc::ir::pass::IrVerifier verifier(target);
    CHECK(verifier.verify(*optimized).empty());
}

TEST_CASE("scalar replacement promotes bounded aggregate locals")
{
    for (auto triple : {"x86_64-elf", "i8086-binary"})
        for (auto shape : {0, 1, 2})
        {
            auto target = *TargetConfig::parse_triple(triple);
            IrContext input(262144, &target);
            auto* module = input.module("scalar_replacement");
            auto* scalar = shape == 2 ? input.usize_t() : input.int_t(32, false);
            IrType const* members[] = {scalar, scalar};
            std::uint64_t offsets[] = {0, scalar->byte_size};
            auto* aggregate = shape == 0   ? input.aggregate_t(members, offsets, scalar->byte_size * 2, scalar->byte_align)
                              : shape == 1 ? input.array_t(scalar, 4)
                                           : input.slice_t(input.int_t(8, false));
            IrType const* parameters[] = {scalar, scalar};
            auto* function = input.function("replace", ir_type_cast<IrFuncType>(input.func_t(scalar, parameters)));
            auto* block = input.basic_block("entry", 0);
            auto* first = input.local("first", 0, scalar);
            auto* second = input.local("second", 1, scalar);
            block->params = {first, second};
            auto* slot = input.alloca(input.pointer_to(aggregate), aggregate);
            auto* value = input.aggregate(aggregate);
            if (shape == 2)
                value->values = {input.null_const(input.pointer_to(input.int_t(8, false))), first};
            else
                for (int i = 0; i < (shape == 1 ? 4 : 2); ++i)
                    value->values.push_back(first);
            auto* field = input.gep(input.pointer_to(scalar), slot);
            if (shape == 1)
                field->indices.push_back({IrGepInst::IndexKind::Array, input.int_const(input.usize_t(), 1), 0});
            else
                field->indices.push_back({IrGepInst::IndexKind::Field, nullptr, 1});
            auto* load = input.load(scalar, field);
            block->instructions = {slot, value, input.store(value, slot), field, input.store(second, field), load};
            block->terminator = input.ret(load);
            function->blocks.push_back(block);
            function->entry_block = block;
            module->functions.push_back(function);
            IrContext output(262144, &target);
            auto* optimized = dcc::ir::pass::global_pass_manager().run(*module, output, dcc::ir::pass::OptLevel::O2);
            REQUIRE(optimized);
            auto* result = optimized->functions.front();
            for (auto* bb : result->blocks)
                for (auto* instruction : bb->instructions)
                {
                    CHECK(instruction->kind != IrNodeKind::Alloca);
                    CHECK(instruction->kind != IrNodeKind::Load);
                    CHECK(instruction->kind != IrNodeKind::Store);
                    CHECK(instruction->kind != IrNodeKind::Gep);
                }
            auto* returned = ir_cast<IrRetInst>(result->entry_block->terminator);
            REQUIRE(returned);
            CHECK(returned->value == result->entry_block->params[1]);
            dcc::ir::pass::IrVerifier verifier(target);
            CHECK(verifier.verify(*optimized).empty());
        }
}

TEST_CASE("scalar replacement retains volatile dynamic large and escaping storage")
{
    for (auto mode : {0, 1, 2, 3})
    {
        auto target = *TargetConfig::parse_triple("x86_64-elf");
        IrContext input(262144, &target);
        auto* module = input.module("retained_storage");
        auto* scalar = input.int_t(32, false);
        auto* aggregate = input.array_t(scalar, mode == 2 ? 17 : 4);
        IrType const* parameters[] = {input.usize_t()};
        auto* function = input.function("retain", ir_type_cast<IrFuncType>(input.func_t(scalar, parameters)));
        auto* block = input.basic_block("entry", 0);
        auto* index = input.local("index", 0, input.usize_t());
        block->params.push_back(index);
        auto* slot = input.alloca(input.pointer_to(aggregate), aggregate);
        auto* value = input.aggregate(aggregate);
        for (int i = 0; i < (mode == 2 ? 17 : 4); ++i)
            value->values.push_back(input.int_const(scalar, i + 1));
        auto* field = input.gep(input.pointer_to(scalar), slot);
        field->indices.push_back({IrGepInst::IndexKind::Array, mode == 1 ? static_cast<IrValue*>(index) : input.int_const(input.usize_t(), 0), 0});
        block->instructions = {slot, value, input.store(value, slot), field};
        if (mode == 3)
        {
            IrType const* arguments[] = {slot->type};
            auto* escape = input.function("escape", ir_type_cast<IrFuncType>(input.func_t(input.void_t(), arguments)));
            auto* call = input.call(input.void_t(), input.func_ref(escape));
            call->args.push_back(slot);
            block->instructions.push_back(call);
            module->functions.push_back(escape);
        }
        IrValue* load = mode == 0 ? static_cast<IrValue*>(input.load_volatile(scalar, field)) : input.load(scalar, field);
        block->instructions.push_back(load);
        block->terminator = input.ret(load);
        function->blocks.push_back(block);
        function->entry_block = block;
        module->functions.insert(module->functions.begin(), function);
        IrContext output(262144, &target);
        auto* optimized = dcc::ir::pass::global_pass_manager().run(*module, output, dcc::ir::pass::OptLevel::O2);
        REQUIRE(optimized);
        bool retained = false;
        for (auto* bb : optimized->functions.front()->blocks)
            for (auto* instruction : bb->instructions)
                if (auto* alloca = ir_cast<IrAllocaInst>(instruction))
                    retained |= alloca->allocated_type->kind == IrTypeKind::Array;
        CHECK(retained);
        dcc::ir::pass::IrVerifier verifier(target);
        CHECK(verifier.verify(*optimized).empty());
    }
}

TEST_CASE("scalar replacement forwards unchanged aggregate ABI values")
{
    for (auto shape : {0, 1, 2, 3})
    {
        auto target = *TargetConfig::parse_triple("x86_64-elf");
        IrContext input(262144, &target);
        auto* scalar = input.int_t(32, false);
        IrType const* members[] = {shape == 3 ? input.slice_t(scalar) : scalar, scalar};
        std::uint64_t offsets[] = {0, shape == 3 ? 16u : 4u};
        auto* type = shape == 3   ? input.aggregate_t(members, offsets, 24, 8)
                     : shape == 0 ? input.aggregate_t(members, offsets, 8, 4)
                     : shape == 1 ? input.array_t(scalar, 3)
                                  : input.slice_t(scalar);
        auto* module = input.module("aggregate_forwarding");
        IrType const* parameters[] = {type};
        auto* function = input.function("forward", ir_type_cast<IrFuncType>(input.func_t(type, parameters)));
        auto* block = input.basic_block("entry", 0);
        auto* argument = input.local("argument", 0, type);
        block->params.push_back(argument);
        auto* slot = input.alloca(input.pointer_to(type), type);
        auto* load = input.load(type, slot);
        block->instructions = {slot, input.store(argument, slot), load};
        block->terminator = input.ret(load);
        function->entry_block = block;
        function->blocks.push_back(block);
        module->functions.push_back(function);
        IrContext output(262144, &target);
        auto* optimized = dcc::ir::pass::global_pass_manager().run(*module, output, dcc::ir::pass::OptLevel::O2);
        REQUIRE(optimized);
        auto* result = optimized->functions.front();
        auto* returned = ir_cast<IrRetInst>(result->entry_block->terminator);
        REQUIRE(returned);
        CHECK(returned->value == result->entry_block->params.front());
        CHECK(result->entry_block->instructions.empty());
        dcc::ir::pass::IrVerifier verifier(target);
        CHECK(verifier.verify(*optimized).empty());
    }
}

TEST_CASE("scalar replacement rewrites nested aggregate constant users")
{
    auto target = *TargetConfig::parse_triple("x86_64-elf");
    IrContext input(262144, &target);
    auto* scalar = input.int_t(32, false);
    IrType const* members[] = {scalar, scalar};
    std::uint64_t offsets[] = {0, 4};
    auto* inner = input.aggregate_t(members, offsets, 8, 4);
    IrType const* outer_members[] = {inner, scalar};
    std::uint64_t outer_offsets[] = {0, 8};
    auto* outer = input.aggregate_t(outer_members, outer_offsets, 12, 4);
    auto* module = input.module("nested_forwarding");
    auto* function = input.function("nested", ir_type_cast<IrFuncType>(input.func_t(outer, outer_members)));
    auto* block = input.basic_block("entry", 0);
    auto* pair = input.local("pair", 0, inner);
    auto* tag = input.local("tag", 1, scalar);
    block->params = {pair, tag};
    auto* first = input.extract(scalar, pair, 0);
    auto* second = input.extract(scalar, pair, 1);
    auto* copy = input.aggregate(inner);
    copy->values = {first, second};
    auto* wrapped = input.aggregate(outer);
    wrapped->values = {copy, tag};
    block->instructions = {first, second, copy};
    block->terminator = input.ret(wrapped);
    function->blocks.push_back(block);
    function->entry_block = block;
    module->functions.push_back(function);
    IrContext output(262144, &target);
    auto* optimized = dcc::ir::pass::global_pass_manager().run(*module, output, dcc::ir::pass::OptLevel::O2);
    REQUIRE(optimized);
    auto* result = optimized->functions.front();
    auto* returned = ir_cast<IrRetInst>(result->entry_block->terminator);
    REQUIRE(returned);
    auto* aggregate = ir_cast<IrAggregateInst>(returned->value);
    REQUIRE(aggregate);
    CHECK(aggregate->values.front() == result->entry_block->params.front());
    dcc::ir::pass::IrVerifier verifier(target);
    CHECK(verifier.verify(*optimized).empty());
}
