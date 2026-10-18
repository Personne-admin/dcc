export module dcc.backend.i8086;

import std;
import dcc.backend;
import dcc.ir;
import dcc.ir.pipeline;
import dcc.target;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.backend.x86.modasm;
import dcc.backend.i8086.isel;
import dcc.backend.i8086.registers;
import dcc.backend.i8086.framelay;
import dcc.backend.i8086.assembler;
import dcc.backend.object.elf32;
import dcc.backend.i8086.link;

using namespace dcc::backend::x86;

export namespace dcc::backend
{
    [[nodiscard]] std::unique_ptr<Backend> make_i8086_backend();
}

namespace dcc::backend
{
    namespace
    {
        class I8086Backend : public Backend
        {
        public:
            [[nodiscard]] std::string_view name() const override { return "custom"; }

            [[nodiscard]] std::set<ArtifactKind> supported_artifacts() const override
            {
                return {ArtifactKind::MirText, ArtifactKind::AsmText, ArtifactKind::ObjectBytes};
            }

            [[nodiscard]] BackendArtifact emit(ir::IrModule const& module, BackendOptions const& opts) override
            {
                BackendArtifact artifact;
                auto fail = [&](std::string message) {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, std::move(message)});
                    return artifact;
                };

                for (auto kind : opts.requested_artifacts)
                    if (!supported_artifacts().contains(kind))
                        return fail(std::format("i8086 backend: {} output is not supported", artifact_kind_name(kind)));

                ir::IrModule const* input = &module;
                ir::IrContext opt_ctx{256 * 1024, &opts.target};
                if (opts.opt_level > ir::pass::OptLevel::O0 && !std::getenv("DCC_BENCH_SKIP_IR_PASSES"))
                    input = ir::pass::global_pass_manager().run(module, opt_ctx, opts.opt_level);

                auto* marked = opt_ctx.module(input->name);
                marked->source_file_id = input->source_file_id;
                marked->functions.assign(input->functions.begin(), input->functions.end());
                marked->globals.assign(input->globals.begin(), input->globals.end());
                marked->module_asms.assign(input->module_asms.begin(), input->module_asms.end());
                auto* byte = opt_ctx.int_t(8, false);
                auto* marker = opt_ctx.global(i8086::model_marker(opts.target.code_model), byte);
                marker->is_declaration = true;
                marker->linkage = ir::Linkage::External;
                auto* pointer = opt_ctx.pointer_to(byte);
                auto* reference = opt_ctx.global("__dcc_i8086_model_reference", pointer, opt_ctx.global_ref(marker, pointer), true);
                reference->section = i8086::model_marker_section;
                reference->retain = true;
                marked->globals.push_back(marker);
                marked->globals.push_back(reference);
                input = marked;

