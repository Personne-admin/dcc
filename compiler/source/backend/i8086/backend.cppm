export module dcc.backend.i8086;

import std;
import dcc.backend;
import dcc.ir;
import dcc.ir.pipeline;
import dcc.target;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
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
        [[nodiscard]] bool has_segmented_type(ir::IrType const* type, std::unordered_set<ir::IrType const*>& seen)
        {
            if (!type || !seen.insert(type).second)
                return false;
            switch (type->kind)
            {
                case ir::IrTypeKind::Pointer: {
                    auto* pointer = static_cast<ir::IrPointerType const*>(type);
                    return pointer->flavor != ir::PointerFlavor::Near || has_segmented_type(pointer->pointee, seen);
                }
                case ir::IrTypeKind::Slice: {
                    auto* slice = static_cast<ir::IrSliceType const*>(type);
                    return slice->flavor != ir::PointerFlavor::Near || has_segmented_type(slice->element, seen);
                }
                case ir::IrTypeKind::Array:
                    return has_segmented_type(static_cast<ir::IrArrayType const*>(type)->element, seen);
                case ir::IrTypeKind::Aggregate:
                    return std::ranges::any_of(static_cast<ir::IrAggregateType const*>(type)->members,
                                               [&](ir::IrType const* member) { return has_segmented_type(member, seen); });
                default:
                    return false;
            }
        }

        [[nodiscard]] bool has_segmented_type(ir::IrType const* type)
        {
            std::unordered_set<ir::IrType const*> seen;
            return has_segmented_type(type, seen);
        }

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

                if (!input->module_asms.empty())
                    return fail("i8086 backend: module asm is not supported yet");
                for (auto* global : input->globals)
                    if (global && has_segmented_type(global->type))
                        return fail(std::format("i8086 backend: global `{}` uses far or based pointers, which are not supported yet", global->name));

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
                    functions.push_back(std::move(mfunc));
                }

                if (opts.requested_artifacts.contains(ArtifactKind::MirText))
                {
                    std::string text;
                    for (auto const& f : functions)
                    {
                        text += std::format("func {} [frame_size={}]\n", f.name(), f.frame_size);
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
                    auto text = i8086::emit_intel_asm(*input, functions, opts.target);
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
                    auto bytes = object::write_elf32(*input, mmod, encoded, opts.target, object::elf32_i8086_policy);
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
