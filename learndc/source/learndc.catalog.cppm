export module learndc.catalog;

import std;
import learndc.text;
import learndc.toml;

export namespace learndc::catalog
{
    constexpr int supported_schema = 1;

    struct ContentError
    {
        std::string file;
        int line = 1;
        int column = 1;
        std::string message;

        [[nodiscard]] std::string format() const { return std::format("{}:{}:{}: {}", file, line, column, message); }
    };

    struct ContentSource
    {
        std::string label;
        std::map<std::string, std::string> files;

        void add(std::string path, std::string data) { files[std::move(path)] = std::move(data); }

        [[nodiscard]] bool contains(std::string_view path) const { return files.find(std::string(path)) != files.end(); }

        [[nodiscard]] std::string const* get(std::string_view path) const
        {
            auto it = files.find(std::string(path));
            return it == files.end() ? nullptr : &it->second;
        }
    };

    enum class ExerciseKind
    {
        RepairCompilation,
        FixBehavior,
        ImplementFunction,
        CompleteProgram,
        UnderstandDiagnostics,
        Refactor,
        MultiModule,
        Libdcext,
        Challenge,
        Lecture,
    };

    enum class Difficulty
    {
        Beginner,
        Intermediate,
        Advanced,
    };

    enum class CheckKind
    {
        Build,
        Run,
        Harness,
        Diagnostic,
    };

    enum class StdoutMatch
    {
        Exact,
        Contains,
        Trimmed,
    };

    enum class LineEndings
    {
        Portable,
        Exact,
    };

    enum class StarterState
    {
        FailsBuild,
        FailsChecks,
    };

    namespace detail
    {
        template <typename E> struct Name
        {
            E value;
            std::string_view text;
        };

        constexpr std::array<Name<ExerciseKind>, 10> kind_names = {{
            {ExerciseKind::RepairCompilation, "repair-compilation"},
            {ExerciseKind::FixBehavior, "fix-behavior"},
            {ExerciseKind::ImplementFunction, "implement-function"},
            {ExerciseKind::CompleteProgram, "complete-program"},
            {ExerciseKind::UnderstandDiagnostics, "understand-diagnostics"},
            {ExerciseKind::Refactor, "refactor"},
            {ExerciseKind::MultiModule, "multi-module"},
            {ExerciseKind::Libdcext, "libdcext"},
            {ExerciseKind::Challenge, "challenge"},
            {ExerciseKind::Lecture, "lecture"},
        }};

        constexpr std::array<Name<Difficulty>, 3> difficulty_names = {{
            {Difficulty::Beginner, "beginner"},
            {Difficulty::Intermediate, "intermediate"},
            {Difficulty::Advanced, "advanced"},
        }};

        constexpr std::array<Name<CheckKind>, 4> check_kind_names = {{
            {CheckKind::Build, "build"},
            {CheckKind::Run, "run"},
            {CheckKind::Harness, "harness"},
            {CheckKind::Diagnostic, "diagnostic"},
        }};

        constexpr std::array<Name<StdoutMatch>, 3> match_names = {{
            {StdoutMatch::Exact, "exact"},
            {StdoutMatch::Contains, "contains"},
            {StdoutMatch::Trimmed, "trimmed"},
        }};

        constexpr std::array<Name<LineEndings>, 2> ending_names = {{
            {LineEndings::Portable, "portable"},
            {LineEndings::Exact, "exact"},
        }};

        constexpr std::array<Name<StarterState>, 2> state_names = {{
            {StarterState::FailsBuild, "fails-build"},
            {StarterState::FailsChecks, "fails-checks"},
        }};

        template <typename E, std::size_t N> [[nodiscard]] constexpr std::string_view name_of(std::array<Name<E>, N> const& table, E value)
        {
            for (auto const& entry : table)
                if (entry.value == value)
                    return entry.text;
            return "unknown";
        }

        template <typename E, std::size_t N> [[nodiscard]] constexpr std::optional<E> parse_name(std::array<Name<E>, N> const& table, std::string_view text)
        {
            for (auto const& entry : table)
                if (entry.text == text)
                    return entry.value;
            return std::nullopt;
        }

        template <typename E, std::size_t N> [[nodiscard]] std::string expected_list(std::array<Name<E>, N> const& table)
        {
            std::string out;
            for (auto const& entry : table)
            {
                if (!out.empty())
                    out += ", ";
                out += entry.text;
            }
            return out;
        }
    } // namespace detail

    [[nodiscard]] constexpr std::string_view to_string(ExerciseKind v)
    {
        return detail::name_of(detail::kind_names, v);
    }
    [[nodiscard]] constexpr std::string_view to_string(Difficulty v)
    {
        return detail::name_of(detail::difficulty_names, v);
    }
    [[nodiscard]] constexpr std::string_view to_string(CheckKind v)
    {
        return detail::name_of(detail::check_kind_names, v);
    }
    [[nodiscard]] constexpr std::string_view to_string(StdoutMatch v)
    {
        return detail::name_of(detail::match_names, v);
    }
    [[nodiscard]] constexpr std::string_view to_string(LineEndings v)
    {
        return detail::name_of(detail::ending_names, v);
    }
    [[nodiscard]] constexpr std::string_view to_string(StarterState v)
    {
        return detail::name_of(detail::state_names, v);
    }

    [[nodiscard]] constexpr std::optional<ExerciseKind> parse_exercise_kind(std::string_view s)
    {
        return detail::parse_name(detail::kind_names, s);
    }
    [[nodiscard]] constexpr std::optional<Difficulty> parse_difficulty(std::string_view s)
    {
        return detail::parse_name(detail::difficulty_names, s);
    }

    [[nodiscard]] inline bool is_slug(std::string_view s)
    {
        if (s.empty() || s.front() == '-' || s.back() == '-')
            return false;
        char previous = '\0';
        for (char c : s)
        {
            bool const ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            if (!ok || (c == '-' && previous == '-'))
                return false;
            previous = c;
        }
        return true;
    }

    [[nodiscard]] inline bool is_exercise_id(std::string_view s)
    {
        auto const slash = s.find('/');
        if (slash == std::string_view::npos)
            return false;
        return is_slug(s.substr(0, slash)) && is_slug(s.substr(slash + 1));
    }

    [[nodiscard]] inline bool is_identifier(std::string_view s)
    {
        if (s.empty() || (s.front() >= '0' && s.front() <= '9'))
            return false;
        return std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
    }

    [[nodiscard]] inline bool is_harness_stem(std::string_view s)
    {
        return !s.empty() && s.front() >= 'a' && s.front() <= 'z' && is_identifier(s);
    }

    [[nodiscard]] inline bool is_version(std::string_view s)
    {
        auto parts = text::split(s, '.');
        if (parts.size() != 3)
            return false;
        return std::ranges::all_of(
            parts, [](std::string const& p) { return !p.empty() && p.size() <= 9 && std::ranges::all_of(p, [](char c) { return c >= '0' && c <= '9'; }); });
    }

    [[nodiscard]] inline std::optional<std::string> path_problem(std::string_view path)
    {
        if (path.empty())
            return "path is empty";
        if (path.front() == '/')
            return "path is absolute";
        if (path.find('\\') != std::string_view::npos)
            return "path contains a backslash";
        if (path.find(':') != std::string_view::npos)
            return "path contains a colon (drive letters are not allowed)";
        for (char c : path)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                return "path contains a control character";
        for (auto const& part : text::split(path, '/'))
        {
            if (part.empty())
                return "path contains an empty component";
            if (part == ".")
                return "path contains a '.' component";
            if (part == "..")
                return "path contains a '..' component";
            if (part.back() == '.' || part.back() == ' ')
                return "path component ends with a dot or a space";
        }
        return std::nullopt;
    }

    struct Check
    {
        CheckKind kind = CheckKind::Build;
        std::string name;
        std::string hint;
        std::vector<std::string> args;
        std::string stdin_text;
        std::optional<std::string> expect_stdout;
        StdoutMatch stdout_match = StdoutMatch::Exact;
        LineEndings line_endings = LineEndings::Portable;
        std::optional<std::string> expect_stderr_contains;
        int expect_exit = 0;
        int max_seconds = 10;
        std::string harness;
        bool expect_build_success = true;
        std::string diagnostic_contains;
        std::string stage;

        [[nodiscard]] std::string harness_stem() const
        {
            if (harness.size() > 10 && harness.starts_with("checks/") && harness.ends_with(".dc"))
                return harness.substr(7, harness.size() - 10);
            return {};
        }
    };

    enum class QuestionType
    {
        Choice,
        Multi,
        Predict,
        Trace,
        Bug,
        Error,
        Order,
        Short,
    };

    namespace detail
    {
        constexpr std::array<std::pair<QuestionType, std::string_view>, 8> question_type_names = {{
            {QuestionType::Choice, "choice"},
            {QuestionType::Multi, "multi"},
            {QuestionType::Predict, "predict"},
            {QuestionType::Trace, "trace"},
            {QuestionType::Bug, "bug"},
            {QuestionType::Error, "error"},
            {QuestionType::Order, "order"},
            {QuestionType::Short, "short"},
        }};
    } // namespace detail

    [[nodiscard]] constexpr std::string_view to_string(QuestionType t)
    {
        for (auto const& [value, name] : detail::question_type_names)
            if (value == t)
                return name;
        return "choice";
    }

    [[nodiscard]] constexpr std::optional<QuestionType> parse_question_type(std::string_view s)
    {
        for (auto const& [value, name] : detail::question_type_names)
            if (name == s)
                return value;
        return std::nullopt;
    }

    struct Question
    {
        std::string id;
        QuestionType type = QuestionType::Choice;
        std::string prompt;
        std::vector<std::string> choices;
        int answer = 1;
        std::vector<int> answers;
        std::vector<std::string> accept;
        std::vector<std::string> why;
        std::string code;
        std::string example;
        std::string expect;
        std::string diagnostic;
        std::string fixed;
        std::vector<int> lines;
        std::string explanation;

        [[nodiscard]] bool has_choices() const { return type == QuestionType::Choice || type == QuestionType::Multi || type == QuestionType::Error; }

        [[nodiscard]] std::vector<std::string> code_lines() const
        {
            auto out = text::split_lines(code);
            while (!out.empty() && text::trim(out.back()).empty())
                out.pop_back();
            return out;
        }
    };

