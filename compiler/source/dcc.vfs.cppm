export module dcc.vfs;

import std;
import dcc.sm;
import dcc.target;
import dcc.types;

export namespace dcc::vfs
{
    constexpr std::string_view kDccCoreScheme = "dcc-core:";
    constexpr std::string_view kDccCorePathPrefix = "<dcc-core>/";

    namespace detail
    {
        constexpr std::size_t kMaxUriLength = 128;

        [[nodiscard]] constexpr std::size_t module_path_to_uri_buffer(std::string_view module_path, char (&dst)[kMaxUriLength]) noexcept
        {
            std::size_t out = 0;

            for (char c : kDccCoreScheme)
            {
                if (out >= kMaxUriLength - 1)
                    return 0;

                dst[out++] = c;
            }

            if (out >= kMaxUriLength - 1)
                return 0;

            dst[out++] = '/';

            for (std::size_t i = 0; i < module_path.size(); ++i)
            {
                char c = module_path[i];
                if (c == ':')
                {
                    if (i + 1 < module_path.size() && module_path[i + 1] == ':')
                    {
                        if (out >= kMaxUriLength - 1)
                            return 0;

                        dst[out++] = '/';
                        ++i;
                        continue;
                    }
                }

                if (out >= kMaxUriLength - 1)
                    return 0;

                dst[out++] = c;
            }

            for (char c : std::string_view{".dc"})
            {
                if (out >= kMaxUriLength - 1)
                    return 0;

                dst[out++] = c;
            }

            dst[out] = '\0';
            return out;
        }

        [[nodiscard]] constexpr std::string_view module_path_prefix(std::string_view uri) noexcept
        {
            if (!uri.starts_with(kDccCoreScheme))
                return {};

            auto rest = uri.substr(kDccCoreScheme.size());
            if (rest.empty() || rest[0] != '/')
                return {};

            rest = rest.substr(1);

            auto slash_pos = rest.find('/');
            auto tail = (slash_pos == std::string_view::npos) ? rest : rest.substr(0, slash_pos);

            if (tail.ends_with(".dc"))
                tail = tail.substr(0, tail.size() - 3);

            return tail;
        }

    } // namespace detail

    struct VirtualModuleEntry
    {
        std::string_view module_path;
        std::string_view uri;
        std::string_view source_text;
    };

    constexpr std::string_view kCoreSourceText = R"dc(module core;

public import core::atomic;
public import core::source_location;
public import core::target;
public import core::seg;

@intrinsic
public void compile_error([]const char message);

@intrinsic
public void compile_warning([]const char message);

@intrinsic
public void compile_note([]const char message);

)dc";

    constexpr std::string_view kCoreAtomicSourceText = R"dc(module core::atomic;

public enum MemoryOrder : u8 {
    Relaxed,
    Acquire,
    Release,
    AcqRel,
    SeqCst,
}

public struct Atomic(T) {
    volatile T value;
}

@intrinsic
public T atomic_load(T)(volatile T* ptr, MemoryOrder order);

@intrinsic
public T atomic_load(T)(Atomic(T)* ptr, MemoryOrder order);

@intrinsic
public void atomic_store(T)(volatile T* ptr, T value, MemoryOrder order);

