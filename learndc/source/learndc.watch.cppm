module;

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

export module learndc.watch;

import std;

export namespace learndc::watch
{
    struct Entry
    {
        std::uintmax_t size = 0;
        std::int64_t mtime_ns = 0;
        std::uint64_t inode = 0;

        friend bool operator==(Entry const&, Entry const&) = default;
    };

    using Snapshot = std::map<std::string, Entry>;

    struct Changes
    {
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::vector<std::string> modified;

        [[nodiscard]] bool empty() const { return added.empty() && removed.empty() && modified.empty(); }

        [[nodiscard]] std::size_t count() const { return added.size() + removed.size() + modified.size(); }
    };

    [[nodiscard]] inline bool ignored_name(std::string_view name)
    {
        if (name.empty())
            return true;
        if (name == "4913" || name == ".DS_Store")
            return true;
        if (name.starts_with(".#"))
            return true;
        if (name.starts_with("#") && name.ends_with("#"))
            return true;
        if (name.ends_with("~") || name.ends_with(".swp") || name.ends_with(".swo") || name.ends_with(".swx") || name.ends_with(".tmp") ||
            name.ends_with(".bak"))
            return true;
        if (name.starts_with(".") && name.contains(".sw") && name.size() > 4)
            return true;
        return false;
    }

    [[nodiscard]] inline bool ignored(std::string_view relative)
    {
        std::size_t start = 0;
        bool first = true;
        while (start <= relative.size())
        {
            auto const slash = relative.find('/', start);
            auto const part = relative.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (first && (part == ".dpm" || part == ".learndc"))
                return true;
            if (slash == std::string_view::npos)
                return ignored_name(part);
            first = false;
            start = slash + 1;
        }
        return false;
    }

    [[nodiscard]] inline Snapshot snapshot(std::filesystem::path const& root)
    {
        Snapshot out;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator const end;
        for (; !ec && it != end; it.increment(ec))
        {
            std::error_code local;
            auto const rel = std::filesystem::relative(it->path(), root, local);
            if (local)
                continue;
            auto const generic_u8 = rel.generic_u8string();
            std::string const generic(reinterpret_cast<char const*>(generic_u8.data()), generic_u8.size());
            auto const status = it->symlink_status(local);
            if (local)
                continue;
            if (status.type() == std::filesystem::file_type::directory)
            {
                if (ignored(generic))
                    it.disable_recursion_pending();
                continue;
            }
            if (status.type() != std::filesystem::file_type::regular || ignored(generic))
                continue;
            Entry entry;
            entry.size = it->file_size(local);
            if (local)
                continue;
            auto const mtime = it->last_write_time(local);
            if (local)
                continue;
            entry.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();
#if !defined(_WIN32)
            struct stat st{};
            if (::stat(it->path().c_str(), &st) == 0)
                entry.inode = static_cast<std::uint64_t>(st.st_ino);
#endif
            out.emplace(generic, entry);
        }
        return out;
    }

    [[nodiscard]] inline Changes compare(Snapshot const& before, Snapshot const& after)
    {
        Changes changes;
        for (auto const& [path, entry] : after)
        {
            auto const it = before.find(path);
            if (it == before.end())
                changes.added.push_back(path);
            else if (!(it->second == entry))
                changes.modified.push_back(path);
        }
        for (auto const& [path, entry] : before)
            if (!after.contains(path))
                changes.removed.push_back(path);
        return changes;
    }

    struct Options
    {
        std::chrono::milliseconds interval{150};
        std::chrono::milliseconds debounce{400};
        std::atomic<bool> const* cancel = nullptr;
    };

    class Watcher
    {
    public:
        using Clock = std::chrono::steady_clock;

        Watcher(std::filesystem::path root, Options options)
            : m_root(std::move(root)), m_options(options), m_baseline(snapshot(m_root)), m_last_seen(m_baseline)
        {
        }

        [[nodiscard]] Snapshot const& baseline() const { return m_baseline; }

        [[nodiscard]] bool pending() const { return m_pending; }

        [[nodiscard]] std::optional<Changes> poll(Clock::time_point now)
        {
            auto current = snapshot(m_root);
            if (current != m_last_seen)
            {
                m_last_seen = std::move(current);
                m_changed_at = now;
                m_pending = !compare(m_baseline, m_last_seen).empty();
                return std::nullopt;
            }
            if (!m_pending || now - m_changed_at < m_options.debounce)
                return std::nullopt;
            auto changes = compare(m_baseline, m_last_seen);
            m_baseline = m_last_seen;
            m_pending = false;
            if (changes.empty())
                return std::nullopt;
            return changes;
        }

        [[nodiscard]] std::optional<Changes> wait()
        {
            for (;;)
            {
                if (m_options.cancel && m_options.cancel->load(std::memory_order_relaxed))
                    return std::nullopt;
                if (auto changes = poll(Clock::now()))
                    return changes;
                std::this_thread::sleep_for(m_options.interval);
            }
        }

        void rebase()
        {
            m_baseline = snapshot(m_root);
            m_last_seen = m_baseline;
            m_pending = false;
        }

    private:
        std::filesystem::path m_root;
        Options m_options;
        Snapshot m_baseline;
        Snapshot m_last_seen;
        Clock::time_point m_changed_at{};
        bool m_pending = false;
    };
} // namespace learndc::watch