    struct Card
    {
        std::string id;
        std::string front;
        std::string back;
    };

    struct Stage
    {
        std::string id;
        std::string title;
        std::string summary;
        bool optional = false;
        int revision = 1;
        std::vector<std::string> hints;
        std::vector<std::string> questions;
    };

    struct Boss
    {
        std::string story;
        int rating = 3;
    };

    struct Example
    {
        std::string name;
        std::string source;
        std::string expect_stdout;
    };

    struct Exercise
    {
        std::string id;
        std::string track;
        std::string name;
        std::string dir;
        int revision = 1;
        std::string title;
        std::string summary;
        ExerciseKind kind = ExerciseKind::CompleteProgram;
        std::string chapter;
        Difficulty difficulty = Difficulty::Beginner;
        int estimated_minutes = 0;
        std::vector<std::string> concepts;
        std::vector<std::string> tags;
        std::vector<std::string> objectives;
        std::string entry_file;
        std::string product;
        std::vector<std::string> prerequisites;
        std::vector<std::string> required_tools;
        std::string min_dcc;
        std::vector<std::string> platforms;
        std::vector<std::string> backends;
        StarterState starter_state = StarterState::FailsChecks;
        std::optional<int> build_timeout_seconds;
        std::vector<Check> checks;
        std::string readme;
        std::array<std::string, 3> hints;
        std::string explanation;
        std::map<std::string, std::string> starter;
        std::map<std::string, std::string> solution;
        std::map<std::string, std::string> check_files;
        bool optional = false;
        std::optional<Boss> boss;
        std::vector<Stage> stages;
        std::vector<Question> questions;
        std::vector<Card> cards;
        std::vector<Example> examples;
        bool drill = false;

        [[nodiscard]] Question const* find_question(std::string_view question_id) const
        {
            for (auto const& q : questions)
                if (q.id == question_id)
                    return &q;
            return nullptr;
        }
        std::vector<std::string> former_ids;
        int chapter_number = 0;

        [[nodiscard]] bool is_lecture() const { return kind == ExerciseKind::Lecture; }
        [[nodiscard]] bool is_boss() const { return boss.has_value(); }
        [[nodiscard]] bool staged() const { return !stages.empty(); }

        [[nodiscard]] Stage const* find_stage(std::string_view stage_id) const
        {
            for (auto const& st : stages)
                if (st.id == stage_id)
                    return &st;
            return nullptr;
        }

        [[nodiscard]] std::vector<Check const*> checks_of(std::string_view stage_id) const
        {
            std::vector<Check const*> out;
            for (auto const& c : checks)
                if (c.stage == stage_id)
                    out.push_back(&c);
            return out;
        }

        [[nodiscard]] bool supports_platform(std::string_view platform) const { return std::ranges::find(platforms, platform) != platforms.end(); }
        [[nodiscard]] bool supports_backend(std::string_view backend) const { return std::ranges::find(backends, backend) != backends.end(); }
    };

    struct Track
    {
        std::string id;
        std::string title;
        std::string summary;
        std::string goal;
        int number = 0;
        std::string path;
        std::vector<std::string> exercise_names;
        std::vector<std::string> exercise_ids;
    };

    enum class PathKind
    {
        Core,
        Specialization,
        Practice,
    };

    [[nodiscard]] constexpr std::string_view to_string(PathKind k) noexcept
    {
        switch (k)
        {
            case PathKind::Core:
                return "core";
            case PathKind::Specialization:
                return "specialization";
            case PathKind::Practice:
                return "practice";
        }
        return "core";
    }

    struct Path
    {
        std::string id;
        std::string title;
        std::string summary;
        PathKind kind = PathKind::Core;
        std::vector<std::string> tracks;
    };

    struct Filter
    {
        std::string path;
        std::string track;
        std::optional<Difficulty> difficulty;
        std::optional<ExerciseKind> kind;
        std::string query;
    };

    namespace detail
    {
        struct Reader
        {
            toml::Value const& table;
            std::string file;
            std::vector<ContentError>& errors;
            std::set<std::string, std::less<>> used;

            Reader(toml::Value const& t, std::string_view f, std::vector<ContentError>& e) : table(t), file(f), errors(e) {}

            void report(toml::Position pos, std::string message) { errors.push_back({file, pos.line, pos.column, std::move(message)}); }

            [[nodiscard]] toml::Value const* get(std::string_view key)
            {
                used.emplace(key);
                return table.find(key);
            }

            void missing(std::string_view key) { report(table.pos, std::format("missing required key '{}'", key)); }

            void wrong_type(std::string_view key, toml::Value const& v, std::string_view wanted)
            {
                report(v.pos, std::format("key '{}' must be {} (found {})", key, wanted, v.type_name()));
            }

            [[nodiscard]] toml::Position pos_of(std::string_view key) const
            {
                if (auto const* e = table.find_entry(key))
                    return e->value.pos;
                return table.pos;
            }

            [[nodiscard]] std::optional<std::string> string(std::string_view key, bool required = true, bool allow_empty = false)
            {
                auto const* v = get(key);
                if (v == nullptr)
                {
                    if (required)
                        missing(key);
                    return std::nullopt;
                }
                if (!v->is_string())
                {
                    wrong_type(key, *v, "a string");
                    return std::nullopt;
                }
                if (!allow_empty && text::trim(v->text).empty())
                {
                    report(v->pos, std::format("key '{}' must not be empty", key));
                    return std::nullopt;
                }
                return v->text;
            }

            [[nodiscard]] std::optional<std::int64_t> integer(std::string_view key, std::int64_t lo, std::int64_t hi, bool required = true)
            {
                auto const* v = get(key);
                if (v == nullptr)
                {
                    if (required)
                        missing(key);
                    return std::nullopt;
                }
                if (!v->is_integer())
                {
                    wrong_type(key, *v, "an integer");
                    return std::nullopt;
                }
                if (v->integer < lo || v->integer > hi)
                {
                    report(v->pos, std::format("key '{}' must be between {} and {}", key, lo, hi));
                    return std::nullopt;
                }
                return v->integer;
            }

            [[nodiscard]] std::optional<bool> boolean(std::string_view key)
            {
                auto const* v = get(key);
                if (v == nullptr)
                    return std::nullopt;
                if (!v->is_boolean())
                {
                    wrong_type(key, *v, "true or false");
                    return std::nullopt;
                }
                return v->boolean;
            }

            [[nodiscard]] std::optional<std::vector<std::string>> strings(std::string_view key, bool required = true, bool allow_empty = true,
                                                                          std::vector<toml::Position>* positions = nullptr)
            {
                auto const* v = get(key);
                if (v == nullptr)
                {
                    if (required)
                        missing(key);
                    return std::nullopt;
                }
                if (!v->is_array())
                {
                    wrong_type(key, *v, "an array of strings");
                    return std::nullopt;
                }
                std::vector<std::string> out;
                bool ok = true;
                for (auto const& item : v->items)
                {
                    if (!item.is_string())
                    {
                        report(item.pos, std::format("every element of '{}' must be a string (found {})", key, item.type_name()));
                        ok = false;
                        continue;
                    }
                    if (text::trim(item.text).empty())
                    {
                        report(item.pos, std::format("elements of '{}' must not be empty", key));
                        ok = false;
                        continue;
                    }
                    out.push_back(item.text);
                    if (positions != nullptr)
                        positions->push_back(item.pos);
                }
                if (ok && !allow_empty && out.empty())
                {
                    report(v->pos, std::format("key '{}' must have at least one element", key));
                    return std::nullopt;
                }
                if (!ok)
                    return std::nullopt;
                return out;
            }

            void reject_unknown()
            {
                for (auto const& entry : table.entries)
                    if (!used.contains(entry.key))
                        report(entry.pos, std::format("unknown key '{}'", entry.key));
            }
        };

        [[nodiscard]] inline bool read_schema(Reader& r)
        {
            auto const* v = r.get("schema");
            if (v == nullptr)
            {
                r.missing("schema");
                return false;
            }
            if (!v->is_integer())
            {
                r.wrong_type("schema", *v, "an integer");
                return false;
            }
            if (v->integer != supported_schema)
            {
                r.report(v->pos, std::format("unsupported schema {} (this learndc supports {})", v->integer, supported_schema));
                return false;
            }
            return true;
        }

        inline void check_ascii(std::string_view file, std::string_view content, std::vector<ContentError>& errors)
        {
            int line = 1;
            std::size_t line_start = 0;
            for (std::size_t i = 0; i < content.size(); ++i)
            {
                if (content[i] == '\n')
                {
                    ++line;
                    line_start = i + 1;
                }
                else if (static_cast<unsigned char>(content[i]) >= 0x80)
                {
                    errors.push_back({std::string(file), line, static_cast<int>(i - line_start) + 1, "non-ASCII character (content text must be ASCII only)"});
                    return;
                }
            }
        }

        [[nodiscard]] inline std::optional<toml::Value> parse_toml_file(ContentSource const& source, std::string const& path, std::vector<ContentError>& errors,
                                                                        bool required = true)
        {
            auto const* content = source.get(path);
            if (content == nullptr)
            {
                if (required)
                    errors.push_back({path, 1, 1, "file is missing"});
                return std::nullopt;
            }
            check_ascii(path, *content, errors);
            auto parsed = toml::parse(*content);
            if (!parsed)
            {
                errors.push_back({path, parsed.error().pos.line, parsed.error().pos.column, parsed.error().message});
                return std::nullopt;
            }
            return std::move(*parsed);
        }

        constexpr std::array<std::string_view, 3> known_tools = {"dcc", "dpm", "dccd"};
        constexpr std::array<std::string_view, 2> known_platforms = {"linux", "windows"};
        constexpr std::array<std::string_view, 2> known_backends = {"llvm", "custom"};

        [[nodiscard]] inline bool key_in(std::string_view key, std::span<std::string_view const> keys)
        {
            return std::ranges::find(keys, key) != keys.end();
        }

