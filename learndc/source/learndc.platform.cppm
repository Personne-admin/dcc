module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;
#endif

export module learndc.platform;

import std;
import learndc.text;
import dcc.host.process;

export namespace learndc::platform
{
    using Env = std::map<std::string, std::string>;

    enum class Os
    {
        Linux,
        Windows,
    };

    [[nodiscard]] constexpr Os host_os()
    {
#if defined(_WIN32)
        return Os::Windows;
#else
        return Os::Linux;
#endif
    }

    struct Overrides
    {
        std::optional<std::filesystem::path> home;
    };

    struct Dirs
    {
        std::filesystem::path config_dir;
        std::filesystem::path data_dir;
        std::filesystem::path cache_dir;
        std::filesystem::path config_file;
        std::filesystem::path progress_file;
        std::filesystem::path progress_lock;
        std::filesystem::path workspaces_dir;
        std::filesystem::path backups_dir;
        std::filesystem::path staging_dir;
        std::filesystem::path logs_dir;
        bool overridden = false;
    };

    namespace detail
    {
#if defined(_WIN32)
        [[nodiscard]] inline std::wstring widen(std::string_view s)
        {
            if (s.empty())
                return {};
            int const n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
            return out;
        }

        [[nodiscard]] inline std::string narrow(std::wstring_view s)
        {
            if (s.empty())
                return {};
            int const n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
            return out;
        }
#endif

        [[nodiscard]] inline std::string last_error_message()
        {
#if defined(_WIN32)
            return std::system_category().message(static_cast<int>(GetLastError()));
#else
            return std::generic_category().message(errno);
#endif
        }

        [[nodiscard]] inline std::optional<std::string> env_lookup(Env const& env, std::string_view name, Os os)
        {
            if (auto it = env.find(std::string(name)); it != env.end())
                return it->second;
            if (os == Os::Windows)
                for (auto const& [key, value] : env)
                    if (key.size() == name.size() && text::lower(key) == text::lower(name))
                        return value;
            return std::nullopt;
        }

        [[nodiscard]] inline std::optional<std::filesystem::path> env_path(Env const& env, std::string_view name, Os os, bool require_absolute)
        {
            auto value = env_lookup(env, name, os);
            if (!value || value->empty())
                return std::nullopt;
            std::filesystem::path p(*value);
            if (require_absolute && !p.is_absolute())
                return std::nullopt;
            return p;
        }
    } // namespace detail

