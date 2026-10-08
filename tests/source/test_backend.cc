import std;

import dcc.ir;
import dcc.types;
import dcc.ir.pass;
import dcc.target;
import dcc.backend;
import dcc.backend.llvm;
import dcc.backend.em64t;

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

TEST_CASE("custom-backend-rejects-far-and-based-ir")
{
    auto target = TargetConfig::host_default();
    IrContext ctx{256 * 1024, &target};
    auto* i8 = ctx.int_t(8, false);
    auto* based = ctx.pointer_to(i8, Segment::Fs, PointerFlavor::Based);
    BackendOptions opts;
    opts.target = target;
    opts.requested_artifacts = {ArtifactKind::ObjectBytes};
    auto backend = make_em64t_backend();
    std::array<IrType const*, 4> types = {based, ctx.pointer_to(i8, Segment::None, PointerFlavor::Far), ctx.slice_t(i8, Segment::Fs, PointerFlavor::Based),
                                          ctx.slice_t(i8, Segment::None, PointerFlavor::Far)};
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