        [[nodiscard]] inline std::span<std::string_view const> allowed_check_keys(CheckKind kind)
        {
            static constexpr std::array<std::string_view, 3> build = {"kind", "name", "hint"};
            static constexpr std::array<std::string_view, 12> run = {
                "kind",        "name",        "hint", "args", "stdin", "expect_stdout", "stdout_match", "line_endings", "expect_stderr_contains",
                "expect_exit", "max_seconds", "stage"};
            static constexpr std::array<std::string_view, 11> harness = {
                "kind",        "name", "hint", "harness", "expect_exit", "expect_stdout", "stdout_match", "line_endings", "expect_stderr_contains",
                "max_seconds", "stage"};
            static constexpr std::array<std::string_view, 6> diagnostic = {"kind", "name", "hint", "expect_build", "diagnostic_contains", "stage"};
            switch (kind)
            {
                case CheckKind::Build:
                    return build;
                case CheckKind::Run:
                    return run;
                case CheckKind::Harness:
                    return harness;
                case CheckKind::Diagnostic:
                    return diagnostic;
            }
            return {};
        }

        [[nodiscard]] inline bool known_check_key(std::string_view key)
        {
            for (auto kind : {CheckKind::Build, CheckKind::Run, CheckKind::Harness, CheckKind::Diagnostic})
                if (key_in(key, allowed_check_keys(kind)))
                    return true;
            return false;
        }

        template <typename E, std::size_t N>
        [[nodiscard]] std::optional<E> read_enum(Reader& r, std::string_view key, std::array<Name<E>, N> const& table, bool required)
        {
            auto text_value = r.string(key, required);
            if (!text_value)
                return std::nullopt;
            auto parsed = parse_name(table, *text_value);
            if (!parsed)
            {
                r.report(r.pos_of(key), std::format("invalid {} '{}' (expected one of: {})", key, *text_value, expected_list(table)));
                return std::nullopt;
            }
            return parsed;
        }

        [[nodiscard]] inline std::optional<Check> parse_check(toml::Value const& table, std::string_view file, std::vector<ContentError>& errors)
        {
            Reader r(table, file, errors);
            auto const before = errors.size();
            auto kind = read_enum(r, "kind", check_kind_names, true);
            Check check;
            if (kind)
            {
                check.kind = *kind;
                for (auto const& entry : table.entries)
                {
                    if (key_in(entry.key, allowed_check_keys(*kind)))
                        continue;
                    r.used.insert(entry.key);
                    if (known_check_key(entry.key))
                        r.report(entry.pos, std::format("key '{}' is not valid for a {} check", entry.key, to_string(*kind)));
                    else
                        r.report(entry.pos, std::format("unknown key '{}'", entry.key));
                }
            }
            if (auto name = r.string("name"))
                check.name = *name;
            if (auto hint = r.string("hint", false))
                check.hint = *hint;
            if (kind && *kind != CheckKind::Build)
                if (auto st = r.string("stage", false))
                    check.stage = *st;
            if (!kind)
            {
                r.reject_unknown();
                return std::nullopt;
            }
            if (*kind == CheckKind::Run || *kind == CheckKind::Harness)
            {
                if (auto m = read_enum(r, "stdout_match", match_names, false))
                    check.stdout_match = *m;
                if (auto l = read_enum(r, "line_endings", ending_names, false))
                    check.line_endings = *l;
                if (auto s = r.string("expect_stdout", false, true))
                    check.expect_stdout = *s;
                if (auto s = r.string("expect_stderr_contains", false, true))
                    check.expect_stderr_contains = *s;
                if (auto e = r.integer("expect_exit", 0, 255, false))
                    check.expect_exit = static_cast<int>(*e);
                if (auto s = r.integer("max_seconds", 1, 60, false))
                    check.max_seconds = static_cast<int>(*s);
            }
            if (*kind == CheckKind::Run)
            {
                if (auto args = r.strings("args", false, true))
                    check.args = std::move(*args);
                if (auto s = r.string("stdin", false, true))
                    check.stdin_text = *s;
            }
            if (*kind == CheckKind::Harness)
            {
                if (auto h = r.string("harness"))
                {
                    check.harness = *h;
                    auto const pos = r.pos_of("harness");
                    if (auto problem = path_problem(*h))
                        r.report(pos, std::format("harness path '{}': {}", *h, *problem));
                    else if (!(h->starts_with("checks/") && h->ends_with(".dc") && h->size() > 10 && h->find('/', 7) == std::string::npos))
                        r.report(pos, std::format("harness '{}' must be of the form checks/<stem>.dc", *h));
                    else if (!is_harness_stem(check.harness_stem()))
                        r.report(pos, std::format("harness stem '{}' must match [a-z][a-z0-9_]*", check.harness_stem()));
                }
            }
            if (*kind == CheckKind::Diagnostic)
            {
                if (auto b = r.string("expect_build"))
                {
                    if (*b == "success")
                        check.expect_build_success = true;
                    else if (*b == "failure")
                        check.expect_build_success = false;
                    else
                        r.report(r.pos_of("expect_build"), std::format("invalid expect_build '{}' (expected one of: success, failure)", *b));
                }
                if (auto s = r.string("diagnostic_contains", false, true))
                    check.diagnostic_contains = *s;
            }
            r.reject_unknown();
            if (errors.size() != before)
                return std::nullopt;
            return check;
        }

        [[nodiscard]] inline std::optional<std::array<std::string, 3>> parse_hints(std::string_view content, std::string_view file,
                                                                                   std::vector<ContentError>& errors)
        {
            auto const before = errors.size();
            auto lines = text::split_lines(content);
            struct Section
            {
                std::size_t heading_line;
                std::string body;
            };
            std::vector<Section> sections;
            bool fenced = false;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                auto const& line = lines[i];
                auto const trimmed = text::trim(line);
                auto const report_line = static_cast<int>(i) + 1;
                if (trimmed.starts_with("```") || trimmed.starts_with("~~~"))
                    fenced = !fenced;
                if (!fenced && line.starts_with("## "))
                {
                    auto const expected = std::format("## Hint {}", sections.size() + 1);
                    if (sections.size() >= 3 || trimmed != expected)
                    {
                        errors.push_back(
                            {std::string(file), report_line, 1,
                             std::format("unexpected heading '{}' (hints.md must hold exactly '## Hint 1', '## Hint 2' and '## Hint 3' in that order)",
                                         trimmed)});
                        continue;
                    }
                    sections.push_back({i, {}});
                    continue;
                }
                if (sections.empty())
                {
                    if (!trimmed.empty())
                        errors.push_back({std::string(file), report_line, 1, "text before the '## Hint 1' heading"});
                    continue;
                }
                sections.back().body += line;
                sections.back().body += '\n';
            }
            if (sections.size() != 3 && errors.size() == before)
                errors.push_back({std::string(file), 1, 1, std::format("expected exactly three hint sections, found {}", sections.size())});
            std::array<std::string, 3> hints;
            for (std::size_t i = 0; i < sections.size() && i < 3; ++i)
            {
                hints[i] = std::string(text::trim(sections[i].body));
                if (hints[i].empty())
                    errors.push_back({std::string(file), static_cast<int>(sections[i].heading_line) + 1, 1, std::format("hint {} is empty", i + 1)});
            }
            if (errors.size() != before)
                return std::nullopt;
            return hints;
        }

        [[nodiscard]] inline std::span<std::string_view const> question_keys(QuestionType t)
        {
            static constexpr std::array<std::string_view, 7> choice = {"id", "type", "prompt", "choices", "answer", "why", "explanation"};
            static constexpr std::array<std::string_view, 7> multi = {"id", "type", "prompt", "choices", "answers", "why", "explanation"};
            static constexpr std::array<std::string_view, 7> predict = {"id", "type", "prompt", "code", "example", "expect", "explanation"};
            static constexpr std::array<std::string_view, 8> trace = {"id", "type", "prompt", "code", "example", "accept", "expect", "explanation"};
            static constexpr std::array<std::string_view, 9> bug = {"id", "type", "prompt", "code", "example", "lines", "fixed", "expect", "explanation"};
            static constexpr std::array<std::string_view, 10> error = {"id",      "type",       "prompt", "code", "example",
                                                                       "choices", "answer",     "why",    "diagnostic", "explanation"};
            static constexpr std::array<std::string_view, 6> order = {"id", "type", "prompt", "code", "example", "explanation"};
            static constexpr std::array<std::string_view, 5> short_answer = {"id", "type", "prompt", "accept", "explanation"};
            switch (t)
            {
                case QuestionType::Choice:
                    return choice;
                case QuestionType::Multi:
                    return multi;
                case QuestionType::Predict:
                    return predict;
                case QuestionType::Trace:
                    return trace;
                case QuestionType::Bug:
                    return bug;
                case QuestionType::Error:
                    return error;
                case QuestionType::Order:
                    return order;
                case QuestionType::Short:
                    return short_answer;
            }
            return {};
        }