    [[nodiscard]] inline std::filesystem::path path_from_utf8(std::string_view s)
    {
        return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(s.data()), s.size()));
    }

    [[nodiscard]] inline std::string path_to_utf8(std::filesystem::path const& p)
    {
        auto const u = p.u8string();
        return std::string(reinterpret_cast<char const*>(u.data()), u.size());
    }

    [[nodiscard]] inline Env current_env()
    {
        Env env;
#if defined(_WIN32)
        wchar_t* block = GetEnvironmentStringsW();
        if (block != nullptr)
        {
            for (wchar_t const* p = block; *p != L'\0';)
            {
                std::wstring_view entry(p);
                p += entry.size() + 1;
                auto const eq = entry.find(std::wstring_view(L"="), 1);
                if (eq == std::wstring_view::npos)
                    continue;
                env[detail::narrow(entry.substr(0, eq))] = detail::narrow(entry.substr(eq + 1));
            }
            FreeEnvironmentStringsW(block);
        }
#else
        for (char** p = environ; p != nullptr && *p != nullptr; ++p)
        {
            std::string_view entry(*p);
            auto const eq = entry.find('=');
            if (eq == std::string_view::npos || eq == 0)
                continue;
            env[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
        }
#endif
        return env;
    }

    [[nodiscard]] inline std::optional<std::string> getenv(std::string_view name)
    {
#if defined(_WIN32)
        auto const wname = detail::widen(name);
        DWORD const needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
        if (needed == 0)
            return std::nullopt;
        std::wstring buffer(needed, L'\0');
        DWORD const got = GetEnvironmentVariableW(wname.c_str(), buffer.data(), needed);
        buffer.resize(got);
        return detail::narrow(buffer);
#else
        std::string const key(name);
        char const* value = std::getenv(key.c_str());
        if (value == nullptr)
            return std::nullopt;
        return std::string(value);
#endif
    }

    [[nodiscard]] inline std::expected<Dirs, text::Error> resolve_dirs(Overrides const& overrides, Env const& env, Os os = host_os())
    {
        Dirs dirs;
        std::optional<std::filesystem::path> home = overrides.home;
        if (!home)
            home = detail::env_path(env, "LEARNDC_HOME", os, false);
        if (home)
        {
            std::error_code ec;
            auto absolute = std::filesystem::absolute(*home, ec);
            if (ec)
                absolute = *home;
            dirs.overridden = true;
            dirs.config_dir = absolute;
            dirs.data_dir = absolute;
            dirs.cache_dir = absolute / "cache";
            dirs.config_file = absolute / "config.json";
            dirs.progress_file = absolute / "progress.json";
            dirs.progress_lock = absolute / "progress.lock";
            dirs.workspaces_dir = absolute / "workspaces";
            dirs.backups_dir = absolute / "backups";
        }
        else if (os == Os::Windows)
        {
            auto roaming = detail::env_path(env, "APPDATA", os, false);
            auto local = detail::env_path(env, "LOCALAPPDATA", os, false);
            auto profile = detail::env_path(env, "USERPROFILE", os, false);
            if (!roaming && profile)
                roaming = *profile / "AppData" / "Roaming";
            if (!local && profile)
                local = *profile / "AppData" / "Local";
            if (!roaming || !local)
                return std::unexpected(
                    text::Error{"cannot locate the user profile (APPDATA, LOCALAPPDATA and USERPROFILE are unset); set LEARNDC_HOME or pass --home"});
            dirs.config_dir = *roaming / "learndc";
            dirs.data_dir = *local / "learndc";
            dirs.cache_dir = dirs.data_dir / "cache";
            dirs.config_file = dirs.config_dir / "config.json";
            dirs.progress_file = dirs.data_dir / "progress.json";
            dirs.progress_lock = dirs.data_dir / "progress.lock";
            dirs.workspaces_dir = dirs.data_dir / "workspaces";
            dirs.backups_dir = dirs.data_dir / "backups";
        }
        else
        {
            auto const home_dir = detail::env_path(env, "HOME", os, true);
            auto config_base = detail::env_path(env, "XDG_CONFIG_HOME", os, true);
            auto data_base = detail::env_path(env, "XDG_DATA_HOME", os, true);
            auto cache_base = detail::env_path(env, "XDG_CACHE_HOME", os, true);
            if (!config_base && home_dir)
                config_base = *home_dir / ".config";
            if (!data_base && home_dir)
                data_base = *home_dir / ".local" / "share";
            if (!cache_base && home_dir)
                cache_base = *home_dir / ".cache";
            if (!config_base || !data_base || !cache_base)
                return std::unexpected(text::Error{"cannot locate the home directory (HOME is unset or not absolute); set LEARNDC_HOME or pass --home"});
            dirs.config_dir = *config_base / "learndc";
            dirs.data_dir = *data_base / "learndc";
            dirs.cache_dir = *cache_base / "learndc";
            dirs.config_file = dirs.config_dir / "config.json";
            dirs.progress_file = dirs.data_dir / "progress.json";
            dirs.progress_lock = dirs.data_dir / "progress.lock";
            dirs.workspaces_dir = dirs.data_dir / "workspaces";
            dirs.backups_dir = dirs.data_dir / "backups";
        }
        dirs.staging_dir = dirs.cache_dir / "staging";
        dirs.logs_dir = dirs.cache_dir / "logs";
        return dirs;
    }

    [[nodiscard]] inline std::optional<std::filesystem::path> executable_path()
    {
#if defined(_WIN32)
        std::wstring buffer(260, L'\0');
        while (true)
        {
            DWORD const n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (n == 0)
                return std::nullopt;
            if (n < buffer.size())
            {
                buffer.resize(n);
                return std::filesystem::path(buffer);
            }
            if (buffer.size() > 65536)
                return std::nullopt;
            buffer.resize(buffer.size() * 2);
        }
#else
        std::string buffer(512, '\0');
        while (true)
        {
            auto const n = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
            if (n < 0)
                return std::nullopt;
            if (static_cast<std::size_t>(n) < buffer.size())
            {
                buffer.resize(static_cast<std::size_t>(n));
                return std::filesystem::path(buffer);
            }
            if (buffer.size() > 65536)
                return std::nullopt;
            buffer.resize(buffer.size() * 2);
        }
#endif
    }

    namespace detail
    {
        [[nodiscard]] inline bool is_executable_file(std::filesystem::path const& p)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(p, ec) || ec)
                return false;
#if defined(_WIN32)
            return true;
#else
            return ::access(p.c_str(), X_OK) == 0;
#endif
        }
    } // namespace detail

    [[nodiscard]] inline std::optional<std::filesystem::path> find_program(std::string_view name, Env const& env, Os os = host_os())
    {
        if (name.empty())
            return std::nullopt;
        std::vector<std::string> suffixes{""};
        if (os == Os::Windows)
        {
            std::string pathext = detail::env_lookup(env, "PATHEXT", os).value_or(".COM;.EXE;.BAT;.CMD");
            if (pathext.empty())
                pathext = ".COM;.EXE;.BAT;.CMD";
            bool has_ext = false;
            auto const lowered = text::lower(name);
            std::vector<std::string> exts;
            for (auto& ext : text::split(pathext, ';'))
                if (!ext.empty())
                    exts.push_back(text::lower(ext));
            if (std::ranges::find(exts, std::string(".exe")) == exts.end())
                exts.push_back(".exe");
            for (auto const& ext : exts)
                if (lowered.size() > ext.size() && lowered.ends_with(ext))
                    has_ext = true;
            if (!has_ext)
            {
                suffixes.clear();
                for (auto const& ext : exts)
                    suffixes.push_back(ext);
            }
        }
        auto const try_in = [&](std::filesystem::path const& dir) -> std::optional<std::filesystem::path> {
            for (auto const& suffix : suffixes)
            {
                auto candidate = dir / std::filesystem::path(std::string(name) + suffix);
                if (detail::is_executable_file(candidate))
                    return candidate;
            }
            return std::nullopt;
        };
        bool const has_separator = name.find('/') != std::string_view::npos || (os == Os::Windows && name.find('\\') != std::string_view::npos);
        if (has_separator)
            return try_in({});
        auto const path_var = detail::env_lookup(env, "PATH", os);
        if (!path_var)
            return std::nullopt;
        for (auto const& dir : text::split(*path_var, os == Os::Windows ? ';' : ':'))
        {
            if (dir.empty())
                continue;
            if (auto found = try_in(path_from_utf8(dir)))
                return found;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::filesystem::path> find_program(std::string_view name)
    {
        return find_program(name, current_env());
    }

    [[nodiscard]] inline std::optional<std::string> read_file(std::filesystem::path const& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad())
            return std::nullopt;
        return data;
    }

    [[nodiscard]] inline std::expected<void, text::Error> atomic_write(std::filesystem::path const& path, std::string_view bytes)
    {
        static std::atomic<unsigned> counter{0};
        auto parent = path.parent_path();
        std::error_code ec;
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
                return std::unexpected(text::Error{std::format("cannot create '{}': {}", path_to_utf8(parent), ec.message())});
        }
#if defined(_WIN32)
        auto const pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
        auto const pid = static_cast<unsigned long>(::getpid());
#endif
        auto const tmp_name = std::format("{}.tmp.{}.{}", path_to_utf8(path.filename()), pid, counter.fetch_add(1));
        auto const tmp = parent / path_from_utf8(tmp_name);
        auto const fail = [&](std::string_view what) -> std::unexpected<text::Error> {
            return std::unexpected(text::Error{std::format("cannot {} '{}': {}", what, path_to_utf8(path), detail::last_error_message())});
        };
#if defined(_WIN32)
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return fail("write");
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            DWORD const chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(h, bytes.data() + offset, chunk, &written, nullptr) || written == 0)
            {
                auto error = fail("write");
                CloseHandle(h);
                DeleteFileW(tmp.c_str());
                return error;
            }
            offset += written;
        }
        if (!FlushFileBuffers(h))
        {
            auto error = fail("flush");
            CloseHandle(h);
            DeleteFileW(tmp.c_str());
            return error;
        }
        CloseHandle(h);
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            auto error = fail("replace");
            DeleteFileW(tmp.c_str());
            return error;
        }
        return {};
