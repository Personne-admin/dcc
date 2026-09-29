module;
#include <cmark.h>
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
        Emph,
        Strong,
        Math,
    };

    struct Inline
    {
        InlineKind kind;
        std::string text;
        std::string target{};
        bool ambiguous{};
        bool resolved{};
        std::vector<Inline> children{};
    };

    enum class BlockKind { Paragraph, Code, Heading, BulletList, NumberList, ListItem, Quote };

    struct Block
    {
        BlockKind kind{BlockKind::Paragraph};
        bool code{};
        int level{};
        int start_number{1};
        std::size_t start{};
        std::string lang;
        std::string text;
        std::vector<Inline> spans;
        std::vector<Block> children{};
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

    [[nodiscard]] std::string inline_text(cmark_node* parent)
    {
        std::string out;
        for (auto* n = cmark_node_first_child(parent); n; n = cmark_node_next(n))
        {
            auto* literal = cmark_node_get_literal(n);
            if (literal) out += literal;
            else if (cmark_node_get_type(n) == CMARK_NODE_SOFTBREAK || cmark_node_get_type(n) == CMARK_NODE_LINEBREAK) out += ' ';
            else out += inline_text(n);
        }
        return out;
    }

    [[nodiscard]] std::vector<Inline> parse_inlines(cmark_node* parent, std::vector<CrossRef> const& refs)
    {
        std::vector<Inline> out;
        for (auto* n = cmark_node_first_child(parent); n; n = cmark_node_next(n))
        {
            auto kind = cmark_node_get_type(n);
            std::string literal = cmark_node_get_literal(n) ? cmark_node_get_literal(n) : "";
            if (kind == CMARK_NODE_TEXT)
            {
                std::size_t pos = 0;
                while (pos < literal.size())
                {
                    auto dollar = literal.find('$', pos);
                    if (dollar == std::string::npos)
                    {
                        out.push_back({.kind = InlineKind::Text, .text = literal.substr(pos)});
                        break;
                    }
                    auto end = literal.find('$', dollar + 1);
                    if (end == std::string::npos || end == dollar + 1)
                    {
                        out.push_back({.kind = InlineKind::Text, .text = literal.substr(pos)});
                        break;
                    }
                    if (dollar > pos) out.push_back({.kind = InlineKind::Text, .text = literal.substr(pos, dollar - pos)});
                    out.push_back({.kind = InlineKind::Math, .text = literal.substr(dollar + 1, end - dollar - 1)});
                    pos = end + 1;
                }
            }
            else if (kind == CMARK_NODE_CODE)
                out.push_back({.kind = InlineKind::Code, .text = literal});
            else if (kind == CMARK_NODE_EMPH || kind == CMARK_NODE_STRONG)
                out.push_back({.kind = kind == CMARK_NODE_EMPH ? InlineKind::Emph : InlineKind::Strong, .text = {},
                               .children = parse_inlines(n, refs)});
            else if (kind == CMARK_NODE_LINK)
            {
                std::string_view url = cmark_node_get_url(n) ? cmark_node_get_url(n) : "";
                if (url.starts_with("dcdoc-ref-"))
                {
                    std::size_t index = 0;
                    auto tail = url.substr(10);
                    auto [ptr, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), index);
                    if (ec == std::errc{} && ptr == tail.data() + tail.size() && index < refs.size())
                    {
                        auto const& r = refs[index];
                        out.push_back({.kind = InlineKind::Link, .text = inline_text(n), .target = r.target,
                                       .ambiguous = r.ambiguous, .resolved = r.resolved});
                        continue;
                    }
                }
                out.push_back({.kind = InlineKind::Text, .text = inline_text(n)});
            }
            else if (kind == CMARK_NODE_SOFTBREAK || kind == CMARK_NODE_LINEBREAK)
                out.push_back({.kind = InlineKind::Text, .text = " "});
            else if (kind == CMARK_NODE_HTML_INLINE)
                out.push_back({.kind = InlineKind::Text, .text = literal});
        }
        return out;
    }

    [[nodiscard]] std::vector<Block> parse_blocks(cmark_node* parent, std::vector<CrossRef> const& refs)
    {
        std::vector<Block> out;
        for (auto* n = cmark_node_first_child(parent); n; n = cmark_node_next(n))
        {
            Block b;
            auto kind = cmark_node_get_type(n);
            if (kind == CMARK_NODE_CODE_BLOCK)
            {
                b.kind = BlockKind::Code;
                b.code = true;
                b.text = cmark_node_get_literal(n) ? cmark_node_get_literal(n) : "";
                b.lang = cmark_node_get_fence_info(n) ? cmark_node_get_fence_info(n) : "";
            }
            else if (kind == CMARK_NODE_HTML_BLOCK)
            {
                b.kind = BlockKind::Paragraph;
                b.spans.push_back({.kind = InlineKind::Text,
                                   .text = cmark_node_get_literal(n) ? cmark_node_get_literal(n) : ""});
            }
            else if (kind == CMARK_NODE_HEADING)
            {
                b.kind = BlockKind::Heading;
                b.level = cmark_node_get_heading_level(n);
                b.spans = parse_inlines(n, refs);
            }
            else if (kind == CMARK_NODE_LIST)
            {
                b.kind = cmark_node_get_list_type(n) == CMARK_ORDERED_LIST ? BlockKind::NumberList : BlockKind::BulletList;
                b.start_number = cmark_node_get_list_start(n);
                b.children = parse_blocks(n, refs);
            }
            else if (kind == CMARK_NODE_ITEM)
            {
                b.kind = BlockKind::ListItem;
                b.children = parse_blocks(n, refs);
            }
            else if (kind == CMARK_NODE_BLOCK_QUOTE)
            {
                b.kind = BlockKind::Quote;
                b.children = parse_blocks(n, refs);
            }
            else if (kind == CMARK_NODE_PARAGRAPH)
            {
                b.kind = BlockKind::Paragraph;
                b.spans = parse_inlines(n, refs);
            }
            else continue;
            out.push_back(std::move(b));
        }
        return out;
    }

    [[nodiscard]] std::vector<Block> parse_doc(std::string_view doc, std::vector<CrossRef> const& refs)
    {
        std::string input;
        std::size_t pos = 0;
        for (std::size_t i = 0; i < refs.size(); ++i)
        {
            auto const& r = refs[i];
            if (r.start == std::size_t(-1) || r.length == 0 || r.start < pos || r.start + r.length > doc.size()) continue;
            input += doc.substr(pos, r.start - pos);
            std::string display = r.display.empty() ? r.raw : r.display;
            input += "[" + display + "](dcdoc-ref-" + std::to_string(i) + ")";
            pos = r.start + r.length;
        }
        input += doc.substr(pos);
        cmark_node* tree = cmark_parse_document(input.data(), input.size(), CMARK_OPT_DEFAULT);
        if (!tree) return {};
        auto out = parse_blocks(tree, refs);
        cmark_node_free(tree);
        return out;
    }
} // namespace dcdoc::prose
