module;

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

export module dpm.cache;

import std;
import dpm.hash;
import dpm.extract;

export namespace dpm::cache
{
    [[nodiscard]] inline std::filesystem::path default_root()
    {
        auto env = [](char const* name) -> std::optional<std::string>
        {
            char const* value = std::getenv(name);
            if (value && *value)
                return std::string{value};
            return std::nullopt;
        };
        if (auto home = env("DPM_HOME"))
            return *home;
#if defined(_WIN32)
        if (auto local = env("LOCALAPPDATA"))
            return std::filesystem::path{*local} / "dpm";
#endif
        if (auto xdg = env("XDG_CACHE_HOME"))
            return std::filesystem::path{*xdg} / "dpm";
        if (auto home = env("HOME"))
            return std::filesystem::path{*home} / ".cache" / "dpm";
        return std::filesystem::path{".dpm-cache"};
    }

    [[nodiscard]] inline bool is_hex_digest(std::string_view s)
    {
        return s.size() == 64 && std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    namespace detail
    {
        inline void set_read_only(std::filesystem::path const& root)
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            auto strip = [&](fs::path const& p)
            {
                fs::permissions(p, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove, ec);
            };
            for (fs::recursive_directory_iterator it{root, ec}, end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec))
                    strip(it->path());
            std::vector<fs::path> dirs{root};
            for (fs::recursive_directory_iterator it{root, ec}, end; !ec && it != end; it.increment(ec))
                if (it->is_directory(ec))
                    dirs.push_back(it->path());
            for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
                strip(*it);
        }

