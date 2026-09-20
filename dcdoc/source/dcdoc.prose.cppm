export module dcdoc.prose;

import std;
import dcc.sm;
import dcc.si;
import dcc.lex.tokens;
import dcc.lex;
import dcdoc.model;

export namespace dcdoc::prose
{
    enum class HlClass
    {
        Plain,
        Keyword,
        Literal,
    };

    struct HlToken
    {
        HlClass cls;
        std::string text;
    };

    class Highlighter
    {
    public:
        [[nodiscard]] std::vector<HlToken> highlight(std::string_view sig)
        {
            std::vector<HlToken> out;
            std::string uri = "dcdoc-sig://" + std::to_string(m_next++);
            dcc::sm::FileId fid = m_sm.open_in_memory(std::move(uri), std::string{sig});
            auto const* file = m_sm.get(fid);
            if (!file)
            {
                out.push_back({.cls = HlClass::Plain, .text = std::string{sig}});
                return out;
            }
            dcc::lex::Lexer lexer{*file, m_interner, false};
            std::string_view text = file->text();
            std::uint32_t prev = 0;
            while (true)
            {
                auto tok = lexer.next();
                if (tok.kind == dcc::lex::TokenKind::Eof)
                    break;
                std::uint32_t begin = tok.range.begin.offset;
                std::uint32_t end = tok.range.end.offset;
                if (begin > text.size())
                    break;
                if (end > static_cast<std::uint32_t>(text.size()))
                    end = static_cast<std::uint32_t>(text.size());
                if (begin > prev)
                    out.push_back({.cls = HlClass::Plain, .text = std::string{text.substr(prev, begin - prev)}});
                prev = end;
                std::string raw{text.substr(begin, end - begin)};
                if (dcc::lex::is_keyword(tok.kind))
                    out.push_back({.cls = HlClass::Keyword, .text = std::move(raw)});
                else if (dcc::lex::is_literal(tok.kind))
                    out.push_back({.cls = HlClass::Literal, .text = std::move(raw)});
                else
                    out.push_back({.cls = HlClass::Plain, .text = std::move(raw)});
                if (begin == end && tok.kind != dcc::lex::TokenKind::Eof)
                    break;
            }
            if (prev < text.size())
                out.push_back({.cls = HlClass::Plain, .text = std::string{text.substr(prev)}});
            return out;
        }

    private:
        dcc::sm::SourceManager m_sm;
        dcc::si::string_interner m_interner;
        std::uint64_t m_next{};
    };

    enum class InlineKind
    {
        Text,
        Code,
        Link,
    };

    struct Inline
    {
        InlineKind kind;
        std::string text;
        std::string target{};
        bool ambiguous{};
        bool resolved{};
    };

    struct Block
    {
        bool code{};
        std::size_t start{};
        std::string lang;
        std::string text;
        std::vector<Inline> spans;
    };

    [[nodiscard]] bool blank_line(std::string_view l) noexcept
    {
        for (char c : l)
            if (c != 32 && c != 9 && c != 13)
                return false;
        return true;
    }

    [[nodiscard]] std::string trim(std::string_view l)
    {
        std::size_t b = 0;
        while (b < l.size() && (l[b] == 32 || l[b] == 9 || l[b] == 13))
            ++b;
        std::size_t e = l.size();
        while (e > b && (l[e - 1] == 32 || l[e - 1] == 9 || l[e - 1] == 13))
            --e;
        return std::string{l.substr(b, e - b)};
    }

    [[nodiscard]] bool fence_line(std::string_view l)
    {
        std::string t = trim(l);
        return t.size() >= 3 && t[0] == 96 && t[1] == 96 && t[2] == 96;
    }

    [[nodiscard]] std::string fence_lang(std::string_view l)
    {
        std::string t = trim(l);
        std::size_t i = 3;
        while (i < t.size() && (t[i] == 32 || t[i] == 9))
            ++i;
        return t.substr(i);
    }

