export module dcc.vfs;

import std;
import dcc.sm;

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
public void atomic_fence(MemoryOrder order);

)dc";

    constexpr std::string_view kCoreSourceLocationSourceText = R"dc(module core::source_location;

public struct SourceLocation {
    []const u8 file;
    u32 line;
    u32 column;
}

@intrinsic
public SourceLocation source_location();

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