#else
        int const fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0)
            return fail("write");
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            auto const n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                auto error = fail("write");
                ::close(fd);
                ::unlink(tmp.c_str());
                return error;
            }
            offset += static_cast<std::size_t>(n);
        }
        if (::fsync(fd) != 0)
        {
            auto error = fail("flush");
            ::close(fd);
            ::unlink(tmp.c_str());
            return error;
        }
        ::close(fd);
        if (::rename(tmp.c_str(), path.c_str()) != 0)
        {
            auto error = fail("replace");
            ::unlink(tmp.c_str());
            return error;
        }
        int const dir_fd = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_CLOEXEC);
        if (dir_fd >= 0)
        {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }
        return {};
#endif
    }

    struct CreateError
    {
        bool exists = false;
        std::string message;
    };

    [[nodiscard]] inline std::expected<void, CreateError> create_new_file(std::filesystem::path const& path, std::string_view bytes)
    {
        auto const fail = [&](bool exists) -> std::unexpected<CreateError> {
            return std::unexpected(CreateError{exists, std::format("cannot create '{}': {}", path_to_utf8(path), detail::last_error_message())});
        };
#if defined(_WIN32)
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            DWORD const code = GetLastError();
            return fail(code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS);
        }
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            DWORD const chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(h, bytes.data() + offset, chunk, &written, nullptr) || written == 0)
            {
                auto error = fail(false);
                CloseHandle(h);
                DeleteFileW(path.c_str());
                return error;
            }
            offset += written;
        }
        FlushFileBuffers(h);
        CloseHandle(h);
        return {};
