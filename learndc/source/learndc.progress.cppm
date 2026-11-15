export module learndc.progress;

import std;
import learndc.text;
import learndc.json;
import learndc.platform;

export namespace learndc::progress
{
    enum class Status
    {
        NotStarted,
        InProgress,
        Completed,
    };

    [[nodiscard]] inline std::string_view to_string(Status s)
    {
        switch (s)
        {
            case Status::NotStarted:
                return "not-started";
            case Status::InProgress:
                return "in-progress";
            case Status::Completed:
                return "completed";
        }
        return "not-started";
    }

    [[nodiscard]] inline std::optional<Status> parse_status(std::string_view s)
    {
        if (s == "not-started")
            return Status::NotStarted;
        if (s == "in-progress")
            return Status::InProgress;
        if (s == "completed")
            return Status::Completed;
        return std::nullopt;
    }

    struct ExerciseProgress
    {
        Status status = Status::NotStarted;
        int attempts = 0;
        int passes = 0;
        int hint_level = 0;
        bool solution_viewed = false;
        std::string first_started_at;
        std::string last_checked_at;
        std::string completed_at;
        int completed_revision = 0;
        int last_passed = 0;
        int last_total = 0;

        friend bool operator==(ExerciseProgress const&, ExerciseProgress const&) = default;
    };

    struct Progress
    {
        int schema = 1;
        std::map<std::string, ExerciseProgress> exercises;
        std::string active_exercise;
        std::vector<std::string> bookmarks;
        std::map<std::string, std::string> workspaces;
        std::vector<std::string> recent;

        friend bool operator==(Progress const&, Progress const&) = default;
    };

    inline constexpr std::size_t max_recent = 10;

    [[nodiscard]] inline bool needs_revisit(ExerciseProgress const& p, int current_revision)
    {
        return p.status == Status::Completed && p.completed_revision < current_revision;
    }

    [[nodiscard]] inline Status status_of(Progress const& p, std::string_view id)
    {
        auto it = p.exercises.find(std::string(id));
        return it == p.exercises.end() ? Status::NotStarted : it->second.status;
    }

    [[nodiscard]] inline bool is_completed(Progress const& p, std::string_view id)
    {
        return status_of(p, id) == Status::Completed;
    }

    inline void touch_recent(Progress& p, std::string_view id)
    {
        std::erase(p.recent, std::string(id));
        p.recent.insert(p.recent.begin(), std::string(id));
        if (p.recent.size() > max_recent)
            p.recent.resize(max_recent);
    }

    inline ExerciseProgress& mark_started(Progress& p, std::string_view id, std::string_view now)
    {
        auto& e = p.exercises[std::string(id)];
        if (e.status == Status::NotStarted)
            e.status = Status::InProgress;
        if (e.first_started_at.empty())
            e.first_started_at = std::string(now);
        touch_recent(p, id);
        return e;
    }

    inline void record_check(Progress& p, std::string_view id, int passed, int total, int revision, std::string_view now)
    {
        auto& e = mark_started(p, id, now);
        ++e.attempts;
        e.last_passed = passed;
        e.last_total = total;
        e.last_checked_at = std::string(now);
        if (total > 0 && passed >= total)
        {
            ++e.passes;
            if (e.status != Status::Completed || e.completed_revision < revision)
            {
                if (e.status != Status::Completed || e.completed_at.empty())
                    e.completed_at = std::string(now);
                e.completed_revision = revision;
            }
            e.status = Status::Completed;
        }
    }

    inline void reveal_hint(Progress& p, std::string_view id, int level, [[maybe_unused]] std::string_view now)
    {
        auto& e = p.exercises[std::string(id)];
        e.hint_level = std::clamp(std::max(e.hint_level, level), 0, 3);
    }

    inline void mark_solution_viewed(Progress& p, std::string_view id, [[maybe_unused]] std::string_view now)
    {
        p.exercises[std::string(id)].solution_viewed = true;
    }

    inline void set_active(Progress& p, std::string_view id)
    {
        p.active_exercise = std::string(id);
    }

    inline bool toggle_bookmark(Progress& p, std::string_view id)
    {
        auto const before = p.bookmarks.size();
        std::erase(p.bookmarks, std::string(id));
        if (p.bookmarks.size() != before)
            return false;
        p.bookmarks.emplace_back(id);
        return true;
    }

