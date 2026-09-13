module;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOUSER
#define NOGDI
#include <windows.h>
#endif

export module dcc.config;

import std;

namespace dcc::config::detail
{
    [[nodiscard]] std::filesystem::path normalize_prefix(const std::filesystem::path& prefix)
    {
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(prefix, ec);
        if (ec)
            canonical = prefix.lexically_normal();

        if (!canonical.is_absolute())
        {
            std::error_code cwd_ec;
            auto cwd = std::filesystem::current_path(cwd_ec);
            if (!cwd_ec)
                canonical = (cwd / canonical).lexically_normal();
        }

        return canonical;
    }

} // namespace dcc::config::detail

export namespace dcc::config
{
    enum class PrefixSource : std::uint8_t
    {
        ExeRelative,
        Baked,
    };

    struct ResolvedPrefix
    {
        std::filesystem::path path;
        PrefixSource source{PrefixSource::Baked};
    };

    [[nodiscard]] std::string_view to_string(PrefixSource source)
    {
        switch (source)
        {
            case PrefixSource::ExeRelative:
                return "exe-relative";
            case PrefixSource::Baked:
                return "baked";
        }
        return "baked";
    }

    [[nodiscard]] std::filesystem::path baked_prefix()
    {
#ifdef DCC_INSTALL_PREFIX
        return std::filesystem::path{DCC_INSTALL_PREFIX};
#else
        return std::filesystem::path{"/usr/local"};
#endif
    }

    [[nodiscard]] std::filesystem::path detect_exe_path(char** argv)
    {
#ifdef _WIN32
        std::ignore = argv;
        wchar_t buf[MAX_PATH];
        DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
            return std::filesystem::path{std::wstring{buf, len}};

        return {};
#else
        std::error_code ec;

        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec)
        {
            auto resolved = std::filesystem::weakly_canonical(exe, ec);
            if (!ec)
                return resolved;

            return exe;
        }

        if (argv == nullptr || argv[0] == nullptr)
            return {};

        std::filesystem::path arg0{argv[0]};

        if (arg0.is_absolute())
        {
            auto resolved = std::filesystem::weakly_canonical(arg0, ec);
            if (!ec)
                return resolved;

            return arg0;
        }

        if (std::string_view{argv[0]}.contains('/'))
        {
            auto resolved = std::filesystem::absolute(arg0, ec);
            if (!ec)
            {
                auto wk = std::filesystem::weakly_canonical(resolved, ec);
                if (!ec)
                    return wk;

                return resolved;
            }
            return arg0;
        }

        auto const* path_env = std::getenv("PATH");
        if (path_env)
        {
            std::string_view path_sv{path_env};
            std::size_t pos = 0;
            while (pos < path_sv.size())
            {
                auto colon = path_sv.find(':', pos);
                auto dir = path_sv.substr(pos, colon - pos);
                pos = (colon == std::string_view::npos) ? path_sv.size() : colon + 1;

                if (dir.empty())
                    continue;

                auto candidate = std::filesystem::path{dir} / arg0;
                if (std::filesystem::exists(candidate, ec))
                {
                    auto wk = std::filesystem::weakly_canonical(candidate, ec);
                    if (!ec)
                        return wk;

                    auto abs = std::filesystem::absolute(candidate, ec);
                    if (!ec)
                        return abs;

                    return candidate;
                }
            }
        }

        return arg0;
#endif
    }

    [[nodiscard]] ResolvedPrefix resolve_prefix(std::filesystem::path const& exe_path, std::filesystem::path const& baked)
    {
        auto fallback = detail::normalize_prefix(baked);
        if (exe_path.empty())
            return ResolvedPrefix{.path = fallback, .source = PrefixSource::Baked};

        auto candidate = exe_path.parent_path().parent_path();

        std::error_code ec;
        bool usable = std::filesystem::is_directory(candidate / "include", ec) && !ec;
        if (!usable)
            return ResolvedPrefix{.path = fallback, .source = PrefixSource::Baked};

        return ResolvedPrefix{.path = detail::normalize_prefix(candidate), .source = PrefixSource::ExeRelative};
    }

    [[nodiscard]] ResolvedPrefix current_prefix(char** argv)
    {
        return resolve_prefix(detect_exe_path(argv), baked_prefix());
    }

} // namespace dcc::config