@intrinsic
public void atomic_store(T)(Atomic(T)* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_exchange(T)(volatile T* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_exchange(T)(Atomic(T)* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_add(T)(volatile T* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_add(T)(Atomic(T)* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_sub(T)(volatile T* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_sub(T)(Atomic(T)* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_and(T)(volatile T* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_and(T)(Atomic(T)* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_or(T)(volatile T* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_or(T)(Atomic(T)* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_xor(T)(volatile T* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_fetch_xor(T)(Atomic(T)* ptr, T value, MemoryOrder order);

@intrinsic
public T atomic_compare_exchange(T)(volatile T* ptr, T expected, T desired, MemoryOrder success, MemoryOrder failure);

@intrinsic
public T atomic_compare_exchange(T)(Atomic(T)* ptr, T expected, T desired, MemoryOrder success, MemoryOrder failure);

@intrinsic
public void atomic_fence(MemoryOrder order);

)dc";

    constexpr std::string_view kCoreSourceLocationSourceText = R"dc(module core::source_location;

public struct SourceLocation {
    []const char file;
    u32 line;
    u32 column;
    []const char function;
}

@intrinsic
public SourceLocation source_location();

public enum FunctionNameStyle { Plain, Qualified, Pretty, Mangled }

@intrinsic
public []const char function_name(FunctionNameStyle style = FunctionNameStyle::Qualified);

)dc";

    constexpr VirtualModuleEntry kCoreModules[] = {
        {
            .module_path = "core",
            .uri = "dcc-core:/core.dc",
            .source_text = kCoreSourceText,
        },
        {
            .module_path = "core::atomic",
            .uri = "dcc-core:/core/atomic.dc",
            .source_text = kCoreAtomicSourceText,
        },
        {
            .module_path = "core::source_location",
            .uri = "dcc-core:/core/source_location.dc",
            .source_text = kCoreSourceLocationSourceText,
        },
    };

    namespace detail
    {
        [[nodiscard]] consteval bool validate_registry() noexcept
        {
            for (std::size_t i = 0; i < std::size(kCoreModules); ++i)
            {
                auto const& e = kCoreModules[i];

                if (e.source_text.empty())
                    return false;

                {
                    char expected[kMaxUriLength]{};
                    auto len = module_path_to_uri_buffer(e.module_path, expected);
                    if (len == 0)
                        return false;

                    if (std::string_view{expected, len} != e.uri)
                        return false;
                }

                {
                    auto prefix = detail::module_path_prefix(e.uri);
                    if (prefix != "core")
                        return false;
                }

                for (std::size_t j = 0; j < i; ++j)
                    if (kCoreModules[j].module_path == e.module_path)
                        return false;

                for (std::size_t j = 0; j < i; ++j)
                    if (kCoreModules[j].uri == e.uri)
                        return false;
            }

            return true;
        }

        static_assert(validate_registry(), "dcc.vfs registry is malformed");

    } // namespace detail

    [[nodiscard]] VirtualModuleEntry const* lookup_by_module_path(std::string_view module_path) noexcept
    {
        for (auto const& entry : kCoreModules)
            if (entry.module_path == module_path)
                return &entry;

        return nullptr;
    }

    [[nodiscard]] VirtualModuleEntry const* lookup_by_uri(std::string_view uri) noexcept
    {
        for (auto const& entry : kCoreModules)
            if (entry.uri == uri)
                return &entry;

        return nullptr;
    }

    [[nodiscard]] bool is_dcc_core_uri(std::string_view uri) noexcept
    {
        return uri.starts_with(kDccCoreScheme);
    }

    struct DynamicModuleEntry
    {
        std::string_view module_path;
        std::string_view uri;
    };

    constexpr DynamicModuleEntry kDynamicCoreModules[] = {
        {"core::i8086_arith", "dcc-core:/core/i8086_arith.dc"},
        {
            .module_path = "core::target",
            .uri = "dcc-core:/core/target.dc",
        },
        {
            .module_path = "core::seg",
            .uri = "dcc-core:/core/seg.dc",
        },
    };

    [[nodiscard]] DynamicModuleEntry const* lookup_dynamic_by_module_path(std::string_view module_path) noexcept
    {
        for (auto const& entry : kDynamicCoreModules)
            if (entry.module_path == module_path)
                return &entry;

        return nullptr;
    }

    [[nodiscard]] DynamicModuleEntry const* lookup_dynamic_by_uri(std::string_view uri) noexcept
    {
        for (auto const& entry : kDynamicCoreModules)
            if (entry.uri == uri)
                return &entry;

        return nullptr;
    }

    [[nodiscard]] std::string generate_core_target_source(dcc::target::TargetConfig const& target)
    {
        auto const* os = target.os == dcc::target::Os::Linux ? "Linux" : target.os == dcc::target::Os::Windows ? "Windows" : "Freestanding";
        auto const* arch = target.arch == dcc::target::Arch::X86_64 ? "X86_64" : target.arch == dcc::target::Arch::I8086 ? "I8086" : "X86";

        bool const float_available = target.float_available();

        return std::format(R"dc(module core::target;

public enum Os {{
    Linux,
    Windows,
    Freestanding,
}}

public enum Arch {{
    X86_64,
    X86,
    I8086,
}}

public enum CodeModel {{
    Default, Small, Kernel, Medium, Large, Unreal, Unreal32,
}}

public const CodeModel CODE_MODEL = CodeModel::{};
public const Os OS = Os::{};
public const Arch ARCH = Arch::{};
public const bool IS_64_BIT = {};
public const bool FLOAT_AVAILABLE = {};

)dc",
                           target.code_model == dcc::target::CodeModel::Unreal ? "Unreal" : target.code_model == dcc::target::CodeModel::Unreal32 ? "Unreal32" :
                           target.code_model == dcc::target::CodeModel::Small ? "Small" : target.code_model == dcc::target::CodeModel::Kernel ? "Kernel" :
                           target.code_model == dcc::target::CodeModel::Medium ? "Medium" : target.code_model == dcc::target::CodeModel::Large ? "Large" : "Default",
                           os, arch, target.pointer_bits == 64, float_available);
    }

    [[nodiscard]] std::string generate_core_seg_source(dcc::target::TargetConfig const& target)
    {
        std::string out = "module core::seg;\n\n";
        if (dcc::types::TypeContext::dynamic_far_allowed(target.arch))
        {
            out += "@intrinsic\npublic u16 segment(T)(T^ ptr);\n\n";
            out += "@intrinsic\npublic u16 segment(T)(const T^ ptr);\n\n";
            out += "@intrinsic\npublic usize offset(T)(T^ ptr);\n\n";
            out += "@intrinsic\npublic usize offset(T)(const T^ ptr);\n\n";
            out += "@intrinsic\npublic T^ with_offset(T)(T^ ptr, usize off);\n\n";
            out += "@intrinsic\npublic const T^ with_offset(T)(const T^ ptr, usize off);\n\n";
            out += "@intrinsic\npublic T^ with_segment(T)(T^ ptr, u16 seg);\n\n";
            out += "@intrinsic\npublic const T^ with_segment(T)(const T^ ptr, u16 seg);\n\n";
            // TODO move this along std::slice::from_raw into core::slice.
            out += "public [^] T from_raw(T)(T^ ptr, usize len) {\n    [^] T slice;\n    slice.ptr = ptr;\n    slice.len = len;\n    return slice;\n}\n\n";
            out += "public [^] const T from_raw_const(T)(const T^ ptr, usize len) {\n    [^] const T slice;\n    slice.ptr = ptr;\n    slice.len = len;\n    "
                   "return slice;\n}\n\n";
        }
        constexpr dcc::types::SegReg regs[] = {
            dcc::types::SegReg::CS, dcc::types::SegReg::DS, dcc::types::SegReg::ES, dcc::types::SegReg::SS, dcc::types::SegReg::FS, dcc::types::SegReg::GS,
        };
        for (auto reg : regs)
        {
            if (!dcc::types::TypeContext::based_register_allowed(target.arch, reg, target.has_i386_segments()))
                continue;
            auto name = dcc::types::TypeContext::seg_reg_name(reg);
            out += std::format("@intrinsic\npublic usize offset(T)(T^{0} ptr);\n\n", name);
            out += std::format("@intrinsic\npublic usize offset(T)(const T^{0} ptr);\n\n", name);
            out += std::format("@intrinsic\npublic T^{0} with_offset(T)(T^{0} ptr, usize off);\n\n", name);
            out += std::format("@intrinsic\npublic const T^{0} with_offset(T)(const T^{0} ptr, usize off);\n\n", name);
            out += std::format("public [^{0}] T from_raw(T)(T^{0} ptr, usize len) {{\n    [^{0}] T slice;\n    slice.ptr = ptr;\n    slice.len = len;\n    "
                               "return slice;\n}}\n\n",
                               name);
            out += std::format("public [^{0}] const T from_raw_const(T)(const T^{0} ptr, usize len) {{\n    [^{0}] const T slice;\n    slice.ptr = ptr;\n    "
                               "slice.len = len;\n    return slice;\n}}\n\n",
                               name);
        }
        if (target.far_offset_bits() != target.pointer_bits)
        {
            for (std::size_t pos = 0; (pos = out.find("usize offset", pos)) != std::string::npos;)
                out.replace(pos, 5, "u32");
            for (std::size_t pos = 0; (pos = out.find("usize off", pos)) != std::string::npos;)
                out.replace(pos, 5, "u32");
        }
        return out;
    }

    [[nodiscard]] std::string generate_i8086_arithmetic_source()
    {
        return R"dc(module core::i8086_arith;
public struct DivResult { u64 quotient; u64 remainder; }
@nomangle @runtime extern u64 __dcc_udivmod32(u32 high, u32 low, u32 divisor);
@nomangle @runtime public u32 __dcc_i64_trap(u32 zero) { return 1 / zero; }
@nomangle @runtime public DivResult __dcc_udivmod64(u64 dividend, u64 divisor) {
    if divisor == 0 { __dcc_i64_trap(0); }
    if (divisor >> 32) == 0 {
        u32 denominator = divisor as u32;
        u32 low = dividend as u32;
        u32 high = (dividend >> 32) as u32;
        if high == 0 {
            u64 result = __dcc_udivmod32(0, low, denominator);
            return DivResult {quotient = (result as u32) as u64, remainder = result >> 32};
        }
        u64 first = __dcc_udivmod32(0, high, denominator);
        u64 second = __dcc_udivmod32((first >> 32) as u32, low, denominator);
        return DivResult {quotient = (((first as u32) as u64) << 32) | ((second as u32) as u64), remainder = second >> 32};
    }
    u64 quotient = 0;
    u64 remainder = 0;
    u16 count = 64;
    while count != 0 {
        count = count - 1;
        bool overflow = (remainder >> 63) != 0;
        remainder = (remainder << 1) | ((dividend >> (count as u64)) & 1);
        if overflow || remainder >= divisor {
            remainder = remainder - divisor;
            quotient = quotient | (1 as u64 << (count as u64));
        }
    }
    return DivResult {quotient = quotient, remainder = remainder};
}
@nomangle @runtime public DivResult __dcc_divmod64(u64 dividend, u64 divisor) {
    bool negative_lhs = (dividend as i64) < 0;
    bool negative_rhs = (divisor as i64) < 0;
    if dividend == 0x8000000000000000 && divisor == 0xffffffffffffffff { __dcc_i64_trap(0); }
    u64 a = dividend;
    u64 b = divisor;
    if negative_lhs { a = 0 - a; }
    if negative_rhs { b = 0 - b; }
    DivResult result = __dcc_udivmod64(a, b);
    if negative_lhs != negative_rhs { result.quotient = 0 - result.quotient; }
    if negative_lhs { result.remainder = 0 - result.remainder; }
    return result;
}
)dc";
    }

    [[nodiscard]] sm::FileId materialize_dynamic(DynamicModuleEntry const& entry, dcc::target::TargetConfig const& target, sm::SourceManager& smgr)
    {
        auto rest = entry.uri.substr(kDccCoreScheme.size());
        if (!rest.empty() && rest[0] == '/')
            rest = rest.substr(1);

        std::string syn_path;
        syn_path.reserve(kDccCorePathPrefix.size() + rest.size());
        syn_path += kDccCorePathPrefix;
        syn_path += rest;

        std::string source = entry.module_path == "core::i8086_arith" ? generate_i8086_arithmetic_source()
                             : entry.module_path == "core::seg"       ? generate_core_seg_source(target)
                                                                      : generate_core_target_source(target);
        return smgr.open_virtual_in_memory(std::string{entry.uri}, std::move(source), std::move(syn_path));
    }

    [[nodiscard]] std::string_view source_text_for_uri(std::string_view uri) noexcept
    {
        auto const* entry = lookup_by_uri(uri);
        return entry ? entry->source_text : std::string_view{};
    }

    [[nodiscard]] sm::FileId materialize(VirtualModuleEntry const& entry, sm::SourceManager& smgr)
    {
        auto rest = entry.uri.substr(kDccCoreScheme.size());
        if (!rest.empty() && rest[0] == '/')
            rest = rest.substr(1);

        std::string syn_path;
        syn_path.reserve(kDccCorePathPrefix.size() + rest.size());
        syn_path += kDccCorePathPrefix;
        syn_path += rest;

        auto id = smgr.open_virtual_in_memory(std::string{entry.uri}, std::string{entry.source_text}, std::move(syn_path));
        return id;
    }

    [[nodiscard]] std::string module_path_to_uri(std::string_view module_path)
    {
        char buf[detail::kMaxUriLength]{};
        auto len = detail::module_path_to_uri_buffer(module_path, buf);
        return std::string{buf, len};
    }

    [[nodiscard]] std::string uri_to_module_path(std::string_view uri)
    {
        if (!uri.starts_with(kDccCoreScheme))
            return {};

        auto rest = uri.substr(kDccCoreScheme.size());

        if (rest.empty())
            return {};

        if (rest[0] == '/')
            rest = rest.substr(1);

        if (rest.size() > 3 && rest.ends_with(".dc"))
            rest = rest.substr(0, rest.size() - 3);

        std::string path;
        for (char c : rest)
        {
            if (c == '/')
                path += "::";
            else
                path += c;
        }

        return path;
    }

} // namespace dcc::vfs
