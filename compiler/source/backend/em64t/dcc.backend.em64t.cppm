module;

#include <cstdio>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

export module dcc.backend.em64t;

import std;
import dcc.backend;
import dcc.ir;
import dcc.ir.pipeline;
import dcc.target;
import dcc.backend.x86.mir;
import dcc.backend.inline_asm;
import dcc.backend.em64t.isel;
import dcc.backend.x86.regalloc;
import dcc.backend.em64t.registers;
import dcc.backend.em64t.framelay;
import dcc.backend.x86.encode;
import dcc.backend.object.elf64;
import dcc.backend.object.coff;
import dcc.backend.object.archive;
import dcc.backend.em64t.assembler;

using namespace dcc::backend::x86;

export namespace dcc::backend
{
    [[nodiscard]] std::unique_ptr<Backend> make_em64t_backend();
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
                    auto* ptr = static_cast<ir::IrPointerType const*>(type);
                    return ptr->flavor != ir::PointerFlavor::Near || has_segmented_type(ptr->pointee, seen);
                }
                case ir::IrTypeKind::Slice: {
                    auto* slice = static_cast<ir::IrSliceType const*>(type);
                    return slice->flavor != ir::PointerFlavor::Near || has_segmented_type(slice->element, seen);
                }
                case ir::IrTypeKind::Array:
                    return has_segmented_type(static_cast<ir::IrArrayType const*>(type)->element, seen);
                case ir::IrTypeKind::Aggregate:
                    for (auto* member : static_cast<ir::IrAggregateType const*>(type)->members)
                        if (has_segmented_type(member, seen))
                            return true;
                    return false;
                case ir::IrTypeKind::Func: {
                    auto* func = static_cast<ir::IrFuncType const*>(type);
                    if (has_segmented_type(func->return_type, seen))
                        return true;
                    for (auto* param : func->params)
                        if (has_segmented_type(param, seen))
                            return true;
                    return false;
                }
                default:
                    return false;
            }
        }

        [[nodiscard]] bool has_segmented_type(ir::IrType const* type)
        {
            std::unordered_set<ir::IrType const*> seen;
            return has_segmented_type(type, seen);
        }

        class Em64tBackendImpl : public Backend
        {
        public:
            Em64tBackendImpl() = default;

            [[nodiscard]] std::string_view name() const override { return "custom"; }

            [[nodiscard]] std::set<ArtifactKind> supported_artifacts() const override
            {
                return {ArtifactKind::MirText,
                        ArtifactKind::AsmText,
                        ArtifactKind::ObjectBytes,
                        ArtifactKind::ExecutableBytes,
                        ArtifactKind::SharedLibraryBytes,
                        ArtifactKind::ArchiveBytes};
            }

            [[nodiscard]] BackendArtifact emit(ir::IrModule const& module, BackendOptions const& opts) override
            {
                BackendArtifact artifact;

                bool want_mir = opts.requested_artifacts.contains(ArtifactKind::MirText);
                bool want_asm = opts.requested_artifacts.contains(ArtifactKind::AsmText);
                bool want_obj = opts.requested_artifacts.contains(ArtifactKind::ObjectBytes);
                bool want_exe = opts.requested_artifacts.contains(ArtifactKind::ExecutableBytes);
                bool want_shared = opts.requested_artifacts.contains(ArtifactKind::SharedLibraryBytes);
                bool want_archive = opts.requested_artifacts.contains(ArtifactKind::ArchiveBytes);
                bool need_encode = want_obj || want_exe || want_shared || want_archive;

                ir::IrModule const* input_module = &module;
                ir::IrContext opt_ctx{256 * 1024, &opts.target};
                // TODO: honor opts.target.cpu baseline for instruction selection
                if (opts.opt_level > dcc::ir::pass::OptLevel::O0 && !std::getenv("DCC_BENCH_SKIP_IR_PASSES"))
                    input_module = dcc::ir::pass::global_pass_manager().run(module, opt_ctx, opts.opt_level);

                for (auto* global : input_module->globals)
                    if (global && has_segmented_type(global->type))
                    {
                        artifact.diagnostics.push_back(BackendDiagnostic{{}, "far and based pointers are not supported by this backend yet"});
                        return artifact;
                    }
                for (auto* func : input_module->functions)
                {
                    if (!func)
                        continue;
                    if (has_segmented_type(func->func_type))
                    {
                        artifact.diagnostics.push_back(BackendDiagnostic{{}, "far and based pointers are not supported by this backend yet"});
                        return artifact;
                    }
                    for (auto* block : func->blocks)
                    {
                        if (!block)
                            continue;
                        for (auto* inst : block->instructions)
                            if (inst &&
                                (has_segmented_type(inst->type) || inst->kind == ir::IrNodeKind::ReadSegment || inst->kind == ir::IrNodeKind::PointerSegment))
                            {
                                artifact.diagnostics.push_back(BackendDiagnostic{{}, "far and based pointers are not supported by this backend yet"});
                                return artifact;
                            }
                    }
                }

                if (auto mismatch = find_bad_global_initializer(*input_module))
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, std::format("custom backend: malformed initializer for global `{}`", *mismatch)});
                    return artifact;
                }
                if (!input_module->module_asms.empty())
                {
                    auto* ma = input_module->module_asms.front();
                    artifact.diagnostics.push_back(
                        BackendDiagnostic{ma ? ma->range : decltype(ma->range){}, "custom backend: module asm is not supported on the native backend"});
                    return artifact;
                }

                std::vector<x86::MFunction> mfuncs;
                mfuncs.reserve(input_module->functions.size());

                for (auto* func : input_module->functions)
                {
                    if (!func || func->blocks.empty())
                        continue;

                    std::vector<InlineAsmDiag> asm_diags;
                    auto mfunc = em64t::isel_function(*func, opts.target, &asm_diags);
                    for (auto& diag : asm_diags)
                        artifact.diagnostics.push_back(BackendDiagnostic{diag.where, "custom backend: " + diag.message});
                    if (!asm_diags.empty())
                        return artifact;

                    em64t::regalloc(mfunc, opts.target);
                    if (opts.target.no_simd)
                    {
                        for (auto const& block : mfunc.blocks)
                            for (auto const& inst : block.instrs)
                                for (unsigned i = 0; i < inst.num_ops; ++i)
                                    if (inst.ops[i].kind == x86::MOpKind::Reg && inst.ops[i].reg.is_physical() &&
                                        x86::reg_class(inst.ops[i].reg.phys_reg()) == x86::RegClass::XMM)
                                    {
                                        artifact.diagnostics.push_back(
                                            BackendDiagnostic{.where = {}, .message = "custom backend: floating-point or SIMD operation requires SIMD"});
                                        return artifact;
                                    }
                    }

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
                    artifact.asm_text = em64t::emit_intel_asm(*input_module, mfuncs, opts.target);

                if (need_encode)
                {
                    x86::MModule mmod;
                    std::vector<x86::EncodeResult> encoded;

                    mmod.functions.reserve(mfuncs.size());
                    encoded.reserve(mfuncs.size());

                    for (auto& mfunc : mfuncs)
                    {
                        auto result = x86::encode_function(mfunc, x86::EncodeMode::Long64);

                        for (auto const& w : result.warnings)
                            if (w.find("encoding as") == std::string_view::npos)
                                artifact.diagnostics.push_back(dcc::backend::BackendDiagnostic{{}, w});

                        mmod.functions.push_back(std::move(mfunc));
                        encoded.push_back(std::move(result));
                    }

                    std::vector<std::uint8_t> object_data;

                    if (opts.target.object_format == dcc::target::ObjectFormat::Coff)
                        object_data = object::write_coff(*input_module, mmod, encoded, opts.target, object::coff_x86_64_policy);
                    else
                        object_data = object::write_elf64(*input_module, mmod, encoded, opts.target, object::elf64_x86_64_policy);

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
                            artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: executable output not supported for COFF target"});
                        }
                        else if (opts.target.arch != dcc::target::Arch::X86_64)
                        {
                            artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: executable output is only supported for x86_64 ELF targets"});
                        }
                        else
                        {
                            auto exe = link_executable(obj_bytes, opts, artifact);
                            if (exe)
                                artifact.executable_bytes = std::move(exe);
                        }
                    }

                    if (want_shared)
                    {
                        if (opts.target.object_format == dcc::target::ObjectFormat::Coff)
                        {
                            auto dll = link_shared_library_coff(obj_bytes, opts, artifact);
                            if (dll)
                                artifact.shared_library_bytes = std::move(dll);
                        }
                        else if (opts.target.arch != dcc::target::Arch::X86_64)
                        {
                            artifact.diagnostics.push_back(
                                BackendDiagnostic{{}, "custom backend: shared library output is only supported for x86_64 ELF targets"});
                        }
                        else
                        {
                            auto so = link_shared_library(obj_bytes, opts, artifact);
                            if (so)
                                artifact.shared_library_bytes = std::move(so);
                        }
                    }

                    if (want_archive)
                    {
                        if (opts.target.object_format == dcc::target::ObjectFormat::Coff)
                        {
                            artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: archive output not supported for COFF target"});
                        }
                        else
                        {
                            std::string member_name{input_module->name};
                            if (member_name.empty())
                                member_name = "module";
                            member_name += ".o";

                            std::vector<std::pair<std::string, std::vector<std::uint8_t>>> archive_members;
                            archive_members.emplace_back(std::move(member_name), std::move(object_data));
                            auto archive_data = object::write_archive_elf(archive_members);

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
            [[nodiscard]] static bool ir_init_compatible(ir::IrValue const* v, ir::IrType const* storage)
            {
                if (!v || !storage)
                    return false;

                switch (v->kind)
                {
                    case ir::IrNodeKind::GlobalRef:
                        return storage->kind == ir::IrTypeKind::Pointer && storage->byte_size >= v->type->byte_size;
                    case ir::IrNodeKind::Aggregate: {
                        auto* agg = static_cast<ir::IrAggregateInst const*>(v);
                        if (storage->kind == ir::IrTypeKind::Aggregate)
                        {
                            auto* at = static_cast<ir::IrAggregateType const*>(storage);
                            auto n = std::min(agg->values.size(), at->members.size());
                            for (std::size_t i = 0; i < n; ++i)
                                if (agg->values[i] && !ir_init_compatible(agg->values[i], at->members[i]))
                                    return false;
                            return true;
                        }
                        if (storage->kind == ir::IrTypeKind::Array)
                        {
                            auto* at = static_cast<ir::IrArrayType const*>(storage);
                            auto n = std::min<std::uint64_t>(agg->values.size(), at->count);
                            for (std::uint64_t i = 0; i < n; ++i)
                                if (agg->values[static_cast<std::size_t>(i)] && !ir_init_compatible(agg->values[static_cast<std::size_t>(i)], at->element))
                                    return false;
                            return true;
                        }
                        if (storage->kind == ir::IrTypeKind::Slice)
                        {
                            if (ir::slice_data_index < agg->values.size() && agg->values[ir::slice_data_index])
                            {
                                auto const* dv = agg->values[ir::slice_data_index];
                                if (!ir_init_compatible(dv, dv->type))
                                    return false;
                            }
                            return true;
                        }
                        return false;
                    }
                    default:
                        return true;
                }
            }

            [[nodiscard]] static std::optional<std::string> find_bad_global_initializer(ir::IrModule const& module)
            {
                for (auto* g : module.globals)
                {
                    if (!g || !g->init)
                        continue;
                    if (!ir_init_compatible(g->init, g->type))
                        return std::string{g->name};
                }
                return std::nullopt;
            }

            [[nodiscard]] static std::string print_function(x86::MFunction const& mfunc)
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
                        out += x86::format_instr(mi);
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
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot get temp directory"});
                    return std::nullopt;
                }

                auto tag = std::format("dcc-em64t-link-{}", std::chrono::steady_clock::now().time_since_epoch().count());
                auto work_dir = tmp_dir / tag;
                if (!fs::create_directories(work_dir, ec))
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot create temp directory"});
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
                        artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot write object file"});
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
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot run linker"});
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
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: linking failed:\n" + link_output});
                    cleanup();
                    return std::nullopt;
                }

                std::ifstream exe_in{exe_path, std::ios::binary};
                if (!exe_in)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot read linked executable"});
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

            [[nodiscard]] static std::optional<std::vector<std::byte>> link_shared_library(std::vector<std::byte> const& object_bytes,
                                                                                           BackendOptions const& opts, BackendArtifact& artifact)
            {
                if (!opts.target.position_independent_code)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: shared library requires -fPIC"});
                    return std::nullopt;
                }

                namespace fs = std::filesystem;

                std::error_code ec;
                auto tmp_dir = fs::temp_directory_path(ec);
                if (ec)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot get temp directory"});
                    return std::nullopt;
                }

                auto tag = std::format("dcc-em64t-link-{}", std::chrono::steady_clock::now().time_since_epoch().count());
                auto work_dir = tmp_dir / tag;
                if (!fs::create_directories(work_dir, ec))
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot create temp directory"});
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
                        artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot write object file"});
                        cleanup();
                        return std::nullopt;
                    }
                    of.write(reinterpret_cast<char const*>(object_bytes.data()), static_cast<std::streamsize>(object_bytes.size()));
                }

                auto so_path = work_dir / "out.so";

                std::string link_cmd = std::format("ld.lld --shared -o {} {}", so_path.string(), obj_path.string());

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
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot run linker"});
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
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: linking failed:\n" + link_output});
                    cleanup();
                    return std::nullopt;
                }

                std::ifstream so_in{so_path, std::ios::binary};
                if (!so_in)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot read linked shared library"});
                    cleanup();
                    return std::nullopt;
                }

                std::vector<std::byte> so_bytes;
                so_in.seekg(0, std::ios::end);
                auto so_size = static_cast<std::size_t>(so_in.tellg());
                so_in.seekg(0, std::ios::beg);
                so_bytes.resize(so_size);
                so_in.read(reinterpret_cast<char*>(so_bytes.data()), static_cast<std::streamsize>(so_size));

                cleanup();
                return so_bytes;
            }

            [[nodiscard]] static std::vector<std::string> parse_coff_exports(std::span<std::byte const> obj)
            {
                std::vector<std::string> exports;
                if (obj.size() < 20)
                    return exports;

                auto rd16 = [&](std::size_t off) -> std::uint16_t {
                    if (off + 2 > obj.size())
                        return 0;
                    auto lo = static_cast<unsigned>(static_cast<unsigned char>(obj[off]));
                    auto hi = static_cast<unsigned>(static_cast<unsigned char>(obj[off + 1]));
                    return static_cast<std::uint16_t>(lo | (hi << 8));
                };
                auto rd32 = [&](std::size_t off) -> std::uint32_t {
                    if (off + 4 > obj.size())
                        return 0;
                    auto b0 = static_cast<unsigned>(static_cast<unsigned char>(obj[off]));
                    auto b1 = static_cast<unsigned>(static_cast<unsigned char>(obj[off + 1]));
                    auto b2 = static_cast<unsigned>(static_cast<unsigned char>(obj[off + 2]));
                    auto b3 = static_cast<unsigned>(static_cast<unsigned char>(obj[off + 3]));
                    return static_cast<std::uint32_t>(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
                };

                auto num_sec = rd16(2);
                auto sym_tab = rd32(8);
                auto num_syms = rd32(12);
                if (sym_tab == 0 || num_syms == 0)
                    return exports;

                auto str_tab_off = static_cast<std::size_t>(sym_tab) + static_cast<std::size_t>(num_syms) * 18;
                if (str_tab_off + 4 > obj.size())
                    return exports;

                (void)num_sec;

                auto read_name = [&](std::size_t entry_off) -> std::string {
                    if (entry_off + 8 > obj.size())
                        return {};
                    std::uint32_t name_1 = rd32(entry_off);
                    if (name_1 == 0)
                    {
                        auto str_off = rd32(entry_off + 4);
                        auto actual_off = str_tab_off + static_cast<std::size_t>(str_off);
                        if (actual_off >= obj.size())
                            return {};
                        std::string name;
                        while (actual_off < obj.size() && obj[actual_off] != std::byte{0})
                            name += static_cast<char>(obj[actual_off++]);
                        return name;
                    }
                    else
                    {
                        std::string name;
                        for (unsigned i = 0; i < 8 && entry_off + i < obj.size(); ++i)
                        {
                            auto c = static_cast<char>(obj[entry_off + i]);
                            if (c == '\0')
                                break;
                            name += c;
                        }
                        return name;
                    }
                };

                for (std::uint32_t idx = 0; idx < num_syms;)
                {
                    auto entry_off = static_cast<std::size_t>(sym_tab) + static_cast<std::size_t>(idx) * 18;
                    if (entry_off + 18 > obj.size())
                        break;

                    auto name = read_name(entry_off);
                    auto sec_num = static_cast<std::int16_t>(rd16(entry_off + 12));
                    auto storage_class = static_cast<std::uint8_t>(obj[entry_off + 16]);
                    auto aux_count = static_cast<std::uint8_t>(obj[entry_off + 17]);

                    if (storage_class == 2 && sec_num > 0)
                    {
                        if (name != ".text" && name != ".data" && name != ".bss" && name != ".rdata" && !name.starts_with("__imp_"))
                        {
                            exports.push_back(name);
                        }
                    }

                    idx += 1U + aux_count;
                }

                return exports;
            }

            [[nodiscard]] static std::optional<std::vector<std::byte>> link_shared_library_coff(std::vector<std::byte> const& object_bytes,
                                                                                                BackendOptions const& opts, BackendArtifact& artifact)
            {
                namespace fs = std::filesystem;

                std::error_code ec;
                auto tmp_dir = fs::temp_directory_path(ec);
                if (ec)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot get temp directory"});
                    return std::nullopt;
                }

                auto tag = std::format("dcc-em64t-link-{}", std::chrono::steady_clock::now().time_since_epoch().count());
                auto work_dir = tmp_dir / tag;
                if (!fs::create_directories(work_dir, ec))
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot create temp directory"});
                    return std::nullopt;
                }

                auto cleanup = [&]() {
                    std::error_code ec2;
                    fs::remove_all(work_dir, ec2);
                };

                auto obj_path = work_dir / "module.obj";
                {
                    std::ofstream of{obj_path, std::ios::binary};
                    if (!of)
                    {
                        artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot write object file"});
                        cleanup();
                        return std::nullopt;
                    }
                    of.write(reinterpret_cast<char const*>(object_bytes.data()), static_cast<std::streamsize>(object_bytes.size()));
                }

                auto exports = parse_coff_exports(object_bytes);

                auto dll_path = work_dir / "out.dll";

                std::string link_cmd = std::format("lld-link /dll /noentry /machine:x64 /out:{} {}", dll_path.string(), obj_path.string());

                for (auto const& sym : exports)
                    link_cmd += std::format(" /export:{}", sym);

                for (auto const& la : opts.linker_args)
                    link_cmd += " " + la;

                link_cmd += " 2>&1";
                auto* pipe = popen(link_cmd.c_str(), "r");
                if (!pipe)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot run lld-link"});
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
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: linking failed:\n" + link_output});
                    cleanup();
                    return std::nullopt;
                }

                std::ifstream dll_in{dll_path, std::ios::binary};
                if (!dll_in)
                {
                    artifact.diagnostics.push_back(BackendDiagnostic{{}, "custom backend: cannot read linked DLL"});
                    cleanup();
                    return std::nullopt;
                }

                std::vector<std::byte> dll_bytes;
                dll_in.seekg(0, std::ios::end);
                auto dll_size = static_cast<std::size_t>(dll_in.tellg());
                dll_in.seekg(0, std::ios::beg);
                dll_bytes.resize(dll_size);
                dll_in.read(reinterpret_cast<char*>(dll_bytes.data()), static_cast<std::streamsize>(dll_size));

                cleanup();
                return dll_bytes;
            }
        };

    } // anonymous namespace

    std::unique_ptr<Backend> make_em64t_backend()
    {
        return std::make_unique<Em64tBackendImpl>();
    }

} // namespace dcc::backend