    [[nodiscard]] inline bool is_bookmarked(Progress const& p, std::string_view id)
    {
        return std::ranges::find(p.bookmarks, std::string(id)) != p.bookmarks.end();
    }

    [[nodiscard]] inline json::Json to_json(Progress const& p)
    {
        auto const text_or_null = [](std::string const& s) { return s.empty() ? json::Json(nullptr) : json::Json(s); };
        auto root = json::Json::make_object();
        root.set("schema", json::Json(1));
        root.set("active_exercise", text_or_null(p.active_exercise));
        auto exercises = json::Json::make_object();
        for (auto const& [id, e] : p.exercises)
        {
            auto o = json::Json::make_object();
            o.set("status", json::Json(std::string(to_string(e.status))));
            o.set("attempts", json::Json(e.attempts));
            o.set("passes", json::Json(e.passes));
            o.set("hint_level", json::Json(e.hint_level));
            o.set("solution_viewed", json::Json(e.solution_viewed));
            o.set("first_started_at", text_or_null(e.first_started_at));
            o.set("last_checked_at", text_or_null(e.last_checked_at));
            o.set("completed_at", text_or_null(e.completed_at));
            o.set("completed_revision", json::Json(e.completed_revision));
            o.set("last_passed", json::Json(e.last_passed));
            o.set("last_total", json::Json(e.last_total));
            exercises.set(id, std::move(o));
        }
        root.set("exercises", std::move(exercises));
        auto bookmarks = json::Json::make_array();
        for (auto const& b : p.bookmarks)
            bookmarks.push(json::Json(b));
        root.set("bookmarks", std::move(bookmarks));
        auto workspaces = json::Json::make_object();
        for (auto const& [id, path] : p.workspaces)
            workspaces.set(id, json::Json(path));
        root.set("workspaces", std::move(workspaces));
        auto recent = json::Json::make_array();
        for (auto const& r : p.recent)
            recent.push(json::Json(r));
        root.set("recent", std::move(recent));
        return root;
    }

    namespace detail
    {
        [[nodiscard]] inline std::unexpected<text::Error> bad(std::string message)
        {
            return std::unexpected(text::Error{std::move(message)});
        }

        [[nodiscard]] inline std::expected<std::string, text::Error> opt_string(json::Json const& o, std::string_view key)
        {
            auto const* v = o.find(key);
            if (v == nullptr || v->is_null())
                return std::string();
            auto s = v->as_string();
            if (!s)
                return bad(std::format("'{}' must be a string or null", key));
            return std::string(*s);
        }

        [[nodiscard]] inline std::expected<int, text::Error> opt_int(json::Json const& o, std::string_view key, int low, int high)
        {
            auto const* v = o.find(key);
            if (v == nullptr)
                return 0;
            auto n = v->as_int();
            if (!n || *n < low || *n > high)
                return bad(std::format("'{}' must be a whole number from {} to {}", key, low, high));
            return static_cast<int>(*n);
        }

        [[nodiscard]] inline std::expected<std::vector<std::string>, text::Error> string_list(json::Json const& o, std::string_view key)
        {
            std::vector<std::string> out;
            auto const* v = o.find(key);
            if (v == nullptr)
                return out;
            auto const* a = v->as_array();
            if (a == nullptr)
                return bad(std::format("'{}' must be an array", key));
            for (auto const& item : *a)
            {
                auto s = item.as_string();
                if (!s)
                    return bad(std::format("'{}' must contain only strings", key));
                out.emplace_back(*s);
            }
            return out;
        }
    } // namespace detail