        inline void remove_tree(std::filesystem::path const& root)
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(fs::symlink_status(root, ec)))
                return;
            fs::permissions(root, fs::perms::owner_write | fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add, ec);
            if (fs::is_directory(fs::symlink_status(root, ec)))
                for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec}, end; !ec && it != end; it.increment(ec))
                    if (it->is_directory(ec) && !it->is_symlink(ec))
                        fs::permissions(it->path(), fs::perms::owner_write | fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add, ec);
            fs::remove_all(root, ec);
        }
    } // namespace detail

    struct InstallResult
    {
        bool ok{};
        bool existed{};
        std::string hash;
        std::filesystem::path path;
        std::vector<extract::Problem> problems;
        std::string error;
    };

    struct VerifyEntry
    {
        std::string name;
        bool ok{};
        std::string message;
    };

    struct CleanResult
    {
        std::vector<std::string> removed;
    };

    class FileLock
    {
    public:
        FileLock() = default;
        FileLock(FileLock const&) = delete;
        FileLock& operator=(FileLock const&) = delete;
        FileLock(FileLock&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
        FileLock& operator=(FileLock&& other) noexcept
        {
            if (this != &other)
            {
                release();
                fd_ = std::exchange(other.fd_, -1);
            }
            return *this;
        }
        ~FileLock() { release(); }

        [[nodiscard]] static std::optional<FileLock> acquire(std::filesystem::path const& path)
        {
            FileLock lock;
#if !defined(_WIN32)
            lock.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (lock.fd_ < 0 || ::flock(lock.fd_, LOCK_EX) != 0)
                return std::nullopt;
#endif
            return lock;
        }

        void release()
        {
#if !defined(_WIN32)
            if (fd_ >= 0)
            {
                ::flock(fd_, LOCK_UN);
                ::close(fd_);
            }
#endif
            fd_ = -1;
        }

    private:
        int fd_{-1};
    };

    class Cache
    {
    public:
        explicit Cache(std::filesystem::path root) : root_{std::move(root)} {}

        [[nodiscard]] std::filesystem::path const& root() const { return root_; }
        [[nodiscard]] std::filesystem::path registry_dir() const { return root_ / "registry"; }
        [[nodiscard]] std::filesystem::path git_dir() const { return root_ / "git"; }
        [[nodiscard]] std::filesystem::path download_dir() const { return root_ / "dl"; }
        [[nodiscard]] std::filesystem::path src_dir() const { return root_ / "src"; }
        [[nodiscard]] std::filesystem::path tmp_dir() const { return root_ / "tmp"; }
        [[nodiscard]] std::filesystem::path lock_path() const { return root_ / "lock"; }

        [[nodiscard]] std::filesystem::path src_path(std::string_view hash) const
        {
            if (hash.starts_with("sha256:"))
                hash.remove_prefix(7);
            return src_dir() / std::string{hash};
        }

        [[nodiscard]] bool ensure(std::string& error) const
        {
            std::error_code ec;
            for (auto const& dir : {registry_dir(), git_dir(), download_dir(), src_dir(), tmp_dir()})
            {
                std::filesystem::create_directories(dir, ec);
                if (ec)
                {
                    error = "cannot create " + dir.string();
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::optional<FileLock> lock() const
        {
            std::error_code ec;
            std::filesystem::create_directories(root_, ec);
            return FileLock::acquire(lock_path());
        }

        [[nodiscard]] std::optional<std::filesystem::path> new_staging(std::string& error) const
        {
            if (!ensure(error))
                return std::nullopt;
            static std::atomic<unsigned> counter{0};
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                auto path = tmp_dir() / std::format("{}-{}-{}", ::getpid(), stamp, counter.fetch_add(1));
                std::error_code ec;
                if (std::filesystem::create_directory(path, ec))
                    return path;
            }
            error = "cannot create a staging directory in " + tmp_dir().string();
            return std::nullopt;
        }

        [[nodiscard]] InstallResult install(std::filesystem::path const& staging, std::string_view expected_hash = {}, extract::Limits const& limits = {}) const
        {
            InstallResult result;
            auto fail = [&](std::string message)
            {
                detail::remove_tree(staging);
                result.error = std::move(message);
                return result;
            };
            std::string error;
            if (!ensure(error))
                return fail(error);
            result.problems = extract::check_tree(staging, limits);
            if (!result.problems.empty())
                return fail("unsafe package contents: " + result.problems.front().format());
            auto tree = hash::tree_hash(staging);
            if (!tree.ok)
                return fail(tree.error);
            result.hash = tree.hash;
            if (!expected_hash.empty() && expected_hash != tree.hash)
                return fail(std::format("hash mismatch: expected {}, got {}", expected_hash, tree.hash));
            result.path = src_path(tree.hash);
            std::error_code ec;
            if (std::filesystem::exists(result.path, ec))
            {
                detail::remove_tree(staging);
                result.existed = true;
                result.ok = true;
                return result;
            }
            std::filesystem::rename(staging, result.path, ec);
            if (ec)
            {
                if (std::filesystem::exists(result.path))
                {
                    detail::remove_tree(staging);
                    result.existed = true;
                    result.ok = true;
                    return result;
                }
                return fail("cannot move package into " + result.path.string());
            }
            detail::set_read_only(result.path);
            result.ok = true;
            return result;
        }

        [[nodiscard]] std::vector<VerifyEntry> verify() const
        {
            std::vector<VerifyEntry> entries;
            std::error_code ec;
            if (!std::filesystem::is_directory(src_dir(), ec))
                return entries;
            for (auto const& item : std::filesystem::directory_iterator{src_dir(), ec})
            {
                auto name = item.path().filename().string();
                VerifyEntry entry{name, false, {}};
                if (!is_hex_digest(name))
                    entry.message = "directory name is not a sha256 digest";
                else if (!item.is_directory(ec) || item.is_symlink(ec))
                    entry.message = "not a directory";
                else if (auto problems = extract::check_tree(item.path()); !problems.empty())
                    entry.message = "unsafe contents: " + problems.front().format();
                else
                {
                    auto tree = hash::tree_hash(item.path());
                    if (!tree.ok)
                        entry.message = tree.error;
                    else if (tree.hash != "sha256:" + name)
                        entry.message = "contents hash to " + tree.hash;
                    else
                        entry.ok = true;
                }
                entries.push_back(std::move(entry));
            }
            std::ranges::sort(entries, [](VerifyEntry const& a, VerifyEntry const& b) { return a.name < b.name; });
            return entries;
        }

        [[nodiscard]] CleanResult clean(bool all) const
        {
            CleanResult result;
            std::vector<std::string> names{"dl", "git", "tmp"};
            if (all)
            {
                names.push_back("registry");
                names.push_back("src");
            }
            for (auto const& name : names)
            {
                auto path = root_ / name;
                std::error_code ec;
                if (!std::filesystem::exists(path, ec))
                    continue;
                detail::remove_tree(path);
                result.removed.push_back(name);
            }
            return result;
        }

    private:
        std::filesystem::path root_;
    };
} // namespace dpm::cache