                std::optional<ModuleAsmObject> module_asm;
                if (!input->module_asms.empty())
                {
                    auto assembled = assemble_module_asm(input->module_asms, EncodeMode::Real16, ModuleAsmFormat::Elf);
                    if (!assembled)
                    {
                        artifact.diagnostics.push_back(BackendDiagnostic{assembled.error().range, "i8086 backend: " + assembled.error().message});
                        return artifact;
                    }
                    for (auto const& sym : assembled->symbols)
                    {
                        if (sym.binding == ModuleAsmBinding::Local)
                            continue;
                        bool const clash =
                            std::ranges::any_of(input->functions, [&](ir::IrFunction const* f) { return f && !f->blocks.empty() && f->name == sym.name; }) ||
                            std::ranges::any_of(input->globals, [&](ir::IrGlobal const* g) { return g && !g->is_declaration && g->name == sym.name; });
                        if (clash)
                            return fail(std::format("i8086 backend: module asm defines `{}`, which this module already defines", sym.name));
                    }
                    for (auto const& section : assembled->sections)
                        for (auto const& r : section.relocations)
                            if (r.kind == Reloc::Kind::Abs64)
                                return fail(std::format("i8086 backend: module asm 64-bit reference to `{}` is not supported", r.symbol));
                    module_asm = std::move(*assembled);
                }
                ModuleAsmObject const* asm_object = module_asm ? &*module_asm : nullptr;
                std::vector<MFunction> functions;
                for (auto* func : input->functions)
                {
                    if (!func || func->blocks.empty())
                        continue;
                    std::vector<std::string> diags;
                    auto mfunc = i8086::isel_function(*func, opts.target, diags);
                    if (!diags.empty())
                    {
                        for (auto& message : diags)
                            artifact.diagnostics.push_back(BackendDiagnostic{{}, std::move(message)});
                        return artifact;
                    }
                    i8086::regalloc(mfunc, opts.target);
                    i8086::frame_layout(mfunc, opts.target);
                    for (auto& block : mfunc.blocks)
                        for (auto& instruction : block.instrs)
                            for (std::uint8_t i = 0; i < instruction.num_ops; ++i)
                                if (auto& operand = instruction.ops[i]; operand.kind == MOpKind::Mem)
                                {
                                    auto& memory = operand.mem;
                                    bool const stack_base = memory.base == VReg::phys(PhysReg::RBP) || memory.base == VReg::phys(PhysReg::RSP);
                                    if (memory.segment == (stack_base ? SegmentOverride::SS : SegmentOverride::DS))
                                        memory.segment = SegmentOverride::None;
                                }
                    if (mfunc.frame_size >= 65532)
                        return fail(std::format("i8086 backend: stack frame in function `{}` exceeds the 16-bit stack", func->name));
                    if (opts.target.code_model == target::CodeModel::Default || opts.target.code_model == target::CodeModel::Small)
                        for (auto const& block : mfunc.blocks)
                            for (auto const& instruction : block.instrs)
                            {
                                bool address32 = instruction.opc == MOpc::REPMOVS_A32 || instruction.opc == MOpc::REPSTOS_A32 ||
                                                 instruction.opc == MOpc::REPMOVSW_A32 || instruction.opc == MOpc::REPSTOSW_A32;
                                for (std::uint8_t i = 0; i < instruction.num_ops; ++i)
                                    address32 |= instruction.ops[i].kind == MOpKind::Mem && instruction.ops[i].mem.address_bits == 32;
                                if (address32)
                                    return fail(std::format("i8086 backend: 32-bit address in small-model function `{}`", func->name));
                            }
                    i8086::simplify_output(mfunc);
                    relax_real16_branches(mfunc);
                    functions.push_back(std::move(mfunc));
                }

                if (opts.requested_artifacts.contains(ArtifactKind::MirText))
                {
                    std::string text;
                    for (auto const& f : functions)
                    {
                        auto spills = std::ranges::count_if(f.frame_slots, [](MFrameSlot const& slot) { return slot.is_spill; });
                        text += std::format("func {} [frame_size={}, spill_slots={}]\n", f.name(), f.frame_size, spills);
                        for (auto const& slot : f.frame_slots)
                            text += std::format("slot offset={} size={} spill={}\n", slot.offset, slot.size, slot.is_spill ? 1 : 0);
                        for (auto const& block : f.blocks)
                        {
                            text += std::format("{}:\n", block.display_name());
                            for (auto const& mi : block.instrs)
                                text += format_instr(mi) + "\n";
                        }
                        text += '\n';
                    }
                    artifact.mir_text = std::move(text);
                }

                if (opts.requested_artifacts.contains(ArtifactKind::AsmText))
                {
                    auto text = i8086::emit_intel_asm(*input, functions, opts.target, asm_object);
                    if (!text)
                        return fail(text.error());
                    artifact.asm_text = std::move(*text);
                }

                if (opts.requested_artifacts.contains(ArtifactKind::ObjectBytes))
                {
                    MModule mmod;
                    std::vector<EncodeResult> encoded;
                    for (auto& f : functions)
                    {
                        auto result = encode_function(f, EncodeMode::Real16);
                        for (auto const& warning : result.warnings)
                            artifact.diagnostics.push_back(BackendDiagnostic{{}, std::format("i8086 backend: {}", warning)});
                        encoded.push_back(std::move(result));
                        mmod.functions.push_back(std::move(f));
                    }
                    if (!artifact.diagnostics.empty())
                        return artifact;
                    auto bytes = object::write_elf32(*input, mmod, encoded, opts.target, object::elf32_i8086_policy, asm_object);
                    std::vector<std::byte> object_bytes(bytes.size());
                    std::ranges::transform(bytes, object_bytes.begin(), [](std::uint8_t b) { return static_cast<std::byte>(b); });
                    artifact.object_bytes = std::move(object_bytes);
                }
                return artifact;
            }
        };

    } // namespace

    std::unique_ptr<Backend> make_i8086_backend()
    {
        return std::make_unique<I8086Backend>();
    }

} // namespace dcc::backend
