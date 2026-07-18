module;

#include <cstdio>

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
import dcc.backend.em64t.assembler;

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
                return {ArtifactKind::MirText, ArtifactKind::AsmText, ArtifactKind::ObjectBytes, ArtifactKind::ExecutableBytes, ArtifactKind::ArchiveBytes};
            }

            [[nodiscard]] BackendArtifact emit(ir::IrModule const& module, BackendOptions const& opts) override
            {
                BackendArtifact artifact;

                bool want_mir = opts.requested_artifacts.contains(ArtifactKind::MirText);
                bool want_asm = opts.requested_artifacts.contains(ArtifactKind::AsmText);
                bool want_obj = opts.requested_artifacts.contains(ArtifactKind::ObjectBytes);
                bool want_exe = opts.requested_artifacts.contains(ArtifactKind::ExecutableBytes);
                bool want_archive = opts.requested_artifacts.contains(ArtifactKind::ArchiveBytes);
                bool need_encode = want_obj || want_exe || want_archive;

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

                if (want_mir)
                {
                    std::string mir_out;
                    for (auto const& mf : mfuncs)
                    {
                        mir_out += print_function(mf);
                        mir_out += '\n';
                    }
                    artifact.mir_text = std::move(mir_out);
                }

                if (want_asm)
                    artifact.asm_text = emit_intel_asm(*input_module, mfuncs, opts.target);

                if (need_encode)
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

                    if (want_obj)
                        artifact.object_bytes = obj_bytes;

                    if (want_exe)
                    {
                        if (opts.target.object_format == dcc::target::ObjectFormat::Coff)
                        {
                            artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: executable output not supported for COFF target"});
                        }
                        else if (opts.target.arch != dcc::target::Arch::X86_64)
                        {
                            artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: executable output is only supported for x86_64 ELF targets"});
                        }
                        else
                        {
                            auto exe = link_executable(obj_bytes, opts, artifact);
                            if (exe)
                                artifact.executable_bytes = std::move(exe);
                        }
                    }

                    if (want_archive)
                    {
                        if (opts.target.object_format == dcc::target::ObjectFormat::Coff)
                        {
                            artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: archive output not supported for COFF target"});
                        }
                        else
                        {
                            std::string member_name{input_module->name};
                            if (member_name.empty())
                                member_name = "module";
                            member_name += ".o";

                            std::vector<std::pair<std::string, std::vector<std::uint8_t>>> archive_members;
                            archive_members.emplace_back(std::move(member_name), std::move(object_data));
                            auto archive_data = em64t::write_archive_elf(archive_members);

                            std::vector<std::byte> archive_bytes;
                            archive_bytes.reserve(archive_data.size());
                            for (auto b : archive_data)
                                archive_bytes.push_back(static_cast<std::byte>(b));
                            artifact.archive_bytes = std::move(archive_bytes);
                        }
                    }
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

            [[nodiscard]] static std::optional<std::vector<std::byte>> link_executable(std::vector<std::byte> const& object_bytes, BackendOptions const& opts,
                                                                                       BackendArtifact& artifact)
            {
                namespace fs = std::filesystem;

                std::error_code ec;
                auto tmp_dir = fs::temp_directory_path(ec);
                if (ec)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: cannot get temp directory"});
                    return std::nullopt;
                }

                auto tag = std::format("dcc-em64t-link-{}", std::chrono::steady_clock::now().time_since_epoch().count());
                auto work_dir = tmp_dir / tag;
                if (!fs::create_directories(work_dir, ec))
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: cannot create temp directory"});
                    return std::nullopt;
                }

                auto cleanup = [&]() {
                    std::error_code ec2;
                    fs::remove_all(work_dir, ec2);
                };

                auto obj_path = work_dir / "module.o";
                {
                    std::ofstream of{obj_path, std::ios::binary};
                    if (!of)
                    {
                        artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: cannot write object file"});
                        cleanup();
                        return std::nullopt;
                    }
                    of.write(reinterpret_cast<char const*>(object_bytes.data()), static_cast<std::streamsize>(object_bytes.size()));
                }

                auto exe_path = work_dir / "out";

                std::string link_cmd = std::format("ld.lld --static --no-dynamic-linker --fatal-warnings -o {} {}", exe_path.string(), obj_path.string());

                for (auto const& obj : opts.additional_objects)
                    link_cmd += " " + obj;

                for (auto const& lp : opts.library_paths)
                    link_cmd += " -L" + lp;

                for (auto const& lib : opts.libraries)
                    link_cmd += " -l" + lib;

                for (auto const& la : opts.linker_args)
                    link_cmd += " " + la;

                link_cmd += " 2>&1";
                auto* pipe = popen(link_cmd.c_str(), "r");
                if (!pipe)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: cannot run linker"});
                    cleanup();
                    return std::nullopt;
                }

                std::string link_output;
                std::array<char, 4096> buf;
                while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
                    link_output += buf.data();

                int link_rc = pclose(pipe);
                if (link_rc != 0)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: linking failed:\n" + link_output});
                    cleanup();
                    return std::nullopt;
                }

                std::ifstream exe_in{exe_path, std::ios::binary};
                if (!exe_in)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "em64t backend: cannot read linked executable"});
                    cleanup();
                    return std::nullopt;
                }

                std::vector<std::byte> exe_bytes;
                exe_in.seekg(0, std::ios::end);
                auto exe_size = static_cast<std::size_t>(exe_in.tellg());
                exe_in.seekg(0, std::ios::beg);
                exe_bytes.resize(exe_size);
                exe_in.read(reinterpret_cast<char*>(exe_bytes.data()), static_cast<std::streamsize>(exe_size));

                cleanup();
                return exe_bytes;
            }
        };

    } // anonymous namespace

    std::unique_ptr<Backend> make_em64t_backend()
    {
        return std::make_unique<Em64tBackendImpl>();
    }

} // namespace dcc::backend
