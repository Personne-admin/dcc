export module dcc.backend.em64t;

import std;
import dcc.backend;
import dcc.ir;
import dcc.ir.pass;
import dcc.ir.transforms;
import dcc.target;
import dcc.backend.em64t.mir;
import dcc.backend.em64t.isel;
import dcc.backend.em64t.regalloc;
import dcc.backend.em64t.framelay;
import dcc.backend.em64t.encode;
import dcc.backend.em64t.objwriter;

export namespace dcc::backend
{
    [[nodiscard]] std::unique_ptr<Backend> make_em64t_backend();
}

namespace dcc::backend
{
    namespace
    {
        class Em64tBackendImpl : public Backend
        {
        public:
            Em64tBackendImpl() = default;

            [[nodiscard]] std::string_view name() const override { return "em64t"; }

            [[nodiscard]] std::set<ArtifactKind> supported_artifacts() const override
            {
                return {ArtifactKind::AsmText, ArtifactKind::ObjectBytes};
            }

            [[nodiscard]] BackendArtifact emit(ir::IrModule const& module, BackendOptions const& opts) override
            {
                BackendArtifact artifact;

                bool want_asm = opts.requested_artifacts.contains(ArtifactKind::AsmText);
                bool want_obj = opts.requested_artifacts.contains(ArtifactKind::ObjectBytes);

                ir::IrModule const* input_module = &module;
                ir::IrContext opt_ctx{256 * 1024, &opts.target};
                if (opts.opt_level > dcc::ir::pass::OptLevel::O0)
                    input_module = dcc::ir::pass::global_pass_manager().run(module, opt_ctx, opts.opt_level);

                std::vector<em64t::MFunction> mfuncs;
                mfuncs.reserve(input_module->functions.size());

                for (auto* func : input_module->functions)
                {
                    if (!func)
                        continue;

                    auto mfunc = em64t::isel_function(*func, opts.target);
                    em64t::regalloc(mfunc, opts.target);
                    em64t::frame_layout(mfunc, opts.target);
                    mfuncs.push_back(std::move(mfunc));
                }

                if (want_asm)
                {
                    std::string asm_text;
                    asm_text += "; dcc em64t MIR backend output\n";
                    asm_text += "; module: ";
                    asm_text += input_module->name.empty() ? "<unnamed>" : std::string{input_module->name};
                    asm_text += "\n\n";

                    for (auto const& mfunc : mfuncs)
                    {
                        asm_text += print_function(mfunc);
                        asm_text += "\n";
                    }

                    artifact.asm_text = std::move(asm_text);
                }

                if (want_obj)
                {
                    em64t::MModule mmod;
                    std::vector<em64t::EncodeResult> encoded;

                    mmod.functions.reserve(mfuncs.size());
                    encoded.reserve(mfuncs.size());

                    for (auto& mfunc : mfuncs)
                    {
                        auto result = em64t::encode_function(mfunc);

                        for (auto const& w : result.warnings)
                            if (w.find("encoding as") == std::string_view::npos)
                                artifact.diagnostics.push_back(dcc::backend::BackendDiagnostic{{}, w});

                        mmod.functions.push_back(std::move(mfunc));
                        encoded.push_back(std::move(result));
                    }

                    std::vector<std::uint8_t> object_data;

                    if (opts.target.object_format == dcc::target::ObjectFormat::Coff)
                        object_data = em64t::write_coff(*input_module, mmod, encoded, opts.target);
                    else
                        object_data = em64t::write_elf64(*input_module, mmod, encoded, opts.target);

                    std::vector<std::byte> obj_bytes;
                    obj_bytes.reserve(object_data.size());
                    for (auto b : object_data)
                        obj_bytes.push_back(static_cast<std::byte>(b));

                    artifact.object_bytes = std::move(obj_bytes);
                }

                return artifact;
            }

        private:
            [[nodiscard]] static std::string print_function(em64t::MFunction const& mfunc)
            {
                std::string out;
                out += "func ";
                out += mfunc.owned_name;
                out += " [frame_size=";
                out += std::to_string(mfunc.frame_size);
                out += ", vregs=";
                out += std::to_string(mfunc.next_vreg_id - 1);
                out += ", blocks=";
                out += std::to_string(mfunc.blocks.size());
                out += "]\n";

                for (auto const& bb : mfunc.blocks)
                {
                    out += bb.display_name();
                    out += ":";
                    out += "  preds=[";
                    bool first = true;
                    for (auto pid : bb.preds)
                    {
                        if (!first)
                            out += ", ";
                        auto* pbb = mfunc.block_by_id(pid);
                        out += pbb ? pbb->display_name() : std::to_string(pid);
                        first = false;
                    }
                    out += "]  succs=[";
                    first = true;
                    for (auto sid : bb.succs)
                    {
                        if (!first)
                            out += ", ";
                        auto* sbb = mfunc.block_by_id(sid);
                        out += sbb ? sbb->display_name() : std::to_string(sid);
                        first = false;
                    }
                    out += "]\n";

                    for (auto const& mi : bb.instrs)
                    {
                        out += em64t::format_instr(mi);
                        out += '\n';
                    }
                }

                return out;
            }
        };

    } // anonymous namespace

    std::unique_ptr<Backend> make_em64t_backend()
    {
        return std::make_unique<Em64tBackendImpl>();
    }

} // namespace dcc::backend