        [[nodiscard]] inline std::vector<Question> parse_questions(toml::Value const& value, std::string_view file, std::vector<ContentError>& errors,
                                                                  std::vector<Example> const& examples)
        {
            std::vector<Question> out;
            if (!value.is_table_array())
            {
                errors.push_back({std::string(file), value.pos.line, value.pos.column, "key 'questions' must be an array of tables ([[questions]])"});
                return out;
            }
            std::set<std::string> ids;
            int number = 0;
            for (auto const& table : value.items)
            {
                ++number;
                Reader qr(table, file, errors);
                auto const question_errors = errors.size();
                Question q;
                q.id = std::format("q{}", number);
                if (auto t = qr.string("type", false))
                {
                    auto parsed = parse_question_type(*t);
                    if (!parsed)
                    {
                        qr.report(qr.pos_of("type"),
                                  std::format("invalid question type '{}' (expected one of: choice, multi, predict, trace, bug, error, order, short)", *t));
                        continue;
                    }
                    q.type = *parsed;
                }
                for (auto const& entry : table.entries)
                    if (!key_in(entry.key, question_keys(q.type)))
                    {
                        qr.used.insert(entry.key);
                        qr.report(entry.pos, std::format("key '{}' is not valid for a {} question", entry.key, to_string(q.type)));
                    }
                if (auto v = qr.string("id", false))
                {
                    if (!is_slug(*v))
                        qr.report(qr.pos_of("id"), std::format("question id '{}' must use lowercase letters, digits and hyphens", *v));
                    q.id = *v;
                }
                if (!ids.insert(q.id).second)
                    qr.report(table.pos, std::format("question id '{}' is used more than once", q.id));
                if (auto v = qr.string("prompt"))
                    q.prompt = *v;
                if (auto v = qr.string("explanation"))
                    q.explanation = *v;
                bool const wants_code = q.type == QuestionType::Predict || q.type == QuestionType::Trace || q.type == QuestionType::Bug ||
                                        q.type == QuestionType::Error || q.type == QuestionType::Order;
                if (wants_code)
                {
                    if (auto v = qr.string("code", false))
                        q.code = *v;
                    if (auto v = qr.string("example", false))
                    {
                        q.example = *v;
                        auto it = std::ranges::find(examples, *v, &Example::name);
                        if (it == examples.end())
                            qr.report(qr.pos_of("example"), std::format("example '{}' is not declared in [[examples]]", *v));
                        else if (q.code.empty())
                            q.code = it->source;
                        else if (it->source.find(q.code) == std::string::npos)
                            qr.report(qr.pos_of("code"), std::format("the code of an order question must appear verbatim in example '{}'", *v));
                    }
                    if (q.code.empty() && errors.size() == question_errors)
                        qr.report(table.pos, std::format("a {} question needs code or an example", to_string(q.type)));
                }
                if (q.has_choices())
                {
                    if (auto v = qr.strings("choices", true, false))
                    {
                        q.choices = std::move(*v);
                        if (q.choices.size() < 2 || q.choices.size() > 6)
                            qr.report(qr.pos_of("choices"), "a question needs two to six choices");
                    }
                    if (auto const* why = qr.get("why"))
                    {
                        if (!why->is_array() || !std::ranges::all_of(why->items, [](toml::Value const& item) { return item.is_string(); }))
                            qr.report(why->pos, "key 'why' must be an array of strings (one per choice, empty for the right one)");
                        else
                        {
                            for (auto const& item : why->items)
                                q.why.push_back(std::string(text::trim(item.text)));
                            if (q.why.size() != q.choices.size())
                                qr.report(why->pos, "'why' needs one explanation per choice");
                        }
                    }
                }
                if (q.type == QuestionType::Choice || q.type == QuestionType::Error)
                    if (auto v = qr.integer("answer", 1, 6))
                    {
                        q.answer = static_cast<int>(*v);
                        if (!q.choices.empty() && q.answer > static_cast<int>(q.choices.size()))
                            qr.report(qr.pos_of("answer"), std::format("answer {} is not one of the {} choices", q.answer, q.choices.size()));
                    }
                auto read_ints = [&](std::string_view key, int high) {
                    std::vector<int> ints;
                    auto const* v = qr.get(key);
                    if (v == nullptr)
                    {
                        qr.missing(key);
                        return ints;
                    }
                    if (!v->is_array() || v->items.empty())
                    {
                        qr.report(v->pos, std::format("key '{}' must be a non-empty array of numbers", key));
                        return ints;
                    }
                    for (auto const& item : v->items)
                    {
                        if (!item.is_integer() || item.integer < 1 || item.integer > high)
                        {
                            qr.report(item.pos, std::format("every element of '{}' must be a number from 1 to {}", key, high));
                            continue;
                        }
                        if (std::ranges::find(ints, static_cast<int>(item.integer)) != ints.end())
                            qr.report(item.pos, std::format("'{}' lists {} twice", key, item.integer));
                        ints.push_back(static_cast<int>(item.integer));
                    }
                    std::ranges::sort(ints);
                    return ints;
                };
                if (q.type == QuestionType::Multi)
                    q.answers = read_ints("answers", std::max<int>(1, static_cast<int>(q.choices.size())));
                if (q.type == QuestionType::Bug)
                    q.lines = read_ints("lines", std::max<int>(1, static_cast<int>(q.code_lines().size())));
                if (q.type == QuestionType::Trace || q.type == QuestionType::Short)
                    if (auto v = qr.strings("accept", true, false))
                        q.accept = std::move(*v);
                if (q.type == QuestionType::Predict)
                {
                    if (auto v = qr.string("expect", true, true))
                        q.expect = *v;
                }
                else if (q.type == QuestionType::Trace || q.type == QuestionType::Bug)
                {
                    if (auto v = qr.string("expect", false, true))
                        q.expect = *v;
                }
                if (q.type == QuestionType::Bug)
                    if (auto v = qr.string("fixed", false))
                        q.fixed = *v;
                if (q.type == QuestionType::Error)
                    if (auto v = qr.string("diagnostic"))
                        q.diagnostic = *v;
                if (q.type == QuestionType::Order)
                {
                    auto const lines = q.code_lines();
                    if (!q.code.empty() && (lines.size() < 2 || lines.size() > 14))
                        qr.report(qr.pos_of("code"), "an order question needs two to fourteen lines of code");
                    std::set<std::string> distinct;
                    for (auto const& l : lines)
                        if (!distinct.insert(std::string(text::trim(l))).second)
                        {
                            qr.report(qr.pos_of("code"), std::format("an order question cannot repeat the line '{}'", text::trim(l)));
                            break;
                        }
                }
                qr.reject_unknown();
                if (errors.size() == question_errors)
                    out.push_back(std::move(q));
            }
            return out;
        }

        [[nodiscard]] inline std::vector<Card> parse_cards(toml::Value const& value, std::string_view file, std::vector<ContentError>& errors)
        {
            std::vector<Card> out;
            if (!value.is_table_array())
            {
                errors.push_back({std::string(file), value.pos.line, value.pos.column, "key 'cards' must be an array of tables ([[cards]])"});
                return out;
            }
            std::set<std::string> ids;
            for (auto const& table : value.items)
            {
                Reader cr(table, file, errors);
                auto const before = errors.size();
                Card c;
                if (auto v = cr.string("id"))
                {
                    c.id = *v;
                    if (!is_slug(*v))
                        cr.report(cr.pos_of("id"), std::format("card id '{}' must use lowercase letters, digits and hyphens", *v));
                    else if (!ids.insert(*v).second)
                        cr.report(cr.pos_of("id"), std::format("card id '{}' is used more than once", *v));
                }
                if (auto v = cr.string("front"))
                    c.front = *v;
                if (auto v = cr.string("back"))
                    c.back = *v;
                cr.reject_unknown();
                if (errors.size() == before)
                    out.push_back(std::move(c));
            }
            return out;
        }

        struct PendingExercise
        {
            Exercise exercise;
            std::string file;
            std::vector<toml::Position> prerequisite_positions;
            toml::Position prerequisites_key;
        };