    [[nodiscard]] inline std::expected<Progress, text::Error> from_json(json::Json const& root, int* unsupported_schema = nullptr)
    {
        if (!root.is_object())
            return detail::bad("progress must be a JSON object");
        Progress p;
        if (auto const* s = root.find("schema"))
        {
            auto n = s->as_int();
            if (!n)
                return detail::bad("'schema' must be a number");
            if (*n != 1)
            {
                if (unsupported_schema != nullptr)
                    *unsupported_schema = static_cast<int>(*n);
                return detail::bad(std::format("unsupported progress schema {} (this learndc supports 1)", *n));
            }
        }
        else
            return detail::bad("'schema' is missing");
        auto active = detail::opt_string(root, "active_exercise");
        if (!active)
            return std::unexpected(active.error());
        p.active_exercise = std::move(*active);
        if (auto const* e = root.find("exercises"))
        {
            auto const* object = e->as_object();
            if (object == nullptr)
                return detail::bad("'exercises' must be an object");
            for (auto const& [id, value] : *object)
            {
                if (!value.is_object())
                    return detail::bad(std::format("exercise '{}' must be an object", id));
                ExerciseProgress ep;
                auto status_text = detail::opt_string(value, "status");
                if (!status_text)
                    return std::unexpected(status_text.error());
                auto status = parse_status(*status_text);
                if (!status)
                    return detail::bad(std::format("exercise '{}' has an invalid status", id));
                ep.status = *status;
                auto attempts = detail::opt_int(value, "attempts", 0, 1000000000);
                auto passes = detail::opt_int(value, "passes", 0, 1000000000);
                auto hint = detail::opt_int(value, "hint_level", 0, 3);
                auto revision = detail::opt_int(value, "completed_revision", 0, 1000000);
                auto last_passed = detail::opt_int(value, "last_passed", 0, 1000000);
                auto last_total = detail::opt_int(value, "last_total", 0, 1000000);
                auto started = detail::opt_string(value, "first_started_at");
                auto checked = detail::opt_string(value, "last_checked_at");
                auto completed = detail::opt_string(value, "completed_at");
                for (auto const* r : {&attempts, &passes, &hint, &revision, &last_passed, &last_total})
                    if (!*r)
                        return detail::bad(std::format("exercise '{}': {}", id, r->error().message));
                for (auto const* r : {&started, &checked, &completed})
                    if (!*r)
                        return detail::bad(std::format("exercise '{}': {}", id, r->error().message));
                ep.attempts = *attempts;
                ep.passes = *passes;
                ep.hint_level = *hint;
                ep.completed_revision = *revision;
                ep.last_passed = *last_passed;
                ep.last_total = *last_total;
                ep.first_started_at = std::move(*started);
                ep.last_checked_at = std::move(*checked);
                ep.completed_at = std::move(*completed);
                if (auto const* sv = value.find("solution_viewed"))
                {
                    auto b = sv->as_bool();
                    if (!b)
                        return detail::bad(std::format("exercise '{}': 'solution_viewed' must be true or false", id));
                    ep.solution_viewed = *b;
                }
                p.exercises.emplace(id, std::move(ep));
            }
        }
        auto bookmarks = detail::string_list(root, "bookmarks");
        if (!bookmarks)
            return std::unexpected(bookmarks.error());
        for (auto& b : *bookmarks)
            if (!is_bookmarked(p, b))
                p.bookmarks.push_back(std::move(b));
        auto recent = detail::string_list(root, "recent");
        if (!recent)
            return std::unexpected(recent.error());
        for (auto& r : *recent)
            if (std::ranges::find(p.recent, r) == p.recent.end() && p.recent.size() < max_recent)
                p.recent.push_back(std::move(r));
        if (auto const* w = root.find("workspaces"))
        {
            auto const* object = w->as_object();
            if (object == nullptr)
                return detail::bad("'workspaces' must be an object");
            for (auto const& [id, value] : *object)
            {
                auto s = value.as_string();
                if (!s)
                    return detail::bad(std::format("workspace '{}' must be a string", id));
                p.workspaces.emplace(id, std::string(*s));
            }
        }
        return p;
    }

    struct Loaded
    {
        Progress progress;
        std::vector<std::string> warnings;
    };

    namespace detail
    {
        enum class ReadKind
        {
            Missing,
            Ok,
            Corrupt,
            Unsupported,
            Unreadable,
        };

        struct ReadResult
        {
            ReadKind kind = ReadKind::Missing;
            Progress progress;
            std::string message;
        };

