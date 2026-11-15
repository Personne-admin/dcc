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

    inline constexpr int current_schema = 2;

    struct StageProgress
    {
        std::string achieved_at;
        int achieved_revision = 0;
        std::string last;
        std::string last_checked_at;
        int hint_level = 0;

        [[nodiscard]] bool achieved() const { return !achieved_at.empty(); }

        friend bool operator==(StageProgress const&, StageProgress const&) = default;
    };

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
        std::string last_fingerprint;
        std::map<std::string, StageProgress> stages;

        friend bool operator==(ExerciseProgress const&, ExerciseProgress const&) = default;
    };

    struct QuestionProgress
    {
        int attempts = 0;
        int correct = 0;
        std::string last;
        std::string last_at;
        std::string first_correct_at;

        [[nodiscard]] bool missed() const { return last == "wrong"; }

        friend bool operator==(QuestionProgress const&, QuestionProgress const&) = default;
    };

    struct CardProgress
    {
        int box = 0;
        int due_day = 0;
        int reviews = 0;
        int lapses = 0;
        std::string last_at;

        friend bool operator==(CardProgress const&, CardProgress const&) = default;
    };

    [[nodiscard]] inline std::string item_key(std::string_view lesson, std::string_view item)
    {
        return std::format("{}#{}", lesson, item);
    }

    struct Progress
    {
        int schema = current_schema;
        std::map<std::string, ExerciseProgress> exercises;
        std::string active_exercise;
        std::vector<std::string> bookmarks;
        std::map<std::string, std::string> workspaces;
        std::vector<std::string> recent;
        std::map<std::string, QuestionProgress> questions;
        std::map<std::string, CardProgress> cards;

        friend bool operator==(Progress const&, Progress const&) = default;
    };

    inline QuestionProgress& record_answer(Progress& p, std::string_view lesson, std::string_view question, bool correct, std::string_view now)
    {
        auto& q = p.questions[item_key(lesson, question)];
        ++q.attempts;
        if (correct)
        {
            ++q.correct;
            if (q.first_correct_at.empty())
                q.first_correct_at = std::string(now);
        }
        q.last = correct ? "right" : "wrong";
        q.last_at = std::string(now);
        return q;
    }

    [[nodiscard]] inline QuestionProgress question_of(Progress const& p, std::string_view lesson, std::string_view question)
    {
        auto it = p.questions.find(item_key(lesson, question));
        return it == p.questions.end() ? QuestionProgress{} : it->second;
    }

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

    inline void record_check(Progress& p, std::string_view id, int passed, int total, int revision, std::string_view now,
                             std::optional<bool> complete = std::nullopt)
    {
        auto& e = mark_started(p, id, now);
        ++e.attempts;
        e.last_passed = passed;
        e.last_total = total;
        e.last_checked_at = std::string(now);
        if (complete.value_or(total > 0 && passed >= total))
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

    struct StageOutcome
    {
        std::string id;
        bool passed = false;
        bool ran = true;
        int revision = 1;
        bool questions_done = true;
    };

    inline void record_stages(Progress& p, std::string_view id, std::vector<StageOutcome> const& outcomes, std::string_view fingerprint, std::string_view now)
    {
        auto& e = p.exercises[std::string(id)];
        e.last_fingerprint = std::string(fingerprint);
        for (auto const& o : outcomes)
        {
            auto& st = e.stages[o.id];
            st.last = !o.ran ? "not-run" : o.passed ? "pass" : "fail";
            st.last_checked_at = std::string(now);
            if (o.passed && o.questions_done && (!st.achieved() || st.achieved_revision < o.revision))
            {
                if (!st.achieved())
                    st.achieved_at = std::string(now);
                st.achieved_revision = o.revision;
            }
        }
    }

    inline void reveal_stage_hint(Progress& p, std::string_view id, std::string_view stage, int level)
    {
        auto& st = p.exercises[std::string(id)].stages[std::string(stage)];
        st.hint_level = std::clamp(std::max(st.hint_level, level), 0, 3);
    }

    inline void forget_stage_results(Progress& p, std::string_view id)
    {
        auto it = p.exercises.find(std::string(id));
        if (it == p.exercises.end())
            return;
        it->second.last_fingerprint.clear();
        for (auto& [name, st] : it->second.stages)
        {
            st.last.clear();
            st.last_checked_at.clear();
        }
    }

    inline bool mark_completed(Progress& p, std::string_view id, int revision, std::string_view now)
    {
        auto& e = mark_started(p, id, now);
        bool const was = e.status == Status::Completed;
        if (!was || e.completed_at.empty())
            e.completed_at = std::string(now);
        e.completed_revision = std::max(e.completed_revision, revision);
        e.status = Status::Completed;
        return !was;
    }

    inline void record_lecture(Progress& p, std::string_view id, bool understood, int revision, std::string_view now)
    {
        auto& e = mark_started(p, id, now);
        ++e.attempts;
        e.last_checked_at = std::string(now);
        if (!understood)
            return;
        ++e.passes;
        if (e.status != Status::Completed || e.completed_at.empty())
            e.completed_at = std::string(now);
        e.completed_revision = std::max(e.completed_revision, revision);
        e.status = Status::Completed;
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
        root.set("schema", json::Json(current_schema));
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
            if (!e.last_fingerprint.empty())
                o.set("last_fingerprint", json::Json(e.last_fingerprint));
            if (!e.stages.empty())
            {
                auto stages = json::Json::make_object();
                for (auto const& [name, st] : e.stages)
                {
                    auto so = json::Json::make_object();
                    so.set("achieved_at", text_or_null(st.achieved_at));
                    so.set("achieved_revision", json::Json(st.achieved_revision));
                    so.set("last", text_or_null(st.last));
                    so.set("last_checked_at", text_or_null(st.last_checked_at));
                    so.set("hint_level", json::Json(st.hint_level));
                    stages.set(name, std::move(so));
                }
                o.set("stages", std::move(stages));
            }
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
        if (!p.questions.empty())
        {
            auto questions = json::Json::make_object();
            for (auto const& [key, q] : p.questions)
            {
                auto o = json::Json::make_object();
                o.set("attempts", json::Json(q.attempts));
                o.set("correct", json::Json(q.correct));
                o.set("last", text_or_null(q.last));
                o.set("last_at", text_or_null(q.last_at));
                o.set("first_correct_at", text_or_null(q.first_correct_at));
                questions.set(key, std::move(o));
            }
            root.set("questions", std::move(questions));
        }
        if (!p.cards.empty())
        {
            auto cards = json::Json::make_object();
            for (auto const& [key, c] : p.cards)
            {
                auto o = json::Json::make_object();
                o.set("box", json::Json(c.box));
                o.set("due_day", json::Json(c.due_day));
                o.set("reviews", json::Json(c.reviews));
                o.set("lapses", json::Json(c.lapses));
                o.set("last_at", text_or_null(c.last_at));
                cards.set(key, std::move(o));
            }
            root.set("cards", std::move(cards));
        }
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
            if (*n != 1 && *n != current_schema)
            {
                if (unsupported_schema != nullptr)
                    *unsupported_schema = static_cast<int>(*n);
                return detail::bad(std::format("unsupported progress schema {} (this learndc supports 1 and {})", *n, current_schema));
            }
            p.schema = static_cast<int>(*n);
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
                auto fingerprint = detail::opt_string(value, "last_fingerprint");
                if (!fingerprint)
                    return detail::bad(std::format("exercise '{}': {}", id, fingerprint.error().message));
                ep.last_fingerprint = std::move(*fingerprint);
                if (auto const* stages = value.find("stages"))
                {
                    auto const* stage_map = stages->as_object();
                    if (stage_map == nullptr)
                        return detail::bad(std::format("exercise '{}': 'stages' must be an object", id));
                    for (auto const& [name, sv] : *stage_map)
                    {
                        if (!sv.is_object())
                            return detail::bad(std::format("exercise '{}': stage '{}' must be an object", id, name));
                        StageProgress st;
                        auto achieved = detail::opt_string(sv, "achieved_at");
                        auto last = detail::opt_string(sv, "last");
                        auto stage_checked = detail::opt_string(sv, "last_checked_at");
                        auto rev = detail::opt_int(sv, "achieved_revision", 0, 1000000);
                        auto hint_level = detail::opt_int(sv, "hint_level", 0, 3);
                        for (auto const* r : {&achieved, &last, &stage_checked})
                            if (!*r)
                                return detail::bad(std::format("exercise '{}' stage '{}': {}", id, name, r->error().message));
                        for (auto const* r : {&rev, &hint_level})
                            if (!*r)
                                return detail::bad(std::format("exercise '{}' stage '{}': {}", id, name, r->error().message));
                        if (!last->empty() && *last != "pass" && *last != "fail" && *last != "not-run")
                            return detail::bad(std::format("exercise '{}' stage '{}': 'last' must be pass, fail or not-run", id, name));
                        st.achieved_at = std::move(*achieved);
                        st.last = std::move(*last);
                        st.last_checked_at = std::move(*stage_checked);
                        st.achieved_revision = *rev;
                        st.hint_level = *hint_level;
                        ep.stages.emplace(name, std::move(st));
                    }
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
        if (auto const* qs = root.find("questions"))
        {
            auto const* object = qs->as_object();
            if (object == nullptr)
                return detail::bad("'questions' must be an object");
            for (auto const& [key, value] : *object)
            {
                if (!value.is_object())
                    return detail::bad(std::format("question '{}' must be an object", key));
                QuestionProgress q;
                auto attempts = detail::opt_int(value, "attempts", 0, 1000000000);
                auto correct = detail::opt_int(value, "correct", 0, 1000000000);
                auto last = detail::opt_string(value, "last");
                auto last_at = detail::opt_string(value, "last_at");
                auto first = detail::opt_string(value, "first_correct_at");
                for (auto const* r : {&attempts, &correct})
                    if (!*r)
                        return detail::bad(std::format("question '{}': {}", key, r->error().message));
                for (auto const* r : {&last, &last_at, &first})
                    if (!*r)
                        return detail::bad(std::format("question '{}': {}", key, r->error().message));
                if (!last->empty() && *last != "right" && *last != "wrong")
                    return detail::bad(std::format("question '{}': 'last' must be right or wrong", key));
                q.attempts = *attempts;
                q.correct = *correct;
                q.last = std::move(*last);
                q.last_at = std::move(*last_at);
                q.first_correct_at = std::move(*first);
                p.questions.emplace(key, std::move(q));
            }
        }
        if (auto const* cs = root.find("cards"))
        {
            auto const* object = cs->as_object();
            if (object == nullptr)
                return detail::bad("'cards' must be an object");
            for (auto const& [key, value] : *object)
            {
                if (!value.is_object())
                    return detail::bad(std::format("card '{}' must be an object", key));
                CardProgress c;
                auto box = detail::opt_int(value, "box", 0, 5);
                auto due = detail::opt_int(value, "due_day", 0, 10000000);
                auto reviews = detail::opt_int(value, "reviews", 0, 1000000000);
                auto lapses = detail::opt_int(value, "lapses", 0, 1000000000);
                auto last_at = detail::opt_string(value, "last_at");
                for (auto const* r : {&box, &due, &reviews, &lapses})
                    if (!*r)
                        return detail::bad(std::format("card '{}': {}", key, r->error().message));
                if (!last_at)
                    return detail::bad(std::format("card '{}': {}", key, last_at.error().message));
                c.box = *box;
                c.due_day = *due;
                c.reviews = *reviews;
                c.lapses = *lapses;
                c.last_at = std::move(*last_at);
                p.cards.emplace(key, std::move(c));
            }
        }
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

        [[nodiscard]] std::filesystem::path schema1_backup_file() const
        {
            auto p = m_file;
            p += ".schema1.bak";
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
            int const read_schema = current.progress.schema;
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
                {
                    if (auto r = platform::atomic_write(backup_file(), *raw); !r)
                        return std::unexpected(r.error());
                    std::error_code ec;
                    if (read_schema == 1 && !std::filesystem::exists(schema1_backup_file(), ec))
                        if (auto r = platform::atomic_write(schema1_backup_file(), *raw); !r)
                            return std::unexpected(r.error());
                }
            }
            loaded.progress.schema = current_schema;
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
        root.set("schema", json::Json(current_schema));
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
        if (*schema->as_int() != 1 && *schema->as_int() != current_schema)
            return std::unexpected(
                text::Error{std::format("unsupported export schema {} (this learndc supports 1 and {})", *schema->as_int(), current_schema)});
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

        [[nodiscard]] inline StageProgress merge_stage(StageProgress const& a, StageProgress const& b)
        {
            StageProgress m;
            m.achieved_at = earliest(a.achieved_at, b.achieved_at);
            m.achieved_revision = std::max(a.achieved_revision, b.achieved_revision);
            bool const b_newer = b.last_checked_at > a.last_checked_at;
            m.last = b_newer ? b.last : a.last;
            m.last_checked_at = latest(a.last_checked_at, b.last_checked_at);
            m.hint_level = std::max(a.hint_level, b.hint_level);
            return m;
        }

        [[nodiscard]] inline QuestionProgress merge_question(QuestionProgress const& a, QuestionProgress const& b)
        {
            QuestionProgress m;
            m.attempts = std::max(a.attempts, b.attempts);
            m.correct = std::max(a.correct, b.correct);
            m.last = b.last_at > a.last_at ? b.last : a.last;
            m.last_at = latest(a.last_at, b.last_at);
            m.first_correct_at = earliest(a.first_correct_at, b.first_correct_at);
            return m;
        }

        [[nodiscard]] inline ExerciseProgress merge_exercise(ExerciseProgress const& cur, ExerciseProgress const& in)
        {
            ExerciseProgress m = cur;
            m.status = merge_status(cur.status, in.status);
            m.attempts = std::max(cur.attempts, in.attempts);
            m.passes = std::max(cur.passes, in.passes);
            m.hint_level = std::max(cur.hint_level, in.hint_level);
            m.solution_viewed = cur.solution_viewed || in.solution_viewed;
            m.first_started_at = earliest(cur.first_started_at, in.first_started_at);
            m.last_checked_at = latest(cur.last_checked_at, in.last_checked_at);
            m.completed_at = earliest(cur.completed_at, in.completed_at);
            m.completed_revision = std::max(cur.completed_revision, in.completed_revision);
            if (in.last_checked_at > cur.last_checked_at)
            {
                m.last_passed = in.last_passed;
                m.last_total = in.last_total;
                m.last_fingerprint = in.last_fingerprint;
            }
            for (auto const& [name, st] : in.stages)
            {
                auto it = m.stages.find(name);
                if (it == m.stages.end())
                    m.stages.emplace(name, st);
                else
                    it->second = merge_stage(it->second, st);
            }
            return m;
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
            m = detail::merge_exercise(cur, in);
            if (cur.status != in.status)
                plan.conflicts.push_back(
                    Conflict{id, "status", std::string(to_string(cur.status)), std::string(to_string(in.status)), std::string(to_string(m.status))});
            if (!cur.completed_at.empty() && !in.completed_at.empty() && cur.completed_at != in.completed_at)
                plan.conflicts.push_back(Conflict{id, "completed_at", cur.completed_at, in.completed_at, m.completed_at});
            if (cur.completed_revision != 0 && in.completed_revision != 0 && cur.completed_revision != in.completed_revision)
                plan.conflicts.push_back(Conflict{id, "completed_revision", std::to_string(cur.completed_revision), std::to_string(in.completed_revision),
                                                  std::to_string(m.completed_revision)});
        }
        for (auto const& [key, q] : incoming.questions)
        {
            auto it = merged.questions.find(key);
            if (it == merged.questions.end())
                merged.questions.emplace(key, q);
            else
                it->second = detail::merge_question(it->second, q);
        }
        for (auto const& [key, c] : incoming.cards)
        {
            auto it = merged.cards.find(key);
            if (it == merged.cards.end() || c.last_at > it->second.last_at)
                merged.cards[key] = c;
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

    inline bool migrate_renamed(Progress& p, std::map<std::string, std::string> const& renamed)
    {
        bool changed = false;
        auto const target = [&](std::string const& id) -> std::string const* {
            auto it = renamed.find(id);
            return it == renamed.end() ? nullptr : &it->second;
        };
        for (auto const& [from, to] : renamed)
        {
            auto it = p.exercises.find(from);
            if (it == p.exercises.end())
                continue;
            auto old = std::move(it->second);
            p.exercises.erase(it);
            auto existing = p.exercises.find(to);
            if (existing == p.exercises.end())
                p.exercises.emplace(to, std::move(old));
            else
                existing->second = detail::merge_exercise(existing->second, old);
            changed = true;
        }
        auto rekey = [&](auto& items, auto merge) {
            std::vector<std::string> moved;
            for (auto const& [key, value] : items)
            {
                auto const hash = key.find('#');
                if (hash != std::string::npos && target(key.substr(0, hash)) != nullptr)
                    moved.push_back(key);
            }
            for (auto const& key : moved)
            {
                auto const hash = key.find('#');
                auto value = items.at(key);
                items.erase(key);
                auto const next = *target(key.substr(0, hash)) + key.substr(hash);
                auto it = items.find(next);
                if (it == items.end())
                    items.emplace(next, std::move(value));
                else
                    it->second = merge(it->second, value);
                changed = true;
            }
        };
        rekey(p.questions, [](QuestionProgress const& a, QuestionProgress const& b) { return detail::merge_question(a, b); });
        rekey(p.cards, [](CardProgress const& a, CardProgress const& b) { return b.last_at > a.last_at ? b : a; });
        std::vector<std::string> bookmarks;
        for (auto const& b : p.bookmarks)
        {
            auto const* t = target(b);
            if (t != nullptr)
                changed = true;
            auto const& id = t != nullptr ? *t : b;
            if (std::ranges::find(bookmarks, id) == bookmarks.end())
                bookmarks.push_back(id);
        }
        p.bookmarks = std::move(bookmarks);
        std::vector<std::string> recent;
        for (auto const& r : p.recent)
        {
            auto const* t = target(r);
            if (t != nullptr)
                changed = true;
            auto const& id = t != nullptr ? *t : r;
            if (std::ranges::find(recent, id) == recent.end())
                recent.push_back(id);
        }
        p.recent = std::move(recent);
        if (auto const* t = target(p.active_exercise))
        {
            p.active_exercise = *t;
            changed = true;
        }
        for (auto const& [from, to] : renamed)
        {
            auto it = p.workspaces.find(from);
            if (it == p.workspaces.end())
                continue;
            if (!p.workspaces.contains(to))
                p.workspaces.emplace(to, it->second);
            p.workspaces.erase(from);
            changed = true;
        }
        return changed;
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
