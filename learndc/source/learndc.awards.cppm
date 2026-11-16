export module learndc.awards;

import std;
import learndc.text;
import learndc.json;
import learndc.platform;
import learndc.catalog;
import learndc.progress;

export namespace learndc::awards
{
    enum class Category
    {
        Milestone,
        Boss,
        Specialization,
        Project,
        Knowledge,
    };

    [[nodiscard]] constexpr std::string_view to_string(Category c) noexcept
    {
        switch (c)
        {
            case Category::Milestone:
                return "milestone";
            case Category::Boss:
                return "boss";
            case Category::Specialization:
                return "specialization";
            case Category::Project:
                return "project";
            case Category::Knowledge:
                return "knowledge";
        }
        return "milestone";
    }

    [[nodiscard]] constexpr std::string_view category_title(Category c) noexcept
    {
        switch (c)
        {
            case Category::Milestone:
                return "Academy milestones";
            case Category::Boss:
                return "Boss Fights";
            case Category::Specialization:
                return "Specializations";
            case Category::Project:
                return "Projects and practice";
            case Category::Knowledge:
                return "Knowledge";
        }
        return "Academy milestones";
    }

    struct Award
    {
        std::string id;
        std::string title;
        std::string description;
        Category category = Category::Milestone;
        std::string path;
        bool hidden = false;
        bool earned = false;
        std::string earned_at;
        int have = 0;
        int need = 1;
        std::string lesson;
    };

    [[nodiscard]] inline Award make_award(std::string id, std::string title, std::string description, Category category, std::string path = {})
    {
        Award a;
        a.id = std::move(id);
        a.title = std::move(title);
        a.description = std::move(description);
        a.category = category;
        a.path = std::move(path);
        return a;
    }

    namespace detail
    {
        struct Facts
        {
            catalog::Catalog const& cat;
            progress::Progress const& p;

            [[nodiscard]] progress::ExerciseProgress const* rec(std::string_view id) const
            {
                auto it = p.exercises.find(std::string(id));
                return it == p.exercises.end() ? nullptr : &it->second;
            }

            [[nodiscard]] bool done(std::string_view id) const { return progress::is_completed(p, id); }

            [[nodiscard]] std::string done_at(std::string_view id) const
            {
                auto const* r = rec(id);
                return r && done(id) ? r->completed_at : std::string();
            }

            [[nodiscard]] std::vector<catalog::Exercise const*> required_of_track(catalog::Track const& t) const
            {
                std::vector<catalog::Exercise const*> out;
                for (auto const& id : t.exercise_ids)
                    if (auto const* e = cat.find(id); e && !e->optional)
                        out.push_back(e);
                if (out.empty())
                    for (auto const& id : t.exercise_ids)
                        if (auto const* e = cat.find(id))
                            out.push_back(e);
                return out;
            }

            [[nodiscard]] std::vector<catalog::Exercise const*> required_of_path(std::string_view path) const
            {
                std::vector<catalog::Exercise const*> out;
                for (auto const& t : cat.tracks())
                    if (t.path == path)
                        for (auto const* e : required_of_track(t))
                            out.push_back(e);
                return out;
            }

            [[nodiscard]] std::vector<catalog::Exercise const*> lessons_of_track(std::string_view track) const
            {
                std::vector<catalog::Exercise const*> out;
                if (auto const* t = cat.find_track(track))
                    for (auto const& id : t->exercise_ids)
                        if (auto const* e = cat.find(id))
                            out.push_back(e);
                return out;
            }

            [[nodiscard]] std::string path_of(catalog::Exercise const& e) const
            {
                auto const* path = cat.path_of(e);
                return path ? path->id : std::string();
            }
        };

        [[nodiscard]] inline std::string latest(std::vector<std::string> const& stamps)
        {
            std::string out;
            for (auto const& s : stamps)
                if (s > out)
                    out = s;
            return out;
        }

        [[nodiscard]] inline std::string earliest(std::vector<std::string> const& stamps)
        {
            std::string out;
            for (auto const& s : stamps)
                if (!s.empty() && (out.empty() || s < out))
                    out = s;
            return out;
        }

