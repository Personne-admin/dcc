module;

#include <array>
#include <cstdio>
#include <llvm-c/Analysis.h>
#include <llvm-c/Comdat.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

export module dcc.backend.llvm;

import std;
import dcc.backend;
import dcc.ir;
import dcc.ir.pass;
import dcc.ir.transforms;
import dcc.backend.inline_asm;
import dcc.target;
import dcc.sm;

export namespace dcc::backend
{
    [[nodiscard]] auto make_llvm_backend() -> std::unique_ptr<Backend>;

} // namespace dcc::backend

module :private;

namespace dcc::backend
{
    namespace
    {
        using namespace dcc::ir;
        using namespace dcc::target;

        [[nodiscard]] LLVMValueRef llvm_const_int(LLVMTypeRef type, unsigned long long value, bool sign_extend)
        {
            auto const width = LLVMGetIntTypeWidth(type);
            if (width < 64)
                value &= (std::uint64_t{1} << width) - 1;
            return LLVMConstInt(type, value, sign_extend);
        }

        struct DebugEmitContext
        {
            LLVMDIBuilderRef dibuilder = nullptr;
            LLVMMetadataRef difile = nullptr;
            LLVMMetadataRef dicu = nullptr;
            std::unordered_map<IrFunction const*, LLVMMetadataRef> subprogram_map;
            std::unordered_map<std::uint32_t, LLVMMetadataRef> file_map;
            sm::SourceManager const* sm = nullptr;
            std::string comp_dir;
            bool finalized = false;

            [[nodiscard]] LLVMMetadataRef get_or_create_file(std::uint32_t file_id)
            {
                if (file_id == static_cast<std::uint32_t>(sm::FileId::Invalid))
                    return difile;

                auto it = file_map.find(file_id);
                if (it != file_map.end())
                    return it->second;

                if (!sm)
                    return difile;

                auto* sf = sm->get(static_cast<sm::FileId>(static_cast<std::uint32_t>(file_id)));
                if (!sf)
                    return difile;

                auto path = sf->path();
                std::string filename;
                std::string directory = comp_dir;

                if (!path.empty())
                {
                    std::error_code ec;
                    auto abs_path = std::filesystem::weakly_canonical(path, ec);
                    if (!ec)
                    {
                        auto rel = std::filesystem::proximate(abs_path, std::filesystem::path(comp_dir), ec);
                        if (!ec && !rel.empty())
                            filename = rel.generic_string();
                        else
                        {
                            filename = abs_path.generic_string();
                            directory.clear();
                        }
                    }
                    else
                    {
                        filename = path.generic_string();
                        directory.clear();
                    }
                }
                else
                {
                    filename = "<unknown>";
                    directory.clear();
                }

                auto* file_node = LLVMDIBuilderCreateFile(dibuilder, filename.c_str(), filename.size(), directory.empty() ? "." : directory.c_str(),
                                                          directory.empty() ? 1 : directory.size());
                file_map[file_id] = file_node;
                return file_node;
            }

            void finalize()
            {
                if (finalized)
                    return;

                if (dibuilder)
                    LLVMDIBuilderFinalize(dibuilder);

                finalized = true;
            }

            ~DebugEmitContext()
            {
                finalize();
                if (dibuilder)
                    LLVMDisposeDIBuilder(dibuilder);
            }
        };

        [[nodiscard]] std::string llvm_codegen_triple(TargetConfig const& cfg, DebugFormat debug_format = DebugFormat::Auto)
        {
            auto const& t = cfg.triple;
            if (t == "x86_64-elf")
                return "x86_64-elf";

            if (t == "x86-elf")
                return "i386-elf";

            if (t == "x86_64-coff")
            {
                if (debug_format == DebugFormat::Dwarf)
                    return "x86_64-w64-windows-gnu";

                return "x86_64-pc-windows-msvc";
            }

            if (t == "x86-coff")
            {
                if (debug_format == DebugFormat::Dwarf)
                    return "i686-w64-windows-gnu";

                return "i386-pc-windows-msvc";
            }

            return t;
        }

        [[nodiscard]] DebugFormat resolve_debug_format(BackendOptions const& opts)
        {
            if (!opts.emit_debug_info || opts.debug_format == DebugFormat::None)
                return DebugFormat::None;

            if (opts.debug_format != DebugFormat::Auto)
                return opts.debug_format;

            if (opts.target.object_format == ObjectFormat::Coff)
                return DebugFormat::Pdb;
            if (opts.target.object_format == ObjectFormat::Elf)
                return DebugFormat::Dwarf;

            auto const& t = opts.target.triple;
            if (t.contains("coff") || t.contains("windows") || t.contains("msvc"))
                return DebugFormat::Pdb;

            return DebugFormat::Dwarf;
        }

        [[nodiscard]] std::string llvm_target_features(TargetConfig const& target)
        {
            std::string features;

            if (target.no_x87)
            {
                if (!features.empty())
                    features += ',';
                features += "-x87";
            }

            if (target.no_simd)
            {
                if (!features.empty())
                    features += ',';
                features += "-mmx,-sse,-sse2,-sse3,-ssse3,-sse4.1,-sse4.2,-avx,-avx2,-avx512f";
            }

            if (target.arch == Arch::X86 || target.arch == Arch::X86_64)
            {
                auto const cpu =
                    target.cpu.empty() ? (target.arch == Arch::X86_64 ? std::string_view{"generic"} : std::string_view{"i686"}) : std::string_view{target.cpu};

                if (TargetConfig::cpu_is_pre_i686(cpu))
                {
                    if (!features.empty())
                        features += ',';
                    features += "-cmov";
                }
            }

            return features;
        }

        [[nodiscard]] LLVMRelocMode llvm_reloc_mode(TargetConfig const& target)
        {
            return target.position_independent_code ? LLVMRelocPIC : LLVMRelocDefault;
        }

        [[nodiscard]] LLVMCodeModel llvm_code_model(TargetConfig const& target)
        {
            switch (target.code_model)
            {
                case CodeModel::Default:
                    return LLVMCodeModelDefault;
                case CodeModel::Small:
                    return LLVMCodeModelSmall;
                case CodeModel::Kernel:
                    return LLVMCodeModelKernel;
                case CodeModel::Medium:
                    return LLVMCodeModelMedium;
                case CodeModel::Large:
                    return LLVMCodeModelLarge;
            }
            return LLVMCodeModelDefault;
        }

        [[nodiscard]] bool is_bool_type(IrType const* t)
        {
            return t && t->kind == IrTypeKind::Bool;
        }

        void add_diag(std::vector<BackendDiagnostic>& diags, sm::SourceRange where, std::string msg)
        {
            diags.push_back(BackendDiagnostic{where, std::move(msg)});
        }

        [[nodiscard]] LLVMTypeRef c_api_type(IrType const* t, LLVMContextRef ctx, bool for_memory = false)
        {
            if (!t)
                return LLVMVoidTypeInContext(ctx);

            switch (t->kind)
            {
                case IrTypeKind::Void:
                    return LLVMVoidTypeInContext(ctx);
                case IrTypeKind::Bool:
                    return for_memory ? LLVMInt8TypeInContext(ctx) : LLVMInt1TypeInContext(ctx);
                case IrTypeKind::Int: {
                    auto* it = static_cast<IrIntType const*>(t);
                    return LLVMIntTypeInContext(ctx, it->bits);
                }
                case IrTypeKind::Float: {
                    auto* ft = static_cast<IrFloatType const*>(t);
                    if (ft->bits == 32)
                        return LLVMFloatTypeInContext(ctx);
                    if (ft->bits == 64)
                        return LLVMDoubleTypeInContext(ctx);
                    return LLVMFloatTypeInContext(ctx);
                }
                case IrTypeKind::Pointer: {
                    auto* pt = static_cast<IrPointerType const*>(t);
                    unsigned address_space = 0;
                    if (pt->flavor == PointerFlavor::Based)
                    {
                        if (pt->seg == Segment::Gs)
                            address_space = 256;
                        else if (pt->seg == Segment::Fs)
                            address_space = 257;
                        else if (pt->seg == Segment::Ss)
                            address_space = 258;
                    }
                    return LLVMPointerTypeInContext(ctx, address_space);
                }
                default:
                    return nullptr;
            }
        }

        enum class AggregateAbi : std::uint8_t
        {
            Native,
            SysV64,
            Win64,
        };

        struct TypeCache
        {
            LLVMContextRef ctx;
            std::uint8_t pointer_bits;
            bool little_endian;
            std::unordered_map<IrType const*, LLVMTypeRef> map;
            std::unordered_map<IrType const*, std::vector<unsigned>> field_index_map;
            AggregateAbi aggregate_abi{AggregateAbi::Native};

            explicit TypeCache(LLVMContextRef c, std::uint8_t pb, bool le, AggregateAbi abi = AggregateAbi::Native)
                : ctx(c), pointer_bits(pb), little_endian(le), aggregate_abi(abi)
            {
            }

            [[nodiscard]] bool indirect(IrType const* t) const noexcept
            {
                if (!t)
                    return false;
                if (t->kind == IrTypeKind::Array && static_cast<IrArrayType const*>(t)->count > 16)
                    return true;
                return (t->kind == IrTypeKind::Aggregate || t->kind == IrTypeKind::Array) && t->byte_size > 2 * (pointer_bits / 8);
            }

            [[nodiscard]] LLVMTypeRef get(IrType const* t, bool for_memory)
            {
                if (!t)
                    return LLVMVoidTypeInContext(ctx);

                if (is_simple_type(t->kind))
                    return c_api_type(t, ctx, for_memory);

                auto it = map.find(t);
                if (it != map.end())
                    return it->second;

                if (t->kind == IrTypeKind::Array)
                {
                    auto* at2 = static_cast<IrArrayType const*>(t);
                    auto* elem_ty = get(at2->element, true);
                    auto* arr_ty = LLVMArrayType2(elem_ty, static_cast<unsigned>(at2->count));
                    map[t] = arr_ty;
                    return arr_ty;
                }

                if (t->kind == IrTypeKind::Slice)
                {
                    auto* slice = static_cast<IrSliceType const*>(t);
                    auto* usize_llvm = LLVMIntTypeInContext(ctx, pointer_bits);
                    unsigned address_space = 0;
                    if (slice->flavor == PointerFlavor::Based)
                    {
                        if (slice->seg == Segment::Gs)
                            address_space = 256;
                        else if (slice->seg == Segment::Fs)
                            address_space = 257;
                        else if (slice->seg == Segment::Ss)
                            address_space = 258;
                    }
                    LLVMTypeRef fields[] = {
                        LLVMPointerTypeInContext(ctx, address_space),
                        usize_llvm,
                    };
                    auto* slice_ty = LLVMStructTypeInContext(ctx, fields, 2, 0);
                    map[t] = slice_ty;
                    return slice_ty;
                }

                if (t->kind == IrTypeKind::Func)
                {
                    auto* ft = static_cast<IrFuncType const*>(t);
                    bool const sret = indirect(ft->return_type);
                    auto* ret_ty = sret ? LLVMVoidTypeInContext(ctx) : get(ft->return_type, for_memory);
                    if (!ret_ty)
                        ret_ty = LLVMVoidTypeInContext(ctx);
                    std::vector<LLVMTypeRef> param_tys;
                    param_tys.reserve(ft->params.size() + static_cast<std::size_t>(sret));
                    if (sret)
                        param_tys.push_back(LLVMPointerTypeInContext(ctx, 0));
                    for (auto* pt : ft->params)
                    {
                        auto* lt = indirect(pt) ? LLVMPointerTypeInContext(ctx, 0) : get(pt, for_memory);
                        if (!lt)
                            lt = LLVMInt32TypeInContext(ctx);
                        param_tys.push_back(lt);
                    }
                    auto* func_ty = LLVMFunctionType(ret_ty, param_tys.data(), static_cast<unsigned>(param_tys.size()), 0);
                    map[t] = func_ty;
                    return func_ty;
                }

                if (t->kind == IrTypeKind::Aggregate && has_overlapping_members(static_cast<IrAggregateType const*>(t)))
                {
                    auto* storage_ty = LLVMArrayType2(LLVMInt8TypeInContext(ctx), t->byte_size);
                    map[t] = storage_ty;
                    return storage_ty;
                }

                auto* opaque = LLVMStructCreateNamed(ctx, "");
                map[t] = opaque;

                build_aggregate_body(t, opaque);
                return opaque;
            }

            [[nodiscard]] unsigned get_llvm_field_index(IrAggregateType const* at, unsigned ir_field_idx)
            {
                std::ignore = get(at, true);
                auto it = field_index_map.find(at);
                if (it != field_index_map.end() && ir_field_idx < it->second.size())
                    return it->second[ir_field_idx];

                return ir_field_idx;
            }

            [[nodiscard]] bool uses_byte_storage(IrType const* t) const
            {
                return t && t->kind == IrTypeKind::Aggregate && has_overlapping_members(static_cast<IrAggregateType const*>(t));
            }

            [[nodiscard]] bool contains_byte_storage(IrType const* t) const
            {
                if (!t)
                    return false;
                if (uses_byte_storage(t))
                    return true;
                if (t->kind == IrTypeKind::Array)
                    return contains_byte_storage(static_cast<IrArrayType const*>(t)->element);
                if (t->kind == IrTypeKind::Aggregate)
                    return std::ranges::any_of(static_cast<IrAggregateType const*>(t)->members, [this](auto* member) { return contains_byte_storage(member); });
                return false;
            }

        private:
            [[nodiscard]] static bool has_overlapping_members(IrAggregateType const* at)
            {
                for (std::size_t i = 0; i < at->members.size(); ++i)
                {
                    auto* lhs = at->members[i];
                    if (!lhs || lhs->byte_size == 0)
                        continue;
                    auto lhs_offset = i < at->member_offsets.size() ? at->member_offsets[i] : 0;

                    for (std::size_t j = 0; j < i; ++j)
                    {
                        auto* rhs = at->members[j];
                        if (!rhs || rhs->byte_size == 0)
                            continue;
                        auto rhs_offset = j < at->member_offsets.size() ? at->member_offsets[j] : 0;
                        if (lhs_offset < rhs_offset + rhs->byte_size && rhs_offset < lhs_offset + lhs->byte_size)
                            return true;
                    }
                }

                return false;
            }

            [[nodiscard]] static bool is_simple_type(IrTypeKind k)
            {
                switch (k)
                {
                    case IrTypeKind::Void:
                    case IrTypeKind::Bool:
                    case IrTypeKind::Int:
                    case IrTypeKind::Float:
                    case IrTypeKind::Pointer:
                        return true;
                    default:
                        return false;
                }
            }

            void build_aggregate_body(IrType const* t, LLVMTypeRef opaque)
            {
                auto* at = static_cast<IrAggregateType const*>(t);
                std::vector<LLVMTypeRef> elems;
                elems.reserve(at->members.size() + 1);
                std::vector<unsigned> index_map;
                index_map.reserve(at->members.size());

                std::uint64_t expected_offset = 0;
                std::uint64_t natural_offset = 0;
                std::uint32_t max_align = 1;
                bool natural_layout = true;
                bool layout_data_ok = true;
                unsigned next_llvm_idx = 0;
                for (std::size_t i = 0; i < at->members.size(); ++i)
                {
                    auto* m = at->members[i];
                    auto offset = i < at->member_offsets.size() ? at->member_offsets[i] : 0;

                    if (offset > expected_offset)
                    {
                        auto pad_size = offset - expected_offset;
                        auto* pad_ty = LLVMArrayType2(LLVMInt8TypeInContext(ctx), static_cast<unsigned>(pad_size));
                        elems.push_back(pad_ty);
                        ++next_llvm_idx;
                        natural_offset += pad_size;
                    }

                    auto* mem_ty = get(m, true);
                    elems.push_back(mem_ty);
                    index_map.push_back(next_llvm_idx);
                    ++next_llvm_idx;

                    auto member_align = m ? static_cast<std::uint32_t>(m->byte_align) : 1u;
                    if (member_align == 0)
                    {
                        member_align = 1;
                        layout_data_ok = false;
                    }
                    natural_offset = (natural_offset + member_align - 1) / member_align * member_align;
                    if (layout_data_ok && natural_offset != offset)
                        natural_layout = false;
                    if (member_align > max_align)
                        max_align = member_align;
                    natural_offset += m ? m->byte_size : 0;

                    expected_offset = offset + (m ? m->byte_size : 0);
                }

                if (expected_offset < at->byte_size)
                {
                    auto pad_size = at->byte_size - expected_offset;
                    auto* pad_ty = LLVMArrayType2(LLVMInt8TypeInContext(ctx), static_cast<unsigned>(pad_size));
                    elems.push_back(pad_ty);
                    natural_offset += pad_size;
                }

                if (layout_data_ok && at->byte_size >= expected_offset && (natural_offset + max_align - 1) / max_align * max_align != at->byte_size)
                    natural_layout = false;

                field_index_map[t] = std::move(index_map);

                LLVMStructSetBody(opaque, elems.data(), static_cast<unsigned>(elems.size()), natural_layout ? 0 : 1);
            }
        };

        [[nodiscard]] LLVMTypeRef c_api_type_cached(TypeCache& tc, IrType const* t, bool for_memory = false)
        {
            if (!t)
                return LLVMVoidTypeInContext(tc.ctx);

            auto* simple = c_api_type(t, tc.ctx, for_memory);
            if (simple)
                return simple;

            return tc.get(t, for_memory);
        }

        [[nodiscard]] LLVMTypeRef llvm_type_cached(TypeCache& tc, IrType const* t)
        {
            return c_api_type_cached(tc, t, false);
        }
        [[nodiscard]] LLVMTypeRef llvm_mem_type_cached(TypeCache& tc, IrType const* t)
        {
            return c_api_type_cached(tc, t, true);
        }

        struct ArgAbi
        {
            enum class Kind : std::uint8_t
            {
                Direct,
                Indirect,
                Coerce,
                Memory,
            };

            Kind kind{Kind::Direct};
            std::vector<LLVMTypeRef> pieces;
            unsigned first_param{};
        };

        struct SignatureAbi
        {
            bool sret{false};
            bool sret_boundary{false};
            ArgAbi ret;
            std::vector<ArgAbi> params;
            std::vector<LLVMTypeRef> param_types;
            LLVMTypeRef ret_type{};
            LLVMTypeRef fn_type{};

            [[nodiscard]] bool rewrites() const noexcept
            {
                if (sret_boundary || ret.kind == ArgAbi::Kind::Coerce)
                    return true;
                return std::ranges::any_of(params, [](ArgAbi const& p) { return p.kind == ArgAbi::Kind::Coerce || p.kind == ArgAbi::Kind::Memory; });
            }
        };

        [[nodiscard]] bool is_aggregate_like(IrType const* t) noexcept
        {
            return t && (t->kind == IrTypeKind::Aggregate || t->kind == IrTypeKind::Array || t->kind == IrTypeKind::Slice) && t->byte_size > 0;
        }

        struct AbiLeaf
        {
            std::uint64_t offset;
            std::uint64_t size;
            bool is_float;
        };

        void flatten_abi_leaves(IrType const* t, std::uint64_t base, std::uint8_t pointer_bytes, std::vector<AbiLeaf>& out)
        {
            if (!t)
                return;
            switch (t->kind)
            {
                case IrTypeKind::Bool:
                case IrTypeKind::Int:
                    out.push_back({base, t->byte_size, false});
                    break;
                case IrTypeKind::Pointer:
                case IrTypeKind::Func:
                    out.push_back({base, pointer_bytes, false});
                    break;
                case IrTypeKind::Float:
                    out.push_back({base, t->byte_size, true});
                    break;
                case IrTypeKind::Slice:
                    out.push_back({base, pointer_bytes, false});
                    out.push_back({base + pointer_bytes, pointer_bytes, false});
                    break;
                case IrTypeKind::Array: {
                    auto const* at = static_cast<IrArrayType const*>(t);
                    std::uint64_t stride = at->element ? at->element->byte_size : 0;
                    for (std::uint64_t i = 0; i < at->count && stride > 0; ++i)
                        flatten_abi_leaves(at->element, base + i * stride, pointer_bytes, out);
                    break;
                }
                case IrTypeKind::Aggregate: {
                    auto const* agg = static_cast<IrAggregateType const*>(t);
                    for (std::size_t i = 0; i < agg->members.size() && i < agg->member_offsets.size(); ++i)
                        flatten_abi_leaves(agg->members[i], base + agg->member_offsets[i], pointer_bytes, out);
                    break;
                }
                default:
                    break;
            }
        }

        [[nodiscard]] bool classify_sysv_aggregate(TypeCache& tc, IrType const* t, std::vector<LLVMTypeRef>& pieces, unsigned& ints, unsigned& sses)
        {
            pieces.clear();
            ints = 0;
            sses = 0;
            std::uint64_t const size = t->byte_size;
            if (size > 16)
                return false;

            std::vector<AbiLeaf> leaves;
            flatten_abi_leaves(t, 0, static_cast<std::uint8_t>(tc.pointer_bits / 8), leaves);

            enum class Cls : std::uint8_t
            {
                None,
                Integer,
                Sse,
            };
            Cls cls[2] = {Cls::None, Cls::None};
            bool only_f32[2] = {true, true};
            for (auto const& leaf : leaves)
            {
                if (leaf.size == 0)
                    continue;
                if (leaf.offset % leaf.size != 0 || leaf.offset / 8 != (leaf.offset + leaf.size - 1) / 8)
                    return false;
                auto e = static_cast<std::size_t>(leaf.offset / 8);
                if (e > 1)
                    return false;
                if (!leaf.is_float)
                    cls[e] = Cls::Integer;
                else if (cls[e] != Cls::Integer)
                    cls[e] = Cls::Sse;
                if (!leaf.is_float || leaf.size != 4)
                    only_f32[e] = false;
            }

            auto const eightbytes = static_cast<std::size_t>((size + 7) / 8);
            for (std::size_t e = 0; e < eightbytes; ++e)
            {
                std::uint64_t const bytes = std::min<std::uint64_t>(8, size - e * 8);
                if (cls[e] == Cls::Sse)
                {
                    ++sses;
                    if (!only_f32[e])
                        pieces.push_back(LLVMDoubleTypeInContext(tc.ctx));
                    else if (bytes <= 4)
                        pieces.push_back(LLVMFloatTypeInContext(tc.ctx));
                    else
                        pieces.push_back(LLVMVectorType(LLVMFloatTypeInContext(tc.ctx), 2));
                }
                else
                {
                    ++ints;
                    pieces.push_back(LLVMIntTypeInContext(tc.ctx, static_cast<unsigned>(bytes * 8)));
                }
            }
            return true;
        }

        [[nodiscard]] AggregateAbi aggregate_abi_for(TypeCache const& tc, IrFunction const* fn)
        {
            if (tc.aggregate_abi == AggregateAbi::Native || !fn)
                return tc.aggregate_abi;
            for (auto const& a : fn->attrs)
            {
                if (a.kind != IrFuncAttr::CallingConv)
                    continue;
                auto lower = [](std::string_view v) {
                    std::string out{v};
                    for (auto& ch : out)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    return out;
                };
                auto cc = lower(a.value);
                if (cc == "win64")
                    return AggregateAbi::Win64;
                if (cc == "sysv" || cc == "systemv")
                    return AggregateAbi::SysV64;
            }
            return tc.aggregate_abi;
        }