#else
        int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0)
            return fail(errno == EEXIST);
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            auto const n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                auto error = fail(false);
                ::close(fd);
                ::unlink(path.c_str());
                return error;
            }
            offset += static_cast<std::size_t>(n);
        }
        ::fsync(fd);
        ::close(fd);
        return {};
#endif
    }

    struct LockError
    {
        bool busy = false;
        std::string message;
    };

    class FileLock
    {
    public:
        FileLock() = default;
        FileLock(FileLock const&) = delete;
        FileLock& operator=(FileLock const&) = delete;

        FileLock(FileLock&& other) noexcept : m_handle(std::exchange(other.m_handle, -1)) {}

        FileLock& operator=(FileLock&& other) noexcept
        {
            if (this != &other)
            {
                release();
                m_handle = std::exchange(other.m_handle, -1);
            }
            return *this;
        }

        ~FileLock() { release(); }

        [[nodiscard]] bool locked() const { return m_handle != -1; }

        void release()
        {
            if (m_handle == -1)
                return;
#if defined(_WIN32)
            HANDLE h = reinterpret_cast<HANDLE>(m_handle);
            OVERLAPPED ov{};
            UnlockFileEx(h, 0, 1, 0, &ov);
            CloseHandle(h);
#else
            ::flock(static_cast<int>(m_handle), LOCK_UN);
            ::close(static_cast<int>(m_handle));
#endif
            m_handle = -1;
        }

        [[nodiscard]] static std::expected<FileLock, LockError> acquire(std::filesystem::path const& path, std::chrono::milliseconds timeout)
        {
            auto parent = path.parent_path();
            if (!parent.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec)
                    return std::unexpected(LockError{false, std::format("cannot create '{}': {}", path_to_utf8(parent), ec.message())});
            }
            auto const deadline = std::chrono::steady_clock::now() + timeout;
#if defined(_WIN32)
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return std::unexpected(LockError{false, std::format("cannot open lock file '{}': {}", path_to_utf8(path), detail::last_error_message())});
            while (true)
            {
                OVERLAPPED ov{};
                if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov))
                {
                    FileLock lock;
                    lock.m_handle = reinterpret_cast<std::intptr_t>(h);
                    return lock;
                }
                DWORD const error = GetLastError();
                if (error != ERROR_LOCK_VIOLATION && error != ERROR_IO_PENDING)
                {
                    auto message = std::format("cannot lock '{}': {}", path_to_utf8(path), detail::last_error_message());
                    CloseHandle(h);
                    return std::unexpected(LockError{false, std::move(message)});
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    CloseHandle(h);
                    return std::unexpected(LockError{true, std::format("'{}' is locked by another process", path_to_utf8(path))});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
#else
            int const fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0)
                return std::unexpected(LockError{false, std::format("cannot open lock file '{}': {}", path_to_utf8(path), detail::last_error_message())});
            while (true)
            {
                if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
                {
                    FileLock lock;
                    lock.m_handle = fd;
                    return lock;
                }
                int const error = errno;
                if (error != EWOULDBLOCK && error != EINTR)
                {
                    auto message = std::format("cannot lock '{}': {}", path_to_utf8(path), detail::last_error_message());
                    ::close(fd);
                    return std::unexpected(LockError{false, std::move(message)});
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    ::close(fd);
                    return std::unexpected(LockError{true, std::format("'{}' is locked by another process", path_to_utf8(path))});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
#endif
        }

        [[nodiscard]] static std::expected<FileLock, LockError> try_acquire(std::filesystem::path const& path)
        {
            return acquire(path, std::chrono::milliseconds(0));
        }

    private:
        std::intptr_t m_handle = -1;
    };

    [[nodiscard]] inline bool stdin_is_tty()
    {
#if defined(_WIN32)
        DWORD mode = 0;
        return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
        return ::isatty(0) == 1;
#endif
    }

    [[nodiscard]] inline bool stdout_is_tty()
    {
#if defined(_WIN32)
        DWORD mode = 0;
        return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
#else
        return ::isatty(1) == 1;
#endif
    }

    [[nodiscard]] inline bool stderr_is_tty()
    {
#if defined(_WIN32)
        DWORD mode = 0;
        return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
#else
        return ::isatty(2) == 1;
#endif
    }

    struct TerminalSize
    {
        int columns = 0;
        int rows = 0;
    };

    [[nodiscard]] inline std::optional<TerminalSize> terminal_size()
    {
#if defined(_WIN32)
        CONSOLE_SCREEN_BUFFER_INFO info{};
        for (DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE})
            if (GetConsoleScreenBufferInfo(GetStdHandle(which), &info))
                return TerminalSize{info.srWindow.Right - info.srWindow.Left + 1, info.srWindow.Bottom - info.srWindow.Top + 1};
        return std::nullopt;