        [[nodiscard]] inline ReadResult read_progress(std::filesystem::path const& file)
        {
            ReadResult r;
            std::error_code ec;
            if (!std::filesystem::exists(file, ec))
                return r;
            auto data = platform::read_file(file);
            if (!data)
            {
                r.kind = ReadKind::Unreadable;
                r.message = "the file cannot be read";
                return r;
            }
            auto parsed = json::parse(*data);
            if (!parsed)
            {
                r.kind = ReadKind::Corrupt;
                r.message = parsed.error().format();
                return r;
            }
            int unsupported = 0;
            auto p = from_json(*parsed, &unsupported);
            if (!p)
            {
                r.kind = unsupported != 0 ? ReadKind::Unsupported : ReadKind::Corrupt;
                r.message = p.error().message;
                return r;
            }
            r.kind = ReadKind::Ok;
            r.progress = std::move(*p);
            return r;
        }

        [[nodiscard]] inline std::string compact_timestamp()
        {
            std::string s = platform::now_utc_iso8601();
            std::erase_if(s, [](char c) { return c == '-' || c == ':'; });
            return s;
        }
    } // namespace detail

    class ProgressStore
    {
    public:
        ProgressStore(std::filesystem::path file, std::filesystem::path lock) : m_file(std::move(file)), m_lock(std::move(lock)) {}

        [[nodiscard]] std::filesystem::path const& file() const { return m_file; }

        [[nodiscard]] std::filesystem::path backup_file() const
        {
            auto p = m_file;
            p += ".bak";
            return p;
        }

        [[nodiscard]] std::expected<Loaded, text::Error> load() const
        {
            auto first = detail::read_progress(m_file);
            if (first.kind == detail::ReadKind::Missing)
                return Loaded{};
            if (first.kind == detail::ReadKind::Ok)
                return Loaded{std::move(first.progress), {}};
            if (first.kind == detail::ReadKind::Unsupported || first.kind == detail::ReadKind::Unreadable)
                return std::unexpected(text::Error{std::format("{}: {}", platform::path_to_utf8(m_file), first.message)});
            return update_locked([](Progress&) {}, false);
        }

        [[nodiscard]] std::expected<Loaded, text::Error> update(std::function<void(Progress&)> const& change) const { return update_locked(change, true); }

    private:
        std::filesystem::path m_file;
        std::filesystem::path m_lock;

        [[nodiscard]] std::expected<Loaded, text::Error> update_locked(std::function<void(Progress&)> const& change, bool always_write) const
        {
            auto lock = platform::FileLock::acquire(m_lock, std::chrono::seconds(15));
            if (!lock)
                return std::unexpected(text::Error{
                    lock.error().busy ? std::format("another learndc is updating '{}'; try again", platform::path_to_utf8(m_file)) : lock.error().message});
            Loaded loaded;
            auto current = detail::read_progress(m_file);
            bool changed_on_disk = false;
            switch (current.kind)
            {
                case detail::ReadKind::Missing:
                    break;
                case detail::ReadKind::Ok:
                    loaded.progress = std::move(current.progress);
                    break;
                case detail::ReadKind::Unsupported:
                case detail::ReadKind::Unreadable:
                    return std::unexpected(text::Error{std::format("{}: {}", platform::path_to_utf8(m_file), current.message)});
                case detail::ReadKind::Corrupt: {
                    auto quarantine = m_file;
                    quarantine += std::format(".corrupt-{}", detail::compact_timestamp());
                    for (int n = 1; std::filesystem::exists(quarantine); ++n)
                    {
                        quarantine = m_file;
                        quarantine += std::format(".corrupt-{}-{}", detail::compact_timestamp(), n);
                    }
                    std::error_code ec;
                    std::filesystem::rename(m_file, quarantine, ec);
                    if (ec)
                        return std::unexpected(text::Error{std::format("progress file '{}' is corrupt ({}) and could not be moved aside: {}",
                                                                       platform::path_to_utf8(m_file), current.message, ec.message())});
                    auto backup = detail::read_progress(backup_file());
                    std::string warning = std::format("progress file '{}' was corrupt ({}); it was moved to '{}'", platform::path_to_utf8(m_file),
                                                      current.message, platform::path_to_utf8(quarantine));
                    if (backup.kind == detail::ReadKind::Ok)
                    {
                        loaded.progress = std::move(backup.progress);
                        warning += "; progress was restored from the previous backup";
                    }
                    else
                        warning += "; starting with empty progress";
                    loaded.warnings.push_back(std::move(warning));
                    changed_on_disk = true;
                    break;
                }
            }
            Progress before = loaded.progress;
            change(loaded.progress);
            if (!always_write && !changed_on_disk && before == loaded.progress)
                return loaded;
            if (current.kind == detail::ReadKind::Ok)
            {
                if (auto raw = platform::read_file(m_file))
                    if (auto r = platform::atomic_write(backup_file(), *raw); !r)
                        return std::unexpected(r.error());
            }
            if (auto r = platform::atomic_write(m_file, to_json(loaded.progress).dump(2) + "\n"); !r)
                return std::unexpected(r.error());
            return loaded;
        }
    };