        [[nodiscard]] inline std::optional<PendingExercise> parse_exercise(ContentSource const& source, std::string_view track, std::string_view name,
                                                                           std::vector<ContentError>& errors)
        {
            auto const before = errors.size();
            std::string const dir = std::format("tracks/{}/{}", track, name);
            std::string const file = dir + "/exercise.toml";
            auto doc = parse_toml_file(source, file, errors);
            if (!doc)
                return std::nullopt;
            PendingExercise pending;
            pending.file = file;
            Exercise& ex = pending.exercise;
            ex.track = std::string(track);
            ex.name = std::string(name);
            ex.dir = dir;
            Reader r(*doc, file, errors);
            if (!read_schema(r))
                return std::nullopt;

            if (auto id = r.string("id"))
            {
                ex.id = *id;
                auto const expected = std::format("{}/{}", track, name);
                if (!is_exercise_id(*id))
                    r.report(r.pos_of("id"), std::format("id '{}' must look like <track>/<name> using lowercase letters, digits and hyphens", *id));
                else if (*id != expected)
                    r.report(r.pos_of("id"), std::format("id '{}' must equal '{}' (the track and directory name)", *id, expected));
            }
            if (auto rev = r.integer("revision", 1, 1000000))
                ex.revision = static_cast<int>(*rev);
            if (auto s = r.string("title"))
                ex.title = *s;
            if (auto s = r.string("summary"))
                ex.summary = *s;
            if (auto k = read_enum(r, "kind", kind_names, true))
            {
                ex.kind = *k;
                if (*k == ExerciseKind::Lecture)
                    r.report(r.pos_of("kind"), "kind 'lecture' belongs in lecture.toml; an exercise.toml needs a practice kind");
            }
            if (auto s = r.string("chapter", false))
            {
                ex.chapter = *s;
                if (!is_slug(*s))
                    r.report(r.pos_of("chapter"), std::format("chapter '{}' must use lowercase letters, digits and hyphens", *s));
            }
            if (auto d = read_enum(r, "difficulty", difficulty_names, true))
                ex.difficulty = *d;
            if (auto m = r.integer("estimated_minutes", 1, 1000))
                ex.estimated_minutes = static_cast<int>(*m);
            if (auto v = r.strings("concepts", true, false))
                ex.concepts = std::move(*v);
            if (auto v = r.strings("tags", false, true))
                ex.tags = std::move(*v);
            if (auto v = r.strings("objectives", true, false))
                ex.objectives = std::move(*v);
            if (auto s = r.string("entry_file"))
            {
                ex.entry_file = *s;
                if (auto problem = path_problem(*s))
                    r.report(r.pos_of("entry_file"), std::format("entry_file '{}': {}", *s, *problem));
            }
            if (auto s = r.string("product"))
            {
                ex.product = *s;
                if (!is_identifier(*s))
                    r.report(r.pos_of("product"), std::format("product '{}' must be an identifier ([a-z_][a-z0-9_]*)", *s));
            }
            pending.prerequisites_key = r.pos_of("prerequisites");
            if (auto v = r.strings("prerequisites", true, true, &pending.prerequisite_positions))
            {
                ex.prerequisites = std::move(*v);
                for (std::size_t i = 0; i < ex.prerequisites.size(); ++i)
                    if (!is_exercise_id(ex.prerequisites[i]))
                        r.report(pending.prerequisite_positions[i], std::format("prerequisite '{}' is not a valid exercise id", ex.prerequisites[i]));
            }
            if (auto v = r.strings("requires", true, false))
            {
                ex.required_tools = std::move(*v);
                for (auto const& tool : ex.required_tools)
                    if (std::ranges::find(known_tools, std::string_view(tool)) == known_tools.end())
                        r.report(r.pos_of("requires"), std::format("unknown tool '{}' in requires", tool));
            }
            if (auto s = r.string("min_dcc"))
            {
                ex.min_dcc = *s;
                if (!is_version(*s))
                    r.report(r.pos_of("min_dcc"), std::format("min_dcc '{}' must look like 1.2.3", *s));
            }
            if (auto v = r.strings("platforms", true, false))
            {
                ex.platforms = std::move(*v);
                for (auto const& p : ex.platforms)
                    if (std::ranges::find(known_platforms, std::string_view(p)) == known_platforms.end())
                        r.report(r.pos_of("platforms"), std::format("unknown platform '{}' (expected linux or windows)", p));
            }
            if (auto v = r.strings("backends", true, false))
            {
                ex.backends = std::move(*v);
                for (auto const& b : ex.backends)
                    if (std::ranges::find(known_backends, std::string_view(b)) == known_backends.end())
                        r.report(r.pos_of("backends"), std::format("unknown backend '{}' (expected llvm or custom)", b));
            }
            if (auto s = read_enum(r, "starter_state", state_names, true))
                ex.starter_state = *s;
            if (auto t = r.integer("build_timeout_seconds", 10, 600, false))
                ex.build_timeout_seconds = static_cast<int>(*t);
            if (auto o = r.boolean("optional"))
                ex.optional = *o;
            if (auto const* questions = r.get("questions"))
                ex.questions = parse_questions(*questions, file, errors, {});
            if (auto const* boss = r.get("boss"))
            {
                if (!boss->is_table())
                    r.wrong_type("boss", *boss, "a table ([boss])");
                else
                {
                    Reader br(*boss, file, errors);
                    Boss b;
                    if (auto v = br.string("story"))
                        b.story = *v;
                    if (auto v = br.integer("rating", 1, 5))
                        b.rating = static_cast<int>(*v);
                    br.reject_unknown();
                    ex.boss = std::move(b);
                }
            }
            if (auto const* stages = r.get("stages"))
            {
                if (!stages->is_table_array())
                    r.wrong_type("stages", *stages, "an array of tables ([[stages]])");
                else
                    for (auto const& table : stages->items)
                    {
                        Reader sr(table, file, errors);
                        auto const stage_errors = errors.size();
                        Stage st;
                        if (auto v = sr.string("id"))
                        {
                            st.id = *v;
                            if (!is_slug(*v))
                                sr.report(sr.pos_of("id"), std::format("stage id '{}' must use lowercase letters, digits and hyphens", *v));
                        }
                        if (auto v = sr.string("title"))
                            st.title = *v;
                        if (auto v = sr.string("summary"))
                            st.summary = *v;
                        if (auto v = sr.boolean("optional"))
                            st.optional = *v;
                        if (auto v = sr.integer("revision", 1, 1000000, false))
                            st.revision = static_cast<int>(*v);
                        if (auto v = sr.strings("hints", false, true))
                        {
                            st.hints = std::move(*v);
                            if (st.hints.size() > 3)
                                sr.report(sr.pos_of("hints"), "a stage has at most three hints");
                        }
                        if (auto v = sr.strings("questions", false, true))
                            st.questions = std::move(*v);
                        sr.reject_unknown();
                        if (errors.size() != stage_errors)
                            continue;
                        if (ex.find_stage(st.id) != nullptr)
                        {
                            errors.push_back({file, table.pos.line, table.pos.column, std::format("stage id '{}' is used more than once", st.id)});
                            continue;
                        }
                        ex.stages.push_back(std::move(st));
                    }
            }

            auto const* checks = r.get("checks");
            if (checks == nullptr)
                r.missing("checks");
            else if (!checks->is_table_array())
                r.wrong_type("checks", *checks, "an array of tables ([[checks]])");
            else
            {
                std::set<std::string> names;
                for (auto const& table : checks->items)
                {
                    auto check = parse_check(table, file, errors);
                    if (!check)
                        continue;
                    if (!names.insert(check->name).second)
                    {
                        errors.push_back({file, table.pos.line, table.pos.column, std::format("check name '{}' is used more than once", check->name)});
                        continue;
                    }
                    ex.checks.push_back(std::move(*check));
                }
                if (checks->items.empty())
                    r.report(checks->pos, "at least one check is required");
            }
            r.reject_unknown();

            for (auto const& check : ex.checks)
            {
                if (check.kind == CheckKind::Build)
                    continue;
                if (ex.stages.empty())
                {
                    if (!check.stage.empty())
                        errors.push_back({file, 1, 1, std::format("check '{}' names stage '{}' but the exercise has no [[stages]]", check.name, check.stage)});
                }
                else if (check.stage.empty())
                    errors.push_back({file, 1, 1, std::format("check '{}' must name its stage (stage = \"...\") because the exercise has stages", check.name)});
                else if (ex.find_stage(check.stage) == nullptr)
                    errors.push_back({file, 1, 1, std::format("check '{}' names the unknown stage '{}'", check.name, check.stage)});
            }
            std::set<std::string> staged_questions;
            for (auto const& st : ex.stages)
            {
                for (auto const& qid : st.questions)
                {
                    if (ex.find_question(qid) == nullptr)
                        errors.push_back({file, 1, 1, std::format("stage '{}' names the unknown question '{}'", st.id, qid)});
                    else if (!staged_questions.insert(qid).second)
                        errors.push_back({file, 1, 1, std::format("question '{}' belongs to more than one stage", qid)});
                }
                if (ex.checks_of(st.id).empty() && st.questions.empty())
                    errors.push_back({file, 1, 1, std::format("stage '{}' has no checks and no questions", st.id)});
                if (ex.boss && st.hints.empty())
                    errors.push_back({file, 1, 1, std::format("boss stage '{}' needs at least one hint (hints = [...])", st.id)});
            }
            for (auto const& q : ex.questions)
                if (!staged_questions.contains(q.id))
                    errors.push_back({file, 1, 1, std::format("question '{}' must belong to a stage (questions = [...])", q.id)});
            if (!ex.stages.empty() && std::ranges::all_of(ex.stages, [](Stage const& st) { return st.optional; }))
                errors.push_back({file, 1, 1, "at least one stage must be required (optional = false)"});
            if (ex.boss && ex.stages.size() < 2)
                errors.push_back({file, 1, 1, "a boss fight needs at least two [[stages]]"});

            if (!std::ranges::any_of(ex.checks, [](Check const& c) { return c.kind == CheckKind::Build; }))
            {
                Check build;
                build.kind = CheckKind::Build;
                build.name = "Compiles";
                if (std::ranges::any_of(ex.checks, [&](Check const& c) { return c.name == build.name; }))
                    build.name = "Compiles (build)";
                ex.checks.insert(ex.checks.begin(), std::move(build));
            }

            for (auto const& leaf : {"README.md", "hints.md", "explanation.md"})
            {
                auto const path = std::format("{}/{}", dir, leaf);
                auto const* content = source.get(path);
                if (content == nullptr)
                {
                    errors.push_back({path, 1, 1, "file is missing"});
                    continue;
                }
                check_ascii(path, *content, errors);
                if (text::trim(*content).empty())
                    errors.push_back({path, 1, 1, "file is empty"});
            }
            if (auto const* c = source.get(dir + "/README.md"))
                ex.readme = *c;
            if (auto const* c = source.get(dir + "/explanation.md"))
                ex.explanation = *c;
            if (auto const* c = source.get(dir + "/hints.md"))
                if (auto hints = parse_hints(*c, dir + "/hints.md", errors))
                    ex.hints = std::move(*hints);

            std::set<std::string> starter_lower;
            std::set<std::string> solution_lower;
            for (auto const& [path, content] : source.files)
            {
                if (!path.starts_with(dir + "/"))
                    continue;
                auto const rest = path.substr(dir.size() + 1);
                auto const slash = rest.find('/');
                auto const top = slash == std::string::npos ? rest : rest.substr(0, slash);
                if (slash == std::string::npos)
                {
                    if (top != "exercise.toml" && top != "README.md" && top != "hints.md" && top != "explanation.md")
                        errors.push_back({path, 1, 1, std::format("unexpected file '{}' in the exercise directory", top)});
                    continue;
                }
                auto const inner = rest.substr(slash + 1);
                if (top == "starter" || top == "solution")
                {
                    auto& lowered = top == "starter" ? starter_lower : solution_lower;
                    if (!lowered.insert(text::lower(inner)).second)
                        errors.push_back({path, 1, 1, "path differs only by letter case from another file"});
                    (top == "starter" ? ex.starter : ex.solution)[inner] = content;
                }
                else if (top == "checks")
                {
                    if (inner.find('/') != std::string::npos || !inner.ends_with(".dc") || !is_harness_stem(inner.substr(0, inner.size() - 3)))
                        errors.push_back({path, 1, 1, "harness files must be checks/<stem>.dc with a stem matching [a-z][a-z0-9_]*"});
                    ex.check_files["checks/" + inner] = content;
                }
                else
                    errors.push_back({path, 1, 1, std::format("unexpected directory '{}' in the exercise directory", top)});
            }

            if (ex.starter.empty())
                errors.push_back({dir + "/starter", 1, 1, "starter directory is missing or empty"});
            else
            {
                if (!ex.starter.contains("project.dcon"))
                    errors.push_back({dir + "/starter/project.dcon", 1, 1, "starter must contain project.dcon"});
                else if (!ex.product.empty() && ex.starter.at("project.dcon").find(std::format("\"{}\"", ex.product)) == std::string::npos)
                    errors.push_back({dir + "/starter/project.dcon", 1, 1, std::format("project.dcon does not mention the product \"{}\"", ex.product)});
                if (!ex.entry_file.empty() && !ex.starter.contains(ex.entry_file))
                {
                    auto const pos = r.pos_of("entry_file");
                    errors.push_back({file, pos.line, pos.column, std::format("entry_file '{}' does not exist in starter/", ex.entry_file)});
                }
            }
            if (ex.solution.empty())
                errors.push_back({dir + "/solution", 1, 1, "solution directory is missing or empty"});

            for (auto const& check : ex.checks)
            {
                if (check.kind != CheckKind::Harness || check.harness.empty())
                    continue;
                auto const* content = ex.check_files.contains(check.harness) ? &ex.check_files.at(check.harness) : nullptr;
                if (content == nullptr)
                {
                    errors.push_back({file, 1, 1, std::format("harness file '{}' of check '{}' does not exist", check.harness, check.name)});
                    continue;
                }
                auto const declaration = std::format("module {};", check.harness_stem());
                bool declared = false;
                for (auto const& line : text::split_lines(*content))
                    if (text::trim(line) == declaration)
                        declared = true;
                if (!declared)
                    errors.push_back({dir + "/" + check.harness, 1, 1, std::format("harness must declare '{}'", declaration)});
            }

            if (errors.size() != before)
                return std::nullopt;
            return pending;
        }