        [[nodiscard]] SignatureAbi compute_signature_abi(TypeCache& tc, IrType const* ret, std::span<IrType const* const> params, AggregateAbi abi)
        {
            SignatureAbi sig;
            auto* ptr_ty = LLVMPointerTypeInContext(tc.ctx, 0);
            unsigned free_ints = 6;
            unsigned free_sses = 8;

            sig.sret = tc.indirect(ret);
            if (!sig.sret && abi != AggregateAbi::Native && is_aggregate_like(ret))
            {
                if (abi == AggregateAbi::Win64)
                {
                    auto n = ret->byte_size;
                    if (n == 1 || n == 2 || n == 4 || n == 8)
                    {
                        sig.ret.kind = ArgAbi::Kind::Coerce;
                        sig.ret.pieces.push_back(LLVMIntTypeInContext(tc.ctx, static_cast<unsigned>(n * 8)));
                    }
                    else
                        sig.sret_boundary = true;
                }
                else
                {
                    unsigned ints = 0;
                    unsigned sses = 0;
                    if (classify_sysv_aggregate(tc, ret, sig.ret.pieces, ints, sses))
                        sig.ret.kind = ArgAbi::Kind::Coerce;
                    else
                        sig.sret_boundary = true;
                }
            }

            if (sig.sret || sig.sret_boundary)
            {
                sig.ret_type = LLVMVoidTypeInContext(tc.ctx);
                sig.param_types.push_back(ptr_ty);
                --free_ints;
            }
            else if (sig.ret.kind == ArgAbi::Kind::Coerce)
                sig.ret_type = sig.ret.pieces.size() == 1
                                   ? sig.ret.pieces[0]
                                   : LLVMStructTypeInContext(tc.ctx, sig.ret.pieces.data(), static_cast<unsigned>(sig.ret.pieces.size()), 0);
            else
            {
                sig.ret_type = llvm_type_cached(tc, ret);
                if (!sig.ret_type)
                    sig.ret_type = LLVMVoidTypeInContext(tc.ctx);
            }

            for (auto const* pt : params)
            {
                ArgAbi arg;
                arg.first_param = static_cast<unsigned>(sig.param_types.size());
                if (tc.indirect(pt))
                {
                    arg.kind = ArgAbi::Kind::Indirect;
                    sig.param_types.push_back(ptr_ty);
                }
                else if (abi != AggregateAbi::Native && is_aggregate_like(pt))
                {
                    if (abi == AggregateAbi::Win64)
                    {
                        auto n = pt->byte_size;
                        if (n == 1 || n == 2 || n == 4 || n == 8)
                        {
                            arg.kind = ArgAbi::Kind::Coerce;
                            arg.pieces.push_back(LLVMIntTypeInContext(tc.ctx, static_cast<unsigned>(n * 8)));
                        }
                        else
                            arg.kind = ArgAbi::Kind::Memory;
                    }
                    else
                    {
                        unsigned ints = 0;
                        unsigned sses = 0;
                        if (classify_sysv_aggregate(tc, pt, arg.pieces, ints, sses) && ints <= free_ints && sses <= free_sses)
                        {
                            arg.kind = ArgAbi::Kind::Coerce;
                            free_ints -= ints;
                            free_sses -= sses;
                        }
                        else
                        {
                            arg.kind = ArgAbi::Kind::Memory;
                            arg.pieces.clear();
                        }
                    }
                    if (arg.kind == ArgAbi::Kind::Coerce)
                        sig.param_types.insert(sig.param_types.end(), arg.pieces.begin(), arg.pieces.end());
                    else
                        sig.param_types.push_back(ptr_ty);
                }
                else
                {
                    auto* lt = llvm_type_cached(tc, pt);
                    if (!lt)
                        lt = LLVMInt32TypeInContext(tc.ctx);
                    sig.param_types.push_back(lt);
                    if (pt && pt->kind == IrTypeKind::Float)
                    {
                        if (free_sses > 0)
                            --free_sses;
                    }
                    else
                    {
                        unsigned need = pt && pt->byte_size > 8 ? 2u : 1u;
                        free_ints = free_ints >= need ? free_ints - need : 0;
                    }
                }
                sig.params.push_back(std::move(arg));
            }

            sig.fn_type = LLVMFunctionType(sig.ret_type, sig.param_types.data(), static_cast<unsigned>(sig.param_types.size()), 0);
            return sig;
        }

        [[nodiscard]] LLVMValueRef abi_spill_slot(LLVMBuilderRef builder, TypeCache& tc, IrType const* t)
        {
            auto* insert_bb = LLVMGetInsertBlock(builder);
            auto* func = insert_bb ? LLVMGetBasicBlockParent(insert_bb) : nullptr;
            auto* entry = func ? LLVMGetEntryBasicBlock(func) : nullptr;
            auto* ty = llvm_type_cached(tc, t);
            LLVMValueRef slot = nullptr;
            if (entry && entry != insert_bb)
            {
                auto* saved_loc = LLVMGetCurrentDebugLocation2(builder);
                auto* first = LLVMGetFirstInstruction(entry);
                if (first)
                    LLVMPositionBuilderBefore(builder, first);
                else
                    LLVMPositionBuilderAtEnd(builder, entry);
                slot = LLVMBuildAlloca(builder, ty, "");
                LLVMPositionBuilderAtEnd(builder, insert_bb);
                LLVMSetCurrentDebugLocation2(builder, saved_loc);
            }
            else
                slot = LLVMBuildAlloca(builder, ty, "");
            LLVMSetAlignment(slot, static_cast<unsigned>(std::max<std::uint64_t>(8, t->byte_align)));
            return slot;
        }

        [[nodiscard]] LLVMValueRef abi_piece_ptr(LLVMBuilderRef builder, TypeCache& tc, LLVMValueRef base, std::size_t index)
        {
            if (index == 0)
                return base;
            auto* i8 = LLVMInt8TypeInContext(tc.ctx);
            LLVMValueRef offset = llvm_const_int(LLVMInt64TypeInContext(tc.ctx), index * 8, 0);
            return LLVMBuildInBoundsGEP2(builder, i8, base, &offset, 1, "");
        }

        [[nodiscard]] std::vector<LLVMValueRef> abi_split(LLVMBuilderRef builder, TypeCache& tc, IrType const* t, LLVMValueRef value,
                                                          std::vector<LLVMTypeRef> const& pieces)
        {
            auto* slot = abi_spill_slot(builder, tc, t);
            LLVMBuildStore(builder, value, slot);
            std::vector<LLVMValueRef> out;
            for (std::size_t i = 0; i < pieces.size(); ++i)
            {
                auto* load = LLVMBuildLoad2(builder, pieces[i], abi_piece_ptr(builder, tc, slot, i), "");
                LLVMSetAlignment(load, 1);
                out.push_back(load);
            }
            return out;
        }

        [[nodiscard]] LLVMValueRef abi_join(LLVMBuilderRef builder, TypeCache& tc, IrType const* t, std::span<LLVMValueRef const> values)
        {
            auto* slot = abi_spill_slot(builder, tc, t);
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                auto* store = LLVMBuildStore(builder, values[i], abi_piece_ptr(builder, tc, slot, i));
                LLVMSetAlignment(store, 1);
            }
            return LLVMBuildLoad2(builder, llvm_type_cached(tc, t), slot, "");
        }

        [[nodiscard]] LLVMValueRef abi_memory_copy(LLVMBuilderRef builder, TypeCache& tc, IrType const* t, LLVMValueRef value)
        {
            auto* slot = abi_spill_slot(builder, tc, t);
            LLVMBuildStore(builder, value, slot);
            return slot;
        }

        void set_constant_error(std::string* error, std::string message)
        {
            if (error && error->empty())
                *error = std::move(message);
        }

        [[nodiscard]] bool is_byte_array_type(IrType const* type)
        {
            if (!type || type->kind != IrTypeKind::Array)
                return false;

            auto* array = static_cast<IrArrayType const*>(type);
            if (!array->element || array->element->kind != IrTypeKind::Int)
                return false;

            return static_cast<IrIntType const*>(array->element)->bits == 8;
        }

        [[nodiscard]] bool is_llvm_byte_array_type(LLVMTypeRef type)
        {
            return type && LLVMGetTypeKind(type) == LLVMArrayTypeKind && LLVMGetTypeKind(LLVMGetElementType(type)) == LLVMIntegerTypeKind &&
                   LLVMGetIntTypeWidth(LLVMGetElementType(type)) == 8;
        }