    struct Change
    {
        std::string exercise;
        std::string field;
        std::string from;
        std::string to;

        [[nodiscard]] std::string describe() const
        {
            if (exercise.empty())
                return std::format("{}: {} -> {}", field, from.empty() ? "(none)" : from, to.empty() ? "(none)" : to);
            return std::format("{} {}: {} -> {}", exercise, field, from.empty() ? "(none)" : from, to.empty() ? "(none)" : to);
        }
    };

    struct Conflict
    {
        std::string exercise;
        std::string field;
        std::string current;
        std::string incoming;
        std::string resolution;

        [[nodiscard]] std::string describe() const
        {
            return std::format("{} {}: current '{}' vs imported '{}'; kept {}", exercise, field, current, incoming, resolution);
        }
    };

    struct ImportPlan
    {
        Progress merged;
        std::vector<Change> changes;
        std::vector<Conflict> conflicts;
    };

    [[nodiscard]] inline std::string export_text(Progress const& p, std::string_view exported_at)
    {
        auto root = json::Json::make_object();
        root.set("format", json::Json("learndc-progress"));
        root.set("schema", json::Json(1));
        root.set("exported_at", json::Json(exported_at));
        root.set("progress", to_json(p));
        return root.dump(2) + "\n";
    }

    [[nodiscard]] inline std::expected<Progress, text::Error> parse_export(std::string_view data)
    {
        auto parsed = json::parse(data);
        if (!parsed)
            return std::unexpected(text::Error{std::format("not a valid export file: {}", parsed.error().format())});
        auto const* format = parsed->find("format");
        if (!parsed->is_object() || format == nullptr || format->as_string() != std::optional<std::string_view>("learndc-progress"))
            return std::unexpected(text::Error{"not a learndc progress export (missing format \"learndc-progress\")"});
        auto const* schema = parsed->find("schema");
        if (schema == nullptr || !schema->is_int())
            return std::unexpected(text::Error{"the export has no schema number"});
        if (*schema->as_int() != 1)
            return std::unexpected(text::Error{std::format("unsupported export schema {} (this learndc supports 1)", *schema->as_int())});
        auto const* body = parsed->find("progress");
        if (body == nullptr)
            return std::unexpected(text::Error{"the export has no progress object"});
        return from_json(*body);
    }

    namespace detail
    {
        [[nodiscard]] inline std::string earliest(std::string const& a, std::string const& b)
        {
            if (a.empty())
                return b;
            if (b.empty())
                return a;
            return std::min(a, b);
        }

        [[nodiscard]] inline std::string latest(std::string const& a, std::string const& b)
        {
            return std::max(a, b);
        }

        [[nodiscard]] inline Status merge_status(Status a, Status b)
        {
            return static_cast<Status>(std::max(static_cast<int>(a), static_cast<int>(b)));
        }

        inline void diff_exercise(std::string const& id, ExerciseProgress const* from, ExerciseProgress const& to, std::vector<Change>& out)
        {
            ExerciseProgress const empty;
            auto const& f = from != nullptr ? *from : empty;
            auto const add = [&](std::string_view field, std::string a, std::string b) {
                if (a != b)
                    out.push_back(Change{id, std::string(field), std::move(a), std::move(b)});
            };
            add("status", std::string(to_string(f.status)), std::string(to_string(to.status)));
            add("attempts", std::to_string(f.attempts), std::to_string(to.attempts));
            add("passes", std::to_string(f.passes), std::to_string(to.passes));
            add("hint_level", std::to_string(f.hint_level), std::to_string(to.hint_level));
            add("solution_viewed", f.solution_viewed ? "yes" : "no", to.solution_viewed ? "yes" : "no");
            add("first_started_at", f.first_started_at, to.first_started_at);
            add("last_checked_at", f.last_checked_at, to.last_checked_at);
            add("completed_at", f.completed_at, to.completed_at);
            add("completed_revision", std::to_string(f.completed_revision), std::to_string(to.completed_revision));
            add("last_result", std::format("{}/{}", f.last_passed, f.last_total), std::format("{}/{}", to.last_passed, to.last_total));
        }
    } // namespace detail