#else
        for (int fd : {1, 2, 0})
        {
            winsize ws{};
            if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
                return TerminalSize{static_cast<int>(ws.ws_col), static_cast<int>(ws.ws_row)};
        }
        return std::nullopt;
#endif
    }

    [[nodiscard]] inline bool utf8_locale(Env const& env)
    {
        if (host_os() == Os::Windows)
            return true;
        for (std::string_view name : {"LC_ALL", "LC_CTYPE", "LANG"})
        {
            auto value = detail::env_lookup(env, name, Os::Linux);
            if (value && !value->empty())
                return text::contains_ci(*value, "utf-8") || text::contains_ci(*value, "utf8");
        }
        return false;
    }

    [[nodiscard]] inline bool utf8_locale()
    {
        return utf8_locale(current_env());
    }

    [[nodiscard]] inline std::string format_utc_iso8601(std::chrono::system_clock::time_point when)
    {
        return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(when));
    }

    [[nodiscard]] inline std::string now_utc_iso8601()
    {
        return format_utc_iso8601(std::chrono::system_clock::now());
    }

    [[nodiscard]] inline bool is_symlink_or_reparse(std::filesystem::path const& path)
    {
#if defined(_WIN32)
        DWORD const attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        struct stat st{};
        return ::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
#endif
    }

    [[nodiscard]] inline std::expected<void, text::Error> spawn_detached(std::vector<std::string> const& argv, std::filesystem::path const& cwd)
    {
        if (argv.empty() || argv.front().empty())
            return std::unexpected(text::Error{"cannot start an empty command"});
#if defined(_WIN32)
        std::string program = argv.front();
        if (auto resolved = find_program(program))
            program = path_to_utf8(*resolved);
        std::string command_line = dcc::host::process::windows_quote(program);
        for (std::size_t i = 1; i < argv.size(); ++i)
        {
            command_line += ' ';
            command_line += dcc::host::process::windows_quote(argv[i]);
        }
        auto wide_command = detail::widen(command_line);
        wide_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION info{};
        std::wstring wide_cwd = cwd.empty() ? std::wstring() : cwd.wstring();
        BOOL const ok = CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                                       wide_cwd.empty() ? nullptr : wide_cwd.c_str(), &startup, &info);
        if (!ok)
            return std::unexpected(text::Error{std::format("cannot start '{}': {}", argv.front(), detail::last_error_message())});
        CloseHandle(info.hProcess);
        CloseHandle(info.hThread);
        return {};