        void write_integer_bytes(std::span<std::uint8_t> output, std::uint64_t value, std::uint8_t fill, bool little_endian)
        {
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                auto significance = little_endian ? i : output.size() - i - 1;
                output[i] = significance < sizeof(value) ? static_cast<std::uint8_t>(value >> (significance * 8)) : fill;
            }
        }

        [[nodiscard]] bool serialize_constant_memory(IrValue const* value, IrType const* storage_type, std::span<std::uint8_t> output, bool little_endian,
                                                     std::string* error)
        {
            if (!value || !storage_type)
            {
                set_constant_error(error, "LLVM backend: opaque payload constant is missing a value or type");
                return false;
            }

            if (storage_type->byte_size > output.size())
            {
                set_constant_error(error, "LLVM backend: opaque payload constant exceeds its storage");
                return false;
            }

            if (is_byte_array_type(storage_type) && value->type && value->type != storage_type && !is_byte_array_type(value->type))
                return serialize_constant_memory(value, value->type, output, little_endian, error);

            switch (value->kind)
            {
                case IrNodeKind::IntConstant: {
                    auto* constant = static_cast<IrIntConstant const*>(value);
                    auto size = static_cast<std::size_t>(storage_type->byte_size);
                    auto fill = static_cast<std::uint8_t>(0);
                    if (value->type && value->type->kind == IrTypeKind::Int)
                    {
                        auto* integer_type = static_cast<IrIntType const*>(value->type);
                        if (integer_type->is_signed && constant->value < 0)
                            fill = 0xff;
                    }
                    write_integer_bytes(output.first(size), static_cast<std::uint64_t>(constant->value), fill, little_endian);
                    return true;
                }
                case IrNodeKind::FloatConstant: {
                    auto* constant = static_cast<IrFloatConstant const*>(value);
                    if (storage_type->byte_size == sizeof(float))
                    {
                        auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(constant->value));
                        write_integer_bytes(output.first(sizeof(bits)), bits, 0, little_endian);
                        return true;
                    }
                    if (storage_type->byte_size == sizeof(double))
                    {
                        auto bits = std::bit_cast<std::uint64_t>(constant->value);
                        write_integer_bytes(output.first(sizeof(bits)), bits, 0, little_endian);
                        return true;
                    }
                    set_constant_error(error, "LLVM backend: opaque payload constant has an unsupported floating-point width");
                    return false;
                }
                case IrNodeKind::BoolConstant:
                    if (!output.empty())
                        output[0] = static_cast<IrBoolConstant const*>(value)->value ? 1 : 0;
                    return true;
                case IrNodeKind::NullConstant:
                    return true;
                case IrNodeKind::PointerConstant: {
                    auto* pointer = static_cast<IrPointerConstant const*>(value);
                    auto* pointer_type = ir_type_cast<IrPointerType>(storage_type);
                    if (!pointer_type)
                        return false;
                    auto const offset_size = pointer_type->flavor == PointerFlavor::Far ? pointer_type->byte_size / 2 : pointer_type->byte_size;
                    write_integer_bytes(output.first(static_cast<std::size_t>(offset_size)), pointer->offset, 0, little_endian);
                    if (pointer_type->flavor == PointerFlavor::Far)
                        write_integer_bytes(output.subspan(static_cast<std::size_t>(offset_size), 2), pointer->segment, 0, little_endian);
                    return true;
                }
                case IrNodeKind::StringConstant: {
                    auto const& string = static_cast<IrStringConstant const*>(value)->value;
                    if (string.size() > output.size())
                    {
                        set_constant_error(error, "LLVM backend: opaque payload string constant exceeds its storage");
                        return false;
                    }
                    for (std::size_t i = 0; i < string.size(); ++i)
                        output[i] = static_cast<std::uint8_t>(string[i]);
                    return true;
                }
                case IrNodeKind::Aggregate: {
                    auto* aggregate = static_cast<IrAggregateInst const*>(value);
                    if (storage_type->kind == IrTypeKind::Aggregate)
                    {
                        auto* aggregate_type = static_cast<IrAggregateType const*>(storage_type);
                        auto count = std::min(aggregate->values.size(), aggregate_type->members.size());
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            if (!aggregate->values[i])
                                continue;
                            auto offset = i < aggregate_type->member_offsets.size() ? aggregate_type->member_offsets[i] : 0;
                            auto* member_type = aggregate_type->members[i];
                            if (!member_type || offset > output.size() || member_type->byte_size > output.size() - offset)
                            {
                                set_constant_error(error, "LLVM backend: opaque aggregate payload has an invalid field layout");
                                return false;
                            }
                            if (!serialize_constant_memory(aggregate->values[i], member_type, output.subspan(offset, member_type->byte_size), little_endian,
                                                           error))
                                return false;
                        }
                        return true;
                    }
                    if (storage_type->kind == IrTypeKind::Array)
                    {
                        auto* array_type = static_cast<IrArrayType const*>(storage_type);
                        if (!array_type->element)
                        {
                            set_constant_error(error, "LLVM backend: opaque array payload is missing its element type");
                            return false;
                        }
                        auto count = std::min<std::uint64_t>(aggregate->values.size(), array_type->count);
                        for (std::uint64_t i = 0; i < count; ++i)
                        {
                            auto offset = i * array_type->element->byte_size;
                            if (!aggregate->values[i])
                                continue;
                            if (offset > output.size() || array_type->element->byte_size > output.size() - offset)
                            {
                                set_constant_error(error, "LLVM backend: opaque array payload has an invalid element layout");
                                return false;
                            }
                            if (!serialize_constant_memory(aggregate->values[i], array_type->element, output.subspan(offset, array_type->element->byte_size),
                                                           little_endian, error))
                                return false;
                        }
                        return true;
                    }
                    if (storage_type->kind == IrTypeKind::Slice)
                    {
                        auto pointer_size = storage_type->byte_size / 2;
                        for (std::size_t i = 0; i < std::min<std::size_t>(aggregate->values.size(), 2); ++i)
                        {
                            if (!aggregate->values[i])
                                continue;
                            auto offset = i * pointer_size;
                            auto* element_type = aggregate->values[i]->type;
                            if (!element_type || offset > output.size() || pointer_size > output.size() - offset || element_type->byte_size > pointer_size)
                            {
                                set_constant_error(error, "LLVM backend: opaque slice payload has an invalid layout");
                                return false;
                            }
                            if (!serialize_constant_memory(aggregate->values[i], element_type, output.subspan(offset, pointer_size), little_endian, error))
                                return false;
                        }
                        return true;
                    }
                    set_constant_error(error, "LLVM backend: opaque payload aggregate has an unsupported storage type");
                    return false;
                }
                case IrNodeKind::GlobalRef:
                    set_constant_error(error, "LLVM backend: relocatable pointer, function, or global references cannot be encoded in opaque payload bytes");
                    return false;
                default:
                    set_constant_error(error, "LLVM backend: unsupported constant value in opaque payload bytes");
                    return false;
            }
        }

        struct InitLeaf
        {
            std::uint64_t offset{};
            std::uint64_t size{};
            IrValue const* value{};
            IrType const* mem_type{};
            bool is_reloc{};
        };

        [[nodiscard]] bool has_global_ref(IrValue const* v)
        {
            if (!v)
                return false;
            if (v->kind == IrNodeKind::GlobalRef)
                return true;
            if (v->kind == IrNodeKind::Aggregate)
            {
                auto* agg = static_cast<IrAggregateInst const*>(v);
                return std::ranges::any_of(agg->values, [](IrValue const* mv) { return has_global_ref(mv); });
            }
            return false;
        }

        void collect_init_leaves(IrValue const* v, IrType const* ty, std::uint64_t base, std::vector<InitLeaf>& leaves)
        {
            if (!v || !ty || ty->byte_size == 0)
                return;

            if (v->kind == IrNodeKind::GlobalRef)
            {
                leaves.push_back({base, ty->byte_size, v, ty, true});
                return;
            }

            if (v->kind == IrNodeKind::Aggregate)
            {
                auto* agg = static_cast<IrAggregateInst const*>(v);
                switch (ty->kind)
                {
                    case IrTypeKind::Aggregate: {
                        auto* at = static_cast<IrAggregateType const*>(ty);
                        auto n = std::min(agg->values.size(), at->members.size());
                        for (std::size_t i = 0; i < n; ++i)
                        {
                            if (!agg->values[i])
                                continue;
                            auto off = i < at->member_offsets.size() ? at->member_offsets[i] : 0;
                            collect_init_leaves(agg->values[i], at->members[i], base + off, leaves);
                        }
                        return;
                    }
                    case IrTypeKind::Array: {
                        auto* at = static_cast<IrArrayType const*>(ty);
                        auto n = std::min<std::uint64_t>(agg->values.size(), at->count);
                        auto elem_size = at->element ? at->element->byte_size : 1;
                        for (std::uint64_t i = 0; i < n; ++i)
                        {
                            if (!agg->values[static_cast<std::size_t>(i)])
                                continue;
                            collect_init_leaves(agg->values[static_cast<std::size_t>(i)], at->element, base + i * elem_size, leaves);
                        }
                        return;
                    }
                    case IrTypeKind::Slice: {
                        if (ir::slice_data_index < agg->values.size() && agg->values[ir::slice_data_index])
                            collect_init_leaves(agg->values[ir::slice_data_index], agg->values[ir::slice_data_index]->type, base, leaves);
                        return;
                    }
                    default:
                        return;
                }
            }

            leaves.push_back({base, ty->byte_size, v, ty, false});
        }

        bool resolve_init_layout(IrValue const* init, IrType const* semantic, std::vector<InitLeaf>& slots, std::vector<std::uint8_t>& bytes,
                                 bool little_endian, std::string* error)
        {
            std::vector<InitLeaf> leaves;
            collect_init_leaves(init, semantic, 0, leaves);

            bytes.assign(semantic->byte_size, 0);
            auto overlap = [](InitLeaf const& a, InitLeaf const& b) { return a.offset < b.offset + b.size && b.offset < a.offset + a.size; };

            for (auto const& leaf : leaves)
            {
                if (leaf.offset + leaf.size > semantic->byte_size)
                {
                    set_constant_error(error, "LLVM backend: aggregate initializer leaf exceeds its storage");
                    return false;
                }

                std::erase_if(slots, [&](InitLeaf const& s) { return overlap(s, leaf); });

                if (leaf.is_reloc)
                {
                    slots.push_back(leaf);
                    auto* gr = static_cast<IrGlobalRef const*>(leaf.value);
                    if (gr->addend != 0)
                    {
                        std::uint64_t uv = static_cast<std::uint64_t>(gr->addend);
                        for (std::uint64_t i = 0; i < leaf.size; ++i)
                            bytes[static_cast<std::size_t>(leaf.offset + i)] = static_cast<std::uint8_t>(uv >> (i * 8));
                    }
                }
                else
                {
                    std::vector<std::uint8_t> region(leaf.size, 0);
                    if (!serialize_constant_memory(leaf.value, leaf.mem_type, region, little_endian, error))
                        return false;
                    std::ranges::copy(region, bytes.begin() + static_cast<std::ptrdiff_t>(leaf.offset));
                }
            }
            return true;
        }

        [[nodiscard]] LLVMValueRef c_api_constant(IrValue const* v, LLVMContextRef ctx, TypeCache& tc,
                                                  std::unordered_map<IrValue const*, LLVMValueRef>& val_map, LLVMTypeRef expected_mem_type,
                                                  std::string* constant_error);

        LLVMTypeRef build_union_storage_type(LLVMContextRef ctx, IrType const* semantic, std::vector<InitLeaf> const& slots_in, bool& ok)
        {
            ok = true;
            std::vector<LLVMTypeRef> elems;
            std::uint64_t cursor = 0;

            auto slots = slots_in;
            std::ranges::sort(slots, {}, &InitLeaf::offset);
            for (auto const& s : slots)
            {
                if (s.offset > cursor)
                    elems.push_back(LLVMArrayType2(LLVMInt8TypeInContext(ctx), static_cast<unsigned>(s.offset - cursor)));
                elems.push_back(LLVMPointerTypeInContext(ctx, 0));
                cursor = s.offset + s.size;
            }
            if (cursor < semantic->byte_size)
                elems.push_back(LLVMArrayType2(LLVMInt8TypeInContext(ctx), static_cast<unsigned>(semantic->byte_size - cursor)));
            if (elems.empty())
                elems.push_back(LLVMArrayType2(LLVMInt8TypeInContext(ctx), static_cast<unsigned>(semantic->byte_size)));

            return LLVMStructTypeInContext(ctx, elems.data(), static_cast<unsigned>(elems.size()), 1);
        }

        LLVMValueRef build_union_storage_init(LLVMContextRef ctx, TypeCache& tc, IrType const* semantic, std::vector<InitLeaf> const& slots_in,
                                              std::vector<std::uint8_t> const& bytes, std::unordered_map<IrValue const*, LLVMValueRef>& val_map,
                                              std::string* error)
        {
            auto slots = slots_in;
            std::ranges::sort(slots, {}, &InitLeaf::offset);

            bool ok = false;
            auto* storage_ty = build_union_storage_type(ctx, semantic, slots, ok);
            if (!ok || !storage_ty)
            {
                set_constant_error(error, "LLVM backend: overlapping aggregate layout cannot be represented");
                return nullptr;
            }

            auto field_count = LLVMCountStructElementTypes(storage_ty);
            std::vector<LLVMValueRef> fields;
            fields.reserve(field_count);

            std::uint64_t cursor = 0;
            std::size_t slot_idx = 0;
            for (unsigned i = 0; i < field_count; ++i)
            {
                auto* field_ty = LLVMStructGetTypeAtIndex(storage_ty, i);
                if (slot_idx < slots.size() && cursor == slots[slot_idx].offset)
                {
                    auto* slot_const = c_api_constant(slots[slot_idx].value, ctx, tc, val_map, LLVMPointerTypeInContext(ctx, 0), error);
                    if (!slot_const)
                        return nullptr;
                    fields.push_back(slot_const);
                    cursor = slots[slot_idx].offset + slots[slot_idx].size;
                    ++slot_idx;
                }
                else
                {
                    auto pad_size = LLVMGetArrayLength2(field_ty);
                    if (pad_size > 0 && cursor + pad_size <= bytes.size())
                    {
                        auto const* ptr = reinterpret_cast<char const*>(bytes.data() + static_cast<std::size_t>(cursor));
                        fields.push_back(LLVMConstStringInContext(ctx, ptr, static_cast<unsigned>(pad_size), 1));
                    }
                    else
                    {
                        fields.push_back(LLVMConstNull(field_ty));
                    }
                    cursor += pad_size;
                }
            }

            return LLVMConstNamedStruct(storage_ty, fields.data(), static_cast<unsigned>(fields.size()));
        }

        [[nodiscard]] LLVMValueRef c_api_constant(IrValue const* v, LLVMContextRef ctx, TypeCache& tc,
                                                  std::unordered_map<IrValue const*, LLVMValueRef>& val_map, LLVMTypeRef expected_mem_type = nullptr,
                                                  std::string* constant_error = nullptr)
        {
            if (!v)
                return nullptr;

            if (v->kind == IrNodeKind::Aggregate && tc.uses_byte_storage(v->type))
            {
                auto* storage_ty = c_api_type_cached(tc, v->type, true);
                auto byte_count = LLVMGetArrayLength2(storage_ty);
                std::vector<std::uint8_t> bytes(byte_count, 0);
                if (!serialize_constant_memory(v, v->type, bytes, tc.little_endian, constant_error))
                    return nullptr;
                return LLVMConstStringInContext(ctx, reinterpret_cast<char const*>(bytes.data()), static_cast<unsigned>(bytes.size()), 1);
            }

            if (is_llvm_byte_array_type(expected_mem_type))
            {
                auto* native_mem_type = v->type ? c_api_type_cached(tc, v->type, true) : nullptr;
                if (native_mem_type != expected_mem_type)
                {
                    auto byte_count = LLVMGetArrayLength2(expected_mem_type);
                    std::vector<std::uint8_t> bytes(byte_count, 0);
                    if (!serialize_constant_memory(v, v->type, bytes, tc.little_endian, constant_error))
                        return nullptr;
                    return LLVMConstStringInContext(ctx, reinterpret_cast<char const*>(bytes.data()), static_cast<unsigned>(bytes.size()), 1);
                }
            }

            switch (v->kind)
            {
                case IrNodeKind::IntConstant: {
                    auto* ic = static_cast<IrIntConstant const*>(v);
                    auto* ty = expected_mem_type ? expected_mem_type : c_api_type_cached(tc, v->type);
                    if (!ty)
                        return nullptr;

                    return llvm_const_int(ty, static_cast<unsigned long long>(ic->value),
                                          v->type && static_cast<IrIntType const*>(v->type)->is_signed);
                }
                case IrNodeKind::FloatConstant: {
                    auto* fc = static_cast<IrFloatConstant const*>(v);
                    auto* ty = c_api_type_cached(tc, v->type);
                    if (!ty)
                        return nullptr;

                    if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind)
                        return LLVMConstReal(ty, static_cast<float>(fc->value));
                    else
                        return LLVMConstReal(ty, fc->value);
                }
                case IrNodeKind::BoolConstant: {
                    auto* bc = static_cast<IrBoolConstant const*>(v);
                    auto* ty = expected_mem_type ? expected_mem_type : LLVMInt1TypeInContext(ctx);
                    return llvm_const_int(ty, bc->value ? 1 : 0, false);
                }
                case IrNodeKind::NullConstant:
                    return LLVMConstNull(expected_mem_type ? expected_mem_type : c_api_type_cached(tc, v->type));
                case IrNodeKind::PointerConstant: {
                    auto* p = static_cast<IrPointerConstant const*>(v);
                    auto* pointer_ty = expected_mem_type ? expected_mem_type : c_api_type_cached(tc, p->type);
                    if (!pointer_ty)
                        return nullptr;
                    auto* offset_ty = LLVMIntTypeInContext(ctx, tc.pointer_bits);
                    auto* offset = llvm_const_int(offset_ty, p->offset, false);
                    return LLVMConstIntToPtr(offset, pointer_ty);
                }
                case IrNodeKind::GlobalRef: {
                    auto* g = static_cast<IrGlobalRef const*>(v);

                    LLVMValueRef result = nullptr;

                    if (g->function)
                        if (auto it = val_map.find(g->function); it != val_map.end())
                            result = it->second;

                    if (!result && g->global)
                        if (auto it = val_map.find(g->global); it != val_map.end())
                            result = it->second;

                    if (!result)
                        if (auto it = val_map.find(g); it != val_map.end())
                            result = it->second;

                    if (!result)
                        return nullptr;

                    if (expected_mem_type && LLVMGetTypeKind(expected_mem_type) == LLVMPointerTypeKind)
                    {
                        LLVMTypeRef gv_value_type = nullptr;
                        if (g->global)
                            gv_value_type = LLVMGlobalGetValueType(result);

                        if (gv_value_type && LLVMGetTypeKind(gv_value_type) == LLVMArrayTypeKind)
                        {
                            LLVMValueRef indices[] = {
                                llvm_const_int(LLVMInt32TypeInContext(ctx), 0, 0),
                                llvm_const_int(LLVMInt32TypeInContext(ctx), 0, 0),
                            };
                            result = LLVMConstGEP2(gv_value_type, result, indices, 2);
                        }
                    }

                    if (g->addend != 0)
                    {
                        LLVMValueRef addend_const = llvm_const_int(LLVMInt64TypeInContext(ctx), static_cast<unsigned long long>(g->addend), 1);
                        result = LLVMConstGEP2(LLVMInt8TypeInContext(ctx), result, &addend_const, 1);
                    }

                    return result;
                }
                case IrNodeKind::StringConstant: {
                    auto* sc = static_cast<IrStringConstant const*>(v);

                    auto* mem_ty = c_api_type_cached(tc, v->type, true);
                    if (!mem_ty)
                        return nullptr;

                    if (LLVMGetTypeKind(mem_ty) == LLVMArrayTypeKind)
                    {
                        auto* elem_ty = LLVMGetElementType(mem_ty);
                        std::vector<LLVMValueRef> elems;
                        elems.reserve(sc->value.size());

                        for (auto ch : sc->value)
                            elems.push_back(llvm_const_int(elem_ty, static_cast<unsigned long long>(ch), false));

                        auto count = LLVMGetArrayLength2(mem_ty);
                        while (elems.size() < count)
                            elems.push_back(LLVMConstNull(elem_ty));

                        return LLVMConstArray2(elem_ty, elems.data(), static_cast<unsigned>(elems.size()));
                    }

                    return LLVMConstNull(mem_ty);
                }
                case IrNodeKind::Aggregate: {
                    auto* agg = static_cast<IrAggregateInst const*>(v);
                    auto* mem_ty = c_api_type_cached(tc, v->type, true);
                    if (!mem_ty)
                        return nullptr;

                    auto ty_kind = LLVMGetTypeKind(mem_ty);

                    if (ty_kind == LLVMStructTypeKind)
                    {
                        auto llvm_field_count = LLVMCountStructElementTypes(mem_ty);
                        std::vector<LLVMValueRef> fields(llvm_field_count, nullptr);

                        if (agg->type && agg->type->kind == IrTypeKind::Aggregate)
                        {
                            auto* ir_agg_type = static_cast<IrAggregateType const*>(agg->type);
                            for (std::size_t i = 0; i < agg->values.size(); ++i)
                            {
                                unsigned llvm_idx = tc.get_llvm_field_index(ir_agg_type, static_cast<unsigned>(i));
                                if (llvm_idx < llvm_field_count)
                                {
                                    auto* field_ty = LLVMStructGetTypeAtIndex(mem_ty, llvm_idx);
                                    auto* cv = c_api_constant(agg->values[i], ctx, tc, val_map, field_ty, constant_error);
                                    if (!cv)
                                        return nullptr;
                                    fields[llvm_idx] = cv;
                                }
                            }
                        }
                        else
                        {
                            for (unsigned i = 0; i < llvm_field_count; ++i)
                            {
                                if (i < agg->values.size())
                                {
                                    auto* field_ty = LLVMStructGetTypeAtIndex(mem_ty, i);
                                    auto* cv = c_api_constant(agg->values[i], ctx, tc, val_map, field_ty, constant_error);
                                    if (!cv)
                                        return nullptr;
                                    fields[i] = cv;
                                }
                            }
                        }

                        for (unsigned i = 0; i < llvm_field_count; ++i)
                            if (!fields[i])
                                fields[i] = LLVMConstNull(LLVMStructGetTypeAtIndex(mem_ty, i));

                        return LLVMConstNamedStruct(mem_ty, fields.data(), static_cast<unsigned>(fields.size()));
                    }

                    if (ty_kind == LLVMArrayTypeKind)
                    {
                        auto* elem_ty = LLVMGetElementType(mem_ty);
                        std::vector<LLVMValueRef> elems;
                        elems.reserve(agg->values.size());
                        for (auto* mv : agg->values)
                        {
                            auto* cv = c_api_constant(mv, ctx, tc, val_map, elem_ty, constant_error);
                            if (!cv)
                                return nullptr;

                            elems.push_back(cv);
                        }

                        auto count = LLVMGetArrayLength2(mem_ty);
                        while (elems.size() < count)
                            elems.push_back(LLVMConstNull(elem_ty));

                        return LLVMConstArray2(elem_ty, elems.data(), static_cast<unsigned>(elems.size()));
                    }

                    return LLVMConstNull(mem_ty);
                }
                default:
                    return nullptr;
            }
        }

        struct LlvmCtxGuard
        {
            LLVMContextRef ctx;
            ~LlvmCtxGuard()
            {
                if (ctx)
                    LLVMContextDispose(ctx);
            }
        };

        struct LlvmBuilderGuard
        {
            LLVMBuilderRef bld;
            ~LlvmBuilderGuard()
            {
                if (bld)
                    LLVMDisposeBuilder(bld);
            }
        };

        [[gnu::constructor]] void ensure_llvm_initialized()
        {
            LLVMInitializeAllTargetInfos();
            LLVMInitializeAllTargets();
            LLVMInitializeAllTargetMCs();
            LLVMInitializeAllAsmParsers();
            LLVMInitializeAllAsmPrinters();
        }

        class LlvmBackendImpl : public Backend
        {
        public:
            LlvmBackendImpl() = default;

            [[nodiscard]] std::string_view name() const override { return "llvm"; }

            [[nodiscard]] std::set<ArtifactKind> supported_artifacts() const override
            {
                return {ArtifactKind::LlvmIrText, ArtifactKind::AsmText, ArtifactKind::ObjectBytes, ArtifactKind::ExecutableBytes};
            }

            [[nodiscard]] BackendArtifact emit(IrModule const& module, BackendOptions const& opts) override
            {
                ensure_llvm_initialized();

                BackendArtifact artifact;
                auto& diags = artifact.diagnostics;

                dcc::ir::IrModule const* input_module = &module;
                dcc::ir::IrContext opt_ctx{256 * 1024, &opts.target};
                if (opts.opt_level > dcc::ir::pass::OptLevel::O0 && !std::getenv("DCC_BENCH_SKIP_IR_PASSES"))
                    input_module = dcc::ir::pass::global_pass_manager().run(module, opt_ctx, opts.opt_level);

                bool has_unsupported = precheck_module(*input_module, diags);
                if (has_unsupported && !opts.requested_artifacts.empty())
                {
                    if (diags.empty())
                        add_diag(diags, {}, "LLVM backend precheck rejected the IR module without a diagnostic");
                    return artifact;
                }

                auto* ctx = LLVMContextCreate();
                LlvmCtxGuard ctx_guard{ctx};

                std::string mod_name = input_module->name.empty() ? "dcc_module" : std::string{input_module->name};
                auto* llvm_mod = LLVMModuleCreateWithNameInContext(mod_name.c_str(), ctx);

                auto resolved_debug_format = resolve_debug_format(opts);
                auto cg_triple = llvm_codegen_triple(opts.target, resolved_debug_format);

                if (opts.emit_debug_info && opts.debug_format == DebugFormat::Pdb && opts.target.object_format != ObjectFormat::Coff &&
                    !opts.target.triple.contains("coff") && !opts.target.triple.contains("windows") && !opts.target.triple.contains("msvc"))
                {
                    add_diag(diags, {}, "PDB/CodeView debug info is only supported for COFF/Windows targets");
                    LLVMDisposeModule(llvm_mod);
                    return artifact;
                }

                LLVMSetTarget(llvm_mod, cg_triple.c_str());

                DebugEmitContext debug;
                bool const wants_debug = resolved_debug_format != DebugFormat::None;

                if (wants_debug)
                {
                    debug.dibuilder = LLVMCreateDIBuilder(llvm_mod);
                    debug.sm = opts.source_manager;

                    {
                        std::error_code ec;
                        auto cwd = std::filesystem::current_path(ec);
                        debug.comp_dir = ec ? "." : cwd.generic_string();
                    }

                    std::string cu_filename;
                    std::string cu_directory = debug.comp_dir;
                    if (input_module->source_file_id != static_cast<std::uint32_t>(sm::FileId::Invalid) && opts.source_manager)
                    {
                        auto* sf = opts.source_manager->get(static_cast<sm::FileId>(static_cast<std::uint32_t>(input_module->source_file_id)));
                        if (sf)
                        {
                            auto path = sf->path();
                            if (!path.empty())
                            {
                                std::error_code ec2;
                                auto abs_path = std::filesystem::weakly_canonical(path, ec2);
                                if (!ec2)
                                {
                                    auto rel = std::filesystem::proximate(abs_path, std::filesystem::path(debug.comp_dir), ec2);
                                    if (!ec2 && !rel.empty())
                                        cu_filename = rel.generic_string();
                                    else
                                    {
                                        cu_filename = abs_path.generic_string();
                                        cu_directory.clear();
                                    }
                                }
                                else
                                    cu_filename = path.filename().generic_string();
                            }
                        }
                    }
                    if (cu_filename.empty())
                    {
                        cu_filename = input_module->name.empty() ? "<unknown>" : std::string{input_module->name};
                        cu_directory = ".";
                    }

                    debug.difile = LLVMDIBuilderCreateFile(debug.dibuilder, cu_filename.c_str(), cu_filename.size(),
                                                           cu_directory.empty() ? "." : cu_directory.c_str(), cu_directory.empty() ? 1 : cu_directory.size());

                    if (input_module->source_file_id != static_cast<std::uint32_t>(sm::FileId::Invalid))
                        debug.file_map[input_module->source_file_id] = debug.difile;

                    debug.dicu = LLVMDIBuilderCreateCompileUnit(debug.dibuilder, LLVMDWARFSourceLanguageC, debug.difile, "dcc", 3, false, "", 0, 0, "", 0,
                                                                LLVMDWARFEmissionFull, 0, false, false, "", 0, "", 0);

                    {
                        auto* ver_md = LLVMValueAsMetadata(llvm_const_int(LLVMInt32TypeInContext(ctx), static_cast<unsigned long long>(3), false));
                        LLVMAddModuleFlag(llvm_mod, LLVMModuleFlagBehaviorWarning, "Debug Info Version", 19, ver_md);
                    }

                    if (resolved_debug_format == DebugFormat::Dwarf)
                    {
                        auto* dwarf_ver_md = LLVMValueAsMetadata(llvm_const_int(LLVMInt32TypeInContext(ctx), 5, false));
                        LLVMAddModuleFlag(llvm_mod, LLVMModuleFlagBehaviorWarning, "Dwarf Version", 13, dwarf_ver_md);
                    }
                    else if (resolved_debug_format == DebugFormat::Pdb)
                    {
                        auto* cv_md = LLVMValueAsMetadata(llvm_const_int(LLVMInt32TypeInContext(ctx), 1, false));
                        LLVMAddModuleFlag(llvm_mod, LLVMModuleFlagBehaviorWarning, "CodeView", 8, cv_md);
                    }
                }

                std::unordered_map<IrValue const*, LLVMValueRef> val_map;
                auto const aggregate_abi = opts.target.arch != Arch::X86_64 ? AggregateAbi::Native
                                           : (opts.target.os == dcc::target::Os::Windows || opts.target.object_format == dcc::target::ObjectFormat::Coff)
                                               ? AggregateAbi::Win64
                                               : AggregateAbi::SysV64;
                TypeCache type_cache{ctx, opts.target.pointer_bits, opts.target.little_endian, aggregate_abi};

                auto* debug_ptr = wants_debug ? &debug : nullptr;

                for (auto* func : input_module->functions)
                {
                    if (!func)
                        continue;

                    auto const diag_count = diags.size();
                    if (!create_function_decl(func, llvm_mod, ctx, type_cache, val_map, opts, diags, debug_ptr))
                    {
                        if (diags.size() == diag_count)
                            add_diag(diags, func->range, std::format("LLVM backend failed to declare function '{}'", func->name));
                        has_unsupported = true;
                    }
                }

                for (auto* g : input_module->globals)
                {
                    if (!g || !g->type)
                        continue;

                    if (auto* gv = declare_global(g, llvm_mod, type_cache))
                        val_map[g] = gv;
                }

                for (auto* g : input_module->globals)
                {
                    if (!g || !g->type)
                        continue;

                    auto it = val_map.find(g);
                    if (it == val_map.end())
                        continue;

                    std::ignore = init_global(g, it->second, ctx, type_cache, val_map, diags);
                }

                for (auto* func : input_module->functions)
                {
                    if (!func)
                        continue;

                    auto const diag_count = diags.size();
                    if (!emit_function_body(func, ctx, type_cache, val_map, opts.target, diags, debug_ptr))
                    {
                        if (diags.size() == diag_count)
                            add_diag(diags, func->range, std::format("LLVM backend failed to lower function '{}'", func->name));
                        has_unsupported = true;
                    }
                }

                for (auto* ma : input_module->module_asms)
                {
                    if (!ma)
                        continue;
                    LLVMAppendModuleInlineAsm(llvm_mod, ma->template_str.data(), ma->template_str.size());
                }
                if (!input_module->module_asms.empty())
                {
                    std::vector<LLVMValueRef> used_vals;
                    for (auto* ma : input_module->module_asms)
                    {
                        if (!ma)
                            continue;
                        for (auto* g : ma->globals)
                        {
                            if (!g || g->is_declaration)
                                continue;
                            auto it = val_map.find(g);
                            if (it == val_map.end() || !it->second)
                                continue;
                            bool seen = false;
                            for (auto* v : used_vals)
                            {
                                if (v == it->second)
                                {
                                    seen = true;
                                    break;
                                }
                            }
                            if (!seen)
                                used_vals.push_back(it->second);
                        }
                        for (auto* f : ma->funcs)
                        {
                            if (!f || f->blocks.empty())
                                continue;
                            auto it = val_map.find(f);
                            if (it == val_map.end() || !it->second)
                                continue;
                            bool seen = false;
                            for (auto* v : used_vals)
                            {
                                if (v == it->second)
                                {
                                    seen = true;
                                    break;
                                }
                            }
                            if (!seen)
                                used_vals.push_back(it->second);
                        }
                    }
                    if (!used_vals.empty() && !LLVMGetNamedGlobal(llvm_mod, "llvm.compiler.used"))
                    {
                        auto* ptr_ty = LLVMPointerTypeInContext(ctx, 0);
                        std::vector<LLVMValueRef> casted;
                        casted.reserve(used_vals.size());
                        for (auto* v : used_vals)
                            casted.push_back(LLVMConstBitCast(v, ptr_ty));
                        auto* arr_ty = LLVMArrayType2(ptr_ty, static_cast<unsigned long long>(casted.size()));
                        auto* arr_const = LLVMConstArray2(ptr_ty, casted.data(), static_cast<unsigned>(casted.size()));
                        auto* used_gv = LLVMAddGlobal(llvm_mod, arr_ty, "llvm.compiler.used");
                        LLVMSetLinkage(used_gv, LLVMAppendingLinkage);
                        LLVMSetSection(used_gv, "llvm.metadata");
                        LLVMSetInitializer(used_gv, arr_const);
                    }
                }

                debug.finalize();

                if (has_unsupported)
                {
                    LLVMDisposeModule(llvm_mod);
                    return artifact;
                }

                {
                    char* verifier_msg = nullptr;
                    if (LLVMVerifyModule(llvm_mod, LLVMReturnStatusAction, &verifier_msg))
                    {
                        char* ir_str = LLVMPrintModuleToString(llvm_mod);
                        auto diag_msg = std::string{"LLVM module verification failed:\n"} + (verifier_msg ? verifier_msg : "unknown error") +
                                        "\n\nFull LLVM IR:\n" + (ir_str ? ir_str : "");

                        add_diag(diags, {}, diag_msg);
                        if (verifier_msg)
                            LLVMDisposeMessage(verifier_msg);

                        if (ir_str)
                            LLVMDisposeMessage(ir_str);

                        LLVMDisposeModule(llvm_mod);
                        return artifact;
                    }
                    if (verifier_msg)
                        LLVMDisposeMessage(verifier_msg);
                }

                bool want_ir = opts.requested_artifacts.contains(ArtifactKind::LlvmIrText);
                bool want_asm = opts.requested_artifacts.contains(ArtifactKind::AsmText);
                bool want_obj = opts.requested_artifacts.contains(ArtifactKind::ObjectBytes);
                bool want_exe = opts.requested_artifacts.contains(ArtifactKind::ExecutableBytes);
                bool need_codegen = want_asm || want_obj || want_exe;
                bool need_llvm_passes = opts.opt_level > dcc::ir::pass::OptLevel::O0;

                if (want_exe)
                {
                    bool is_elf_target = opts.target.object_format == ObjectFormat::Elf || opts.target.triple.contains("elf");
                    bool is_coff_target = opts.target.object_format == ObjectFormat::Coff || opts.target.triple.contains("coff");

                    if (is_elf_target && opts.target.triple != "x86_64-elf")
                    {
                        add_diag(diags, {}, std::format("executable linking is currently only supported for x86_64-elf (target: '{}')", opts.target.triple));
                        LLVMDisposeModule(llvm_mod);
                        return artifact;
                    }

                    if (is_coff_target)
                    {
                        add_diag(diags, {}, "COFF/PE executable emission is not yet supported");
                        LLVMDisposeModule(llvm_mod);
                        return artifact;
                    }

                    if (!is_elf_target && !is_coff_target)
                    {
                        add_diag(diags, {}, std::format("executable emission is not supported for target '{}'", opts.target.triple));
                        LLVMDisposeModule(llvm_mod);
                        return artifact;
                    }
                }

                LLVMTargetMachineRef tm = nullptr;
                bool tm_created = false;

                if (need_llvm_passes || need_codegen)
                {
                    LLVMTargetRef target_ref = nullptr;
                    char* err_msg = nullptr;

                    if (LLVMGetTargetFromTriple(cg_triple.c_str(), &target_ref, &err_msg))
                    {
                        add_diag(diags, {}, std::format("LLVM backend: unsupported target triple '{}': {}", cg_triple, err_msg ? err_msg : "unknown error"));
                        if (err_msg)
                            LLVMDisposeMessage(err_msg);

                        LLVMDisposeModule(llvm_mod);
                        return artifact;
                    }

                    const auto* const cpu = [&]() -> const char* {
                        if (!opts.target.cpu.empty())
                            return opts.target.cpu.c_str();
                        if (opts.target.arch == Arch::X86_64)
                            return "generic";
                        return "i686";
                    }();

                    auto features = llvm_target_features(opts.target);
                    tm = LLVMCreateTargetMachine(target_ref, cg_triple.c_str(), cpu, features.c_str(), LLVMCodeGenLevelDefault, llvm_reloc_mode(opts.target),
                                                 llvm_code_model(opts.target));
                    if (!tm)
                    {
                        add_diag(diags, {}, std::format("LLVM backend: could not create TargetMachine for '{}'", cg_triple));
                        LLVMDisposeModule(llvm_mod);
                        return artifact;
                    }
                    tm_created = true;

                    {
                        auto* td = LLVMCreateTargetDataLayout(tm);
                        char* dl_str = LLVMCopyStringRepOfTargetData(td);
                        LLVMSetDataLayout(llvm_mod, dl_str);
                        LLVMDisposeMessage(dl_str);
                        LLVMDisposeTargetData(td);
                    }
                }

                if (need_llvm_passes && tm_created)
                {
                    char const* pipeline = nullptr;
                    if (opts.opt_level == dcc::ir::pass::OptLevel::O1)
                        pipeline = "default<O1>";
                    else if (opts.opt_level == dcc::ir::pass::OptLevel::O2)
                        pipeline = "default<O2>";
                    else if (opts.opt_level == dcc::ir::pass::OptLevel::Os)
                        pipeline = "default<Os>";

                    if (pipeline)
                    {
                        auto* pass_opts = LLVMCreatePassBuilderOptions();
                        auto err = LLVMRunPasses(llvm_mod, pipeline, tm, pass_opts);
                        LLVMDisposePassBuilderOptions(pass_opts);

                        if (err)
                        {
                            char* err_msg = LLVMGetErrorMessage(err);
                            add_diag(diags, {}, std::format("LLVM pass pipeline failed: {}", err_msg ? err_msg : "unknown error"));
                            if (err_msg)
                                LLVMDisposeErrorMessage(err_msg);

                            if (tm_created)
                                LLVMDisposeTargetMachine(tm);
                            LLVMDisposeModule(llvm_mod);
                            return artifact;
                        }
                    }
                }

                if (want_ir)
                {
                    char* ir_str = LLVMPrintModuleToString(llvm_mod);
                    artifact.llvm_ir_text = std::string{ir_str};
                    LLVMDisposeMessage(ir_str);
                }

                std::vector<std::byte> obj_bytes_for_link;

                if (need_codegen && tm_created)
                {
                    if (want_asm)
                    {
                        LLVMMemoryBufferRef membuf = nullptr;
                        char* emit_err = nullptr;
                        if (LLVMTargetMachineEmitToMemoryBuffer(tm, llvm_mod, LLVMAssemblyFile, &emit_err, &membuf))
                        {
                            add_diag(diags, {}, std::format("LLVM backend: assembly emission failed: {}", emit_err ? emit_err : "unknown error"));
                            if (emit_err)
                                LLVMDisposeMessage(emit_err);
                        }
                        else
                        {
                            auto const* data = LLVMGetBufferStart(membuf);
                            auto size = LLVMGetBufferSize(membuf);
                            artifact.asm_text = std::string(data, data + size);
                            LLVMDisposeMemoryBuffer(membuf);
                        }

                        if (emit_err)
                            LLVMDisposeMessage(emit_err);
                    }

                    if (want_obj || want_exe)
                    {
                        LLVMMemoryBufferRef membuf = nullptr;
                        char* emit_err = nullptr;
                        if (LLVMTargetMachineEmitToMemoryBuffer(tm, llvm_mod, LLVMObjectFile, &emit_err, &membuf))
                        {
                            add_diag(diags, {}, std::format("LLVM backend: object emission failed: {}", emit_err ? emit_err : "unknown error"));
                            if (emit_err)
                                LLVMDisposeMessage(emit_err);
                        }
                        else
                        {
                            auto const* data = LLVMGetBufferStart(membuf);
                            auto size = LLVMGetBufferSize(membuf);
                            std::vector<std::byte> bytes(reinterpret_cast<std::byte const*>(data), reinterpret_cast<std::byte const*>(data) + size);

                            if (want_obj)
                                artifact.object_bytes = bytes;
                            if (want_exe)
                                obj_bytes_for_link = std::move(bytes);

                            LLVMDisposeMemoryBuffer(membuf);
                        }

                        if (emit_err)
                            LLVMDisposeMessage(emit_err);
                    }
                }

                if (tm_created)
                    LLVMDisposeTargetMachine(tm);

                if (want_exe && !obj_bytes_for_link.empty())
                {
                    auto link_result = link_executable(obj_bytes_for_link, opts, diags);
                    if (link_result)
                        artifact.executable_bytes = std::move(*link_result);
                }

                LLVMDisposeModule(llvm_mod);
                return artifact;
            }

        private:
            [[nodiscard]] static std::optional<std::vector<std::byte>> link_executable(std::vector<std::byte> const& object_bytes, BackendOptions const& opts,
                                                                                       std::vector<BackendDiagnostic>& diags)
            {
                namespace fs = std::filesystem;

                std::error_code ec;
                auto tmp_dir = fs::temp_directory_path(ec);
                if (ec)
                {
                    add_diag(diags, {}, "failed to locate temporary directory for linking");
                    return std::nullopt;
                }

                auto tag = std::format("dcc-link-{}", std::chrono::steady_clock::now().time_since_epoch().count());

                auto obj_path = tmp_dir / std::format("{}.o", tag);
                auto exe_path = tmp_dir / tag;

                {
                    std::ofstream obj_out{obj_path, std::ios::binary};
                    if (!obj_out)
                    {
                        add_diag(diags, {}, "failed to write temporary object file for linking");
                        return std::nullopt;
                    }
                    obj_out.write(reinterpret_cast<char const*>(object_bytes.data()), static_cast<std::streamsize>(object_bytes.size()));
                    obj_out.close();
                    if (!obj_out)
                    {
                        fs::remove(obj_path, ec);
                        add_diag(diags, {}, "failed to write temporary object file for linking");
                        return std::nullopt;
                    }
                }

                struct TempCleanup
                {
                    fs::path obj;
                    fs::path exe;
                    ~TempCleanup()
                    {
                        std::error_code ec;
                        fs::remove(obj, ec);
                        fs::remove(exe, ec);
                    }
                } cleanup{.obj = obj_path, .exe = exe_path};

                std::string cmd = "ld.lld --static --no-dynamic-linker --fatal-warnings -o ";
                cmd += exe_path.string();
                cmd += " ";
                cmd += obj_path.string();

                for (auto const& extra_obj : opts.additional_objects)
                {
                    cmd += " ";
                    cmd += extra_obj;
                }

                for (auto const& lp : opts.library_paths)
                {
                    cmd += " -L";
                    cmd += lp;
                }

                for (auto const& lib : opts.libraries)
                {
                    cmd += " -l";
                    cmd += lib;
                }

                for (auto const& la : opts.linker_args)
                {
                    cmd += " ";
                    cmd += la;
                }

                cmd += " 2>&1";

                std::array<char, 4096> buf{};
                std::string captured;
                auto* pipe = popen(cmd.c_str(), "r");
                if (pipe)
                {
                    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
                        captured += buf.data();
                    auto rc = pclose(pipe);

                    if (rc != 0)
                    {
                        std::string msg;
                        if (!captured.empty())
                            msg = std::format("linker error:\n{}", captured);
                        else
                            msg = "linker failed with unknown error";

                        add_diag(diags, {}, msg);
                        return std::nullopt;
                    }
                }
                else
                {
                    add_diag(diags, {}, "failed to invoke linker");
                    return std::nullopt;
                }

                std::error_code read_ec;
                auto exe_size = fs::file_size(exe_path, read_ec);
                if (read_ec || exe_size == 0)
                {
                    add_diag(diags, {}, "failed to read linked executable");
                    return std::nullopt;
                }

                std::ifstream exe_in{exe_path, std::ios::binary};
                if (!exe_in)
                {
                    add_diag(diags, {}, "failed to open linked executable");
                    return std::nullopt;
                }

                std::vector<std::byte> exe_bytes(static_cast<std::size_t>(exe_size));
                exe_in.read(reinterpret_cast<char*>(exe_bytes.data()), static_cast<std::streamsize>(exe_size));
                if (!exe_in)
                {
                    add_diag(diags, {}, "failed to read linked executable");
                    return std::nullopt;
                }

                return exe_bytes;
            }

            [[nodiscard]] static bool precheck_module(IrModule const& module, std::vector<BackendDiagnostic>& diags)
            {
                bool has_unsupported = false;
                std::unordered_set<IrType const*> seen_types;
                std::function<void(IrType const*)> check_type = [&](IrType const* type) {
                    if (!type || !seen_types.insert(type).second)
                        return;
                    switch (type->kind)
                    {
                        case IrTypeKind::Pointer: {
                            auto* ptr = static_cast<IrPointerType const*>(type);
                            if (ptr->flavor == PointerFlavor::Far)
                            {
                                add_diag(diags, {}, "dynamic far pointers are not supported by the llvm backend");
                                has_unsupported = true;
                            }
                            else if (ptr->flavor == PointerFlavor::Based && ptr->seg != Segment::Fs && ptr->seg != Segment::Gs && ptr->seg != Segment::Ss)
                            {
                                add_diag(diags, {}, "CS, DS, and ES based pointers are not supported by the llvm backend");
                                has_unsupported = true;
                            }
                            check_type(ptr->pointee);
                            break;
                        }
                        case IrTypeKind::Slice: {
                            auto* slice = static_cast<IrSliceType const*>(type);
                            if (slice->flavor == PointerFlavor::Far)
                            {
                                add_diag(diags, {}, "dynamic far slices are not supported by the llvm backend");
                                has_unsupported = true;
                            }
                            else if (slice->flavor == PointerFlavor::Based && slice->seg != Segment::Fs && slice->seg != Segment::Gs &&
                                     slice->seg != Segment::Ss)
                            {
                                add_diag(diags, {}, "CS, DS, and ES based slices are not supported by the llvm backend");
                                has_unsupported = true;
                            }
                            check_type(slice->element);
                            break;
                        }
                        case IrTypeKind::Array:
                            check_type(static_cast<IrArrayType const*>(type)->element);
                            break;
                        case IrTypeKind::Aggregate:
                            for (auto* member : static_cast<IrAggregateType const*>(type)->members)
                                check_type(member);
                            break;
                        case IrTypeKind::Func: {
                            auto* func_type = static_cast<IrFuncType const*>(type);
                            check_type(func_type->return_type);
                            for (auto* param : func_type->params)
                                check_type(param);
                            break;
                        }
                        default:
                            break;
                    }
                };

                for (auto* func : module.functions)
                {
                    if (!func)
                        continue;
                    check_type(func->func_type);

                    for (auto* bb : func->blocks)
                    {
                        if (!bb)
                            continue;

                        for (auto* inst : bb->instructions)
                        {
                            if (inst)
                                check_type(inst->type);
                            has_unsupported |= precheck_instruction(inst, diags);
                        }
                        if (bb->terminator)
                            has_unsupported |= precheck_terminator(bb->terminator, diags);
                    }
                }

                for (auto* g : module.globals)
                {
                    if (!g)
                        continue;

                    if (!g->type)
                    {
                        add_diag(diags, g->range, "global missing type");
                        has_unsupported = true;
                        continue;
                    }
                    check_type(g->type);

                    switch (g->type->kind)
                    {
                        case IrTypeKind::Func:
                            add_diag(diags, g->range, "LLVM backend does not yet support function-pointer globals");
                            has_unsupported = true;
                            break;
                        default:
                            break;
                    }

                    if (g->init)
                        std::ignore = g->init->kind;
                }

                return has_unsupported;
            }

            [[nodiscard]] static bool precheck_instruction(IrValue const* inst, std::vector<BackendDiagnostic>& diags)
            {
                if (!inst)
                    return false;

                switch (inst->kind)
                {
                    case IrNodeKind::ReadSegment:
                    case IrNodeKind::PointerSegment:
                        add_diag(diags, inst->range, "dynamic far pointers are not supported by the llvm backend");
                        return true;
                    case IrNodeKind::MakePointer:
                        if (static_cast<IrMakePointerInst const*>(inst)->segment)
                        {
                            add_diag(diags, inst->range, "dynamic far pointers are not supported by the llvm backend");
                            return true;
                        }
                        break;
                    case IrNodeKind::AtomicLoad:
                    case IrNodeKind::AtomicStore:
                    case IrNodeKind::AtomicRmw:
                    case IrNodeKind::AtomicCmpXchg: {
                        IrValue const* pointer = nullptr;
                        if (inst->kind == IrNodeKind::AtomicLoad)
                            pointer = static_cast<IrAtomicLoadInst const*>(inst)->pointer;
                        else if (inst->kind == IrNodeKind::AtomicStore)
                            pointer = static_cast<IrAtomicStoreInst const*>(inst)->pointer;
                        else if (inst->kind == IrNodeKind::AtomicRmw)
                            pointer = static_cast<IrAtomicRmwInst const*>(inst)->pointer;
                        else
                            pointer = static_cast<IrAtomicCmpXchgInst const*>(inst)->pointer;
                        if (pointer && pointer->type && pointer->type->kind == IrTypeKind::Pointer &&
                            static_cast<IrPointerType const*>(pointer->type)->flavor != PointerFlavor::Near)
                        {
                            add_diag(diags, inst->range, "atomics through based or dynamic far pointers are not supported by the llvm backend");
                            return true;
                        }
                        break;
                    }
                    case IrNodeKind::Load:
                        if (!static_cast<IrLoadInst const*>(inst)->pointer)
                        {
                            add_diag(diags, inst->range, "LLVM backend: load has no pointer operand");
                            return true;
                        }
                        break;
                    case IrNodeKind::Store: {
                        auto const* store = static_cast<IrStoreInst const*>(inst);
                        if (!store->pointer || !store->value)
                        {
                            add_diag(diags, inst->range,
                                     std::format("LLVM backend: store has no {} operand", !store->pointer && !store->value ? "pointer or value"
                                                                                          : !store->pointer                ? "pointer"
                                                                                                                           : "value"));
                            return true;
                        }
                        break;
                    }
                    default:
                        break;
                }
                return false;
            }

            [[nodiscard]] static bool precheck_terminator(IrNode const* term, std::vector<BackendDiagnostic>& diags)
            {
                std::ignore = term;
                std::ignore = diags;
                return false;
            }

            [[nodiscard]] static LLVMValueRef declare_global(IrGlobal const* g, LLVMModuleRef mod, TypeCache& tc)
            {
                auto* mem_ty = llvm_mem_type_cached(tc, g->type);
                if (!mem_ty)
                    return nullptr;

                std::vector<InitLeaf> slots;
                std::vector<std::uint8_t> bytes;
                std::string layout_error;
                bool is_reloc_union = g->init && g->init->kind == IrNodeKind::Aggregate && tc.uses_byte_storage(g->type) && has_global_ref(g->init);
                if (is_reloc_union && resolve_init_layout(g->init, g->type, slots, bytes, tc.little_endian, &layout_error))
                {
                    bool ok = false;
                    auto* storage_ty = build_union_storage_type(tc.ctx, g->type, slots, ok);
                    if (ok && storage_ty)
                        mem_ty = storage_ty;
                }

                auto* gv = LLVMAddGlobal(mod, mem_ty, std::string{g->name}.c_str());
                apply_linkage_and_comdat(gv, g->linkage, mod, g->name);
                if (g->is_dll_import)
                    LLVMSetDLLStorageClass(gv, LLVMDLLImportStorageClass);
                else if (g->is_dll_export)
                    LLVMSetDLLStorageClass(gv, LLVMDLLExportStorageClass);
                if (!g->section.empty())
                    LLVMSetSection(gv, std::string{g->section}.c_str());
                return gv;
            }

            [[nodiscard]] static bool init_global(IrGlobal const* g, LLVMValueRef gv, LLVMContextRef ctx, TypeCache& tc,
                                                  std::unordered_map<IrValue const*, LLVMValueRef>& val_map, std::vector<BackendDiagnostic>& diags)
            {
                auto* mem_ty = LLVMGlobalGetValueType(gv);

                LLVMValueRef init_val = nullptr;
                if (g->init)
                {
                    std::string constant_error;

                    if (g->init->kind == IrNodeKind::Aggregate && tc.uses_byte_storage(g->type) && has_global_ref(g->init))
                    {
                        std::vector<InitLeaf> slots;
                        std::vector<std::uint8_t> bytes;
                        if (resolve_init_layout(g->init, g->type, slots, bytes, tc.little_endian, &constant_error))
                        {
                            init_val = build_union_storage_init(ctx, tc, g->type, slots, bytes, val_map, &constant_error);
                        }
                        else
                            init_val = nullptr;
                    }

                    if (!init_val)
                        init_val = c_api_constant(g->init, ctx, tc, val_map, mem_ty, &constant_error);
                    if (!init_val)
                    {
                        add_diag(diags, g->range, constant_error.empty() ? "LLVM backend: unsupported global initializer" : std::move(constant_error));
                        return false;
                    }
                    LLVMSetInitializer(gv, init_val);
                }
                else if (!g->is_declaration)
                {
                    auto* null_val = LLVMConstNull(mem_ty);
                    LLVMSetInitializer(gv, null_val);
                }

                if (g->is_constant)
                    LLVMSetGlobalConstant(gv, 1);

                if (g->alignment > 0)
                    LLVMSetAlignment(gv, g->alignment);
                else if (g->type && tc.uses_byte_storage(g->type) && g->init && g->init->kind == IrNodeKind::Aggregate && has_global_ref(g->init))
                    LLVMSetAlignment(gv, static_cast<unsigned>(g->type->byte_align));

                return true;
            }

            [[nodiscard]] static LLVMLinkage llvm_linkage(Linkage l)
            {
                switch (l)
                {
                    case Linkage::Internal:
                        return LLVMInternalLinkage;
                    case Linkage::External:
                        return LLVMExternalLinkage;
                    case Linkage::LinkOnceODR:
                        return LLVMLinkOnceODRLinkage;
                    case Linkage::WeakODR:
                        return LLVMWeakODRLinkage;
                }
                return LLVMInternalLinkage;
            }

            static void apply_linkage_and_comdat(LLVMValueRef gv, Linkage l, LLVMModuleRef mod, std::string_view name)
            {
                LLVMSetLinkage(gv, llvm_linkage(l));
                if (l == Linkage::LinkOnceODR || l == Linkage::WeakODR)
                {
                    LLVMComdatRef comdat = LLVMGetOrInsertComdat(mod, std::string{name}.c_str());
                    LLVMSetComdatSelectionKind(comdat, LLVMAnyComdatSelectionKind);
                    LLVMSetComdat(gv, comdat);
                }
            }

            [[nodiscard]] static std::optional<unsigned> map_calling_conv_to_llvm(IrFunction const* func, TargetConfig const& target,
                                                                                  std::vector<BackendDiagnostic>& diags, bool& found_cc_attr)
            {
                std::string_view cc;
                found_cc_attr = false;
                for (auto const& a : func->attrs)
                    if (a.kind == IrFuncAttr::CallingConv)
                    {
                        cc = a.value;
                        found_cc_attr = true;
                        break;
                    }

                if (!found_cc_attr)
                    return std::nullopt;

                if (cc.empty())
                    return LLVMCCallConv;

                unsigned llvm_cc;
                auto eq = [](std::string_view x, std::string_view y) {
                    return std::ranges::equal(
                        x, y, [](char cx, char cy) { return std::tolower(static_cast<unsigned char>(cx)) == std::tolower(static_cast<unsigned char>(cy)); });
                };

                if (eq(cc, "cdecl"))
                    llvm_cc = LLVMCCallConv;
                else if (eq(cc, "stdcall"))
                {
                    if (target.arch != Arch::X86_64 && target.arch != Arch::X86)
                    {
                        add_diag(diags, func->range, std::format("calling convention '{}' requires x86 or x86_64 target (current: {})", cc, target.triple));
                        return std::nullopt;
                    }
                    llvm_cc = LLVMX86StdcallCallConv;
                }
                else if (eq(cc, "fastcall"))
                {
                    if (target.arch != Arch::X86_64 && target.arch != Arch::X86)
                    {
                        add_diag(diags, func->range, std::format("calling convention '{}' requires x86 or x86_64 target (current: {})", cc, target.triple));
                        return std::nullopt;
                    }
                    llvm_cc = LLVMX86FastcallCallConv;
                }
                else if (eq(cc, "vectorcall"))
                {
                    if (target.arch != Arch::X86_64 && target.arch != Arch::X86)
                    {
                        add_diag(diags, func->range, std::format("calling convention '{}' requires x86 or x86_64 target (current: {})", cc, target.triple));
                        return std::nullopt;
                    }
                    llvm_cc = LLVMX86VectorCallCallConv;
                }
                else if (eq(cc, "systemv") || eq(cc, "sysv"))
                {
                    if (target.arch != Arch::X86_64)
                    {
                        add_diag(diags, func->range, std::format("calling convention '{}' requires x86_64 target (current: {})", cc, target.triple));
                        return std::nullopt;
                    }
                    llvm_cc = LLVMX8664SysVCallConv;
                }
                else if (eq(cc, "win64"))
                {
                    if (target.arch != Arch::X86_64)
                    {
                        add_diag(diags, func->range, std::format("calling convention '{}' requires x86_64 target (current: {})", cc, target.triple));
                        return std::nullopt;
                    }
                    llvm_cc = LLVMWin64CallConv;
                }
                else
                {
                    add_diag(diags, func->range, std::format("unknown calling convention '{}'", cc));
                    return std::nullopt;
                }

                return llvm_cc;
            }

            [[nodiscard]] static std::optional<unsigned> get_calling_conv_for_call(IrValue const* callee, TargetConfig const& target,
                                                                                   std::vector<BackendDiagnostic>& diags, bool& had_error)
            {
                had_error = false;
                const auto* gref = ir_cast<IrGlobalRef>(callee);
                if (!gref || !gref->function)
                    return std::nullopt;

                bool found = false;
                auto result = map_calling_conv_to_llvm(gref->function, target, diags, found);
                if (found && !result)
                    had_error = true;
                return result;
            }

            [[nodiscard]] static bool create_function_decl(IrFunction const* func, LLVMModuleRef mod, LLVMContextRef ctx, TypeCache& tc,
                                                           std::unordered_map<IrValue const*, LLVMValueRef>& val_map, BackendOptions const& opts,
                                                           std::vector<BackendDiagnostic>& diags, DebugEmitContext* debug)
            {
                const auto* ft = func->func_type;
                if (!ft)
                    return false;

                auto const sig = compute_signature_abi(tc, ft->return_type, ft->params, aggregate_abi_for(tc, func));
                bool const sret = sig.sret || sig.sret_boundary;

                auto* llvm_func = LLVMAddFunction(mod, std::string{func->name}.c_str(), sig.fn_type);
                apply_linkage_and_comdat(llvm_func, func->linkage, mod, func->name);
                val_map[func] = llvm_func;
                auto const uwtable_kind = LLVMGetEnumAttributeKindForName("uwtable", 7);
                auto* uwtable = LLVMCreateEnumAttribute(ctx, uwtable_kind, 2);
                LLVMAddAttributeAtIndex(llvm_func, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), uwtable);

                auto add_indirect_attr = [&](unsigned index, IrType const* type, const char* name) {
                    auto kind = LLVMGetEnumAttributeKindForName(name, static_cast<unsigned>(std::strlen(name)));
                    if (kind != 0)
                    {
                        auto* attr = LLVMCreateTypeAttribute(ctx, kind, llvm_type_cached(tc, type));
                        LLVMAddAttributeAtIndex(llvm_func, index, attr);
                    }
                };
                if (sret)
                    add_indirect_attr(1, ft->return_type, "sret");
                for (std::size_t i = 0; i < ft->params.size() && i < sig.params.size(); ++i)
                    if (sig.params[i].kind == ArgAbi::Kind::Indirect || sig.params[i].kind == ArgAbi::Kind::Memory)
                        add_indirect_attr(sig.params[i].first_param + 1, ft->params[i], "byval");

                if (debug && debug->dibuilder && debug->difile)
                {
                    auto* sub_ty = LLVMDIBuilderCreateSubroutineType(debug->dibuilder, debug->difile, nullptr, 0, LLVMDIFlagZero);

                    std::string_view sp_name = func->source_name.empty() ? func->name : func->source_name;

                    unsigned decl_line = func->decl_line;
                    if (decl_line == 0)
                        decl_line = 1;

                    auto* sp_file = debug->get_or_create_file(func->decl_file_id);

                    auto* sp = LLVMDIBuilderCreateFunction(debug->dibuilder, debug->dicu, std::string{sp_name}.c_str(), sp_name.size(), "", 0, sp_file,
                                                           decl_line, sub_ty, false, true, decl_line, LLVMDIFlagZero, false);

                    if (!func->blocks.empty())
                        LLVMSetSubprogram(llvm_func, sp);

                    debug->subprogram_map[func] = sp;
                }

                bool found_cc_attr = false;
                auto cc_opt = map_calling_conv_to_llvm(func, opts.target, diags, found_cc_attr);
                if (cc_opt)
                    LLVMSetFunctionCallConv(llvm_func, *cc_opt);
                else if (found_cc_attr)
                    return false;

                if (opts.target.no_red_zone)
                {
                    auto kind = LLVMGetEnumAttributeKindForName("noredzone", 9);
                    if (kind != 0)
                    {
                        auto* attr = LLVMCreateEnumAttribute(ctx, kind, 0);
                        LLVMAddAttributeAtIndex(llvm_func, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), attr);
                    }
                }

                if (opts.target.no_stack_protector)
                {
                    auto const* key = "stack-protector";
                    auto const* val = "none";
                    auto* attr = LLVMCreateStringAttribute(ctx, key, static_cast<unsigned>(std::strlen(key)), val, static_cast<unsigned>(std::strlen(val)));
                    LLVMAddAttributeAtIndex(llvm_func, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), attr);
                }

                if (opts.target.no_stack_probe)
                {
                    auto const* key = "stack-probe-size";
                    auto const* val = "4294967295";
                    auto* attr = LLVMCreateStringAttribute(ctx, key, static_cast<unsigned>(std::strlen(key)), val, static_cast<unsigned>(std::strlen(val)));
                    LLVMAddAttributeAtIndex(llvm_func, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), attr);

                    auto const* no_arg_probe_key = "no-stack-arg-probe";
                    auto* no_arg_probe_attr = LLVMCreateStringAttribute(ctx, no_arg_probe_key, static_cast<unsigned>(std::strlen(no_arg_probe_key)), "", 0);
                    LLVMAddAttributeAtIndex(llvm_func, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), no_arg_probe_attr);
                }

                if (!opts.omit_frame_pointer || opts.target.object_format == dcc::target::ObjectFormat::Coff)
                {
                    auto const* fp_key = "frame-pointer";
                    auto const* fp_val = "all";
                    auto* attr =
                        LLVMCreateStringAttribute(ctx, fp_key, static_cast<unsigned>(std::strlen(fp_key)), fp_val, static_cast<unsigned>(std::strlen(fp_val)));

                    LLVMAddAttributeAtIndex(llvm_func, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), attr);
                }

                for (auto const& a : func->attrs)
                {
                    if (a.kind == IrFuncAttr::Inline)
                    {
                        auto kind = LLVMGetEnumAttributeKindForName("alwaysinline", 12);
                        if (kind != 0)
                        {
                            auto* attr = LLVMCreateEnumAttribute(ctx, kind, 0);
                            LLVMAddAttributeAtIndex(llvm_func, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), attr);
                        }
                    }
                    else if (a.kind == IrFuncAttr::NoInline)
                    {
                        auto kind = LLVMGetEnumAttributeKindForName("noinline", 8);
                        if (kind != 0)
                        {
                            auto* attr = LLVMCreateEnumAttribute(ctx, kind, 0);
                            LLVMAddAttributeAtIndex(llvm_func, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), attr);
                        }
                    }
                }

                if (func->entry_block)
                {
                    for (std::size_t i = 0; i < func->entry_block->params.size() && i < sig.params.size(); ++i)
                    {
                        auto const& abi = sig.params[i];
                        if (abi.kind != ArgAbi::Kind::Direct && abi.kind != ArgAbi::Kind::Indirect)
                            continue;
                        auto* param = func->entry_block->params[i];
                        auto* pv = LLVMGetParam(llvm_func, abi.first_param);
                        if (param && !param->name.empty())
                            LLVMSetValueName2(pv, std::string{param->name}.c_str(), param->name.size());

                        val_map[param] = pv;
                    }
                }

                return true;
            }

            [[nodiscard]] static bool emit_function_body(IrFunction const* func, LLVMContextRef ctx, TypeCache& tc,
                                                         std::unordered_map<IrValue const*, LLVMValueRef>& val_map, TargetConfig const& target,
                                                         std::vector<BackendDiagnostic>& diags, DebugEmitContext* debug)
            {
                if (func->blocks.empty())
                    return true;

                auto* llvm_func = lookup_function_val(func, val_map);
                if (!llvm_func)
                    return false;

                struct LocKey
                {
                    std::uint32_t block_id;
                    std::uint32_t instruction_index;
                    bool is_terminator;

                    bool operator==(LocKey const& o) const noexcept
                    {
                        return block_id == o.block_id && instruction_index == o.instruction_index && is_terminator == o.is_terminator;
                    }
                };

                struct LocKeyHash
                {
                    std::size_t operator()(LocKey const& k) const noexcept
                    {
                        std::size_t h = std::hash<std::uint32_t>{}(k.block_id);
                        h ^= std::hash<std::uint32_t>{}(k.instruction_index) + 0x9e3779b9 + (h << 6) + (h >> 2);
                        h ^= std::hash<bool>{}(k.is_terminator) + 0x9e3779b9 + (h << 6) + (h >> 2);
                        return h;
                    }
                };

                std::unordered_map<LocKey, IrDebugLocation const*, LocKeyHash> loc_index;
                std::unordered_map<std::uint32_t, LLVMMetadataRef> scope_map;
                std::unordered_map<std::uint32_t, std::tuple<std::uint32_t, unsigned, unsigned>> scope_first_loc;

                if (debug)
                {
                    for (auto const& dl : func->debug_locations)
                    {
                        LocKey key{.block_id = dl.block_id, .instruction_index = dl.instruction_index, .is_terminator = dl.is_terminator};
                        loc_index[key] = &dl;
                    }

                    for (auto const& dl : func->debug_locations)
                    {
                        if (dl.loc.scope_id == 0)
                            continue;

                        if (!scope_first_loc.contains(dl.loc.scope_id))
                            scope_first_loc[dl.loc.scope_id] = {dl.loc.file_id, dl.loc.line, dl.loc.column};
                    }
                }

                auto get_subprogram = [&](IrFunction const* f) -> LLVMMetadataRef {
                    if (!debug)
                        return nullptr;

                    auto it = debug->subprogram_map.find(f);
                    return it != debug->subprogram_map.end() ? it->second : nullptr;
                };

                auto get_scope = [&](IrFunction const* f, std::uint32_t scope_id) -> LLVMMetadataRef {
                    if (!debug)
                        return nullptr;

                    if (scope_id == 0)
                        return get_subprogram(f);

                    auto it = scope_map.find(scope_id);
                    if (it != scope_map.end())
                        return it->second;

                    auto* sp = get_subprogram(f);
                    if (!sp || !debug->difile)
                        return nullptr;

                    unsigned line = 0;
                    unsigned col = 0;
                    std::uint32_t file_id = 0;
                    auto loc_it = scope_first_loc.find(scope_id);
                    if (loc_it != scope_first_loc.end())
                    {
                        file_id = std::get<0>(loc_it->second);
                        line = std::get<1>(loc_it->second);
                        col = std::get<2>(loc_it->second);
                    }

                    auto* scope_file = debug->get_or_create_file(file_id);

                    auto* lb = LLVMDIBuilderCreateLexicalBlock(debug->dibuilder, sp, scope_file, line, col);
                    scope_map[scope_id] = lb;
                    return lb;
                };

                auto apply_debug_loc = [&](LLVMBuilderRef builder, LocKey key) {
                    if (!debug)
                        return;

                    auto it = loc_index.find(key);
                    if (it == loc_index.end() || it->second == nullptr)
                    {
                        LLVMSetCurrentDebugLocation2(builder, nullptr);
                        return;
                    }

                    auto const& loc = it->second->loc;
                    if (loc.line == 0)
                    {
                        LLVMSetCurrentDebugLocation2(builder, nullptr);
                        return;
                    }

                    auto* scope = get_scope(func, loc.scope_id);
                    if (!scope)
                    {
                        LLVMSetCurrentDebugLocation2(builder, nullptr);
                        return;
                    }

                    auto* diloc = LLVMDIBuilderCreateDebugLocation(ctx, loc.line, loc.column, scope, nullptr);
                    LLVMSetCurrentDebugLocation2(builder, diloc);
                };

                std::unordered_map<IrBasicBlock const*, LLVMBasicBlockRef> bb_map;
                for (auto* bb : func->blocks)
                {
                    if (!bb)
                        continue;

                    std::string bb_name;
                    if (!bb->name.empty())
                        bb_name = std::string{bb->name};
                    else
                        bb_name = std::format("bb{}", bb->id);

                    auto* llvm_bb = LLVMAppendBasicBlockInContext(ctx, llvm_func, bb_name.c_str());
                    bb_map[bb] = llvm_bb;
                }

                auto* builder = LLVMCreateBuilderInContext(ctx);
                LlvmBuilderGuard bld_guard{builder};

                if (func->entry_block && func->func_type)
                {
                    auto* entry_bb = bb_map[func->entry_block];
                    auto const sig = compute_signature_abi(tc, func->func_type->return_type, func->func_type->params, aggregate_abi_for(tc, func));
                    if (entry_bb && sig.rewrites())
                    {
                        LLVMPositionBuilderAtEnd(builder, entry_bb);
                        for (std::size_t i = 0; i < func->entry_block->params.size() && i < sig.params.size(); ++i)
                        {
                            auto const& abi = sig.params[i];
                            auto* param = func->entry_block->params[i];
                            auto const* pt = func->func_type->params[i];
                            if (abi.kind == ArgAbi::Kind::Coerce)
                            {
                                std::vector<LLVMValueRef> parts;
                                for (std::size_t k = 0; k < abi.pieces.size(); ++k)
                                    parts.push_back(LLVMGetParam(llvm_func, abi.first_param + static_cast<unsigned>(k)));
                                val_map[param] = abi_join(builder, tc, pt, parts);
                            }
                            else if (abi.kind == ArgAbi::Kind::Memory)
                                val_map[param] = LLVMBuildLoad2(builder, llvm_type_cached(tc, pt), LLVMGetParam(llvm_func, abi.first_param), "");
                        }
                    }
                }

                std::vector<IrBasicBlock*> emit_order;
                {
                    std::unordered_map<IrBasicBlock const*, std::size_t> position;
                    for (std::size_t i = 0; i < func->blocks.size(); ++i)
                        if (func->blocks[i])
                            position[func->blocks[i]] = i;
                    auto block_successors = [&](IrBasicBlock* bb) {
                        std::vector<IrBasicBlock*> successors;
                        if (bb && bb->terminator)
                        {
                            switch (bb->terminator->kind)
                            {
                                case IrNodeKind::Br:
                                    successors.push_back(static_cast<IrBrInst*>(bb->terminator)->target);
                                    break;
                                case IrNodeKind::BrCond: {
                                    auto* branch = static_cast<IrBrCondInst*>(bb->terminator);
                                    successors.push_back(branch->true_target);
                                    successors.push_back(branch->false_target);
                                    break;
                                }
                                case IrNodeKind::Switch: {
                                    auto* sw = static_cast<IrSwitchInst*>(bb->terminator);
                                    successors.push_back(sw->default_target);
                                    for (auto& c : sw->cases)
                                        successors.push_back(c.target);
                                    break;
                                }
                                default:
                                    break;
                            }
                        }
                        auto rank = [&](IrBasicBlock* b) {
                            auto it = position.find(b);
                            return it != position.end() ? it->second : std::numeric_limits<std::size_t>::max();
                        };
                        std::ranges::sort(successors, [&](IrBasicBlock* a, IrBasicBlock* b) { return rank(a) > rank(b); });
                        return successors;
                    };
                    std::unordered_set<IrBasicBlock const*> visited;
                    std::vector<std::pair<IrBasicBlock*, std::size_t>> stack;
                    std::vector<IrBasicBlock*> finished;
                    if (func->entry_block && position.contains(func->entry_block))
                    {
                        visited.insert(func->entry_block);
                        stack.emplace_back(func->entry_block, 0);
                    }
                    while (!stack.empty())
                    {
                        auto& [current, next] = stack.back();
                        auto successors = block_successors(current);
                        if (next < successors.size())
                        {
                            auto* follower = successors[next++];
                            if (follower && visited.insert(follower).second)
                                stack.emplace_back(follower, 0);
                        }
                        else
                        {
                            finished.push_back(current);
                            stack.pop_back();
                        }
                    }
                    std::ranges::reverse(finished);
                    std::unordered_set<IrBasicBlock const*> seen(finished.begin(), finished.end());
                    emit_order.assign(finished.begin(), finished.end());
                    for (auto* bb : func->blocks)
                        if (bb && !seen.contains(bb))
                            emit_order.push_back(bb);
                }

                std::uint32_t instruction_index = 0;

                for (auto* bb : emit_order)
                {
                    if (!bb)
                        continue;

                    auto* llvm_bb = bb_map[bb];
                    LLVMPositionBuilderAtEnd(builder, llvm_bb);

                    instruction_index = 0;
                    for (auto* inst : bb->instructions)
                    {
                        apply_debug_loc(builder, LocKey{bb->id, instruction_index, false});
                        auto const diag_count = diags.size();
                        if (!emit_instruction(inst, builder, ctx, tc, val_map, bb_map, target, diags))
                        {
                            if (diags.size() == diag_count)
                                add_diag(diags, inst ? inst->range : func->range,
                                         std::format("LLVM backend failed to lower IR instruction kind {} in function '{}' block {} at instruction {}",
                                                     inst ? static_cast<int>(inst->kind) : -1, func->name, bb->id, instruction_index));
                            return false;
                        }

                        ++instruction_index;
                    }

                    if (bb->terminator)
                    {
                        apply_debug_loc(builder, LocKey{bb->id, instruction_index, true});
                        auto const diag_count = diags.size();
                        if (!emit_terminator(bb->terminator, builder, ctx, tc, val_map, bb_map, diags, llvm_func, func))
                        {
                            if (diags.size() == diag_count)
                                add_diag(diags, bb->terminator->range,
                                         std::format("LLVM backend failed to lower IR terminator kind {} in function '{}' block {}",
                                                     static_cast<int>(bb->terminator->kind), func->name, bb->id));
                            return false;
                        }
                    }
                    else
                    {
                        LLVMSetCurrentDebugLocation2(builder, nullptr);
                        LLVMBuildUnreachable(builder);
                    }

                    ++instruction_index;
                }

                for (auto* bb : func->blocks)
                {
                    if (!bb)
                        continue;

                    for (auto* inst : bb->instructions)
                    {
                        if (!inst || inst->kind != IrNodeKind::Phi)
                            continue;

                        auto* p = static_cast<IrPhiInst const*>(inst);
                        auto phi_it = val_map.find(inst);
                        if (phi_it == val_map.end())
                        {
                            add_diag(diags, inst->range, "LLVM backend: PHI node missing from value map");
                            return false;
                        }

                        auto* phi = phi_it->second;
                        if (!phi)
                            continue;

                        for (auto const& pred : p->incoming)
                        {
                            auto* val = [&]() -> LLVMValueRef {
                                if (!pred.value)
                                    return nullptr;

                                if (auto it = val_map.find(pred.value); it != val_map.end())
                                    return it->second;

                                auto* c = c_api_constant(pred.value, ctx, tc, val_map);
                                if (c)
                                {
                                    if (tc.indirect(pred.value->type))
                                    {
                                        auto* global = LLVMAddGlobal(LLVMGetGlobalParent(llvm_func), llvm_type_cached(tc, pred.value->type), "");
                                        LLVMSetLinkage(global, LLVMPrivateLinkage);
                                        LLVMSetGlobalConstant(global, 1);
                                        LLVMSetAlignment(global, static_cast<unsigned>(pred.value->type->byte_align));
                                        LLVMSetInitializer(global, c);
                                        c = global;
                                    }
                                    val_map[pred.value] = c;
                                }

                                return c;
                            }();

                            auto bb_it = bb_map.find(pred.block);
                            if (!val || bb_it == bb_map.end())
                            {
                                add_diag(diags, inst->range, "LLVM backend: PHI incoming value or block could not be resolved");
                                return false;
                            }

                            LLVMValueRef vals[] = {val};
                            LLVMBasicBlockRef blocks[] = {bb_it->second};
                            LLVMAddIncoming(phi, vals, blocks, 1);
                        }
                    }
                }

                return true;
            }

            [[nodiscard]] static LLVMValueRef lookup_function_val(IrFunction const* func, std::unordered_map<IrValue const*, LLVMValueRef> const& val_map)
            {
                auto it = val_map.find(func);
                return it != val_map.end() ? it->second : nullptr;
            }

            [[nodiscard]] static LLVMAtomicOrdering to_llvm_ordering(IrMemoryOrdering ord)
            {
                switch (ord)
                {
                    case IrMemoryOrdering::Relaxed:
                        return LLVMAtomicOrderingMonotonic;
                    case IrMemoryOrdering::Acquire:
                        return LLVMAtomicOrderingAcquire;
                    case IrMemoryOrdering::Release:
                        return LLVMAtomicOrderingRelease;
                    case IrMemoryOrdering::AcqRel:
                        return LLVMAtomicOrderingAcquireRelease;
                    case IrMemoryOrdering::SeqCst:
                        return LLVMAtomicOrderingSequentiallyConsistent;
                }
                return LLVMAtomicOrderingSequentiallyConsistent;
            }

            [[nodiscard]] static LLVMAtomicRMWBinOp to_llvm_rmw_op(IrAtomicRmwOp op)
            {
                switch (op)
                {
                    case IrAtomicRmwOp::Xchg:
                        return LLVMAtomicRMWBinOpXchg;
                    case IrAtomicRmwOp::Add:
                        return LLVMAtomicRMWBinOpAdd;
                    case IrAtomicRmwOp::Sub:
                        return LLVMAtomicRMWBinOpSub;
                    case IrAtomicRmwOp::And:
                        return LLVMAtomicRMWBinOpAnd;
                    case IrAtomicRmwOp::Or:
                        return LLVMAtomicRMWBinOpOr;
                    case IrAtomicRmwOp::Xor:
                        return LLVMAtomicRMWBinOpXor;
                }
                return LLVMAtomicRMWBinOpXchg;
            }

            [[nodiscard]] static LLVMValueRef build_frame_slot(LLVMBuilderRef builder, LLVMTypeRef slot_type)
            {
                auto* insert_bb = LLVMGetInsertBlock(builder);
                if (!insert_bb)
                    return LLVMBuildAlloca(builder, slot_type, "");

                auto* func = LLVMGetBasicBlockParent(insert_bb);
                if (!func)
                    return LLVMBuildAlloca(builder, slot_type, "");

                auto* entry = LLVMGetEntryBasicBlock(func);
                if (!entry || entry == insert_bb)
                    return LLVMBuildAlloca(builder, slot_type, "");

                auto* saved_loc = LLVMGetCurrentDebugLocation2(builder);
                if (auto* first = LLVMGetFirstInstruction(entry))
                    LLVMPositionBuilderBefore(builder, first);
                else
                    LLVMPositionBuilderAtEnd(builder, entry);

                auto* slot = LLVMBuildAlloca(builder, slot_type, "");
                LLVMPositionBuilderAtEnd(builder, insert_bb);
                LLVMSetCurrentDebugLocation2(builder, saved_loc);
                return slot;
            }

            static void copy_indirect(LLVMBuilderRef builder, LLVMContextRef ctx, IrType const* type, LLVMValueRef dst, LLVMValueRef src)
            {
                auto* size = llvm_const_int(LLVMInt64TypeInContext(ctx), type->byte_size, false);
                LLVMBuildMemCpy(builder, dst, static_cast<unsigned>(type->byte_align), src, static_cast<unsigned>(type->byte_align), size);
            }

            static void zero_indirect(LLVMBuilderRef builder, LLVMContextRef ctx, IrType const* type, LLVMValueRef dst)
            {
                auto* zero = llvm_const_int(LLVMInt8TypeInContext(ctx), 0, false);
                auto* size = llvm_const_int(LLVMInt64TypeInContext(ctx), type->byte_size, false);
                LLVMBuildMemSet(builder, dst, zero, size, static_cast<unsigned>(type->byte_align));
            }

            [[nodiscard]] static LLVMValueRef materialize_indirect_constant(LLVMBuilderRef builder, LLVMContextRef ctx, TypeCache& tc, IrType const* type,
                                                                            LLVMValueRef c)
            {
                auto* slot = build_frame_slot(builder, llvm_type_cached(tc, type));
                if (LLVMIsConstant(c) && LLVMIsNull(c))
                    zero_indirect(builder, ctx, type, slot);
                else
                {
                    auto* insert_bb = LLVMGetInsertBlock(builder);
                    auto* func = insert_bb ? LLVMGetBasicBlockParent(insert_bb) : nullptr;
                    auto* mod = func ? LLVMGetGlobalParent(func) : nullptr;
                    if (!mod)
                        return nullptr;
                    auto* global = LLVMAddGlobal(mod, llvm_type_cached(tc, type), "");
                    LLVMSetLinkage(global, LLVMPrivateLinkage);
                    LLVMSetGlobalConstant(global, 1);
                    LLVMSetAlignment(global, static_cast<unsigned>(type->byte_align));
                    LLVMSetInitializer(global, c);
                    copy_indirect(builder, ctx, type, slot, global);
                }
                return slot;
            }

            [[nodiscard]] static bool emit_instruction(IrValue const* inst, LLVMBuilderRef builder, LLVMContextRef ctx, TypeCache& tc,
                                                       std::unordered_map<IrValue const*, LLVMValueRef>& val_map,
                                                       [[maybe_unused]] std::unordered_map<IrBasicBlock const*, LLVMBasicBlockRef>& bb_map,
                                                       TargetConfig const& target, std::vector<BackendDiagnostic>& diags)
            {
                if (!inst)
                    return true;

                auto set_name = [&](LLVMValueRef v) {
                    if (!inst->name.empty() && v && !LLVMIsConstant(v))
                        LLVMSetValueName2(v, std::string{inst->name}.c_str(), inst->name.size());
                };

                auto lookup = [&](IrValue const* v) -> LLVMValueRef {
                    if (!v)
                        return nullptr;

                    if (auto it = val_map.find(v); it != val_map.end())
                        return it->second;

                    auto* c = c_api_constant(v, ctx, tc, val_map);
                    if (c)
                    {
                        if (tc.indirect(v->type))
                        {
                            c = materialize_indirect_constant(builder, ctx, tc, v->type, c);
                            if (!c)
                                return nullptr;
                        }
                        val_map[v] = c;
                        return c;
                    }

                    return nullptr;
                };

                switch (inst->kind)
                {
                    case IrNodeKind::Alloca: {
                        auto* a = static_cast<IrAllocaInst const*>(inst);
                        auto* at = llvm_mem_type_cached(tc, a->allocated_type);
                        if (!at)
                            return false;

                        LLVMValueRef ai = nullptr;
                        if (a->count)
                        {
                            auto* count_val = lookup(a->count);
                            if (!count_val)
                                return false;
                            ai = LLVMBuildArrayAlloca(builder, at, count_val, "");
                        }
                        else
                            ai = build_frame_slot(builder, at);
                        auto alignment = a->alignment;
                        if (alignment == 0 && tc.contains_byte_storage(a->allocated_type))
                            alignment = static_cast<std::uint32_t>(a->allocated_type->byte_align);
                        if (alignment > 0)
                            LLVMSetAlignment(ai, alignment);

                        set_name(ai);
                        val_map[inst] = ai;
                        break;
                    }
                    case IrNodeKind::Load: {
                        auto* l = static_cast<IrLoadInst const*>(inst);
                        auto* ptr = lookup(l->pointer);
                        if (!ptr)
                        {
                            add_diag(
                                diags, inst->range,
                                std::format("LLVM backend cannot resolve load pointer (IR kind {})", l->pointer ? static_cast<int>(l->pointer->kind) : -1));
                            return false;
                        }

                        LLVMValueRef result = nullptr;
                        if (tc.indirect(l->type))
                        {
                            auto* slot = build_frame_slot(builder, llvm_type_cached(tc, l->type));
                            copy_indirect(builder, ctx, l->type, slot, ptr);
                            result = slot;
                        }
                        else if (is_bool_type(l->type))
                        {
                            auto* raw = LLVMBuildLoad2(builder, LLVMInt8TypeInContext(ctx), ptr, "");
                            result = LLVMBuildTrunc(builder, raw, LLVMInt1TypeInContext(ctx), "");
                        }
                        else
                        {
                            auto* lt = llvm_type_cached(tc, l->type);
                            if (!lt)
                            {
                                add_diag(diags, inst->range, "LLVM backend cannot lower load result type");
                                return false;
                            }

                            result = LLVMBuildLoad2(builder, lt, ptr, "");
                        }

                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::Store: {
                        auto* s = static_cast<IrStoreInst const*>(inst);
                        auto* ptr = lookup(s->pointer);
                        auto* val = lookup(s->value);
                        if (!ptr || !val)
                        {
                            add_diag(diags, inst->range,
                                     std::format("LLVM backend cannot resolve store {} (pointer kind {}, value kind {}, source file {}, offset {})",
                                                 !ptr && !val ? "pointer or value"
                                                 : !ptr       ? "pointer"
                                                              : "value",
                                                 s->pointer ? static_cast<int>(s->pointer->kind) : -1, s->value ? static_cast<int>(s->value->kind) : -1,
                                                 static_cast<std::uint32_t>(inst->range.begin.fileId), inst->range.begin.offset));
                            return false;
                        }

                        if (tc.indirect(s->value->type))
                            copy_indirect(builder, ctx, s->value->type, ptr, val);
                        else if (is_bool_type(s->value->type))
                        {
                            auto* ext = LLVMBuildZExt(builder, val, LLVMInt8TypeInContext(ctx), "");
                            LLVMBuildStore(builder, ext, ptr);
                        }
                        else
                            LLVMBuildStore(builder, val, ptr);

                        break;
                    }
                    case IrNodeKind::LoadVolatile: {
                        auto* l = static_cast<IrLoadVolatileInst const*>(inst);
                        auto* ptr = lookup(l->pointer);
                        if (!ptr)
                            return false;

                        LLVMValueRef result = nullptr;
                        if (is_bool_type(l->type))
                        {
                            auto* raw = LLVMBuildLoad2(builder, LLVMInt8TypeInContext(ctx), ptr, "");
                            LLVMSetVolatile(raw, 1);
                            result = LLVMBuildTrunc(builder, raw, LLVMInt1TypeInContext(ctx), "");
                        }
                        else
                        {
                            auto* lt = llvm_type_cached(tc, l->type);
                            if (!lt)
                                return false;

                            result = LLVMBuildLoad2(builder, lt, ptr, "");
                            LLVMSetVolatile(result, 1);
                        }

                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::StoreVolatile: {
                        auto* s = static_cast<IrStoreVolatileInst const*>(inst);
                        auto* ptr = lookup(s->pointer);
                        auto* val = lookup(s->value);
                        if (!ptr || !val)
                            return false;

                        if (is_bool_type(s->value->type))
                        {
                            auto* ext = LLVMBuildZExt(builder, val, LLVMInt8TypeInContext(ctx), "");
                            auto* store_inst = LLVMBuildStore(builder, ext, ptr);
                            LLVMSetVolatile(store_inst, 1);
                        }
                        else
                        {
                            auto* store_inst = LLVMBuildStore(builder, val, ptr);
                            LLVMSetVolatile(store_inst, 1);
                        }

                        break;
                    }
                    case IrNodeKind::AtomicLoad: {
                        auto* al = static_cast<IrAtomicLoadInst const*>(inst);
                        auto* ptr = lookup(al->pointer);
                        if (!ptr)
                            return false;

                        bool const is_bool = is_bool_type(al->type);
                        auto* lt = is_bool ? LLVMInt8TypeInContext(ctx) : llvm_type_cached(tc, al->type);
                        if (!lt)
                            return false;

                        auto* load_inst = LLVMBuildLoad2(builder, lt, ptr, "");
                        LLVMSetOrdering(load_inst, to_llvm_ordering(al->ordering));
                        LLVMSetAlignment(load_inst, static_cast<unsigned>(al->type->byte_align));
                        auto* result = is_bool ? LLVMBuildTrunc(builder, load_inst, LLVMInt1TypeInContext(ctx), "") : load_inst;
                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::AtomicStore: {
                        auto* as = static_cast<IrAtomicStoreInst const*>(inst);
                        auto* ptr = lookup(as->pointer);
                        auto* val = lookup(as->value);
                        if (!ptr || !val)
                            return false;

                        if (is_bool_type(as->value->type))
                            val = LLVMBuildZExt(builder, val, LLVMInt8TypeInContext(ctx), "");
                        auto* store_inst = LLVMBuildStore(builder, val, ptr);
                        LLVMSetOrdering(store_inst, to_llvm_ordering(as->ordering));
                        LLVMSetAlignment(store_inst, static_cast<unsigned>(as->value->type->byte_align));
                        break;
                    }
                    case IrNodeKind::AtomicRmw: {
                        auto* ar = static_cast<IrAtomicRmwInst const*>(inst);
                        auto* ptr = lookup(ar->pointer);
                        auto* val = lookup(ar->value);
                        if (!ptr || !val)
                            return false;

                        bool const is_bool = is_bool_type(ar->type);
                        if (is_bool)
                            val = LLVMBuildZExt(builder, val, LLVMInt8TypeInContext(ctx), "");
                        auto* rmw_inst = LLVMBuildAtomicRMW(builder, to_llvm_rmw_op(ar->op), ptr, val, to_llvm_ordering(ar->ordering), false);
                        LLVMSetAlignment(rmw_inst, static_cast<unsigned>(ar->type->byte_align));
                        auto* result = is_bool ? LLVMBuildTrunc(builder, rmw_inst, LLVMInt1TypeInContext(ctx), "") : rmw_inst;
                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::AtomicCmpXchg: {
                        auto* cx = static_cast<IrAtomicCmpXchgInst const*>(inst);
                        auto* ptr = lookup(cx->pointer);
                        auto* expected = lookup(cx->expected);
                        auto* desired = lookup(cx->desired);
                        if (!ptr || !expected || !desired)
                            return false;

                        auto* pair = LLVMBuildAtomicCmpXchg(builder, ptr, expected, desired, to_llvm_ordering(cx->success_ordering),
                                                            to_llvm_ordering(cx->failure_ordering), false);
                        LLVMSetAlignment(pair, static_cast<unsigned>(cx->type->byte_align));
                        auto* result = LLVMBuildExtractValue(builder, pair, 0, "");
                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::Fence: {
                        auto* f = static_cast<IrFenceInst const*>(inst);
                        LLVMBuildFence(builder, to_llvm_ordering(f->ordering), false, "");
                        val_map[inst] = nullptr;
                        break;
                    }
                    case IrNodeKind::Gep: {
                        auto* g = static_cast<IrGepInst const*>(inst);

                        if (g->indices.empty())
                        {
                            add_diag(diags, inst->range, "LLVM backend cannot emit GEP with no indices");
                            return false;
                        }

                        auto* base_ptr = lookup(g->base);
                        if (!base_ptr)
                            return false;

                        if (g->base && g->base->type && g->base->type->kind == IrTypeKind::Aggregate)
                        {
                            auto* base_agg = static_cast<IrAggregateType const*>(g->base->type);
                            if (g->indices.size() != 1 || g->indices[0].kind != IrGepInst::IndexKind::Field)
                                return false;
                            auto const field = g->indices[0].field_index;
                            if (field >= base_agg->members.size() || field >= base_agg->member_offsets.size())
                                return false;
                            auto* slot_ty = llvm_type_cached(tc, g->base->type);
                            if (!slot_ty)
                                return false;
                            if (LLVMGetTypeKind(LLVMTypeOf(base_ptr)) != LLVMPointerTypeKind)
                            {
                                auto* slot = build_frame_slot(builder, slot_ty);
                                LLVMBuildStore(builder, base_ptr, slot);
                                base_ptr = slot;
                            }
                            LLVMValueRef field_ptr = nullptr;
                            if (tc.uses_byte_storage(g->base->type))
                            {
                                auto const offset = base_agg->member_offsets[field];
                                field_ptr = base_ptr;
                                if (offset != 0)
                                {
                                    LLVMValueRef offset_value = llvm_const_int(LLVMInt64TypeInContext(ctx), offset, false);
                                    field_ptr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(ctx), base_ptr, &offset_value, 1, "");
                                }
                            }
                            else
                            {
                                auto const llvm_idx = tc.get_llvm_field_index(base_agg, field);
                                field_ptr = LLVMBuildStructGEP2(builder, slot_ty, base_ptr, llvm_idx, "");
                            }
                            set_name(field_ptr);
                            val_map[inst] = field_ptr;
                            break;
                        }

                        IrType const* source_elem = nullptr;
                        if (g->base && g->base->type && g->base->type->kind == IrTypeKind::Pointer)
                        {
                            auto* base_ptr_t = static_cast<IrPointerType const*>(g->base->type);
                            source_elem = base_ptr_t->pointee;
                        }

                        if (!source_elem && g->type && g->type->kind == IrTypeKind::Pointer)
                        {
                            auto* res_ptr_t = static_cast<IrPointerType const*>(g->type);
                            source_elem = res_ptr_t->pointee;
                        }

                        if (!source_elem)
                        {
                            add_diag(diags, inst->range, "LLVM backend cannot emit GEP without element type information");
                            return false;
                        }

                        bool has_byte_storage_field = false;
                        IrType const* path_type = source_elem;
                        for (auto const& ir_idx : g->indices)
                        {
                            if (ir_idx.kind == IrGepInst::IndexKind::Field)
                            {
                                if (path_type && path_type->kind == IrTypeKind::Aggregate)
                                {
                                    auto* aggregate = static_cast<IrAggregateType const*>(path_type);
                                    has_byte_storage_field |= tc.uses_byte_storage(aggregate);
                                    path_type = ir_idx.field_index < aggregate->members.size() ? aggregate->members[ir_idx.field_index] : nullptr;
                                }
                                else
                                    path_type = nullptr;
                            }
                            else if (path_type && path_type->kind == IrTypeKind::Array)
                                path_type = static_cast<IrArrayType const*>(path_type)->element;
                            else if (path_type && path_type->kind == IrTypeKind::Pointer)
                                path_type = static_cast<IrPointerType const*>(path_type)->pointee;
                            else
                                path_type = nullptr;
                        }

                        if (has_byte_storage_field)
                        {
                            auto* current_ptr = base_ptr;
                            bool pointer_changed = false;
                            IrType const* current_type = source_elem;

                            for (auto const& ir_idx : g->indices)
                            {
                                if (ir_idx.kind == IrGepInst::IndexKind::Field)
                                {
                                    if (current_type && current_type->kind == IrTypeKind::Aggregate)
                                    {
                                        auto* aggregate = static_cast<IrAggregateType const*>(current_type);
                                        if (ir_idx.field_index >= aggregate->members.size())
                                            return false;

                                        if (tc.uses_byte_storage(aggregate))
                                        {
                                            auto offset =
                                                ir_idx.field_index < aggregate->member_offsets.size() ? aggregate->member_offsets[ir_idx.field_index] : 0;
                                            if (offset != 0)
                                            {
                                                LLVMValueRef offset_value = llvm_const_int(LLVMInt64TypeInContext(ctx), offset, false);
                                                current_ptr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(ctx), current_ptr, &offset_value, 1, "");
                                                pointer_changed = true;
                                            }
                                        }
                                        else
                                        {
                                            auto* aggregate_type = llvm_mem_type_cached(tc, aggregate);
                                            auto llvm_index = tc.get_llvm_field_index(aggregate, ir_idx.field_index);
                                            current_ptr = LLVMBuildStructGEP2(builder, aggregate_type, current_ptr, llvm_index, "");
                                            pointer_changed = true;
                                        }

                                        current_type = aggregate->members[ir_idx.field_index];
                                    }
                                    else if (current_type && current_type->kind == IrTypeKind::Slice)
                                    {
                                        auto* slice_type = llvm_mem_type_cached(tc, current_type);
                                        current_ptr = LLVMBuildStructGEP2(builder, slice_type, current_ptr, ir_idx.field_index, "");
                                        pointer_changed = true;
                                        current_type = nullptr;
                                    }
                                    else
                                        return false;
                                }
                                else
                                {
                                    auto* index_value = lookup(ir_idx.dynamic_index);
                                    if (!index_value)
                                        return false;

                                    if (current_type && current_type->kind == IrTypeKind::Array)
                                    {
                                        auto* array_type = llvm_mem_type_cached(tc, current_type);
                                        LLVMValueRef indices[] = {
                                            llvm_const_int(LLVMInt32TypeInContext(ctx), 0, false),
                                            index_value,
                                        };
                                        current_ptr = LLVMBuildGEP2(builder, array_type, current_ptr, indices, 2, "");
                                        pointer_changed = true;
                                        current_type = static_cast<IrArrayType const*>(current_type)->element;
                                    }
                                    else
                                    {
                                        auto* element_type = llvm_mem_type_cached(tc, current_type);
                                        if (!element_type)
                                            return false;
                                        current_ptr = LLVMBuildGEP2(builder, element_type, current_ptr, &index_value, 1, "");
                                        pointer_changed = true;
                                        if (current_type && current_type->kind == IrTypeKind::Pointer)
                                            current_type = static_cast<IrPointerType const*>(current_type)->pointee;
                                    }
                                }
                            }

                            if (pointer_changed)
                                set_name(current_ptr);
                            else
                            {
                                bool already_named_by_gep = false;
                                for (auto const& [ir_value, llvm_value] : val_map)
                                    if (llvm_value == current_ptr && ir_value && ir_value->kind == IrNodeKind::Gep)
                                    {
                                        already_named_by_gep = true;
                                        break;
                                    }
                                if (!already_named_by_gep)
                                    set_name(current_ptr);
                            }
                            val_map[inst] = current_ptr;
                            break;
                        }

                        std::vector<LLVMValueRef> llvm_indices;
                        IrType const* current_type = source_elem;

                        if (!g->indices.empty())
                        {
                            auto const& first_idx = g->indices[0];
                            if (first_idx.kind == IrGepInst::IndexKind::Field)
                                llvm_indices.push_back(llvm_const_int(LLVMInt32TypeInContext(ctx), 0, 0));
                            else if (first_idx.kind == IrGepInst::IndexKind::Array && source_elem && source_elem->kind == IrTypeKind::Array)
                                llvm_indices.push_back(llvm_const_int(LLVMInt32TypeInContext(ctx), 0, 0));
                        }

                        for (auto const& ir_idx : g->indices)
                        {
                            if (ir_idx.kind == IrGepInst::IndexKind::Field)
                            {
                                unsigned llvm_field_idx = ir_idx.field_index;
                                if (current_type && current_type->kind == IrTypeKind::Aggregate)
                                    llvm_field_idx = tc.get_llvm_field_index(static_cast<IrAggregateType const*>(current_type), ir_idx.field_index);

                                llvm_indices.push_back(llvm_const_int(LLVMInt32TypeInContext(ctx), llvm_field_idx, 0));

                                if (current_type && current_type->kind == IrTypeKind::Aggregate)
                                {
                                    auto* agg = static_cast<IrAggregateType const*>(current_type);
                                    if (ir_idx.field_index < agg->members.size())
                                        current_type = agg->members[ir_idx.field_index];
                                    else
                                        current_type = nullptr;
                                }
                                else if (current_type && current_type->kind == IrTypeKind::Slice)
                                    current_type = nullptr;
                                else
                                    current_type = nullptr;
                            }
                            else
                            {
                                auto* idx_val = lookup(ir_idx.dynamic_index);
                                if (!idx_val)
                                    return false;

                                llvm_indices.push_back(idx_val);

                                if (current_type && current_type->kind == IrTypeKind::Array)
                                {
                                    auto* arr = static_cast<IrArrayType const*>(current_type);
                                    current_type = arr->element;
                                }
                                else if (current_type && current_type->kind == IrTypeKind::Pointer)
                                {
                                    auto* ptr_t = static_cast<IrPointerType const*>(current_type);
                                    current_type = ptr_t->pointee;
                                }
                                else
                                    current_type = nullptr;
                            }
                        }

                        auto* gep_source_ty = llvm_mem_type_cached(tc, source_elem);
                        if (!gep_source_ty)
                            return false;

                        auto* gep_res = LLVMBuildGEP2(builder, gep_source_ty, base_ptr, llvm_indices.data(), static_cast<unsigned>(llvm_indices.size()), "");
                        set_name(gep_res);
                        val_map[inst] = gep_res;
                        break;
                    }
                    case IrNodeKind::Add: {
                        auto* a = static_cast<IrAddInst const*>(inst);
                        auto* lhs = lookup(a->lhs);
                        auto* rhs = lookup(a->rhs);
                        if (!lhs || !rhs)
                            return false;

                        LLVMValueRef r = nullptr;
                        if (a->type && a->type->kind == IrTypeKind::Float)
                            r = LLVMBuildFAdd(builder, lhs, rhs, "");
                        else
                            r = LLVMBuildAdd(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Sub: {
                        auto* s = static_cast<IrSubInst const*>(inst);
                        auto* lhs = lookup(s->lhs);
                        auto* rhs = lookup(s->rhs);
                        if (!lhs || !rhs)
                            return false;

                        LLVMValueRef r = nullptr;
                        if (s->type && s->type->kind == IrTypeKind::Float)
                            r = LLVMBuildFSub(builder, lhs, rhs, "");
                        else
                            r = LLVMBuildSub(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Mul: {
                        auto* m = static_cast<IrMulInst const*>(inst);
                        auto* lhs = lookup(m->lhs);
                        auto* rhs = lookup(m->rhs);
                        if (!lhs || !rhs)
                            return false;

                        LLVMValueRef r = nullptr;
                        if (m->type && m->type->kind == IrTypeKind::Float)
                            r = LLVMBuildFMul(builder, lhs, rhs, "");
                        else
                            r = LLVMBuildMul(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::SDiv: {
                        auto* s = static_cast<IrSDivInst const*>(inst);
                        auto* lhs = lookup(s->lhs);
                        auto* rhs = lookup(s->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildSDiv(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::UDiv: {
                        auto* u = static_cast<IrUDivInst const*>(inst);
                        auto* lhs = lookup(u->lhs);
                        auto* rhs = lookup(u->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildUDiv(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::SRem: {
                        auto* s = static_cast<IrSRemInst const*>(inst);
                        auto* lhs = lookup(s->lhs);
                        auto* rhs = lookup(s->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildSRem(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::URem: {
                        auto* u = static_cast<IrURemInst const*>(inst);
                        auto* lhs = lookup(u->lhs);
                        auto* rhs = lookup(u->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildURem(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::FDiv: {
                        auto* f = static_cast<IrFDivInst const*>(inst);
                        auto* lhs = lookup(f->lhs);
                        auto* rhs = lookup(f->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildFDiv(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::FRem: {
                        auto* f = static_cast<IrFRemInst const*>(inst);
                        auto* lhs = lookup(f->lhs);
                        auto* rhs = lookup(f->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildFRem(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::And: {
                        auto* a = static_cast<IrAndInst const*>(inst);
                        auto* lhs = lookup(a->lhs);
                        auto* rhs = lookup(a->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildAnd(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Or: {
                        auto* o = static_cast<IrOrInst const*>(inst);
                        auto* lhs = lookup(o->lhs);
                        auto* rhs = lookup(o->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildOr(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Xor: {
                        auto* x = static_cast<IrXorInst const*>(inst);
                        auto* lhs = lookup(x->lhs);
                        auto* rhs = lookup(x->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildXor(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Shl: {
                        auto* s = static_cast<IrShlInst const*>(inst);
                        auto* lhs = lookup(s->lhs);
                        auto* rhs = lookup(s->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildShl(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::LShr: {
                        auto* l = static_cast<IrLShrInst const*>(inst);
                        auto* lhs = lookup(l->lhs);
                        auto* rhs = lookup(l->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildLShr(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::AShr: {
                        auto* a = static_cast<IrAShrInst const*>(inst);
                        auto* lhs = lookup(a->lhs);
                        auto* rhs = lookup(a->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildAShr(builder, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpEq: {
                        auto* c = static_cast<IrCmpEqInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        LLVMValueRef r = nullptr;
                        if (c->lhs->type && c->lhs->type->kind == IrTypeKind::Float)
                            r = LLVMBuildFCmp(builder, LLVMRealOEQ, lhs, rhs, "");
                        else
                            r = LLVMBuildICmp(builder, LLVMIntEQ, lhs, rhs, "");

                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpNe: {
                        auto* c = static_cast<IrCmpNeInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        LLVMValueRef r = nullptr;
                        if (c->lhs->type && c->lhs->type->kind == IrTypeKind::Float)
                            r = LLVMBuildFCmp(builder, LLVMRealONE, lhs, rhs, "");
                        else
                            r = LLVMBuildICmp(builder, LLVMIntNE, lhs, rhs, "");

                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpLt: {
                        auto* c = static_cast<IrCmpLtInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildICmp(builder, LLVMIntSLT, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpLe: {
                        auto* c = static_cast<IrCmpLeInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildICmp(builder, LLVMIntSLE, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpGt: {
                        auto* c = static_cast<IrCmpGtInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildICmp(builder, LLVMIntSGT, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpGe: {
                        auto* c = static_cast<IrCmpGeInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildICmp(builder, LLVMIntSGE, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpULt: {
                        auto* c = static_cast<IrCmpULtInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildICmp(builder, LLVMIntULT, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpULe: {
                        auto* c = static_cast<IrCmpULeInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildICmp(builder, LLVMIntULE, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpUGt: {
                        auto* c = static_cast<IrCmpUGtInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildICmp(builder, LLVMIntUGT, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpUGe: {
                        auto* c = static_cast<IrCmpUGeInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildICmp(builder, LLVMIntUGE, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpOLt: {
                        auto* c = static_cast<IrCmpOLtInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildFCmp(builder, LLVMRealOLT, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpOLe: {
                        auto* c = static_cast<IrCmpOLeInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildFCmp(builder, LLVMRealOLE, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpOGt: {
                        auto* c = static_cast<IrCmpOGtInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildFCmp(builder, LLVMRealOGT, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::CmpOGe: {
                        auto* c = static_cast<IrCmpOGeInst const*>(inst);
                        auto* lhs = lookup(c->lhs);
                        auto* rhs = lookup(c->rhs);
                        if (!lhs || !rhs)
                            return false;

                        auto* r = LLVMBuildFCmp(builder, LLVMRealOGE, lhs, rhs, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Zext: {
                        auto* z = static_cast<IrZextInst const*>(inst);
                        auto* op = lookup(z->operand);
                        auto* dst_ty = llvm_type_cached(tc, z->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildZExt(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Sext: {
                        auto* s = static_cast<IrSextInst const*>(inst);
                        auto* op = lookup(s->operand);
                        auto* dst_ty = llvm_type_cached(tc, s->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildSExt(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Trunc: {
                        auto* t = static_cast<IrTruncInst const*>(inst);
                        auto* op = lookup(t->operand);
                        auto* dst_ty = llvm_type_cached(tc, t->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildTrunc(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::FpExt: {
                        auto* f = static_cast<IrFpExtInst const*>(inst);
                        auto* op = lookup(f->operand);
                        auto* dst_ty = llvm_type_cached(tc, f->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildFPExt(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::FpTrunc: {
                        auto* f = static_cast<IrFpTruncInst const*>(inst);
                        auto* op = lookup(f->operand);
                        auto* dst_ty = llvm_type_cached(tc, f->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildFPTrunc(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::FpToI: {
                        auto* f = static_cast<IrFpToIInst const*>(inst);
                        auto* op = lookup(f->operand);
                        auto* dst_ty = llvm_type_cached(tc, f->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildFPToSI(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::IToFp: {
                        auto* it = static_cast<IrIToFpInst const*>(inst);
                        auto* op = lookup(it->operand);
                        auto* dst_ty = llvm_type_cached(tc, it->type);
                        if (!op || !dst_ty)
                            return false;

                        bool is_unsigned = false;
                        if (it->operand->type && it->operand->type->kind == IrTypeKind::Int)
                            is_unsigned = !static_cast<IrIntType const*>(it->operand->type)->is_signed;

                        LLVMValueRef r = nullptr;
                        if (is_unsigned)
                            r = LLVMBuildUIToFP(builder, op, dst_ty, "");
                        else
                            r = LLVMBuildSIToFP(builder, op, dst_ty, "");

                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::PtrToI: {
                        auto* p = static_cast<IrPtrToIInst const*>(inst);
                        auto* op = lookup(p->operand);
                        auto* dst_ty = llvm_type_cached(tc, p->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildPtrToInt(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::IToPtr: {
                        auto* it = static_cast<IrIToPtrInst const*>(inst);
                        auto* op = lookup(it->operand);
                        auto* dst_ty = llvm_type_cached(tc, it->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildIntToPtr(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::MakePointer: {
                        auto* p = static_cast<IrMakePointerInst const*>(inst);
                        if (p->segment)
                            return false;
                        auto* offset = lookup(p->offset);
                        auto* dst_ty = llvm_type_cached(tc, p->type);
                        if (!offset || !dst_ty)
                            return false;
                        auto* r = LLVMBuildIntToPtr(builder, offset, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::PointerOffset: {
                        auto* p = static_cast<IrPointerOffsetInst const*>(inst);
                        auto* pointer = lookup(p->pointer);
                        auto* dst_ty = llvm_type_cached(tc, p->type);
                        if (!pointer || !dst_ty)
                            return false;
                        auto* r = LLVMBuildPtrToInt(builder, pointer, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::ReadSegment:
                    case IrNodeKind::PointerSegment:
                        return false;
                    case IrNodeKind::Bitcast: {
                        auto* b = static_cast<IrBitcastInst const*>(inst);
                        auto* op = lookup(b->operand);
                        auto* dst_ty = llvm_type_cached(tc, b->type);
                        if (!op || !dst_ty)
                            return false;

                        auto* r = LLVMBuildBitCast(builder, op, dst_ty, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Phi: {
                        auto* p = static_cast<IrPhiInst const*>(inst);
                        auto* phi_ty = tc.indirect(p->type) ? LLVMPointerTypeInContext(ctx, 0) : llvm_type_cached(tc, p->type);
                        if (!phi_ty)
                            return false;

                        auto* phi = LLVMBuildPhi(builder, phi_ty, "");
                        set_name(phi);
                        val_map[inst] = phi;
                        break;
                    }
                    case IrNodeKind::Neg: {
                        auto* n = static_cast<IrNegInst const*>(inst);
                        auto* op = lookup(n->operand);
                        if (!op)
                            return false;

                        LLVMValueRef r = nullptr;
                        if (n->type && n->type->kind == IrTypeKind::Float)
                            r = LLVMBuildFNeg(builder, op, "");
                        else
                            r = LLVMBuildNeg(builder, op, "");

                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Not: {
                        auto* n = static_cast<IrNotInst const*>(inst);
                        auto* op = lookup(n->operand);
                        if (!op)
                            return false;

                        auto* r = LLVMBuildNot(builder, op, "");
                        set_name(r);
                        val_map[inst] = r;
                        break;
                    }
                    case IrNodeKind::Call: {
                        auto* c = static_cast<IrCallInst const*>(inst);
                        auto* callee = lookup(c->callee);
                        if (!callee)
                            return false;

                        std::vector<IrType const*> arg_types;
                        arg_types.reserve(c->args.size());
                        for (auto* a : c->args)
                            arg_types.push_back(a->type);
                        auto const* callee_ref = ir_cast<IrGlobalRef>(c->callee);
                        auto const sig = compute_signature_abi(tc, c->type, arg_types, aggregate_abi_for(tc, callee_ref ? callee_ref->function : nullptr));

                        std::vector<LLVMValueRef> args;
                        bool const sret = sig.sret;
                        LLVMValueRef result_slot = nullptr;
                        if (sret)
                        {
                            result_slot = build_frame_slot(builder, llvm_type_cached(tc, c->type));
                            args.push_back(result_slot);
                        }
                        else if (sig.sret_boundary)
                        {
                            result_slot = abi_spill_slot(builder, tc, c->type);
                            args.push_back(result_slot);
                        }
                        for (std::size_t ai = 0; ai < c->args.size(); ++ai)
                        {
                            auto* av = lookup(c->args[ai]);
                            if (!av)
                                return false;

                            auto const& abi = sig.params[ai];
                            if (abi.kind == ArgAbi::Kind::Coerce)
                            {
                                auto parts = abi_split(builder, tc, c->args[ai]->type, av, abi.pieces);
                                args.insert(args.end(), parts.begin(), parts.end());
                            }
                            else if (abi.kind == ArgAbi::Kind::Memory)
                                args.push_back(abi_memory_copy(builder, tc, c->args[ai]->type, av));
                            else
                                args.push_back(av);
                        }

                        if (auto* gref = ir_cast<IrGlobalRef>(c->callee))
                        {
                            if (gref->function)
                            {
                                auto* llvm_func_val = lookup(gref->function);
                                if (llvm_func_val)
                                {
                                    auto* declared_func_ty = LLVMGlobalGetValueType(llvm_func_val);
                                    if (declared_func_ty && LLVMGetTypeKind(declared_func_ty) == LLVMFunctionTypeKind)
                                    {
                                        auto declared_param_count = LLVMCountParamTypes(declared_func_ty);
                                        if (declared_param_count != static_cast<unsigned>(args.size()))
                                        {
                                            add_diag(diags, inst->range,
                                                     std::format("LLVM backend: call to '{}' has {} args but function declares {} params", gref->function->name,
                                                                 args.size(), declared_param_count));
                                            return false;
                                        }

                                        std::vector<LLVMTypeRef> declared_params(declared_param_count);
                                        LLVMGetParamTypes(declared_func_ty, declared_params.data());

                                        for (unsigned pi = 0; pi < declared_param_count; ++pi)
                                        {
                                            auto* actual_type = LLVMTypeOf(args[pi]);
                                            if (actual_type != declared_params[pi])
                                            {
                                                add_diag(diags, inst->range,
                                                         std::format("LLVM backend: call to '{}' arg {} type mismatch: actual type != declared param type",
                                                                     gref->function->name, pi));
                                                return false;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        auto* call_inst = LLVMBuildCall2(builder, sig.fn_type, callee, args.data(), static_cast<unsigned>(args.size()), "");
                        auto add_indirect_call_attr = [&](unsigned index, IrType const* type, const char* name) {
                            auto kind = LLVMGetEnumAttributeKindForName(name, static_cast<unsigned>(std::strlen(name)));
                            if (kind != 0)
                                LLVMAddCallSiteAttribute(call_inst, index, LLVMCreateTypeAttribute(ctx, kind, llvm_type_cached(tc, type)));
                        };
                        if (sret || sig.sret_boundary)
                            add_indirect_call_attr(1, c->type, "sret");
                        for (std::size_t i = 0; i < c->args.size(); ++i)
                            if (sig.params[i].kind == ArgAbi::Kind::Indirect || sig.params[i].kind == ArgAbi::Kind::Memory)
                                add_indirect_call_attr(sig.params[i].first_param + 1, c->args[i]->type, "byval");

                        bool call_cc_error = false;
                        auto cc_opt = get_calling_conv_for_call(c->callee, target, diags, call_cc_error);
                        if (call_cc_error)
                            return false;
                        if (cc_opt)
                            LLVMSetInstructionCallConv(call_inst, *cc_opt);

                        if (c->is_noinline)
                        {
                            auto kind = LLVMGetEnumAttributeKindForName("noinline", 8);
                            if (kind != 0)
                            {
                                auto* attr = LLVMCreateEnumAttribute(ctx, kind, 0);
                                LLVMAddCallSiteAttribute(call_inst, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), attr);
                            }
                        }

                        LLVMValueRef result = sret ? result_slot : call_inst;
                        if (sig.sret_boundary)
                            result = LLVMBuildLoad2(builder, llvm_type_cached(tc, c->type), result_slot, "");
                        else if (sig.ret.kind == ArgAbi::Kind::Coerce)
                        {
                            std::vector<LLVMValueRef> parts;
                            if (sig.ret.pieces.size() == 1)
                                parts.push_back(call_inst);
                            else
                                for (std::size_t k = 0; k < sig.ret.pieces.size(); ++k)
                                    parts.push_back(LLVMBuildExtractValue(builder, call_inst, static_cast<unsigned>(k), ""));
                            result = abi_join(builder, tc, c->type, parts);
                        }
                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::CallTail: {
                        auto* c = static_cast<IrCallTailInst const*>(inst);
                        auto* callee = lookup(c->callee);
                        if (!callee)
                            return false;

                        std::vector<IrType const*> arg_types;
                        arg_types.reserve(c->args.size());
                        for (auto* a : c->args)
                            arg_types.push_back(a->type);
                        auto const* callee_ref = ir_cast<IrGlobalRef>(c->callee);
                        auto const sig = compute_signature_abi(tc, c->type, arg_types, aggregate_abi_for(tc, callee_ref ? callee_ref->function : nullptr));

                        std::vector<LLVMValueRef> args;
                        bool const sret = sig.sret;
                        LLVMValueRef result_slot = nullptr;
                        if (sret)
                        {
                            result_slot = build_frame_slot(builder, llvm_type_cached(tc, c->type));
                            args.push_back(result_slot);
                        }
                        else if (sig.sret_boundary)
                        {
                            result_slot = abi_spill_slot(builder, tc, c->type);
                            args.push_back(result_slot);
                        }
                        for (std::size_t ai = 0; ai < c->args.size(); ++ai)
                        {
                            auto* av = lookup(c->args[ai]);
                            if (!av)
                                return false;

                            auto const& abi = sig.params[ai];
                            if (abi.kind == ArgAbi::Kind::Coerce)
                            {
                                auto parts = abi_split(builder, tc, c->args[ai]->type, av, abi.pieces);
                                args.insert(args.end(), parts.begin(), parts.end());
                            }
                            else if (abi.kind == ArgAbi::Kind::Memory)
                                args.push_back(abi_memory_copy(builder, tc, c->args[ai]->type, av));
                            else
                                args.push_back(av);
                        }

                        if (auto* gref = ir_cast<IrGlobalRef>(c->callee))
                        {
                            if (gref->function)
                            {
                                auto* llvm_func_val = lookup(gref->function);
                                if (llvm_func_val)
                                {
                                    auto* declared_func_ty = LLVMGlobalGetValueType(llvm_func_val);
                                    if (declared_func_ty && LLVMGetTypeKind(declared_func_ty) == LLVMFunctionTypeKind)
                                    {
                                        auto declared_param_count = LLVMCountParamTypes(declared_func_ty);
                                        if (declared_param_count != static_cast<unsigned>(args.size()))
                                        {
                                            add_diag(diags, inst->range,
                                                     std::format("LLVM backend: tail call to '{}' has {} args but function declares {} params",
                                                                 gref->function->name, args.size(), declared_param_count));
                                            return false;
                                        }

                                        std::vector<LLVMTypeRef> declared_params(declared_param_count);
                                        LLVMGetParamTypes(declared_func_ty, declared_params.data());

                                        for (unsigned pi = 0; pi < declared_param_count; ++pi)
                                        {
                                            auto* actual_type = LLVMTypeOf(args[pi]);
                                            if (actual_type != declared_params[pi])
                                            {
                                                add_diag(diags, inst->range,
                                                         std::format("LLVM backend: tail call to '{}' arg {} type mismatch", gref->function->name, pi));
                                                return false;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        auto* call_inst = LLVMBuildCall2(builder, sig.fn_type, callee, args.data(), static_cast<unsigned>(args.size()), "");
                        if (sret || sig.sret_boundary)
                        {
                            auto kind = LLVMGetEnumAttributeKindForName("sret", 4);
                            if (kind != 0)
                                LLVMAddCallSiteAttribute(call_inst, 1, LLVMCreateTypeAttribute(ctx, kind, llvm_type_cached(tc, c->type)));
                        }
                        for (std::size_t i = 0; i < c->args.size(); ++i)
                            if (sig.params[i].kind == ArgAbi::Kind::Indirect || sig.params[i].kind == ArgAbi::Kind::Memory)
                            {
                                auto kind = LLVMGetEnumAttributeKindForName("byval", 5);
                                if (kind != 0)
                                    LLVMAddCallSiteAttribute(call_inst, sig.params[i].first_param + 1,
                                                             LLVMCreateTypeAttribute(ctx, kind, llvm_type_cached(tc, c->args[i]->type)));
                            }

                        bool call_cc_error = false;
                        auto cc_opt = get_calling_conv_for_call(c->callee, target, diags, call_cc_error);
                        if (call_cc_error)
                            return false;
                        if (cc_opt)
                            LLVMSetInstructionCallConv(call_inst, *cc_opt);

                        if (c->is_noinline)
                        {
                            auto kind = LLVMGetEnumAttributeKindForName("noinline", 8);
                            if (kind != 0)
                            {
                                auto* attr = LLVMCreateEnumAttribute(ctx, kind, 0);
                                LLVMAddCallSiteAttribute(call_inst, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), attr);
                            }
                        }

                        if (!sret && !sig.rewrites() && std::ranges::none_of(c->args, [&](auto* a) { return tc.indirect(a->type); }))
                            LLVMSetTailCallKind(call_inst, LLVMTailCallKindMustTail);
                        LLVMValueRef result = sret ? result_slot : call_inst;
                        if (sig.sret_boundary)
                            result = LLVMBuildLoad2(builder, llvm_type_cached(tc, c->type), result_slot, "");
                        else if (sig.ret.kind == ArgAbi::Kind::Coerce)
                        {
                            std::vector<LLVMValueRef> parts;
                            if (sig.ret.pieces.size() == 1)
                                parts.push_back(call_inst);
                            else
                                for (std::size_t k = 0; k < sig.ret.pieces.size(); ++k)
                                    parts.push_back(LLVMBuildExtractValue(builder, call_inst, static_cast<unsigned>(k), ""));
                            result = abi_join(builder, tc, c->type, parts);
                        }
                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::Aggregate: {
                        auto* agg = static_cast<IrAggregateInst const*>(inst);
                        auto* agg_ty = llvm_type_cached(tc, agg->type);
                        if (!agg_ty)
                            return false;

                        auto* const_val = c_api_constant(inst, ctx, tc, val_map);
                        if (const_val)
                        {
                            if (tc.indirect(agg->type))
                            {
                                const_val = materialize_indirect_constant(builder, ctx, tc, agg->type, const_val);
                                if (!const_val)
                                    return false;
                            }
                            set_name(const_val);
                            val_map[inst] = const_val;
                            break;
                        }

                        if (tc.indirect(agg->type))
                        {
                            auto* storage = build_frame_slot(builder, agg_ty);
                            zero_indirect(builder, ctx, agg->type, storage);
                            for (std::size_t i = 0; i < agg->values.size(); ++i)
                            {
                                auto* member_ir = agg->values[i];
                                if (!member_ir)
                                    continue;
                                auto* member = lookup(member_ir);
                                if (!member)
                                    return false;
                                LLVMValueRef member_ptr = storage;
                                if (agg->type->kind == IrTypeKind::Aggregate && tc.uses_byte_storage(agg->type))
                                {
                                    auto* at = static_cast<IrAggregateType const*>(agg->type);
                                    if (i >= at->member_offsets.size())
                                        return false;
                                    auto* offset = llvm_const_int(LLVMInt64TypeInContext(ctx), at->member_offsets[i], false);
                                    member_ptr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(ctx), storage, &offset, 1, "");
                                }
                                else
                                {
                                    auto* zero = llvm_const_int(LLVMInt32TypeInContext(ctx), 0, false);
                                    auto* index =
                                        llvm_const_int(LLVMInt32TypeInContext(ctx),
                                                     agg->type->kind == IrTypeKind::Aggregate
                                                         ? tc.get_llvm_field_index(static_cast<IrAggregateType const*>(agg->type), static_cast<unsigned>(i))
                                                         : static_cast<unsigned>(i),
                                                     false);
                                    LLVMValueRef indices[] = {zero, index};
                                    member_ptr = LLVMBuildGEP2(builder, agg_ty, storage, indices, 2, "");
                                }
                                if (tc.indirect(member_ir->type))
                                    copy_indirect(builder, ctx, member_ir->type, member_ptr, member);
                                else
                                {
                                    if (is_bool_type(member_ir->type))
                                        member = LLVMBuildZExt(builder, member, LLVMInt8TypeInContext(ctx), "");
                                    LLVMBuildStore(builder, member, member_ptr);
                                }
                            }
                            set_name(storage);
                            val_map[inst] = storage;
                            break;
                        }

                        LLVMValueRef result = nullptr;

                        if (tc.uses_byte_storage(agg->type))
                        {
                            auto* aggregate_type = static_cast<IrAggregateType const*>(agg->type);
                            auto* storage = build_frame_slot(builder, agg_ty);
                            LLVMSetAlignment(storage, static_cast<unsigned>(aggregate_type->byte_align));
                            LLVMBuildStore(builder, LLVMConstNull(agg_ty), storage);

                            auto count = std::min(agg->values.size(), aggregate_type->members.size());
                            for (std::size_t i = 0; i < count; ++i)
                            {
                                if (!agg->values[i])
                                    continue;
                                auto* member = lookup(agg->values[i]);
                                if (!member)
                                    return false;
                                if (tc.indirect(agg->values[i]->type))
                                    member = LLVMBuildLoad2(builder, llvm_type_cached(tc, agg->values[i]->type), member, "");
                                auto offset = i < aggregate_type->member_offsets.size() ? aggregate_type->member_offsets[i] : 0;
                                auto* member_ptr = storage;
                                if (offset != 0)
                                {
                                    LLVMValueRef offset_value = llvm_const_int(LLVMInt64TypeInContext(ctx), offset, false);
                                    member_ptr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(ctx), storage, &offset_value, 1, "");
                                }
                                if (is_bool_type(aggregate_type->members[i]))
                                    member = LLVMBuildZExt(builder, member, LLVMInt8TypeInContext(ctx), "");
                                LLVMBuildStore(builder, member, member_ptr);
                            }

                            result = LLVMBuildLoad2(builder, agg_ty, storage, "");
                            set_name(result);
                            val_map[inst] = result;
                            break;
                        }

                        result = LLVMGetUndef(agg_ty);
                        if (agg->type && agg->type->kind == IrTypeKind::Aggregate)
                        {
                            auto* ir_agg_type = static_cast<IrAggregateType const*>(agg->type);
                            for (std::size_t i = 0; i < agg->values.size(); ++i)
                            {
                                if (!agg->values[i])
                                    continue;
                                auto* mv = lookup(agg->values[i]);
                                if (!mv)
                                    return false;
                                if (tc.indirect(agg->values[i]->type))
                                    mv = LLVMBuildLoad2(builder, llvm_type_cached(tc, agg->values[i]->type), mv, "");

                                if (is_bool_type(agg->values[i]->type))
                                    mv = LLVMBuildZExt(builder, mv, LLVMInt8TypeInContext(ctx), "");

                                unsigned llvm_idx = tc.get_llvm_field_index(ir_agg_type, static_cast<unsigned>(i));
                                result = LLVMBuildInsertValue(builder, result, mv, llvm_idx, "");
                            }
                        }
                        else
                        {
                            for (std::size_t i = 0; i < agg->values.size(); ++i)
                            {
                                if (!agg->values[i])
                                    continue;
                                auto* mv = lookup(agg->values[i]);
                                if (!mv)
                                    return false;
                                if (tc.indirect(agg->values[i]->type))
                                    mv = LLVMBuildLoad2(builder, llvm_type_cached(tc, agg->values[i]->type), mv, "");
                                if (is_bool_type(agg->values[i]->type))
                                    mv = LLVMBuildZExt(builder, mv, LLVMInt8TypeInContext(ctx), "");
                                result = LLVMBuildInsertValue(builder, result, mv, static_cast<unsigned>(i), "");
                            }
                        }

                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::Extract: {
                        auto* e = static_cast<IrExtractInst const*>(inst);
                        auto* agg_val = lookup(e->aggregate);
                        if (!agg_val)
                            return false;

                        if (tc.indirect(e->aggregate->type))
                        {
                            auto* parent_ty = llvm_type_cached(tc, e->aggregate->type);
                            LLVMValueRef field_ptr = agg_val;
                            if (e->aggregate->type->kind == IrTypeKind::Aggregate && tc.uses_byte_storage(e->aggregate->type))
                            {
                                auto* at = static_cast<IrAggregateType const*>(e->aggregate->type);
                                if (e->field_index >= at->member_offsets.size())
                                    return false;
                                auto* offset = llvm_const_int(LLVMInt64TypeInContext(ctx), at->member_offsets[e->field_index], false);
                                field_ptr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(ctx), agg_val, &offset, 1, "");
                            }
                            else
                            {
                                auto* zero = llvm_const_int(LLVMInt32TypeInContext(ctx), 0, false);
                                auto* index =
                                    llvm_const_int(LLVMInt32TypeInContext(ctx),
                                                 e->aggregate->type->kind == IrTypeKind::Aggregate
                                                     ? tc.get_llvm_field_index(static_cast<IrAggregateType const*>(e->aggregate->type), e->field_index)
                                                     : e->field_index,
                                                 false);
                                LLVMValueRef indices[] = {zero, index};
                                field_ptr = LLVMBuildGEP2(builder, parent_ty, agg_val, indices, 2, "");
                            }
                            LLVMValueRef result = nullptr;
                            if (tc.indirect(e->type))
                            {
                                result = build_frame_slot(builder, llvm_type_cached(tc, e->type));
                                copy_indirect(builder, ctx, e->type, result, field_ptr);
                            }
                            else if (is_bool_type(e->type))
                            {
                                auto* raw = LLVMBuildLoad2(builder, LLVMInt8TypeInContext(ctx), field_ptr, "");
                                result = LLVMBuildTrunc(builder, raw, LLVMInt1TypeInContext(ctx), "");
                            }
                            else
                                result = LLVMBuildLoad2(builder, llvm_type_cached(tc, e->type), field_ptr, "");
                            set_name(result);
                            val_map[inst] = result;
                            break;
                        }

                        if (e->aggregate && tc.uses_byte_storage(e->aggregate->type))
                        {
                            auto* aggregate_type = static_cast<IrAggregateType const*>(e->aggregate->type);
                            if (e->field_index >= aggregate_type->members.size())
                                return false;
                            auto* aggregate_llvm_type = llvm_type_cached(tc, aggregate_type);
                            auto* storage = build_frame_slot(builder, aggregate_llvm_type);
                            LLVMSetAlignment(storage, static_cast<unsigned>(aggregate_type->byte_align));
                            LLVMBuildStore(builder, agg_val, storage);
                            auto offset = e->field_index < aggregate_type->member_offsets.size() ? aggregate_type->member_offsets[e->field_index] : 0;
                            auto* member_ptr = storage;
                            if (offset != 0)
                            {
                                LLVMValueRef offset_value = llvm_const_int(LLVMInt64TypeInContext(ctx), offset, false);
                                member_ptr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(ctx), storage, &offset_value, 1, "");
                            }
                            auto* member_type = aggregate_type->members[e->field_index];
                            LLVMValueRef result = nullptr;
                            if (is_bool_type(member_type))
                            {
                                auto* raw = LLVMBuildLoad2(builder, LLVMInt8TypeInContext(ctx), member_ptr, "");
                                result = LLVMBuildTrunc(builder, raw, LLVMInt1TypeInContext(ctx), "");
                            }
                            else if (tc.indirect(e->type))
                            {
                                result = build_frame_slot(builder, llvm_type_cached(tc, e->type));
                                copy_indirect(builder, ctx, e->type, result, member_ptr);
                            }
                            else
                                result = LLVMBuildLoad2(builder, llvm_type_cached(tc, member_type), member_ptr, "");
                            set_name(result);
                            val_map[inst] = result;
                            break;
                        }

                        unsigned llvm_field_idx = e->field_index;
                        if (e->aggregate && e->aggregate->type && e->aggregate->type->kind == IrTypeKind::Aggregate)
                            llvm_field_idx = tc.get_llvm_field_index(static_cast<IrAggregateType const*>(e->aggregate->type), e->field_index);

                        auto* result = LLVMBuildExtractValue(builder, agg_val, llvm_field_idx, "");
                        if (tc.indirect(e->type))
                        {
                            auto* slot = build_frame_slot(builder, llvm_type_cached(tc, e->type));
                            LLVMBuildStore(builder, result, slot);
                            result = slot;
                        }
                        if (is_bool_type(e->type))
                            result = LLVMBuildTrunc(builder, result, LLVMInt1TypeInContext(ctx), "");
                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::Insert: {
                        auto* ins = static_cast<IrInsertInst const*>(inst);
                        auto* agg_val = lookup(ins->aggregate);
                        auto* val = lookup(ins->value);
                        if (!agg_val || !val)
                            return false;

                        if (tc.indirect(ins->aggregate->type))
                        {
                            auto* parent_ty = llvm_type_cached(tc, ins->aggregate->type);
                            auto* storage = build_frame_slot(builder, parent_ty);
                            copy_indirect(builder, ctx, ins->aggregate->type, storage, agg_val);
                            LLVMValueRef field_ptr = storage;
                            if (ins->aggregate->type->kind == IrTypeKind::Aggregate && tc.uses_byte_storage(ins->aggregate->type))
                            {
                                auto* at = static_cast<IrAggregateType const*>(ins->aggregate->type);
                                if (ins->field_index >= at->member_offsets.size())
                                    return false;
                                auto* offset = llvm_const_int(LLVMInt64TypeInContext(ctx), at->member_offsets[ins->field_index], false);
                                field_ptr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(ctx), storage, &offset, 1, "");
                            }
                            else
                            {
                                auto* zero = llvm_const_int(LLVMInt32TypeInContext(ctx), 0, false);
                                auto* index =
                                    llvm_const_int(LLVMInt32TypeInContext(ctx),
                                                 ins->aggregate->type->kind == IrTypeKind::Aggregate
                                                     ? tc.get_llvm_field_index(static_cast<IrAggregateType const*>(ins->aggregate->type), ins->field_index)
                                                     : ins->field_index,
                                                 false);
                                LLVMValueRef indices[] = {zero, index};
                                field_ptr = LLVMBuildGEP2(builder, parent_ty, storage, indices, 2, "");
                            }
                            if (tc.indirect(ins->value->type))
                                copy_indirect(builder, ctx, ins->value->type, field_ptr, val);
                            else
                            {
                                if (is_bool_type(ins->value->type))
                                    val = LLVMBuildZExt(builder, val, LLVMInt8TypeInContext(ctx), "");
                                LLVMBuildStore(builder, val, field_ptr);
                            }
                            set_name(storage);
                            val_map[inst] = storage;
                            break;
                        }

                        if (ins->aggregate && tc.uses_byte_storage(ins->aggregate->type))
                        {
                            auto* aggregate_type = static_cast<IrAggregateType const*>(ins->aggregate->type);
                            if (ins->field_index >= aggregate_type->members.size())
                                return false;
                            auto* aggregate_llvm_type = llvm_type_cached(tc, aggregate_type);
                            auto* storage = build_frame_slot(builder, aggregate_llvm_type);
                            LLVMSetAlignment(storage, static_cast<unsigned>(aggregate_type->byte_align));
                            LLVMBuildStore(builder, agg_val, storage);
                            auto offset = ins->field_index < aggregate_type->member_offsets.size() ? aggregate_type->member_offsets[ins->field_index] : 0;
                            auto* member_ptr = storage;
                            if (offset != 0)
                            {
                                LLVMValueRef offset_value = llvm_const_int(LLVMInt64TypeInContext(ctx), offset, false);
                                member_ptr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(ctx), storage, &offset_value, 1, "");
                            }
                            if (is_bool_type(aggregate_type->members[ins->field_index]))
                                val = LLVMBuildZExt(builder, val, LLVMInt8TypeInContext(ctx), "");
                            if (tc.indirect(ins->value->type))
                                copy_indirect(builder, ctx, ins->value->type, member_ptr, val);
                            else
                                LLVMBuildStore(builder, val, member_ptr);
                            auto* result = LLVMBuildLoad2(builder, aggregate_llvm_type, storage, "");
                            set_name(result);
                            val_map[inst] = result;
                            break;
                        }

                        unsigned llvm_field_idx = ins->field_index;
                        if (ins->aggregate && ins->aggregate->type && ins->aggregate->type->kind == IrTypeKind::Aggregate)
                            llvm_field_idx = tc.get_llvm_field_index(static_cast<IrAggregateType const*>(ins->aggregate->type), ins->field_index);

                        if (is_bool_type(ins->value->type))
                            val = LLVMBuildZExt(builder, val, LLVMInt8TypeInContext(ctx), "");
                        if (tc.indirect(ins->value->type))
                            val = LLVMBuildLoad2(builder, llvm_type_cached(tc, ins->value->type), val, "");

                        auto* result = LLVMBuildInsertValue(builder, agg_val, val, llvm_field_idx, "");
                        set_name(result);
                        val_map[inst] = result;
                        break;
                    }
                    case IrNodeKind::InlineAsm: {
                        auto* ai = static_cast<IrInlineAsmInst const*>(inst);
                        auto lowered = dcc::backend::prepare_llvm_asm(*ai, target.arch);
                        if (!lowered.error.empty())
                        {
                            add_diag(diags, inst->range, std::format("LLVM backend: {}", lowered.error));
                            return false;
                        }
                        std::vector<LLVMValueRef> input_values;
                        std::vector<std::pair<unsigned, LLVMTypeRef>> mem_elemtypes;
                        auto push_address = [&](std::uint32_t index) -> bool {
                            if (index >= ai->operands.size())
                            {
                                add_diag(diags, inst->range, "LLVM backend: inline assembly operand reference out of range");
                                return false;
                            }
                            auto const& op = ai->operands[index];
                            if (!op.value || op.placement_kind != IrAsmOperand::PlacementKind::Mem)
                            {
                                add_diag(diags, inst->range, "LLVM backend: inline assembly memory operand has no address");
                                return false;
                            }
                            auto* v = lookup(op.value);
                            if (!v)
                                return false;
                            auto* ir_ptr_ty = op.value->type;
                            if (ir_ptr_ty && ir_ptr_ty->kind == IrTypeKind::Pointer)
                            {
                                auto* ir_pointee_ty = static_cast<IrPointerType const*>(ir_ptr_ty)->pointee;
                                if (ir_pointee_ty)
                                {
                                    auto* llvm_pointee = llvm_type_cached(tc, ir_pointee_ty);
                                    if (llvm_pointee)
                                        mem_elemtypes.emplace_back(static_cast<unsigned>(input_values.size()), llvm_pointee);
                                }
                            }
                            input_values.push_back(v);
                            return true;
                        };
                        for (auto index : lowered.output_address_indices)
                            if (!push_address(index))
                                return false;
                        for (auto index : lowered.input_operand_indices)
                        {
                            if (index >= ai->operands.size())
                            {
                                add_diag(diags, inst->range, "LLVM backend: inline assembly operand reference out of range");
                                return false;
                            }
                            auto const& op = ai->operands[index];
                            if (!op.value)
                            {
                                add_diag(diags, inst->range, "LLVM backend: inline assembly input operand has no value");
                                return false;
                            }
                            auto* v = lookup(op.value);
                            if (!v)
                                return false;
                            if (op.placement_kind == IrAsmOperand::PlacementKind::Mem)
                            {
                                auto* ir_ptr_ty = op.value->type;
                                if (ir_ptr_ty && ir_ptr_ty->kind == IrTypeKind::Pointer)
                                {
                                    auto* ir_pointee_ty = static_cast<IrPointerType const*>(ir_ptr_ty)->pointee;
                                    if (ir_pointee_ty)
                                    {
                                        auto* llvm_pointee = llvm_type_cached(tc, ir_pointee_ty);
                                        if (llvm_pointee)
                                            mem_elemtypes.emplace_back(static_cast<unsigned>(input_values.size()), llvm_pointee);
                                    }
                                }
                            }
                            input_values.push_back(v);
                        }
                        std::string constraint_str = lowered.constraints;
                        for (auto const& clobber : lowered.clobbers)
                        {
                            if (!constraint_str.empty())
                                constraint_str += ",";
                            constraint_str += "~{" + std::string(clobber) + "}";
                        }

                        auto* ret_ty = llvm_type_cached(tc, inst->type);
                        bool single_flag = false;
                        if (inst->type && inst->type->kind == IrTypeKind::Bool)
                        {
                            for (auto const& op : ai->operands)
                            {
                                if (op.direction != IrAsmOperand::Direction::In && op.placement_kind == IrAsmOperand::PlacementKind::Flag)
                                {
                                    single_flag = true;
                                    break;
                                }
                            }
                            if (single_flag)
                                ret_ty = LLVMInt8TypeInContext(ctx);
                        }
                        if (!ret_ty)
                            ret_ty = LLVMVoidTypeInContext(ctx);

                        std::vector<LLVMTypeRef> param_tys;
                        param_tys.reserve(input_values.size());
                        for (auto* iv : input_values)
                            param_tys.push_back(LLVMTypeOf(iv));

                        auto* asm_func_ty = LLVMFunctionType(ret_ty, param_tys.data(), static_cast<unsigned>(param_tys.size()), 0);

                        auto* asm_val = LLVMGetInlineAsm(asm_func_ty, lowered.template_str.data(), lowered.template_str.size(), constraint_str.c_str(),
                                                         constraint_str.size(), ai->is_volatile ? 1 : 0, ai->align_stack ? 1 : 0,
                                                         ai->dialect == IrAsmDialect::Intel ? LLVMInlineAsmDialectIntel : LLVMInlineAsmDialectATT, 0);

                        auto* call_inst = LLVMBuildCall2(builder, asm_func_ty, asm_val, input_values.data(), static_cast<unsigned>(input_values.size()), "");

                        if (!mem_elemtypes.empty())
                        {
                            unsigned elem_kind = LLVMGetEnumAttributeKindForName("elementtype", 11);
                            if (elem_kind)
                            {
                                for (auto const& [param_idx, llvm_pointee] : mem_elemtypes)
                                {
                                    auto* elem_attr = LLVMCreateTypeAttribute(ctx, elem_kind, llvm_pointee);
                                    if (elem_attr)
                                        LLVMAddCallSiteAttribute(call_inst, param_idx + 1, elem_attr);
                                }
                            }
                        }

                        if (single_flag)
                        {
                            auto* trunc = LLVMBuildTrunc(builder, call_inst, LLVMInt1TypeInContext(ctx), "");
                            set_name(trunc);
                            val_map[inst] = trunc;
                        }
                        else
                        {
                            set_name(call_inst);
                            val_map[inst] = call_inst;
                        }
                        break;
                    }
                    default:
                        add_diag(diags, inst->range, std::format("LLVM backend: unsupported instruction kind {}", static_cast<int>(inst->kind)));
                        return false;
                }
                return true;
            }

            [[nodiscard]] static bool emit_terminator(IrNode const* term, LLVMBuilderRef builder, LLVMContextRef ctx, TypeCache& tc,
                                                      std::unordered_map<IrValue const*, LLVMValueRef>& val_map,
                                                      std::unordered_map<IrBasicBlock const*, LLVMBasicBlockRef>& bb_map, std::vector<BackendDiagnostic>& diags,
                                                      LLVMValueRef llvm_func, IrFunction const* func)
            {
                if (!term)
                {
                    LLVMBuildUnreachable(builder);
                    return true;
                }

                auto lookup = [&](IrValue const* v) -> LLVMValueRef {
                    if (!v)
                        return nullptr;

                    if (auto it = val_map.find(v); it != val_map.end())
                        return it->second;

                    auto* c = c_api_constant(v, ctx, tc, val_map);
                    if (c)
                    {
                        if (tc.indirect(v->type))
                        {
                            c = materialize_indirect_constant(builder, ctx, tc, v->type, c);
                            if (!c)
                                return nullptr;
                        }
                        val_map[v] = c;
                        return c;
                    }

                    return nullptr;
                };

                switch (term->kind)
                {
                    case IrNodeKind::Br: {
                        auto* b = static_cast<IrBrInst const*>(term);
                        auto* target_bb = bb_map[b->target];
                        if (!target_bb)
                            return false;

                        LLVMBuildBr(builder, target_bb);
                        break;
                    }
                    case IrNodeKind::BrCond: {
                        auto* bc = static_cast<IrBrCondInst const*>(term);
                        auto* cond = lookup(bc->condition);
                        auto* true_bb = bb_map[bc->true_target];
                        auto* false_bb = bb_map[bc->false_target];
                        if (!cond || !true_bb || !false_bb)
                            return false;

                        if (!is_bool_type(bc->condition->type))
                        {
                            auto* zero = llvm_const_int(LLVMTypeOf(cond), 0, 0);
                            cond = LLVMBuildICmp(builder, LLVMIntNE, cond, zero, "tobool");
                        }

                        LLVMBuildCondBr(builder, cond, true_bb, false_bb);
                        break;
                    }
                    case IrNodeKind::Ret: {
                        auto* r = static_cast<IrRetInst const*>(term);
                        if (r->value)
                        {
                            auto* v = lookup(r->value);
                            if (!v)
                                return false;

                            auto const ret_sig = compute_signature_abi(tc, func->func_type ? func->func_type->return_type : r->value->type,
                                                                       func->func_type ? std::span<IrType const* const>{func->func_type->params}
                                                                                       : std::span<IrType const* const>{},
                                                                       aggregate_abi_for(tc, func));
                            if (tc.indirect(r->value->type))
                            {
                                copy_indirect(builder, ctx, r->value->type, LLVMGetParam(llvm_func, 0), v);
                                LLVMBuildRetVoid(builder);
                            }
                            else if (ret_sig.sret_boundary)
                            {
                                LLVMBuildStore(builder, v, LLVMGetParam(llvm_func, 0));
                                LLVMBuildRetVoid(builder);
                            }
                            else if (ret_sig.ret.kind == ArgAbi::Kind::Coerce)
                            {
                                auto parts = abi_split(builder, tc, r->value->type, v, ret_sig.ret.pieces);
                                if (parts.size() == 1)
                                    LLVMBuildRet(builder, parts[0]);
                                else
                                {
                                    LLVMValueRef agg = LLVMGetUndef(ret_sig.ret_type);
                                    for (std::size_t k = 0; k < parts.size(); ++k)
                                        agg = LLVMBuildInsertValue(builder, agg, parts[k], static_cast<unsigned>(k), "");
                                    LLVMBuildRet(builder, agg);
                                }
                            }
                            else
                                LLVMBuildRet(builder, v);
                        }
                        else
                            LLVMBuildRetVoid(builder);
                        break;
                    }
                    case IrNodeKind::Unreachable: {
                        auto* current = LLVMGetInsertBlock(builder);
                        auto* last = current ? LLVMGetLastInstruction(current) : nullptr;
                        if (last && LLVMIsACallInst(last))
                        {
                            auto const trap_id = LLVMLookupIntrinsicID("llvm.trap", 9);
                            auto* trap_fn = LLVMGetIntrinsicDeclaration(LLVMGetGlobalParent(llvm_func), trap_id, nullptr, 0);
                            LLVMBuildCall2(builder, LLVMIntrinsicGetType(ctx, trap_id, nullptr, 0), trap_fn, nullptr, 0, "");
                        }
                        LLVMBuildUnreachable(builder);
                        break;
                    }
                    case IrNodeKind::Switch: {
                        auto* sw = static_cast<IrSwitchInst const*>(term);
                        auto* val = lookup(sw->value);
                        if (!val)
                            return false;

                        auto* default_bb = bb_map[sw->default_target];
                        if (!default_bb)
                            return false;

                        constexpr std::int64_t kSwitchThreshold = 64;
                        std::int64_t total_values = 0;
                        std::int64_t max_range_size = 0;
                        for (auto const& c : sw->cases)
                        {
                            auto range_size = c.end - c.start + 1;
                            if (range_size > max_range_size)
                                max_range_size = range_size;
                            total_values += range_size;
                        }

                        bool use_cascade = (max_range_size > kSwitchThreshold || total_values > kSwitchThreshold);

                        if (use_cascade)
                        {
                            auto* llvm_val_ty = LLVMTypeOf(val);
                            auto* parent_bb = LLVMGetInsertBlock(builder);
                            std::string parent_name = LLVMGetBasicBlockName(parent_bb);

                            for (std::size_t i = 0; i < sw->cases.size(); ++i)
                            {
                                auto const& c = sw->cases[i];
                                auto* case_target = bb_map[c.target];
                                if (!case_target)
                                    return false;

                                bool is_last = (i == sw->cases.size() - 1);

                                if (c.start == c.end)
                                {
                                    auto* cmp = LLVMBuildICmp(builder, LLVMIntEQ, val,
                                                              llvm_const_int(llvm_val_ty, static_cast<unsigned long long>(c.start),
                                                                           static_cast<IrIntType const*>(sw->value->type) &&
                                                                               static_cast<IrIntType const*>(sw->value->type)->is_signed),
                                                              "");

                                    if (is_last)
                                        LLVMBuildCondBr(builder, cmp, case_target, default_bb);
                                    else
                                    {
                                        auto* next_bb = LLVMAppendBasicBlockInContext(ctx, llvm_func, std::format("switch.case.{}", i + 1).c_str());
                                        LLVMBuildCondBr(builder, cmp, case_target, next_bb);
                                        LLVMPositionBuilderAtEnd(builder, next_bb);
                                    }
                                }
                                else
                                {
                                    bool is_signed = false;
                                    if (auto const* ir_int_type = static_cast<IrIntType const*>(sw->value->type))
                                        is_signed = ir_int_type->is_signed;

                                    auto* start_val = llvm_const_int(llvm_val_ty, static_cast<unsigned long long>(c.start), is_signed);
                                    auto* end_val = llvm_const_int(llvm_val_ty, static_cast<unsigned long long>(c.end), is_signed);

                                    LLVMIntPredicate ge_pred = is_signed ? LLVMIntSGE : LLVMIntUGE;
                                    LLVMIntPredicate le_pred = is_signed ? LLVMIntSLE : LLVMIntULE;

                                    auto* ge_cmp = LLVMBuildICmp(builder, ge_pred, val, start_val, "");
                                    auto* le_cmp = LLVMBuildICmp(builder, le_pred, val, end_val, "");
                                    auto* in_range = LLVMBuildAnd(builder, ge_cmp, le_cmp, "inrange");

                                    if (is_last)
                                        LLVMBuildCondBr(builder, in_range, case_target, default_bb);
                                    else
                                    {
                                        auto* next_bb = LLVMAppendBasicBlockInContext(ctx, llvm_func, std::format("switch.case.{}", i + 1).c_str());
                                        LLVMBuildCondBr(builder, in_range, case_target, next_bb);
                                        LLVMPositionBuilderAtEnd(builder, next_bb);
                                    }
                                }
                            }
                        }
                        else
                        {
                            auto* switch_inst = LLVMBuildSwitch(builder, val, default_bb, static_cast<unsigned>(total_values));

                            for (auto const& c : sw->cases)
                            {
                                auto* case_target = bb_map[c.target];
                                if (!case_target)
                                    return false;

                                for (std::int64_t v = c.start; v <= c.end; ++v)
                                {
                                    auto* case_val = llvm_const_int(LLVMTypeOf(val), static_cast<unsigned long long>(v),
                                                                  static_cast<IrIntType const*>(sw->value->type) &&
                                                                      static_cast<IrIntType const*>(sw->value->type)->is_signed);

                                    LLVMAddCase(switch_inst, case_val, case_target);
                                }
                            }
                        }
                        break;
                    }
                    default:
                        add_diag(diags, term->range, std::format("LLVM backend: unsupported terminator kind {}", static_cast<int>(term->kind)));
                        return false;
                }
                return true;
            }
        };

    } // anonymous namespace

    auto make_llvm_backend() -> std::unique_ptr<Backend>
    {
        return std::make_unique<LlvmBackendImpl>();
    }

} // namespace dcc::backend