        [[nodiscard]] inline std::string expand_examples(std::string_view readme, std::vector<Example> const& examples, std::string_view file,
                                                         std::vector<ContentError>& errors)
        {
            std::string out;
            auto const lines = text::split_lines(readme);
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                auto const trimmed = text::trim(lines[i]);
                constexpr std::string_view marker = "```dc example=";
                if (!trimmed.starts_with(marker))
                {
                    out += lines[i];
                    out += '\n';
                    continue;
                }
                auto const name = std::string(text::trim(trimmed.substr(marker.size())));
                auto it = std::ranges::find(examples, name, &Example::name);
                if (it == examples.end())
                {
                    errors.push_back({std::string(file), static_cast<int>(i) + 1, 1, std::format("example '{}' is not declared in [[examples]]", name)});
                    continue;
                }
                if (i + 1 >= lines.size() || text::trim(lines[i + 1]) != "```")
                {
                    errors.push_back({std::string(file), static_cast<int>(i) + 1, 1,
                                      "an example fence must be closed on the next line; the code comes from the example file"});
                    continue;
                }
                ++i;
                out += "```dc\n";
                out += it->source;
                if (!it->source.ends_with('\n'))
                    out += '\n';
                out += "```\n";
            }
            return out;
        }

        [[nodiscard]] inline std::optional<PendingExercise> parse_lecture(ContentSource const& source, std::string_view track, std::string_view name,
                                                                          std::vector<ContentError>& errors)
        {
            auto const before = errors.size();
            std::string const dir = std::format("tracks/{}/{}", track, name);
            std::string const file = dir + "/lecture.toml";
            auto doc = parse_toml_file(source, file, errors);
            if (!doc)
                return std::nullopt;
            PendingExercise pending;
            pending.file = file;
            Exercise& ex = pending.exercise;
            ex.track = std::string(track);
            ex.name = std::string(name);
            ex.dir = dir;
            ex.kind = ExerciseKind::Lecture;
            ex.platforms = {"linux", "windows"};
            ex.backends = {"llvm", "custom"};
            Reader r(*doc, file, errors);
            if (!read_schema(r))
                return std::nullopt;
            if (auto id = r.string("id"))
            {
                ex.id = *id;
                auto const expected = std::format("{}/{}", track, name);
                if (!is_exercise_id(*id))
                    r.report(r.pos_of("id"), std::format("id '{}' must look like <track>/<name> using lowercase letters, digits and hyphens", *id));
                else if (*id != expected)
                    r.report(r.pos_of("id"), std::format("id '{}' must equal '{}' (the track and directory name)", *id, expected));
            }
            if (auto rev = r.integer("revision", 1, 1000000))
                ex.revision = static_cast<int>(*rev);
            if (auto s = r.string("title"))
                ex.title = *s;
            if (auto s = r.string("summary"))
                ex.summary = *s;
            if (auto d = read_enum(r, "difficulty", difficulty_names, true))
                ex.difficulty = *d;
            if (auto m = r.integer("estimated_minutes", 1, 1000))
                ex.estimated_minutes = static_cast<int>(*m);
            if (auto v = r.strings("concepts", true, false))
                ex.concepts = std::move(*v);
            if (auto v = r.strings("tags", false, true))
                ex.tags = std::move(*v);
            if (auto v = r.strings("objectives", true, false))
                ex.objectives = std::move(*v);
            pending.prerequisites_key = r.pos_of("prerequisites");
            if (auto v = r.strings("prerequisites", true, true, &pending.prerequisite_positions))
            {
                ex.prerequisites = std::move(*v);
                for (std::size_t i = 0; i < ex.prerequisites.size(); ++i)
                    if (!is_exercise_id(ex.prerequisites[i]))
                        r.report(pending.prerequisite_positions[i], std::format("prerequisite '{}' is not a valid exercise id", ex.prerequisites[i]));
            }
            if (auto o = r.boolean("optional"))
                ex.optional = *o;
            if (auto const* examples = r.get("examples"))
            {
                if (!examples->is_table_array())
                    r.wrong_type("examples", *examples, "an array of tables ([[examples]])");
                else
                    for (auto const& table : examples->items)
                    {
                        Reader er(table, file, errors);
                        Example e;
                        if (auto v = er.string("name"))
                        {
                            e.name = *v;
                            if (!is_harness_stem(*v))
                                er.report(er.pos_of("name"), std::format("example name '{}' must match [a-z][a-z0-9_]*", *v));
                            else if (auto const* code = source.get(std::format("{}/examples/{}.dc", dir, *v)))
                                e.source = *code;
                            else
                                er.report(er.pos_of("name"), std::format("example file 'examples/{}.dc' does not exist", *v));
                        }
                        if (auto v = er.string("expect_stdout", true, true))
                            e.expect_stdout = *v;
                        er.reject_unknown();
                        if (std::ranges::find(ex.examples, e.name, &Example::name) != ex.examples.end())
                            er.report(table.pos, std::format("example '{}' is declared more than once", e.name));
                        else
                            ex.examples.push_back(std::move(e));
                    }
            }
            if (auto const* questions = r.get("questions"))
                ex.questions = parse_questions(*questions, file, errors, ex.examples);
            if (auto const* cards = r.get("cards"))
                ex.cards = parse_cards(*cards, file, errors);
            if (auto k = r.string("kind", false))
            {
                if (*k == "drill")
                    ex.drill = true;
                else if (*k != "lecture")
                    r.report(r.pos_of("kind"), std::format("invalid kind '{}' for a lecture.toml (expected lecture or drill)", *k));
            }
            if (ex.drill && ex.questions.empty())
                errors.push_back({file, 1, 1, "a drill needs at least one [[questions]] entry"});
            r.reject_unknown();
            bool const runs_code = !ex.examples.empty() || std::ranges::any_of(ex.questions, [](Question const& q) { return !q.code.empty(); });
            if (runs_code)
                ex.required_tools = {"dcc", "dpm"};

            for (auto const& [path, content] : source.files)
            {
                if (!path.starts_with(dir + "/"))
                    continue;
                auto const rest = path.substr(dir.size() + 1);
                if (rest == "lecture.toml" || rest == "README.md")
                    continue;
                if (rest.starts_with("examples/"))
                {
                    auto const leaf = rest.substr(9);
                    auto const stem = leaf.ends_with(".dc") ? leaf.substr(0, leaf.size() - 3) : std::string();
                    if (stem.empty() || !is_harness_stem(stem))
                        errors.push_back({path, 1, 1, "example files must be examples/<name>.dc with a name matching [a-z][a-z0-9_]*"});
                    else if (std::ranges::find(ex.examples, stem, &Example::name) == ex.examples.end())
                        errors.push_back({path, 1, 1, std::format("example file '{}' is not declared in [[examples]]", rest)});
                    continue;
                }
                errors.push_back({path, 1, 1, std::format("unexpected file '{}' in a lecture directory (lecture.toml, README.md and examples/ only)", rest)});
            }
            auto const readme_path = dir + "/README.md";
            if (auto const* content = source.get(readme_path))
            {
                check_ascii(readme_path, *content, errors);
                if (text::trim(*content).empty())
                    errors.push_back({readme_path, 1, 1, "file is empty"});
                ex.readme = expand_examples(*content, ex.examples, readme_path, errors);
            }
            else
                errors.push_back({readme_path, 1, 1, "file is missing"});
            if (errors.size() != before)
                return std::nullopt;
            return pending;
        }
    } // namespace detail

    class Catalog
    {
    public:
        [[nodiscard]] static std::expected<Catalog, std::vector<ContentError>> load(ContentSource const& source)
        {
            std::vector<ContentError> errors;
            Catalog catalog;
            for (auto const& [path, content] : source.files)
                if (auto problem = path_problem(path))
                    errors.push_back({path, 1, 1, *problem});
            if (!errors.empty())
                return std::unexpected(std::move(errors));

            auto root = detail::parse_toml_file(source, "catalog.toml", errors);
            if (!root)
                return std::unexpected(std::move(errors));
            detail::Reader r(*root, "catalog.toml", errors);
            std::vector<std::string> track_names;
            std::vector<toml::Position> track_positions;
            std::vector<std::tuple<std::string, std::string, toml::Position>> renames;
            std::vector<std::pair<Path, toml::Position>> paths;
            if (detail::read_schema(r))
            {
                if (auto t = r.string("title"))
                    catalog.m_title = *t;
                if (auto names = r.strings("tracks", true, false, &track_positions))
                    track_names = std::move(*names);
                if (auto const* ps = r.get("paths"))
                {
                    if (!ps->is_table_array())
                        r.wrong_type("paths", *ps, "an array of tables ([[paths]])");
                    else
                        for (auto const& table : ps->items)
                        {
                            detail::Reader pr(table, "catalog.toml", errors);
                            Path path;
                            if (auto v = pr.string("id"))
                            {
                                path.id = *v;
                                if (!is_slug(*v))
                                    pr.report(pr.pos_of("id"), std::format("path id '{}' must use lowercase letters, digits and hyphens", *v));
                            }
                            if (auto v = pr.string("title"))
                                path.title = *v;
                            if (auto v = pr.string("summary"))
                                path.summary = *v;
                            if (auto v = pr.string("kind"))
                            {
                                if (*v == "core")
                                    path.kind = PathKind::Core;
                                else if (*v == "specialization")
                                    path.kind = PathKind::Specialization;
                                else if (*v == "practice")
                                    path.kind = PathKind::Practice;
                                else
                                    pr.report(pr.pos_of("kind"), std::format("path kind '{}' must be core, specialization or practice", *v));
                            }
                            if (auto v = pr.strings("tracks", true, false))
                                path.tracks = std::move(*v);
                            pr.reject_unknown();
                            paths.push_back({std::move(path), table.pos});
                        }
                }
                if (auto const* rn = r.get("renamed"))
                {
                    if (!rn->is_table())
                        r.wrong_type("renamed", *rn, "a table ([renamed])");
                    else
                        for (auto const& e : rn->entries)
                        {
                            if (!e.value.is_string())
                                errors.push_back(
                                    {"catalog.toml", e.pos.line, e.pos.column, std::format("renamed '{}' must map to an exercise id string", e.key)});
                            else
                                renames.push_back({e.key, e.value.text, e.pos});
                        }
                }
                r.reject_unknown();
            }
            else
                return std::unexpected(std::move(errors));
            std::set<std::string> seen_tracks;
            for (std::size_t i = 0; i < track_names.size(); ++i)
            {
                if (!is_slug(track_names[i]))
                {
                    errors.push_back({"catalog.toml", track_positions[i].line, track_positions[i].column,
                                      std::format("track name '{}' must use lowercase letters, digits and hyphens", track_names[i])});
                    continue;
                }
                if (!seen_tracks.insert(track_names[i]).second)
                    errors.push_back({"catalog.toml", track_positions[i].line, track_positions[i].column,
                                      std::format("track '{}' is listed more than once", track_names[i])});
            }
            std::set<std::string> directories;
            for (auto const& [path, content] : source.files)
            {
                if (!path.starts_with("tracks/"))
                    continue;
                auto const rest = path.substr(7);
                auto const slash = rest.find('/');
                directories.insert(slash == std::string::npos ? rest : rest.substr(0, slash));
            }
            for (auto const& dir : directories)
                if (!seen_tracks.contains(dir))
                    errors.push_back({"catalog.toml", 1, 1, std::format("track directory 'tracks/{}' is not listed in catalog.toml", dir)});

            std::map<std::string, std::string> path_of_track;
            if (paths.empty())
            {
                Path core;
                core.id = "core";
                core.title = catalog.m_title;
                core.kind = PathKind::Core;
                core.tracks = track_names;
                for (auto const& t : track_names)
                    path_of_track[t] = core.id;
                catalog.m_paths.push_back(std::move(core));
            }
            else
            {
                std::set<std::string> path_ids;
                for (auto const& [path, pos] : paths)
                {
                    if (!path_ids.insert(path.id).second)
                        errors.push_back({"catalog.toml", pos.line, pos.column, std::format("path '{}' is listed more than once", path.id)});
                    for (auto const& t : path.tracks)
                    {
                        if (!seen_tracks.contains(t))
                            errors.push_back(
                                {"catalog.toml", pos.line, pos.column, std::format("path '{}' names track '{}', which is not in tracks", path.id, t)});
                        else if (auto [it, inserted] = path_of_track.emplace(t, path.id); !inserted)
                            errors.push_back(
                                {"catalog.toml", pos.line, pos.column, std::format("track '{}' is in path '{}' and in path '{}'", t, it->second, path.id)});
                    }
                }
                for (std::size_t i = 0; i < track_names.size(); ++i)
                    if (seen_tracks.contains(track_names[i]) && !path_of_track.contains(track_names[i]))
                        errors.push_back(
                            {"catalog.toml", track_positions[i].line, track_positions[i].column, std::format("track '{}' is not in any path", track_names[i])});
                if (std::ranges::none_of(paths, [](auto const& p) { return p.first.kind == PathKind::Core; }))
                    errors.push_back({"catalog.toml", paths.front().second.line, paths.front().second.column, "one path must have kind = \"core\""});
                for (auto& [path, pos] : paths)
                    catalog.m_paths.push_back(std::move(path));
            }

            std::vector<detail::PendingExercise> pending;
            for (std::size_t i = 0; i < track_names.size(); ++i)
            {
                auto const& track_name = track_names[i];
                if (!is_slug(track_name))
                    continue;
                auto const track_file = std::format("tracks/{}/track.toml", track_name);
                if (!source.contains(track_file))
                {
                    errors.push_back(
                        {"catalog.toml", track_positions[i].line, track_positions[i].column, std::format("track '{}' has no {}", track_name, track_file)});
                    continue;
                }
                load_track(source, track_name, catalog, pending, errors);
            }

            std::map<std::string, std::size_t> index;
            for (std::size_t i = 0; i < pending.size(); ++i)
                index[pending[i].exercise.id] = i;
            for (auto const& p : pending)
                for (std::size_t k = 0; k < p.exercise.prerequisites.size(); ++k)
                {
                    auto const& prereq = p.exercise.prerequisites[k];
                    auto const pos = p.prerequisite_positions[k];
                    auto const moved = std::ranges::find_if(renames, [&](auto const& rn) { return std::get<0>(rn) == prereq; });
                    if (prereq == p.exercise.id)
                        errors.push_back({p.file, pos.line, pos.column, std::format("exercise '{}' lists itself as a prerequisite", prereq)});
                    else if (moved != renames.end())
                        errors.push_back({p.file, pos.line, pos.column, std::format("prerequisite '{}' was renamed to '{}'", prereq, std::get<1>(*moved))});
                    else if (!index.contains(prereq))
                        errors.push_back({p.file, pos.line, pos.column, std::format("prerequisite '{}' does not exist", prereq)});
                }
            detect_cycles(pending, index, errors);
            for (auto const& [from, to, pos] : renames)
            {
                if (!is_exercise_id(from))
                    errors.push_back({"catalog.toml", pos.line, pos.column, std::format("renamed id '{}' is not a valid exercise id", from)});
                else if (index.contains(from))
                    errors.push_back({"catalog.toml", pos.line, pos.column, std::format("'{}' is listed in [renamed] but still exists", from)});
                else if (!index.contains(to))
                    errors.push_back({"catalog.toml", pos.line, pos.column, std::format("'{}' is renamed to '{}', which does not exist", from, to)});
                else
                {
                    pending[index.at(to)].exercise.former_ids.push_back(from);
                    catalog.m_renamed[from] = to;
                }
            }

            if (!errors.empty())
                return std::unexpected(std::move(errors));
            std::map<std::string, int> numbers;
            std::map<std::string, int> track_numbers;
            for (auto& t : catalog.m_tracks)
            {
                if (auto it = path_of_track.find(t.id); it != path_of_track.end())
                    t.path = it->second;
                t.number = ++numbers[t.path];
                track_numbers[t.id] = t.number;
            }
            for (auto& p : pending)
            {
                if (auto it = track_numbers.find(p.exercise.track); it != track_numbers.end())
                    p.exercise.chapter_number = it->second;
                catalog.m_exercises.push_back(std::move(p.exercise));
            }
            for (std::size_t i = 0; i < catalog.m_exercises.size(); ++i)
                catalog.m_index[catalog.m_exercises[i].id] = i;
            return catalog;
        }

        [[nodiscard]] std::string const& title() const { return m_title; }
        [[nodiscard]] std::vector<Track> const& tracks() const { return m_tracks; }
        [[nodiscard]] std::vector<Path> const& paths() const { return m_paths; }

        [[nodiscard]] Path const* find_path(std::string_view id) const
        {
            for (auto const& p : m_paths)
                if (p.id == id)
                    return &p;
            return nullptr;
        }

        [[nodiscard]] Path const* path_of(Exercise const& e) const
        {
            auto const* t = find_track(e.track);
            return t ? find_path(t->path) : nullptr;
        }

        [[nodiscard]] bool in_core(Exercise const& e) const
        {
            auto const* p = path_of(e);
            return !p || p->kind == PathKind::Core;
        }
        [[nodiscard]] std::vector<Exercise> const& exercises() const { return m_exercises; }

        [[nodiscard]] Exercise const* find(std::string_view id) const
        {
            auto it = m_index.find(std::string(id));
            if (it == m_index.end())
                if (auto moved = m_renamed.find(std::string(id)); moved != m_renamed.end())
                    it = m_index.find(moved->second);
            return it == m_index.end() ? nullptr : &m_exercises[it->second];
        }

        [[nodiscard]] std::map<std::string, std::string> const& renamed() const { return m_renamed; }

        [[nodiscard]] Track const* find_track(std::string_view id) const
        {
            for (auto const& t : m_tracks)
                if (t.id == id)
                    return &t;
            return nullptr;
        }

        [[nodiscard]] std::vector<Exercise const*> in_track(std::string_view track) const
        {
            std::vector<Exercise const*> out;
            for (auto const& e : m_exercises)
                if (e.track == track)
                    out.push_back(&e);
            return out;
        }

        [[nodiscard]] bool prerequisites_met(Exercise const& exercise, std::function<bool(std::string_view)> const& completed) const
        {
            return std::ranges::all_of(exercise.prerequisites, [&](std::string const& id) { return completed(id); });
        }

        [[nodiscard]] std::vector<Exercise const*> search(std::string_view query) const
        {
            std::vector<std::string> tokens;
            for (auto const& piece : text::split(text::lower(query), ' '))
                if (!piece.empty())
                    tokens.push_back(piece);
            std::vector<std::pair<int, Exercise const*>> scored;
            for (auto const& e : m_exercises)
            {
                int total = 0;
                bool all = true;
                for (auto const& token : tokens)
                {
                    int score = score_token(e, token);
                    if (score == 0)
                    {
                        all = false;
                        break;
                    }
                    total += score;
                }
                if (all)
                    scored.emplace_back(total, &e);
            }
            std::ranges::stable_sort(scored, [](auto const& a, auto const& b) { return a.first > b.first; });
            std::vector<Exercise const*> out;
            for (auto const& [score, e] : scored)
                out.push_back(e);
            return out;
        }

        [[nodiscard]] std::vector<Exercise const*> filter(Filter const& filter, std::function<bool(Exercise const&)> const& extra = {}) const
        {
            std::vector<Exercise const*> out;
            auto candidates = filter.query.empty() ? all_pointers() : search(filter.query);
            for (auto const* e : candidates)
            {
                if (!filter.track.empty() && e->track != filter.track)
                    continue;
                if (!filter.path.empty())
                    if (auto const* path = path_of(*e); !path || path->id != filter.path)
                        continue;
                if (filter.difficulty && e->difficulty != *filter.difficulty)
                    continue;
                if (filter.kind && e->kind != *filter.kind)
                    continue;
                if (extra && !extra(*e))
                    continue;
                out.push_back(e);
            }
            return out;
        }

        [[nodiscard]] std::vector<Exercise const*> topological_order() const
        {
            std::vector<std::size_t> remaining(m_exercises.size(), 0);
            for (std::size_t i = 0; i < m_exercises.size(); ++i)
                remaining[i] = m_exercises[i].prerequisites.size();
            std::vector<bool> done(m_exercises.size(), false);
            std::vector<Exercise const*> order;
            while (order.size() < m_exercises.size())
            {
                bool progressed = false;
                for (std::size_t i = 0; i < m_exercises.size(); ++i)
                {
                    if (done[i] || remaining[i] != 0)
                        continue;
                    done[i] = true;
                    order.push_back(&m_exercises[i]);
                    progressed = true;
                    for (std::size_t j = 0; j < m_exercises.size(); ++j)
                        if (!done[j] && std::ranges::find(m_exercises[j].prerequisites, m_exercises[i].id) != m_exercises[j].prerequisites.end())
                            --remaining[j];
                    break;
                }
                if (!progressed)
                    break;
            }
            return order;
        }

        [[nodiscard]] Exercise const* next_recommended(std::function<bool(std::string_view)> const& completed, std::string_view after = {}) const
        {
            auto const order = topological_order();
            if (order.empty())
                return nullptr;
            std::size_t start = 0;
            if (!after.empty())
                for (std::size_t i = 0; i < order.size(); ++i)
                    if (order[i]->id == after)
                    {
                        start = i + 1;
                        break;
                    }
            std::string const current_path = [&] {
                if (auto const* e = after.empty() ? nullptr : find(after))
                    if (auto const* path = path_of(*e))
                        return path->id;
                return std::string{};
            }();
            auto const pick = [&](auto const& wanted) -> Exercise const* {
                for (std::size_t step = 0; step < order.size(); ++step)
                {
                    auto const* e = order[(start + step) % order.size()];
                    if (wanted(*e) && !e->optional && !completed(e->id) && prerequisites_met(*e, completed))
                        return e;
                }
                return nullptr;
            };
            if (!current_path.empty())
                if (auto const* e = pick([&](Exercise const& e) {
                        auto const* path = path_of(e);
                        return path && path->id == current_path;
                    }))
                    return e;
            return pick([&](Exercise const& e) { return in_core(e); });
        }

    private:
        std::string m_title;
        std::vector<Track> m_tracks;
        std::vector<Path> m_paths;
        std::vector<Exercise> m_exercises;
        std::map<std::string, std::size_t> m_index;
        std::map<std::string, std::string> m_renamed;

        [[nodiscard]] std::vector<Exercise const*> all_pointers() const
        {
            std::vector<Exercise const*> out;
            for (auto const& e : m_exercises)
                out.push_back(&e);
            return out;
        }

        [[nodiscard]] static int score_token(Exercise const& e, std::string const& token)
        {
            int score = 0;
            auto const has = [&](std::string_view field) { return text::lower(field).find(token) != std::string::npos; };
            if (has(e.title))
                score += 5;
            if (has(e.id))
                score += 4;
            for (auto const& concept_name : e.concepts)
            {
                auto const lowered = text::lower(concept_name);
                if (lowered == token)
                    score += 4;
                else if (lowered.find(token) != std::string::npos)
                    score += 3;
            }
            for (auto const& tag : e.tags)
                if (text::lower(tag).find(token) != std::string::npos)
                    score += 2;
            if (has(e.summary))
                score += 1;
            if (has(e.chapter))
                score += 1;
            if (has(to_string(e.kind)))
                score += 1;
            return score;
        }

        static void load_track(ContentSource const& source, std::string const& track_name, Catalog& catalog, std::vector<detail::PendingExercise>& pending,
                               std::vector<ContentError>& errors)
        {
            auto const file = std::format("tracks/{}/track.toml", track_name);
            auto doc = detail::parse_toml_file(source, file, errors);
            if (!doc)
                return;
            detail::Reader r(*doc, file, errors);
            if (!detail::read_schema(r))
                return;
            Track track;
            if (auto id = r.string("id"))
            {
                track.id = *id;
                if (*id != track_name)
                    r.report(r.pos_of("id"), std::format("id '{}' must equal the directory name '{}'", *id, track_name));
            }
            if (auto s = r.string("title"))
                track.title = *s;
            if (auto s = r.string("summary"))
                track.summary = *s;
            if (auto s = r.string("goal", false))
                track.goal = *s;
            track.number = static_cast<int>(catalog.m_tracks.size()) + 1;
            std::vector<toml::Position> positions;
            std::vector<std::string> names;
            if (auto v = r.strings("exercises", true, true, &positions))
                names = std::move(*v);
            r.reject_unknown();
            std::set<std::string> listed;
            std::set<std::string> directories;
            auto const prefix = std::format("tracks/{}/", track_name);
            for (auto const& [path, content] : source.files)
            {
                if (!path.starts_with(prefix))
                    continue;
                auto const rest = path.substr(prefix.size());
                auto const slash = rest.find('/');
                if (slash != std::string::npos)
                    directories.insert(rest.substr(0, slash));
                else if (rest != "track.toml")
                    errors.push_back({path, 1, 1, "unexpected file in a track directory"});
            }
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                auto const& name = names[i];
                if (!is_slug(name))
                {
                    errors.push_back(
                        {file, positions[i].line, positions[i].column, std::format("exercise name '{}' must use lowercase letters, digits and hyphens", name)});
                    continue;
                }
                if (!listed.insert(name).second)
                {
                    errors.push_back({file, positions[i].line, positions[i].column, std::format("exercise '{}' is listed more than once", name)});
                    continue;
                }
                bool const has_exercise = source.contains(std::format("{}{}/exercise.toml", prefix, name));
                bool const has_lecture = source.contains(std::format("{}{}/lecture.toml", prefix, name));
                if (!directories.contains(name) || (!has_exercise && !has_lecture))
                {
                    errors.push_back({file, positions[i].line, positions[i].column,
                                      std::format("exercise '{}' has no {}{}/exercise.toml or lecture.toml", name, prefix, name)});
                    continue;
                }
                if (has_exercise && has_lecture)
                {
                    errors.push_back({std::format("{}{}/lecture.toml", prefix, name), 1, 1, "a lesson has either exercise.toml or lecture.toml, not both"});
                    continue;
                }
                track.exercise_names.push_back(name);
                track.exercise_ids.push_back(std::format("{}/{}", track_name, name));
                auto ex = has_lecture ? detail::parse_lecture(source, track_name, name, errors) : detail::parse_exercise(source, track_name, name, errors);
                if (ex)
                {
                    ex->exercise.chapter_number = static_cast<int>(catalog.m_tracks.size()) + 1;
                    pending.push_back(std::move(*ex));
                }
            }
            for (auto const& dir : directories)
                if (!listed.contains(dir))
                    errors.push_back({file, 1, 1, std::format("exercise directory '{}{}' is not listed in track.toml", prefix, dir)});
            catalog.m_tracks.push_back(std::move(track));
        }

        static void detect_cycles(std::vector<detail::PendingExercise> const& pending, std::map<std::string, std::size_t> const& index,
                                  std::vector<ContentError>& errors)
        {
            enum class Color
            {
                White,
                Grey,
                Black,
            };
            std::vector<Color> color(pending.size(), Color::White);
            std::vector<std::size_t> stack;
            std::set<std::size_t> reported;
            std::function<void(std::size_t)> visit = [&](std::size_t node) {
                color[node] = Color::Grey;
                stack.push_back(node);
                for (auto const& prereq : pending[node].exercise.prerequisites)
                {
                    auto it = index.find(prereq);
                    if (it == index.end() || it->second == node)
                        continue;
                    if (color[it->second] == Color::White)
                        visit(it->second);
                    else if (color[it->second] == Color::Grey)
                    {
                        auto begin = std::ranges::find(stack, it->second);
                        std::string chain;
                        for (auto s = begin; s != stack.end(); ++s)
                            chain += pending[*s].exercise.id + " -> ";
                        chain += pending[it->second].exercise.id;
                        if (reported.insert(*begin).second)
                        {
                            auto const& first = pending[*begin];
                            errors.push_back(
                                {first.file, first.prerequisites_key.line, first.prerequisites_key.column, std::format("prerequisite cycle: {}", chain)});
                        }
                    }
                }
                stack.pop_back();
                color[node] = Color::Black;
            };
            for (std::size_t i = 0; i < pending.size(); ++i)
                if (color[i] == Color::White)
                    visit(i);
        }
    };

    [[nodiscard]] inline std::expected<ContentSource, text::Error> load_directory(std::filesystem::path const& root)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            return std::unexpected(text::Error{std::format("content directory '{}' does not exist", root.string())});
        ContentSource source;
        source.label = root.string();
        constexpr std::array<std::string_view, 5> allowed = {".toml", ".md", ".dc", ".dcon", ".txt"};
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                return std::unexpected(text::Error{std::format("cannot read '{}': {}", root.string(), ec.message())});
            auto const& entry = *it;
            auto const relative = entry.path().lexically_relative(root);
            auto const u8 = relative.generic_u8string();
            std::string rel(reinterpret_cast<char const*>(u8.data()), u8.size());
            if (entry.is_symlink(ec))
                return std::unexpected(text::Error{std::format("{}: symbolic links are not allowed in learndc content", rel)});
            auto const parts = text::split(rel, '/');
            bool hidden = false;
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                bool const last = i + 1 == parts.size();
                if (parts[i].starts_with('.') && !(last && parts[i] == ".gitignore"))
                    hidden = true;
            }
            if (hidden)
            {
                if (entry.is_directory(ec))
                    it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(ec))
                continue;
            auto const extension = entry.path().extension().string();
            if (entry.path().filename() != ".gitignore" && std::ranges::find(allowed, std::string_view(extension)) == allowed.end())
                return std::unexpected(text::Error{std::format("{}: unsupported content file type (allowed: .toml, .md, .dc, .dcon, .txt)", rel)});
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in)
                return std::unexpected(text::Error{std::format("{}: cannot read file", rel)});
            std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            source.files[rel] = std::move(data);
        }
        if (ec)
            return std::unexpected(text::Error{std::format("cannot read '{}': {}", root.string(), ec.message())});
        return source;
    }

    [[nodiscard]] inline std::string format_errors(std::vector<ContentError> const& errors)
    {
        std::string out;
        for (auto const& e : errors)
        {
            out += e.format();
            out += '\n';
        }
        return out;
    }
} // namespace learndc::catalog