        inline void all_of(Award& a, Facts const& f, std::vector<catalog::Exercise const*> const& lessons)
        {
            a.need = static_cast<int>(lessons.size());
            std::vector<std::string> stamps;
            for (auto const* e : lessons)
                if (f.done(e->id))
                {
                    ++a.have;
                    stamps.push_back(f.done_at(e->id));
                }
            a.earned = a.need > 0 && a.have == a.need;
            if (a.earned)
                a.earned_at = latest(stamps);
        }

        inline void count_of(Award& a, int need, std::vector<std::string> const& stamps_of_hits)
        {
            a.need = need;
            a.have = std::min(need, static_cast<int>(stamps_of_hits.size()));
            a.earned = static_cast<int>(stamps_of_hits.size()) >= need;
            if (a.earned)
            {
                auto sorted = stamps_of_hits;
                std::ranges::sort(sorted);
                a.earned_at = sorted[static_cast<std::size_t>(need - 1)];
            }
        }

        [[nodiscard]] inline std::string path_title(catalog::Catalog const& cat, std::string_view id)
        {
            auto const* p = cat.find_path(id);
            return p ? p->title : std::string(id);
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<Award> evaluate(catalog::Catalog const& cat, progress::Progress const& p)
    {
        detail::Facts const f{cat, p};
        std::vector<Award> out;
        auto add = [&](Award a) { out.push_back(std::move(a)); };
        std::string core_id;
        for (auto const& path : cat.paths())
            if (path.kind == catalog::PathKind::Core)
            {
                core_id = path.id;
                break;
            }

        {
            Award a =
                make_award("first-program", "Hello, Academy", "Complete your first exercise: write code and pass its checks.", awards::Category::Milestone);
            std::vector<std::string> hits;
            for (auto const& e : cat.exercises())
                if (!e.is_lecture() && f.done(e.id))
                    hits.push_back(f.done_at(e.id));
            detail::count_of(a, 1, hits);
            add(std::move(a));
        }
        {
            Award a = make_award("chapter-conqueror", "Chapter Conqueror", "Complete every required lesson of a chapter.", awards::Category::Milestone);
            std::vector<std::string> hits;
            int best = 0;
            int best_need = 1;
            for (auto const& t : cat.tracks())
            {
                auto const lessons = f.required_of_track(t);
                int have = 0;
                std::vector<std::string> stamps;
                for (auto const* e : lessons)
                    if (f.done(e->id))
                    {
                        ++have;
                        stamps.push_back(f.done_at(e->id));
                    }
                if (!lessons.empty() && have == static_cast<int>(lessons.size()))
                    hits.push_back(detail::latest(stamps));
                if (lessons.size() > 0 && have * best_need > best * static_cast<int>(lessons.size()))
                {
                    best = have;
                    best_need = static_cast<int>(lessons.size());
                }
            }
            a.need = best_need;
            a.have = best;
            if (!hits.empty())
            {
                a.earned = true;
                a.have = a.need;
                a.earned_at = detail::earliest(hits);
            }
            add(std::move(a));
        }
        if (!core_id.empty())
        {
            Award a =
                make_award("foundation", "Foundation Builder", "Complete every required lesson of the core curriculum.", awards::Category::Milestone, core_id);
            detail::all_of(a, f, f.required_of_path(core_id));
            add(std::move(a));
        }
        {
            Award a = make_award("persistent", "Persistent Learner",
                                 "Solve an exercise after at least two checks that did not pass: going back to a hard problem is how programmers learn.",
                                 awards::Category::Milestone);
            std::vector<std::string> hits;
            for (auto const& e : cat.exercises())
                if (auto const* r = f.rec(e.id); r && !e.is_lecture() && f.done(e.id) && r->attempts - r->passes >= 2)
                    hits.push_back(r->completed_at);
            detail::count_of(a, 1, hits);
            add(std::move(a));
        }
        {
            Award a =
                make_award("self-reliant", "Self-Reliant", "Complete ten exercises without revealing a hint or the solution.", awards::Category::Milestone);
            std::vector<std::string> hits;
            for (auto const& e : cat.exercises())
                if (auto const* r = f.rec(e.id); r && !e.is_lecture() && f.done(e.id) && r->hint_level == 0 && !r->solution_viewed &&
                                                 std::ranges::all_of(r->stages, [](auto const& kv) { return kv.second.hint_level == 0; }))
                    hits.push_back(r->completed_at);
            detail::count_of(a, 10, hits);
            add(std::move(a));
        }

        std::vector<catalog::Exercise const*> bosses;
        for (auto const& t : cat.tracks())
            for (auto const& id : t.exercise_ids)
                if (auto const* e = cat.find(id); e && e->is_boss())
                    bosses.push_back(e);
        if (!bosses.empty())
        {
            Award a = make_award("first-boss", "Boss Slayer", "Defeat your first Boss Fight.", awards::Category::Boss);
            std::vector<std::string> hits;
            for (auto const* e : bosses)
                if (f.done(e->id))
                    hits.push_back(f.done_at(e->id));
            detail::count_of(a, 1, hits);
            add(std::move(a));
        }
        for (auto const* e : bosses)
        {
            Award a = make_award(std::format("boss-{}", text::replace_all(e->id, "/", "-")), std::format("{} Defeated", e->title),
                                 std::format("Defeat the Boss Fight {} ({}).", e->title, detail::path_title(cat, f.path_of(*e))), awards::Category::Boss,
                                 f.path_of(*e));
            a.lesson = e->id;
            a.need = static_cast<int>(std::ranges::count_if(e->stages, [](auto const& st) { return !st.optional; }));
            if (auto const* r = f.rec(e->id))
                for (auto const& st : e->stages)
                    if (!st.optional)
                        if (auto it = r->stages.find(st.id); it != r->stages.end() && it->second.achieved())
                            ++a.have;
            a.need = std::max(a.need, 1);
            if (f.done(e->id))
            {
                a.earned = true;
                a.have = a.need;
                a.earned_at = f.done_at(e->id);
            }
            add(std::move(a));
        }
        if (!bosses.empty())
        {
            Award a = make_award("perfect-victory", "Perfect Victory", "Defeat a Boss Fight with every bonus stage cleared and without hints or the solution.",
                                 awards::Category::Boss);
            a.hidden = true;
            std::vector<std::string> hits;
            for (auto const* e : bosses)
            {
                auto const* r = f.rec(e->id);
                if (!r || !f.done(e->id) || r->solution_viewed || r->hint_level > 0)
                    continue;
                bool ok = true;
                for (auto const& st : e->stages)
                {
                    auto it = r->stages.find(st.id);
                    if (it == r->stages.end() || !it->second.achieved() || it->second.hint_level > 0)
                        ok = false;
                }
                if (ok)
                    hits.push_back(r->completed_at);
            }
            detail::count_of(a, 1, hits);
            add(std::move(a));
        }
        if (!core_id.empty() && !bosses.empty())
        {
            Award a = make_award("boss-master", "Master of Bosses", "Defeat every Boss Fight of the core curriculum.", awards::Category::Boss, core_id);
            std::vector<catalog::Exercise const*> core_bosses;
            for (auto const* e : bosses)
                if (f.path_of(*e) == core_id)
                    core_bosses.push_back(e);
            detail::all_of(a, f, core_bosses);
            if (!core_bosses.empty())
                add(std::move(a));
        }

        struct Named
        {
            std::string_view path;
            std::string_view id;
            std::string_view title;
            std::string_view description;
        };
        static constexpr std::array<Named, 4> specializations = {{
            {"advanced", "systems-programmer", "Systems Programmer",
             "Complete the Advanced DC path: memory, interop, compile-time programming, data structures and design."},
            {"libdcext", "library-explorer", "Standard Library Explorer", "Complete the standard library path."},
            {"dpm", "package-smith", "Package Smith", "Complete the DPM mastery path, from projects to a packaged library."},
            {"games", "game-developer", "SDL3 Game Developer", "Complete the game development path and its three games."},
        }};
        for (auto const& s : specializations)
        {
            if (!cat.find_path(s.path))
                continue;
            Award a = make_award(std::string(s.id), std::string(s.title), std::string(s.description), awards::Category::Specialization, std::string(s.path));
            detail::all_of(a, f, f.required_of_path(s.path));
            add(std::move(a));
        }
        for (auto const& path : cat.paths())
        {
            if (path.kind != catalog::PathKind::Specialization)
                continue;
            Award a = make_award(std::format("explorer-{}", path.id), std::format("{}: first chapter", path.title),
                                 std::format("Complete a whole chapter of {}.", path.title), awards::Category::Specialization, path.id);
            std::vector<std::string> hits;
            for (auto const& t : cat.tracks())
            {
                if (t.path != path.id)
                    continue;
                auto const lessons = f.required_of_track(t);
                std::vector<std::string> stamps;
                bool all = !lessons.empty();
                for (auto const* e : lessons)
                {
                    if (!f.done(e->id))
                        all = false;
                    stamps.push_back(f.done_at(e->id));
                }
                if (all)
                    hits.push_back(detail::latest(stamps));
            }
            detail::count_of(a, 1, hits);
            add(std::move(a));
        }

        struct TrackAward
        {
            std::string_view track;
            std::string_view id;
            std::string_view title;
            std::string_view description;
            int need;
        };
        static constexpr std::array<TrackAward, 7> track_awards = {{
            {"lab", "first-project", "Builder", "Finish your first Project Lab project.", 1},
            {"lab", "lab-graduate", "Lab Graduate", "Finish every Project Lab project.", 0},
            {"clinic", "master-debugger", "Master Debugger", "Diagnose and fix every case of the Debugging Clinic.", 0},
            {"reading", "code-reader", "Code Reader", "Work through every Code Reading lesson.", 0},
            {"challenges", "challenger", "Challenger", "Solve five problems of the Challenge Board.", 5},
            {"mastery", "first-mastery", "Proven", "Pass a Mastery Challenge.", 1},
            {"mastery", "grandmaster", "Grandmaster", "Pass every Mastery Challenge.", 0},
        }};
        for (auto const& t : track_awards)
        {
            auto const lessons = f.lessons_of_track(t.track);
            if (lessons.empty())
                continue;
            Award a = make_award(std::string(t.id), std::string(t.title), std::string(t.description), awards::Category::Project, f.path_of(*lessons.front()));
            if (t.need == 0)
                detail::all_of(a, f, lessons);
            else
            {
                std::vector<std::string> hits;
                for (auto const* e : lessons)
                    if (f.done(e->id))
                        hits.push_back(f.done_at(e->id));
                detail::count_of(a, std::min<int>(t.need, static_cast<int>(lessons.size())), hits);
            }
            add(std::move(a));
        }

        {
            Award a = make_award("second-chance", "Second Chance", "Answer a question correctly after getting it wrong.", awards::Category::Knowledge);
            std::vector<std::string> hits;
            for (auto const& [key, q] : p.questions)
                if (q.correct > 0 && q.attempts > q.correct && q.last == "right")
                    hits.push_back(q.last_at);
            detail::count_of(a, 1, hits);
            add(std::move(a));
        }
        {
            Award a = make_award("sharp-mind", "Sharp Mind", "Answer fifty questions correctly.", awards::Category::Knowledge);
            std::vector<std::string> hits;
            for (auto const& [key, q] : p.questions)
                if (q.correct > 0)
                    hits.push_back(q.first_correct_at);
            detail::count_of(a, 50, hits);
            add(std::move(a));
        }
        {
            Award a = make_award("long-memory", "Long Memory", "Bring ten flashcards to the fourth review box by remembering them on several days.",
                                 awards::Category::Knowledge);
            std::vector<std::string> hits;
            for (auto const& [key, c] : p.cards)
                if (c.box >= 4)
                    hits.push_back(c.last_at);
            detail::count_of(a, 10, hits);
            add(std::move(a));
        }
        {
            Award a =
                make_award("comeback", "Comeback", "Forget a flashcard, then remember it again until it reaches the third box.", awards::Category::Knowledge);
            a.hidden = true;
            std::vector<std::string> hits;
            for (auto const& [key, c] : p.cards)
                if (c.lapses > 0 && c.box >= 3)
                    hits.push_back(c.last_at);
            detail::count_of(a, 1, hits);
            add(std::move(a));
        }
        for (auto& a : out)
            if (a.earned && a.earned_at.empty())
                a.earned_at = "earlier";
        return out;
    }

    enum class Level
    {
        Unseen,
        Seen,
        Practiced,
        Demonstrated,
    };

    [[nodiscard]] constexpr std::string_view to_string(Level l) noexcept
    {
        switch (l)
        {
            case Level::Unseen:
                return "not started";
            case Level::Seen:
                return "seen";
            case Level::Practiced:
                return "practiced";
            case Level::Demonstrated:
                return "demonstrated";
        }
        return "not started";
    }

    struct Concept
    {
        std::string name;
        int lessons = 0;
        int started = 0;
        int completed = 0;
        int demanding = 0;
        int questions_right = 0;
        Level level = Level::Unseen;
    };

    [[nodiscard]] inline std::vector<Concept> concepts(catalog::Catalog const& cat, progress::Progress const& p, int min_lessons = 3)
    {
        std::map<std::string, Concept> by;
        for (auto const& e : cat.exercises())
        {
            std::set<std::string> names;
            for (auto const& c : e.concepts)
                names.insert(text::lower(text::trim(c)));
            for (auto const& name : names)
            {
                if (name.empty())
                    continue;
                auto& c = by[name];
                c.name = name;
                ++c.lessons;
                auto const st = progress::status_of(p, e.id);
                if (st != progress::Status::NotStarted)
                    ++c.started;
                if (st == progress::Status::Completed && !e.is_lecture())
                {
                    ++c.completed;
                    if (e.is_boss() || e.staged() || e.kind == catalog::ExerciseKind::Challenge)
                        ++c.demanding;
                }
                for (auto const& q : e.questions)
                    if (auto it = p.questions.find(progress::item_key(e.id, q.id)); it != p.questions.end() && it->second.correct > 0)
                        ++c.questions_right;
            }
        }
        std::vector<Concept> out;
        for (auto& [name, c] : by)
        {
            if (c.lessons < min_lessons)
                continue;
            if (c.completed >= 3 && c.demanding >= 1)
                c.level = Level::Demonstrated;
            else if (c.completed >= 1)
                c.level = Level::Practiced;
            else if (c.started > 0 || c.questions_right > 0)
                c.level = Level::Seen;
            out.push_back(std::move(c));
        }
        std::ranges::sort(out, [](Concept const& a, Concept const& b) {
            if (a.level != b.level)
                return a.level > b.level;
            if (a.lessons != b.lessons)
                return a.lessons > b.lessons;
            return a.name < b.name;
        });
        return out;
    }

    struct Seen
    {
        std::map<std::string, std::string> ids;
        bool existed = false;
    };

    [[nodiscard]] inline Seen load_seen(std::filesystem::path const& file)
    {
        Seen s;
        auto data = platform::read_file(file);
        if (!data)
            return s;
        s.existed = true;
        auto parsed = json::parse(*data);
        if (!parsed)
            return s;
        if (auto const* seen = parsed->find("seen"); seen && seen->as_object())
            for (auto const& [k, v] : *seen->as_object())
                if (auto str = v.as_string())
                    s.ids[k] = std::string(*str);
        return s;
    }

    [[nodiscard]] inline std::expected<void, text::Error> save_seen(std::filesystem::path const& file, Seen const& s)
    {
        auto root = json::Json::make_object();
        root.set("schema", json::Json(std::int64_t{1}));
        auto seen = json::Json::make_object();
        for (auto const& [k, v] : s.ids)
            seen.set(k, json::Json(v));
        root.set("seen", std::move(seen));
        return platform::atomic_write(file, root.dump() + "\n");
    }

    [[nodiscard]] inline std::vector<Award const*> unseen(std::vector<Award> const& all, Seen const& seen)
    {
        std::vector<Award const*> out;
        for (auto const& a : all)
            if (a.earned && !seen.ids.contains(a.id))
                out.push_back(&a);
        return out;
    }

    [[nodiscard]] inline Award const* next_milestone(std::vector<Award> const& all)
    {
        Award const* best = nullptr;
        double best_ratio = -1;
        for (auto const& a : all)
        {
            if (a.earned || a.hidden || a.need <= 0)
                continue;
            double const ratio = static_cast<double>(a.have) / a.need;
            if (a.have > 0 && ratio > best_ratio)
            {
                best = &a;
                best_ratio = ratio;
            }
        }
        if (!best)
            for (auto const& a : all)
                if (!a.earned && !a.hidden)
                    return &a;
        return best;
    }
} // namespace learndc::awards
