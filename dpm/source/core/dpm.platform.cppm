module;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

export module dpm.platform;

import std;

export namespace dpm::platform
{
    inline constexpr char const* null_device =
#ifdef _WIN32
        "NUL";
#else
        "/dev/null";
#endif
    inline constexpr char const* disabled_askpass =
#ifdef _WIN32
        "";
#else
        "/bin/false";
#endif

    [[nodiscard]] inline std::uint64_t process_id()
    {
#ifdef _WIN32
        return ::GetCurrentProcessId();
#else
        return static_cast<std::uint64_t>(::getpid());
#endif
    }

    [[nodiscard]] inline bool replace(std::filesystem::path const& from, std::filesystem::path const& to, std::error_code& ec)
    {
#ifdef _WIN32
        if (!::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            ec = {static_cast<int>(::GetLastError()), std::system_category()};
        else
            ec.clear();
#else
        std::filesystem::rename(from, to, ec);
#endif
        return !ec;
    }

    [[nodiscard]] inline bool executable(std::filesystem::path const& path)
    {
#ifdef _WIN32
        auto metadata = path;
        metadata += L":dpm.executable";
        std::ifstream in{metadata, std::ios::binary};
        return in.get() == '1';
#else
        std::error_code ec;
        return (std::filesystem::status(path, ec).permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none && !ec;
#endif
    }

    [[nodiscard]] inline std::optional<bool> multiple_links(std::filesystem::path const& path)
    {
#ifdef _WIN32
        auto handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::nullopt;
        BY_HANDLE_FILE_INFORMATION info{};
        bool ok = ::GetFileInformationByHandle(handle, &info) != 0;
        ::CloseHandle(handle);
        if (!ok)
            return std::nullopt;
        return info.nNumberOfLinks > 1;
#else
        struct stat info{};
        if (::lstat(path.c_str(), &info) != 0)
            return std::nullopt;
        return info.st_nlink > 1;
#endif
    }

    [[nodiscard]] inline bool valid_component(std::string_view component)
    {
#ifdef _WIN32
        if (!::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, component.data(), static_cast<int>(component.size()), nullptr, 0))
            return false;
        if (component.empty() || component.back() == '.' || component.back() == ' ' || component.find_first_of("<>:\"|?*") != std::string_view::npos)
            return false;
        std::string base{component.substr(0, component.find('.'))};
        std::ranges::transform(base, base.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' && base[3] <= '9'))
            return false;
#else
        std::ignore = component;
#endif
        return true;
    }

    struct PathLess
    {
        bool operator()(std::string const& a, std::string const& b) const
        {
#ifdef _WIN32
            auto left = std::filesystem::path{std::u8string{a.begin(), a.end()}}.native();
            auto right = std::filesystem::path{std::u8string{b.begin(), b.end()}}.native();
            return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
#else
            return a < b;
#endif
        }
    };

    [[nodiscard]] inline bool set_executable(std::filesystem::path const& path, bool value)
    {
#ifdef _WIN32
        auto metadata = path;
        metadata += L":dpm.executable";
        if (!value)
        {
            if (::DeleteFileW(metadata.c_str()))
                return true;
            return ::GetLastError() == ERROR_FILE_NOT_FOUND;
        }
        std::ofstream out{metadata, std::ios::binary | std::ios::trunc};
        out.put('1');
        out.close();
        return static_cast<bool>(out);
#else
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                     value ? std::filesystem::perm_options::add : std::filesystem::perm_options::remove, ec);
        return !ec;
#endif
    }

    class File
    {
    public:
        File() = default;
        File(File const&) = delete;
        File& operator=(File const&) = delete;
        File(File&& other) noexcept : handle_{std::exchange(other.handle_, invalid)} {}
        File& operator=(File&& other) noexcept
        {
            if (this != &other)
            {
                close();
                handle_ = std::exchange(other.handle_, invalid);
            }
            return *this;
        }
        ~File() { close(); }
        explicit operator bool() const { return handle_ != invalid; }
        static File create(std::filesystem::path const& path, bool exec = false)
        {
            File file;
#ifdef _WIN32
            file.handle_ = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
            file.handle_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, exec ? 0755 : 0644);
#endif
#ifdef _WIN32
            if (file && exec && !set_executable(path, true))
            {
                file.close();
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
#endif
            return file;
        }
        bool write(std::string_view data)
        {
            while (!data.empty())
            {
#ifdef _WIN32
                DWORD count{};
                if (!::WriteFile(handle_, data.data(), static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 30)), &count, nullptr) || !count)
                    return false;
#else
                auto count = ::write(handle_, data.data(), data.size());
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return false;
#endif
                data.remove_prefix(static_cast<std::size_t>(count));
            }
            return true;
        }
        bool sync()
        {
#ifdef _WIN32
            return ::FlushFileBuffers(handle_) != 0;
#else
            return ::fsync(handle_) == 0;
#endif
        }
        void close()
        {
            if (handle_ != invalid)
            {
#ifdef _WIN32
                ::CloseHandle(handle_);
#else
                ::close(handle_);
#endif
                handle_ = invalid;
            }
        }
    private:
#ifdef _WIN32
        static inline HANDLE const invalid = INVALID_HANDLE_VALUE;
        HANDLE handle_{invalid};
#else
        static constexpr int invalid = -1;
        int handle_{invalid};
#endif
    };

    class FileLock
    {
    public:
        FileLock() = default;
        FileLock(FileLock const&) = delete;
        FileLock& operator=(FileLock const&) = delete;
        FileLock(FileLock&& other) noexcept : handle_{std::exchange(other.handle_, invalid)} {}
        FileLock& operator=(FileLock&& other) noexcept
        {
            if (this != &other)
            {
                release();
                handle_ = std::exchange(other.handle_, invalid);
            }
            return *this;
        }
        ~FileLock() { release(); }
        static std::optional<FileLock> acquire(std::filesystem::path const& path)
        {
            FileLock lock;
#ifdef _WIN32
            lock.handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            OVERLAPPED position{};
            if (lock.handle_ == invalid || !::LockFileEx(lock.handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &position))
                return std::nullopt;
#else
            lock.handle_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (lock.handle_ < 0 || ::flock(lock.handle_, LOCK_EX) != 0)
                return std::nullopt;
#endif
            return lock;
        }
        void release()
        {
            if (handle_ == invalid)
                return;
#ifdef _WIN32
            OVERLAPPED position{};
            ::UnlockFileEx(handle_, 0, 1, 0, &position);
            ::CloseHandle(handle_);
#else
            ::flock(handle_, LOCK_UN);
            ::close(handle_);
#endif
            handle_ = invalid;
        }
    private:
#ifdef _WIN32
        static inline HANDLE const invalid = INVALID_HANDLE_VALUE;
        HANDLE handle_{invalid};
#else
        static constexpr int invalid = -1;
        int handle_{invalid};
#endif
    };

    [[nodiscard]] inline bool atomic_write(std::filesystem::path const& path, std::string_view content, std::string& error)
    {
        static std::atomic<std::uint64_t> serial{};
        auto temporary = path;
        temporary += std::format(".dpm-tmp-{}-{}", process_id(), serial.fetch_add(1));
        struct Cleanup
        {
            std::filesystem::path path;
            bool owned{};
            ~Cleanup() { if (owned) { std::error_code ec; std::filesystem::remove(path, ec); } }
        } cleanup{temporary};
        auto file = File::create(temporary, executable(path));
        cleanup.owned = static_cast<bool>(file);
        if (!file || !file.write(content) || !file.sync())
        {
            error = "cannot write/sync " + temporary.string();
            return false;
        }
#ifndef _WIN32
        std::error_code mode_ec;
        auto mode = std::filesystem::status(path, mode_ec).permissions();
        if (!mode_ec)
            std::filesystem::permissions(temporary, mode, mode_ec);
#endif
        file.close();
        std::error_code ec;
        if (!replace(temporary, path, ec))
        {
            error = "cannot replace " + path.string() + ": " + ec.message();
            return false;
        }
        return true;
    }
}