#else
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (auto const& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        std::string const cwd_text = cwd.string();
        int fds[2];
        if (::pipe(fds) != 0)
            return std::unexpected(text::Error{std::format("cannot start '{}': {}", argv.front(), detail::last_error_message())});
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        pid_t const pid = ::fork();
        if (pid < 0)
        {
            auto message = std::format("cannot start '{}': {}", argv.front(), detail::last_error_message());
            ::close(fds[0]);
            ::close(fds[1]);
            return std::unexpected(text::Error{std::move(message)});
        }
        if (pid == 0)
        {
            int failure = 0;
            if (::setsid() < 0)
                failure = errno;
            pid_t second = failure == 0 ? ::fork() : 0;
            if (failure == 0 && second < 0)
                failure = errno;
            if (failure == 0 && second > 0)
                ::_exit(0);
            if (failure == 0)
            {
                int const devnull = ::open("/dev/null", O_RDWR);
                if (devnull >= 0)
                {
                    ::dup2(devnull, 0);
                    ::dup2(devnull, 1);
                    ::dup2(devnull, 2);
                    if (devnull > 2)
                        ::close(devnull);
                }
                if (!cwd_text.empty() && ::chdir(cwd_text.c_str()) != 0)
                    failure = errno;
                else
                {
                    ::execvp(args[0], args.data());
                    failure = errno;
                }
            }
            ssize_t const ignored = ::write(fds[1], &failure, sizeof failure);
            (void)ignored;
            ::_exit(127);
        }
        ::close(fds[1]);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
        {
        }
        int failure = 0;
        ssize_t got = 0;
        while (true)
        {
            got = ::read(fds[0], &failure, sizeof failure);
            if (got < 0 && errno == EINTR)
                continue;
            break;
        }
        ::close(fds[0]);
        if (got == static_cast<ssize_t>(sizeof failure))
            return std::unexpected(text::Error{std::format("cannot start '{}': {}", argv.front(), std::generic_category().message(failure))});
        return {};
#endif
    }
} // namespace learndc::platform
