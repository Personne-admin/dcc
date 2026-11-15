export module learndc.activity;

import std;
import learndc.text;
import learndc.catalog;
import learndc.progress;

export namespace learndc::activity
{
    struct Answer
    {
        std::optional<int> choice;
        std::vector<int> choices;
        std::string text;
        std::optional<int> line;
        std::vector<int> order;
    };

    struct Feedback
    {
        bool correct = false;
        std::string verdict;
        std::vector<std::string> notes;
        std::string explanation;
        bool revealed = false;
    };

    [[nodiscard]] inline std::string normalize_short(std::string_view in)
    {
        std::string s = text::lower(text::trim(in));
        auto strip = [&](char c) {
            while (s.size() >= 2 && s.front() == c && s.back() == c)
                s = s.substr(1, s.size() - 2);
        };
        strip('`');
        strip('"');
        strip('\'');
        while (!s.empty() && (s.back() == '.' || s.back() == ';'))
            s.pop_back();
        std::string out;
        bool space = false;
        for (char c : s)
        {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                space = !out.empty();
                continue;
            }
            if (space)
                out += ' ';
            space = false;
            out += c;
        }
        return out;
    }

    [[nodiscard]] inline std::vector<std::string> normalize_output(std::string_view in)
    {
        std::vector<std::string> lines;
        for (auto const& line : text::split_lines(text::normalize_newlines(in)))
        {
            std::string l = line;
            while (!l.empty() && (l.back() == ' ' || l.back() == '\t'))
                l.pop_back();
            lines.push_back(std::move(l));
        }
        while (!lines.empty() && lines.back().empty())
            lines.pop_back();
        return lines;
    }

    [[nodiscard]] inline std::uint64_t seed_of(std::string_view s)
    {
        std::uint64_t h = 14695981039346656037ull;
        for (char c : s)
        {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    [[nodiscard]] inline std::vector<int> shuffled_order(catalog::Question const& q)
    {
        int const n = static_cast<int>(q.code_lines().size());
        std::vector<int> order(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            order[static_cast<std::size_t>(i)] = i;
        std::uint64_t state = seed_of(q.id + "\n" + q.code);
        for (int i = n - 1; i > 0; --i)
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            int const j = static_cast<int>((state >> 33) % static_cast<std::uint64_t>(i + 1));
            std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(j)]);
        }
        bool identity = true;
        for (int i = 0; i < n; ++i)
            if (order[static_cast<std::size_t>(i)] != i)
                identity = false;
        if (identity && n >= 2)
            std::rotate(order.begin(), order.begin() + 1, order.end());
        return order;
    }

    [[nodiscard]] inline std::string type_word(catalog::QuestionType t)
    {
        switch (t)
        {
            case catalog::QuestionType::Choice:
                return "Quiz";
            case catalog::QuestionType::Multi:
                return "Pick every right answer";
            case catalog::QuestionType::Predict:
                return "Predict the output";
            case catalog::QuestionType::Trace:
                return "Trace the program";
            case catalog::QuestionType::Bug:
                return "Find the bug";
            case catalog::QuestionType::Error:
                return "Explain the error";
            case catalog::QuestionType::Order:
                return "Put the code in order";
            case catalog::QuestionType::Short:
                return "Recall";
        }
        return "Quiz";
    }

    [[nodiscard]] inline bool answered(catalog::Question const& q, Answer const& a)
    {
        switch (q.type)
        {
            case catalog::QuestionType::Choice:
            case catalog::QuestionType::Error:
                return a.choice.has_value();
            case catalog::QuestionType::Multi:
                return !a.choices.empty();
            case catalog::QuestionType::Predict:
            case catalog::QuestionType::Trace:
            case catalog::QuestionType::Short:
                return !text::trim(a.text).empty();
            case catalog::QuestionType::Bug:
                return a.line.has_value();
            case catalog::QuestionType::Order:
                return a.order.size() == q.code_lines().size();
        }
        return false;
    }

    [[nodiscard]] inline Feedback grade(catalog::Question const& q, Answer const& a, int attempt = 1)
    {
        Feedback f;
        auto why_of = [&](int choice) -> std::string {
            if (choice >= 1 && choice <= static_cast<int>(q.why.size()))
                return q.why[static_cast<std::size_t>(choice - 1)];
            return {};
        };
        switch (q.type)
        {
            case catalog::QuestionType::Choice:
            case catalog::QuestionType::Error: {
                int const picked = a.choice.value_or(0);
                f.correct = picked == q.answer;
                if (auto w = why_of(picked); !w.empty())
                    f.notes.push_back(std::move(w));
                break;
            }
            case catalog::QuestionType::Multi: {
                auto picked = a.choices;
                std::ranges::sort(picked);
                picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
                f.correct = picked == q.answers;
                int hits = 0;
                for (int c : picked)
                {
                    if (std::ranges::find(q.answers, c) != q.answers.end())
                        ++hits;
                    else if (auto w = why_of(c); !w.empty())
                        f.notes.push_back(std::format("Choice {} does not hold: {}", c, w));
                    else
                        f.notes.push_back(std::format("Choice {} does not hold.", c));
                }
                if (!f.correct)
                    f.notes.push_back(std::format("You found {} of the {} statements that hold.", hits, q.answers.size()));
                break;
            }
            case catalog::QuestionType::Predict: {
                auto const want = normalize_output(q.expect);
                auto const got = normalize_output(a.text);
                f.correct = want == got;
                if (!f.correct)
                {
                    if (got.size() != want.size())
                        f.notes.push_back(std::format("The program prints {} line{}; you wrote {}.", want.size(), want.size() == 1 ? "" : "s", got.size()));
                    for (std::size_t i = 0; i < std::min(want.size(), got.size()); ++i)
                        if (want[i] != got[i])
                        {
                            f.notes.push_back(std::format("Line {} is the first one that differs.", i + 1));
                            break;
                        }
                }
                break;
            }
            case catalog::QuestionType::Trace:
            case catalog::QuestionType::Short: {
                auto const got = normalize_short(a.text);
                f.correct = std::ranges::any_of(q.accept, [&](std::string const& s) { return normalize_short(s) == got; });
                break;
            }
            case catalog::QuestionType::Bug: {
                int const line = a.line.value_or(0);
                f.correct = std::ranges::find(q.lines, line) != q.lines.end();
                if (!f.correct && line > 0)
                    f.notes.push_back(std::format("Line {} does what it says. Look for the line whose effect does not match the intent.", line));
                break;
            }
            case catalog::QuestionType::Order: {
                int in_place = 0;
                bool all = a.order.size() == q.code_lines().size();
                for (std::size_t i = 0; i < a.order.size(); ++i)
                    if (a.order[i] == static_cast<int>(i))
                        ++in_place;
                    else
                        all = false;
                f.correct = all;
                if (!f.correct)
                    f.notes.push_back(std::format("{} of {} lines are in the right place.", in_place, q.code_lines().size()));
                break;
            }
        }
        f.verdict = f.correct ? "Right" : "Not yet";
        f.revealed = f.correct || attempt >= 2;
        if (f.revealed)
            f.explanation = q.explanation;
        return f;
    }

    [[nodiscard]] inline std::string reveal_answer(catalog::Question const& q)
    {
        switch (q.type)
        {
            case catalog::QuestionType::Choice:
            case catalog::QuestionType::Error:
                return std::format("The answer is {}: {}", q.answer, q.choices[static_cast<std::size_t>(q.answer - 1)]);
            case catalog::QuestionType::Multi: {
                std::vector<std::string> nums;
                for (int c : q.answers)
                    nums.push_back(std::to_string(c));
                return std::format("The statements that hold are {}.", text::join(nums, ", "));
            }
            case catalog::QuestionType::Predict:
                return "The program prints:\n" + text::join(normalize_output(q.expect), "\n");
            case catalog::QuestionType::Trace:
            case catalog::QuestionType::Short:
                return std::format("The answer is {}.", q.accept.front());
            case catalog::QuestionType::Bug: {
                std::vector<std::string> nums;
                for (int c : q.lines)
                    nums.push_back(std::to_string(c));
                return std::format("The bug is on line {}.", text::join(nums, " or "));
            }
            case catalog::QuestionType::Order:
                return "The right order is:\n" + text::join(q.code_lines(), "\n");
        }
        return {};
    }

    [[nodiscard]] inline std::expected<Answer, std::string> parse_answer(catalog::Question const& q, std::string_view in)
    {
        Answer a;
        auto const parse_int = [](std::string_view s) -> std::optional<int> {
            int v = 0;
            auto t = text::trim(s);
            auto r = std::from_chars(t.data(), t.data() + t.size(), v);
            if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size())
                return std::nullopt;
            return v;
        };
        auto const numbers = [&](std::string_view s) -> std::optional<std::vector<int>> {
            std::vector<int> out;
            std::string flat(s);
            std::ranges::replace(flat, ',', ' ');
            for (auto const& piece : text::split(flat, ' '))
            {
                if (piece.empty())
                    continue;
                auto v = parse_int(piece);
                if (!v)
                    return std::nullopt;
                out.push_back(*v);
            }
            return out;
        };
        switch (q.type)
        {
            case catalog::QuestionType::Choice:
            case catalog::QuestionType::Error: {
                auto v = parse_int(in);
                if (!v || *v < 1 || *v > static_cast<int>(q.choices.size()))
                    return std::unexpected(std::format("give one choice number from 1 to {}", q.choices.size()));
                a.choice = *v;
                return a;
            }
            case catalog::QuestionType::Multi: {
                auto v = numbers(in);
                if (!v || v->empty() || std::ranges::any_of(*v, [&](int c) { return c < 1 || c > static_cast<int>(q.choices.size()); }))
                    return std::unexpected(std::format("give the choice numbers that hold, such as 1,3 (choices 1 to {})", q.choices.size()));
                a.choices = std::move(*v);
                return a;
            }
            case catalog::QuestionType::Bug: {
                auto v = parse_int(in);
                if (!v || *v < 1 || *v > static_cast<int>(q.code_lines().size()))
                    return std::unexpected(std::format("give the number of the buggy line (1 to {})", q.code_lines().size()));
                a.line = *v;
                return a;
            }
            case catalog::QuestionType::Order: {
                auto v = numbers(in);
                auto const n = q.code_lines().size();
                auto const shown = shuffled_order(q);
                if (!v || v->size() != n)
                    return std::unexpected(std::format("give all {} shown line numbers in the order they belong, such as 3 1 2", n));
                std::vector<int> seen;
                for (int shown_number : *v)
                {
                    if (shown_number < 1 || shown_number > static_cast<int>(n) || std::ranges::find(seen, shown_number) != seen.end())
                        return std::unexpected(std::format("use each shown line number from 1 to {} exactly once", n));
                    seen.push_back(shown_number);
                    a.order.push_back(shown[static_cast<std::size_t>(shown_number - 1)]);
                }
                return a;
            }
            case catalog::QuestionType::Predict:
            case catalog::QuestionType::Trace:
            case catalog::QuestionType::Short:
                a.text = text::replace_all(std::string(in), "\\n", "\n");
                return a;
        }
        return a;
    }

    enum class Rating
    {
        Again,
        Hard,
        Good,
        Easy,
    };

    [[nodiscard]] inline std::optional<Rating> parse_rating(std::string_view s)
    {
        if (s == "again")
            return Rating::Again;
        if (s == "hard")
            return Rating::Hard;
        if (s == "good")
            return Rating::Good;
        if (s == "easy")
            return Rating::Easy;
        return std::nullopt;
    }

    inline constexpr std::array<int, 6> box_interval_days = {0, 1, 2, 4, 8, 16};

    [[nodiscard]] inline int day_number(std::string_view iso)
    {
        if (iso.size() < 10)
            return 0;
        auto num = [&](std::size_t at, std::size_t len) {
            int v = 0;
            std::from_chars(iso.data() + at, iso.data() + at + len, v);
            return v;
        };
        int y = num(0, 4);
        int const m = num(5, 2);
        int const d = num(8, 2);
        y -= m <= 2 ? 1 : 0;
        int const era = (y >= 0 ? y : y - 399) / 400;
        int const yoe = y - era * 400;
        int const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        int const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    [[nodiscard]] inline progress::CardProgress schedule(progress::CardProgress c, Rating r, int today, std::string_view now)
    {
        ++c.reviews;
        c.last_at = std::string(now);
        switch (r)
        {
            case Rating::Again:
                c.box = 1;
                ++c.lapses;
                c.due_day = today;
                return c;
            case Rating::Hard:
                c.box = std::max(1, c.box);
                c.due_day = today + std::max(1, box_interval_days[static_cast<std::size_t>(c.box)] / 2);
                return c;
            case Rating::Good:
                c.box = std::min(5, c.box + 1);
                break;
            case Rating::Easy:
                c.box = std::min(5, c.box + 2);
                break;
        }
        c.due_day = today + std::max(1, box_interval_days[static_cast<std::size_t>(c.box)]);
        return c;
    }

    [[nodiscard]] inline bool card_due(progress::CardProgress const& c, int today)
    {
        return c.reviews == 0 || c.due_day <= today;
    }

} // namespace learndc::activity