    [[nodiscard]] std::vector<Inline> split_inline(std::string_view s)
    {
        std::vector<Inline> out;
        std::size_t i = 0;
        while (i < s.size())
        {
            std::size_t j = s.find(96, i);
            if (j == std::string_view::npos)
            {
                out.push_back({.kind = InlineKind::Text, .text = std::string{s.substr(i)}});
                break;
            }
            std::size_t k = s.find(96, j + 1);
            if (k == std::string_view::npos)
            {
                out.push_back({.kind = InlineKind::Text, .text = std::string{s.substr(i)}});
                break;
            }
            if (j > i)
                out.push_back({.kind = InlineKind::Text, .text = std::string{s.substr(i, j - i)}});
            out.push_back({.kind = InlineKind::Code, .text = std::string{s.substr(j + 1, k - j - 1)}});
            i = k + 1;
        }
        return out;
    }

    [[nodiscard]] std::vector<Block> parse_doc(std::string_view doc, std::vector<CrossRef> const& refs)
    {
        std::vector<CrossRef const*> ordered;
        for (auto const& r : refs)
            if (r.length > 0 && r.start != std::size_t(-1) && r.start + r.length <= doc.size())
                ordered.push_back(&r);
        std::ranges::sort(ordered, {}, [](CrossRef const* r) { return r->start; });
        std::vector<Block> out;
        std::string cur;
        std::size_t cur_start = 0;
        bool cur_code = false;
        std::string cur_lang;
        bool in_code = false;
        bool has_start = false;
        auto push = [&] {
            if (cur.empty())
                return;
            Block b;
            b.code = cur_code;
            b.start = cur_start;
            if (!cur_code)
            {
                while (!cur.empty() && cur.back() == 10)
                    cur.pop_back();
                if (cur.empty())
                    return;
                b.text = cur;
                std::size_t pos = 0;
                for (auto const* r : ordered)
                {
                    if (r->start < b.start || r->start + r->length > b.start + b.text.size())
                        continue;
                    std::size_t ls = r->start - b.start;
                    if (ls < pos)
                        continue;
                    for (auto& sp : split_inline(b.text.substr(pos, ls - pos)))
                        b.spans.push_back(std::move(sp));
                    std::string display = r->display.empty() ? r->raw : r->display;
                    b.spans.push_back(
                        {.kind = InlineKind::Link, .text = std::move(display), .target = r->target, .ambiguous = r->ambiguous, .resolved = r->resolved});
                    pos = ls + r->length;
                }
                for (auto& sp : split_inline(b.text.substr(pos)))
                    b.spans.push_back(std::move(sp));
            }
            else
            {
                b.lang = cur_lang;
                b.text = cur;
                b.spans.push_back({.kind = InlineKind::Text, .text = cur});
            }
            out.push_back(std::move(b));
            cur.clear();
        };
        std::size_t i = 0;
        while (i <= doc.size())
        {
            std::size_t j = doc.find(10, i);
            if (j == std::string_view::npos)
                j = doc.size();
            std::string_view line = doc.substr(i, j - i);
            if (fence_line(line))
            {
                if (!in_code)
                    cur_lang = fence_lang(line);
                push();
                in_code = !in_code;
                cur_code = in_code;
                has_start = false;
            }
            else if (in_code)
            {
                if (!has_start)
                {
                    cur_start = i;
                    has_start = true;
                }
                cur += std::string{line};
                cur += 10;
            }
            else if (blank_line(line))
            {
                push();
                cur_code = false;
                has_start = false;
            }
            else
            {
                if (!has_start)
                {
                    cur_start = i;
                    has_start = true;
                }
                cur_code = false;
                cur += std::string{line};
                cur += 10;
            }
            if (j == doc.size())
                break;
            i = j + 1;
        }
        push();
        return out;
    }
} // namespace dcdoc::prose