    [[nodiscard]] inline ImportPlan plan_import(Progress const& current, Progress const& incoming)
    {
        ImportPlan plan;
        plan.merged = current;
        auto& merged = plan.merged;
        for (auto const& [id, in] : incoming.exercises)
        {
            auto it = merged.exercises.find(id);
            if (it == merged.exercises.end())
            {
                merged.exercises.emplace(id, in);
                continue;
            }
            auto const& cur = current.exercises.at(id);
            auto& m = it->second;
            m.status = detail::merge_status(cur.status, in.status);
            m.attempts = std::max(cur.attempts, in.attempts);
            m.passes = std::max(cur.passes, in.passes);
            m.hint_level = std::max(cur.hint_level, in.hint_level);
            m.solution_viewed = cur.solution_viewed || in.solution_viewed;
            m.first_started_at = detail::earliest(cur.first_started_at, in.first_started_at);
            m.last_checked_at = detail::latest(cur.last_checked_at, in.last_checked_at);
            m.completed_at = detail::earliest(cur.completed_at, in.completed_at);
            m.completed_revision = std::max(cur.completed_revision, in.completed_revision);
            if (in.last_checked_at > cur.last_checked_at)
            {
                m.last_passed = in.last_passed;
                m.last_total = in.last_total;
            }
            if (cur.status != in.status)
                plan.conflicts.push_back(
                    Conflict{id, "status", std::string(to_string(cur.status)), std::string(to_string(in.status)), std::string(to_string(m.status))});
            if (!cur.completed_at.empty() && !in.completed_at.empty() && cur.completed_at != in.completed_at)
                plan.conflicts.push_back(Conflict{id, "completed_at", cur.completed_at, in.completed_at, m.completed_at});
            if (cur.completed_revision != 0 && in.completed_revision != 0 && cur.completed_revision != in.completed_revision)
                plan.conflicts.push_back(Conflict{id, "completed_revision", std::to_string(cur.completed_revision), std::to_string(in.completed_revision),
                                                  std::to_string(m.completed_revision)});
        }
        for (auto const& b : incoming.bookmarks)
            if (!is_bookmarked(merged, b))
                merged.bookmarks.push_back(b);
        for (auto const& [id, path] : incoming.workspaces)
        {
            auto it = merged.workspaces.find(id);
            if (it == merged.workspaces.end())
                merged.workspaces.emplace(id, path);
            else if (it->second != path)
                plan.conflicts.push_back(Conflict{id, "workspace", it->second, path, it->second});
        }
        for (auto const& r : incoming.recent)
            if (merged.recent.size() < max_recent && std::ranges::find(merged.recent, r) == merged.recent.end())
                merged.recent.push_back(r);
        if (!incoming.active_exercise.empty() && current.active_exercise != incoming.active_exercise && !current.active_exercise.empty())
            plan.conflicts.push_back(Conflict{"", "active_exercise", current.active_exercise, incoming.active_exercise, current.active_exercise});

        for (auto const& [id, to] : merged.exercises)
        {
            auto it = current.exercises.find(id);
            detail::diff_exercise(id, it == current.exercises.end() ? nullptr : &it->second, to, plan.changes);
        }
        for (auto const& b : merged.bookmarks)
            if (!is_bookmarked(current, b))
                plan.changes.push_back(Change{"", "bookmark", "", b});
        for (auto const& [id, path] : merged.workspaces)
            if (!current.workspaces.contains(id))
                plan.changes.push_back(Change{id, "workspace", "", path});
        return plan;
    }

    inline void apply_import(Progress& current, ImportPlan const& plan)
    {
        current = plan.merged;
    }

    [[nodiscard]] inline std::expected<Loaded, text::Error> import_into(ProgressStore const& store, Progress const& incoming)
    {
        return store.update([&](Progress& p) { apply_import(p, plan_import(p, incoming)); });
    }
} // namespace learndc::progress
