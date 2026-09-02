export module dccd.format;

import std;
import dcc.sm;
import dcc.si;
import dcc.lex;
import dcc.lex.tokens;
import dcc.ast;
import dcc.ast.visitor;
import dcc.parser;
import dcc.diag;
import dccd.protocol;

export namespace dccd::format
{
    struct FormatAnalysisStats
    {
        std::size_t token_count{};
        std::size_t delimiter_match_lookups{};
        std::size_t enum_region_lookups{};
        std::size_t range_scan_steps{};
    };

    [[nodiscard]] std::optional<protocol::TextEdit> format_document(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                                    protocol::FormattingOptions const& options,
                                                                    dcc::sm::PositionEncoding position_encoding = dcc::sm::PositionEncoding::Utf16);
    [[nodiscard]] std::optional<protocol::TextEdit> format_document(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                                    protocol::FormattingOptions const& options, FormatAnalysisStats& out_stats,
                                                                    dcc::sm::PositionEncoding position_encoding = dcc::sm::PositionEncoding::Utf16);

    [[nodiscard]] std::vector<protocol::TextEdit> format_range(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                               protocol::FormattingOptions const& options, protocol::LspRange const& range,
                                                               dcc::sm::PositionEncoding position_encoding = dcc::sm::PositionEncoding::Utf16);
    [[nodiscard]] std::vector<protocol::TextEdit> format_range(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                               protocol::FormattingOptions const& options, protocol::LspRange const& range,
                                                               FormatAnalysisStats& out_stats,
                                                               dcc::sm::PositionEncoding position_encoding = dcc::sm::PositionEncoding::Utf16);

    [[nodiscard]] std::vector<protocol::TextEdit> format_on_type(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                                 protocol::FormattingOptions const& options, std::string_view trigger,
                                                                 protocol::LspPosition const& position,
                                                                 dcc::sm::PositionEncoding position_encoding = dcc::sm::PositionEncoding::Utf16);
    [[nodiscard]] std::vector<protocol::TextEdit> format_on_type(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                                 protocol::FormattingOptions const& options, std::string_view trigger,
                                                                 protocol::LspPosition const& position, FormatAnalysisStats& out_stats,
                                                                 dcc::sm::PositionEncoding position_encoding = dcc::sm::PositionEncoding::Utf16);

} // namespace dccd::format

module :private;

namespace dccd::format
{
    namespace
    {
        using dcc::lex::TokenKind;
        using dcc::sm::Offset;

        constexpr std::size_t kMaxLineWidth = 80;
        constexpr std::size_t kNoTokenIndex = std::numeric_limits<std::size_t>::max();

        [[nodiscard]] std::string make_indent(int level, protocol::FormattingOptions const& opts)
        {
            std::string s;
            if (opts.insertSpaces)
            {
                auto total = static_cast<std::size_t>(level) * static_cast<std::size_t>(opts.tabSize);
                s.reserve(total);
                for (std::size_t i = 0; i < total; ++i)
                    s += ' ';
            }
            else
            {
                s.reserve(static_cast<std::size_t>(level));
                for (int i = 0; i < level; ++i)
                    s += '\t';
            }
            return s;
        }

        [[nodiscard]] std::string_view spelling_at(std::string_view src, dcc::lex::Token const& tok) noexcept
        {
            if (!tok.interned.empty())
                return tok.interned;

            auto const begin = static_cast<std::size_t>(tok.range.begin.offset);
            auto const end = static_cast<std::size_t>(tok.range.end.offset);
            if (begin <= end && end <= src.size())
                return src.substr(begin, end - begin);

            return {};
        }

        [[nodiscard]] bool is_type_keyword(TokenKind k) noexcept
        {
            return (k >= TokenKind::Kwu8 && k <= TokenKind::KwIsize) || k == TokenKind::KwConst || k == TokenKind::KwRestrict || k == TokenKind::KwVolatile;
        }

        struct DelimitedGroup
        {
            std::size_t open_tok{};
            std::size_t close_tok{};
            std::size_t recovery_tok{};
            TokenKind open_kind{TokenKind::Eof};
            bool matched{true};
            bool block{false};
            bool compact{false};
            bool wrap{false};
            bool has_direct_newline{false};
            bool tight{false};
            bool has_comment_inside{false};
        };

        struct StructuralInfo
        {
            bool parsed{false};
            std::vector<bool> template_bang;
            std::vector<bool> binary_operator;
            std::vector<bool> unary_operator;
            std::vector<bool> pointer_star;
            std::unordered_set<std::size_t> ast_compact_braces;
            std::unordered_set<std::size_t> ast_block_braces;
            std::unordered_set<std::size_t> ast_struct_braces;
            std::unordered_set<std::size_t> ast_restricted_braces;
            std::unordered_set<std::size_t> ast_restricted_tokens;
            std::unordered_set<std::size_t> lambda_pipe;
            std::unordered_set<std::size_t> width_eligible_parens;
            std::unordered_map<std::size_t, std::size_t> paren_context;
            std::unordered_set<std::size_t> reanchor_starts;
            std::unordered_set<std::size_t> for_header_parens;
            std::unordered_set<std::size_t> for_header_semicolons;
            int tab_size{4};
            std::unordered_map<std::size_t, DelimitedGroup> groups;
        };

        [[nodiscard]] bool space_before_token(std::vector<dcc::lex::Token> const& tokens, std::size_t i, StructuralInfo const& info) noexcept
        {
            if (i == 0)
                return false;

            auto const cur = tokens[i].kind;
            auto const prev = tokens[i - 1].kind;

            if (cur == TokenKind::LBrace && info.ast_restricted_braces.contains(i))
                return false;
            if (prev == TokenKind::DotDot && info.ast_restricted_tokens.contains(i))
                return false;

            if (prev == TokenKind::KwFor)
                return true;

            if (info.lambda_pipe.count(i - 1) > 0)
                if (cur != TokenKind::Arrow)
                    return false;

            bool const is_binary_operator = i < info.binary_operator.size() && info.binary_operator[i];

            bool const prev_is_unary_operator = i - 1 < info.unary_operator.size() && info.unary_operator[i - 1];
            if (prev_is_unary_operator)
                return false;

            if (cur == TokenKind::Dot && prev == TokenKind::Comma)
                return true;

            if (cur == TokenKind::Bang)
            {
                bool template_bang = i < info.template_bang.size() && info.template_bang[i];
                if (template_bang)
                    return false;

                if (prev == TokenKind::Identifier || prev == TokenKind::RParen || prev == TokenKind::RBracket || is_type_keyword(prev))
                    return false;

                switch (prev)
                {
                    case TokenKind::Bang:
                    case TokenKind::LParen:
                    case TokenKind::LBracket:
                    case TokenKind::LBrace:
                    case TokenKind::Comma:
                    case TokenKind::Semicolon:
                        return false;
                    default:
                        return true;
                }
            }

            switch (cur)
            {
                case TokenKind::Comma:
                case TokenKind::Semicolon:
                case TokenKind::RParen:
                case TokenKind::RBracket:
                case TokenKind::RBrace:
                case TokenKind::Dot:
                case TokenKind::ColonColon:
                case TokenKind::DotDot:
                case TokenKind::Ellipsis:
                case TokenKind::Colon:
                    return false;
                default:
                    break;
            }

            switch (prev)
            {
                case TokenKind::LParen:
                case TokenKind::LBracket:
                case TokenKind::LBrace:
                case TokenKind::Dot:
                case TokenKind::ColonColon:
                case TokenKind::Bang:
                case TokenKind::At:
                case TokenKind::Hash:
                case TokenKind::Dollar:
                case TokenKind::Tilde:
                    return false;
                default:
                    break;
            }

            if (cur == TokenKind::Star)
            {
                if (i < info.pointer_star.size() && info.pointer_star[i])
                    return false;

                if (is_binary_operator)
                    return true;

                if (prev == TokenKind::Star)
                    return i - 1 < info.binary_operator.size() && info.binary_operator[i - 1];

                if (prev == TokenKind::Identifier || is_type_keyword(prev) || prev == TokenKind::RParen || prev == TokenKind::RBracket ||
                    prev == TokenKind::RBrace)
                    return false;
            }

            if (cur == TokenKind::Increment || cur == TokenKind::Decrement)
            {
                switch (prev)
                {
                    case TokenKind::Identifier:
                    case TokenKind::RParen:
                    case TokenKind::RBracket:
                    case TokenKind::IntLiteral:
                    case TokenKind::FloatLiteral:
                        return false;
                    default:
                        break;
                }
            }

            if (cur == TokenKind::LBracket)
            {
                if (prev == TokenKind::Star || prev == TokenKind::Identifier || is_type_keyword(prev) || prev == TokenKind::RBracket ||
                    prev == TokenKind::RParen || prev == TokenKind::RBrace)
                    return false;
            }

            if (cur == TokenKind::LParen)
            {
                if (prev == TokenKind::Identifier || prev == TokenKind::Bang || prev == TokenKind::RParen || prev == TokenKind::RBracket ||
                    is_type_keyword(prev))
                    return false;

                if (prev == TokenKind::Star && i >= 2 && !(i - 1 < info.binary_operator.size() && info.binary_operator[i - 1]))
                {
                    auto const prev2 = tokens[i - 2].kind;
                    if (is_type_keyword(prev2) || prev2 == TokenKind::LParen || prev2 == TokenKind::RParen || prev2 == TokenKind::RBracket ||
                        prev2 == TokenKind::Star || prev2 == TokenKind::Comma)
                        return false;
                }
            }

            if (prev == TokenKind::Amp)
            {
                bool is_unary = false;
                if (i >= 2)
                {
                    switch (tokens[i - 2].kind)
                    {
                        case TokenKind::LParen:
                        case TokenKind::LBracket:
                        case TokenKind::LBrace:
                        case TokenKind::Comma:
                        case TokenKind::Eq:
                        case TokenKind::FatArrow:
                        case TokenKind::KwReturn:
                        case TokenKind::Semicolon:
                        case TokenKind::Bang:
                        case TokenKind::Tilde:
                        case TokenKind::Colon:
                        case TokenKind::Question:
                        case TokenKind::Plus:
                        case TokenKind::Minus:
                        case TokenKind::Star:
                        case TokenKind::Amp:
                        case TokenKind::Pipe:
                        case TokenKind::Caret:
                            is_unary = true;
                            break;
                        default:
                            break;
                    }
                }
                else if (i == 1)
                    is_unary = true;

                if (is_unary)
                    return false;
            }

            if (prev == TokenKind::Increment || prev == TokenKind::Decrement)
            {
                bool is_prefix = false;
                if (i >= 2)
                {
                    switch (tokens[i - 2].kind)
                    {
                        case TokenKind::LParen:
                        case TokenKind::LBracket:
                        case TokenKind::LBrace:
                        case TokenKind::Comma:
                        case TokenKind::Eq:
                        case TokenKind::FatArrow:
                        case TokenKind::KwReturn:
                        case TokenKind::Semicolon:
                            is_prefix = true;
                            break;
                        default:
                            break;
                    }
                }
                else if (i == 1)
                    is_prefix = true;

                if (is_prefix)
                    return false;
            }

            return true;
        }

        [[nodiscard]] bool brace_is_block_context(std::vector<dcc::lex::Token> const& tokens, std::size_t i) noexcept
        {
            if (i == 0)
                return false;

            auto const prev = tokens[i - 1].kind;
            switch (prev)
            {
                case TokenKind::RParen: {
                    bool found_compiles = false;
                    for (std::size_t k = i; k > 0; --k)
                    {
                        auto const pk = tokens[k - 1].kind;
                        if (pk == TokenKind::KwCompiles)
                        {
                            found_compiles = true;
                            break;
                        }
                        if (pk == TokenKind::Semicolon || pk == TokenKind::LBrace || pk == TokenKind::Eq || pk == TokenKind::KwStruct ||
                            pk == TokenKind::KwEnum || pk == TokenKind::KwUnion)
                            break;
                    }
                    return !found_compiles;
                }
                case TokenKind::Identifier: {
                    bool found_equals_or_comma = false;
                    for (std::size_t k = i; k > 0; --k)
                    {
                        auto const pk = tokens[k - 1].kind;
                        if (pk == TokenKind::Eq || pk == TokenKind::Colon || pk == TokenKind::Comma || pk == TokenKind::LParen || pk == TokenKind::KwReturn ||
                            pk == TokenKind::FatArrow)
                        {
                            found_equals_or_comma = true;
                            break;
                        }

                        if (pk == TokenKind::Semicolon || pk == TokenKind::LBrace || pk == TokenKind::RBrace || pk == TokenKind::KwStruct ||
                            pk == TokenKind::KwEnum || pk == TokenKind::KwUnion || pk == TokenKind::KwModule || pk == TokenKind::KwImport)
                            break;

                        if (pk == TokenKind::Identifier || pk == TokenKind::Dot || is_type_keyword(pk))
                            continue;

                        break;
                    }
                    return !found_equals_or_comma;
                }
                default:
                    return is_type_keyword(prev);
            }
        }

        [[nodiscard]] bool is_major_decl_start(TokenKind k) noexcept
        {
            switch (k)
            {
                case TokenKind::KwModule:
                case TokenKind::KwImport:
                case TokenKind::KwStruct:
                case TokenKind::KwUnion:
                case TokenKind::KwEnum:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] int desired_newlines(int src_nl, int indent, TokenKind cur_kind, TokenKind prev_kind) noexcept
        {
            if (src_nl == 0)
                return 0;

            int target = (src_nl >= 4) ? 3 : src_nl;

            if (indent == 0 && target < 2)
            {
                if (prev_kind == TokenKind::Semicolon || prev_kind == TokenKind::RBrace)
                    if (is_major_decl_start(cur_kind) || cur_kind == TokenKind::Identifier)
                        target = 2;
            }

            if (indent > 0 && target > 2)
                target = 2;

            return target;
        }

        enum class TriviaKind : std::uint8_t
        {
            Whitespace,
            LineComment,
            BlockComment,
        };

        struct TriviaPiece
        {
            TriviaKind kind{TriviaKind::Whitespace};
            std::size_t begin{};
            std::size_t end{};
            std::size_t newlines{};
            std::string_view text;
        };

        struct Trivia
        {
            std::vector<std::vector<TriviaPiece>> gaps;
        };

        void scan_gap(std::string_view src, std::size_t begin, std::size_t end, std::vector<TriviaPiece>& out)
        {
            std::size_t pos = begin;
            while (pos < end)
            {
                char const c = src[pos];

                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    std::size_t const start = pos;
                    std::size_t nl = 0;
                    while (pos < end)
                    {
                        char const w = src[pos];
                        if (w == '\n')
                        {
                            ++nl;
                            ++pos;
                        }
                        else if (w == ' ' || w == '\t' || w == '\r')
                            ++pos;
                        else
                            break;
                    }
                    out.push_back({TriviaKind::Whitespace, start, pos, nl, {}});
                    continue;
                }

                if (c == '/' && pos + 1 < end && src[pos + 1] == '/')
                {
                    std::size_t const start = pos;
                    pos += 2;
                    while (pos < end && src[pos] != '\n')
                        ++pos;
                    out.push_back({TriviaKind::LineComment, start, pos, 0, src.substr(start, pos - start)});
                    continue;
                }

                if (c == '/' && pos + 1 < end && src[pos + 1] == '*')
                {
                    std::size_t const start = pos;
                    pos += 2;
                    int depth = 1;
                    while (pos < end && depth > 0)
                    {
                        if (src[pos] == '/' && pos + 1 < end && src[pos + 1] == '*')
                        {
                            pos += 2;
                            ++depth;
                        }
                        else if (src[pos] == '*' && pos + 1 < end && src[pos + 1] == '/')
                        {
                            pos += 2;
                            --depth;
                        }
                        else
                            ++pos;
                    }

                    std::size_t nl = 0;
                    for (std::size_t k = start; k < pos; ++k)
                        if (src[k] == '\n')
                            ++nl;

                    out.push_back({TriviaKind::BlockComment, start, pos, nl, src.substr(start, pos - start)});
                    continue;
                }

                ++pos;
            }
        }

        [[nodiscard]] Trivia build_trivia(std::string_view src, std::vector<dcc::lex::Token> const& tokens)
        {
            Trivia trivia;
            trivia.gaps.resize(tokens.size());
            std::size_t prev_end = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                auto const begin = static_cast<std::size_t>(tokens[i].range.begin.offset);
                scan_gap(src, prev_end, begin, trivia.gaps[i]);
                prev_end = static_cast<std::size_t>(tokens[i].range.end.offset);
            }
            return trivia;
        }

        [[nodiscard]] int gap_newline_count(Trivia const& trivia, std::size_t i) noexcept
        {
            if (i >= trivia.gaps.size())
                return 0;
            int n = 0;
            for (auto const& p : trivia.gaps[i])
                n += static_cast<int>(p.newlines);
            return n;
        }

        [[nodiscard]] bool gap_has_comment(Trivia const& trivia, std::size_t i) noexcept
        {
            if (i >= trivia.gaps.size())
                return false;
            for (auto const& p : trivia.gaps[i])
                if (p.kind != TriviaKind::Whitespace)
                    return true;
            return false;
        }

        [[nodiscard]] std::unordered_map<std::size_t, DelimitedGroup> analyze_groups(std::vector<dcc::lex::Token> const& tokens, std::string_view src,
                                                                                     Trivia const& trivia, StructuralInfo const& info)
        {
            std::unordered_map<std::size_t, DelimitedGroup> result;

            std::vector<DelimitedGroup> groups;
            std::unordered_map<std::size_t, std::size_t> open_to_index;
            {
                struct Frame
                {
                    std::size_t open_tok{};
                    TokenKind kind{TokenKind::Eof};
                };
                std::vector<Frame> stack;
                for (std::size_t i = 0; i < tokens.size(); ++i)
                {
                    auto const k = tokens[i].kind;
                    if (k == TokenKind::LParen || k == TokenKind::LBracket || k == TokenKind::LBrace)
                    {
                        stack.push_back({i, k});
                    }
                    else if (k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace)
                    {
                        TokenKind const expect_open = k == TokenKind::RParen     ? TokenKind::LParen
                                                      : k == TokenKind::RBracket ? TokenKind::LBracket
                                                                                 : TokenKind::LBrace;
                        if (!stack.empty() && stack.back().kind == expect_open)
                        {
                            auto const open_tok = stack.back().open_tok;
                            stack.pop_back();
                            open_to_index[open_tok] = groups.size();
                            groups.push_back(DelimitedGroup{.open_tok = open_tok, .close_tok = i, .open_kind = expect_open, .matched = true});
                        }
                        else if (!stack.empty())
                        {
                            auto const open_tok = stack.back().open_tok;
                            auto const kind = stack.back().kind;
                            stack.pop_back();
                            open_to_index[open_tok] = groups.size();
                            groups.push_back(
                                DelimitedGroup{.open_tok = open_tok, .close_tok = kNoTokenIndex, .recovery_tok = i, .open_kind = kind, .matched = false});
                        }
                    }
                }
                for (auto const& f : stack)
                {
                    open_to_index[f.open_tok] = groups.size();
                    groups.push_back(DelimitedGroup{
                        .open_tok = f.open_tok, .close_tok = kNoTokenIndex, .recovery_tok = kNoTokenIndex, .open_kind = f.kind, .matched = false});
                }
            }

            if (groups.empty())
                return result;

            {
                std::vector<std::size_t> stack;
                for (std::size_t i = 0; i < tokens.size(); ++i)
                {
                    auto const k = tokens[i].kind;
                    if (!stack.empty())
                    {
                        auto& g = groups[stack.back()];
                        if (!g.matched && g.open_kind != TokenKind::LBrace)
                        {
                            bool const is_for_header = g.open_kind == TokenKind::LParen && info.for_header_parens.contains(g.open_tok);
                            if (!is_for_header && info.reanchor_starts.contains(i))
                                if (g.recovery_tok == kNoTokenIndex || i < g.recovery_tok)
                                    g.recovery_tok = i;
                        }
                    }

                    if (k == TokenKind::LParen || k == TokenKind::LBracket || k == TokenKind::LBrace)
                    {
                        if (auto it = open_to_index.find(i); it != open_to_index.end())
                            stack.push_back(it->second);
                    }
                    else if (k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace)
                    {
                        if (!stack.empty())
                            stack.pop_back();
                    }
                }
            }

            {
                std::vector<std::size_t> stack;
                for (std::size_t i = 0; i < tokens.size(); ++i)
                {
                    auto const k = tokens[i].kind;
                    bool const eof_gap_is_content = k == TokenKind::Eof && gap_has_comment(trivia, i);
                    if (gap_newline_count(trivia, i) > 0 && !stack.empty() && (k != TokenKind::Eof || eof_gap_is_content))
                        groups[stack.back()].has_direct_newline = true;

                    if (k == TokenKind::LParen || k == TokenKind::LBracket || k == TokenKind::LBrace)
                    {
                        if (auto it = open_to_index.find(i); it != open_to_index.end())
                            stack.push_back(it->second);
                    }
                    else if (k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace)
                    {
                        if (!stack.empty())
                            stack.pop_back();
                    }
                }
            }

            std::vector<std::size_t> tok_width(tokens.size(), 0);
            for (std::size_t k = 0; k < tokens.size(); ++k)
            {
                std::size_t w = spelling_at(src, tokens[k]).size();
                if (k > 0 && tokens[k].kind != TokenKind::Eof && space_before_token(tokens, k, info))
                    ++w;
                tok_width[k] = w;
            }
            std::vector<std::size_t> cum(tokens.size() + 1, 0);
            for (std::size_t k = 0; k < tokens.size(); ++k)
                cum[k + 1] = cum[k] + tok_width[k];

            std::vector<std::size_t> line_start(tokens.size(), 0);
            {
                std::size_t last = 0;
                for (std::size_t k = 0; k < tokens.size(); ++k)
                {
                    line_start[k] = last;
                    auto const kk = tokens[k].kind;
                    if (kk == TokenKind::Semicolon || kk == TokenKind::RBrace || kk == TokenKind::LBrace)
                        last = k + 1;
                }
            }

            std::vector<int> brace_depth(tokens.size(), 0);
            {
                int d = 0;
                for (std::size_t k = 0; k < tokens.size(); ++k)
                {
                    brace_depth[k] = d;
                    if (tokens[k].kind == TokenKind::LBrace)
                        ++d;
                    else if (tokens[k].kind == TokenKind::RBrace && d > 0)
                        --d;
                }
            }

            std::vector<std::size_t> comment_prefix(tokens.size() + 1, 0);
            for (std::size_t k = 0; k < tokens.size(); ++k)
                comment_prefix[k + 1] = comment_prefix[k] + (gap_has_comment(trivia, k) ? 1 : 0);

            std::vector<std::size_t> block_opens;
            for (std::size_t g = 0; g < groups.size(); ++g)
            {
                if (groups[g].open_kind != TokenKind::LBrace)
                    continue;
                if (info.parsed && info.ast_compact_braces.contains(groups[g].open_tok))
                    continue;
                if ((info.parsed && info.ast_block_braces.contains(groups[g].open_tok)) || brace_is_block_context(tokens, groups[g].open_tok))
                    block_opens.push_back(groups[g].open_tok);
            }
            std::ranges::sort(block_opens);

            auto has_inner_block = [&block_opens](std::size_t open_tok, std::size_t end_tok) -> bool {
                auto it = std::ranges::upper_bound(block_opens, open_tok);
                return it != block_opens.end() && *it < end_tok;
            };

            auto content_end = [&tokens](DelimitedGroup const& g) -> std::size_t {
                if (g.matched)
                    return g.close_tok;
                if (g.recovery_tok != kNoTokenIndex)
                    return g.recovery_tok;
                return tokens.size() >= 2 ? tokens.size() - 2 : 0;
            };
            auto width_end = [&tokens](DelimitedGroup const& g) -> std::size_t {
                if (g.matched)
                    return g.close_tok + 1;
                if (g.recovery_tok != kNoTokenIndex)
                    return g.recovery_tok + 1;
                return tokens.size() - 1;
            };
            auto boundary_tok = [](DelimitedGroup const& g) -> std::size_t {
                if (g.matched)
                    return g.close_tok;
                return g.recovery_tok;
            };

            std::vector<std::size_t> order(groups.size());
            for (std::size_t g = 0; g < groups.size(); ++g)
                order[g] = g;
            std::ranges::sort(order, [&groups](std::size_t a, std::size_t b) { return groups[a].open_tok < groups[b].open_tok; });

            std::vector<std::size_t> wrap_stack;
            for (std::size_t const gi : order)
            {
                auto& g = groups[gi];
                while (!wrap_stack.empty() && boundary_tok(groups[wrap_stack.back()]) < g.open_tok)
                    wrap_stack.pop_back();
                bool const nested = !wrap_stack.empty();

                if (g.open_kind == TokenKind::LBrace)
                {
                    bool const empty_brace = g.matched && g.open_tok + 1 == g.close_tok;
                    bool const ast_compact = info.parsed && info.ast_compact_braces.contains(g.open_tok);
                    bool const ast_struct = info.parsed && info.ast_struct_braces.contains(g.open_tok);
                    bool const ast_restricted = info.parsed && info.ast_restricted_braces.contains(g.open_tok);
                    bool const ast_block = info.parsed && info.ast_block_braces.contains(g.open_tok);
                    bool const block_context = brace_is_block_context(tokens, g.open_tok);

                    bool want_compact = false;
                    if (empty_brace || ast_compact)
                        want_compact = true;
                    else if (!info.parsed || !ast_block)
                    {
                        std::size_t const content_width = cum[content_end(g)] - cum[g.open_tok + 1];
                        if (!block_context && content_width <= kMaxLineWidth)
                            want_compact = true;
                    }
                    if (want_compact && g.has_direct_newline && !ast_restricted)
                        want_compact = false;
                    if (want_compact && has_inner_block(g.open_tok, content_end(g)))
                        want_compact = false;

                    if (want_compact)
                    {
                        g.compact = true;
                        g.tight = ast_struct || ast_restricted;
                    }
                    else if (ast_block || block_context)
                        g.block = true;
                    else
                        g.wrap = true;
                }
                else
                {
                    bool const direct_nl = g.has_direct_newline;
                    bool const block_inside = has_inner_block(g.open_tok, content_end(g));
                    bool const width_eligible = g.open_kind == TokenKind::LParen && info.width_eligible_parens.contains(g.open_tok);

                    if (direct_nl || block_inside)
                    {
                        g.wrap = true;
                    }
                    else if (width_eligible)
                    {
                        std::size_t context = line_start[g.open_tok];
                        if (nested)
                            if (auto it = info.paren_context.find(g.open_tok); it != info.paren_context.end())
                                context = it->second;

                        std::size_t width = cum[width_end(g)] - cum[context];
                        if (context > 0 && space_before_token(tokens, context, info))
                            --width;
                        if (!nested)
                            width += static_cast<std::size_t>(brace_depth[context]) * static_cast<std::size_t>(info.tab_size);
                        if (width > kMaxLineWidth)
                            g.wrap = true;
                    }
                }

                if (g.wrap)
                    wrap_stack.push_back(gi);
            }

            for (auto& g : groups)
            {
                if (g.open_kind != TokenKind::LBrace)
                    continue;
                auto const b = boundary_tok(g);
                if (b != kNoTokenIndex && g.open_tok + 1 <= b && b + 1 < comment_prefix.size())
                    g.has_comment_inside = comment_prefix[b + 1] - comment_prefix[g.open_tok + 1] > 0;
            }

            for (auto const& g : groups)
                result.emplace(g.open_tok, g);
            return result;
        }

        struct StructureCollector : dcc::ast::RecursiveAstVisitor
        {
            std::vector<dcc::lex::Token> const& tokens;
            std::vector<Offset> const& begins;
            std::string_view src;
            StructuralInfo& info;
            std::vector<std::size_t> const& paren_match;
            std::vector<std::size_t> const& paren_positions;
            std::vector<std::size_t> const& brace_positions;
            FormatAnalysisStats& stats;

            StructureCollector(std::vector<dcc::lex::Token> const& toks, std::vector<Offset> const& token_begins, std::string_view source,
                               StructuralInfo& structural, std::vector<std::size_t> const& pmatch, std::vector<std::size_t> const& ppos,
                               std::vector<std::size_t> const& bpos, FormatAnalysisStats& st)
                : tokens{toks}, begins{token_begins}, src{source}, info{structural}, paren_match{pmatch}, paren_positions{ppos}, brace_positions{bpos},
                  stats{st}
            {
            }

            void visitUnaryExpr(dcc::ast::UnaryExpr const* e) override
            {
                collect_unary_operator(e);
                dcc::ast::RecursiveAstVisitor::visitUnaryExpr(e);
            }

            void visitLambdaExpr(dcc::ast::LambdaExpr const* e) override
            {
                if (e && e->range.valid())
                {
                    auto const idx = token_at(e->range.begin.offset);
                    if (idx != kNoTokenIndex && idx < tokens.size() && tokens[idx].kind == TokenKind::Pipe)
                        info.lambda_pipe.insert(idx);
                }
                dcc::ast::RecursiveAstVisitor::visitLambdaExpr(e);
            }

            void visitStmt(dcc::ast::Stmt const* s) override
            {
                if (s && s->range.valid())
                {
                    auto const idx = token_index_at_or_after(s->range.begin.offset);
                    if (idx != kNoTokenIndex)
                        info.reanchor_starts.insert(idx);
                }
                dcc::ast::RecursiveAstVisitor::visitStmt(s);
            }

            void visitDecl(dcc::ast::Decl const* d) override
            {
                if (d && d->range.valid())
                {
                    auto const idx = token_index_at_or_after(d->range.begin.offset);
                    if (idx != kNoTokenIndex)
                        info.reanchor_starts.insert(idx);
                }
                dcc::ast::RecursiveAstVisitor::visitDecl(d);
            }

            void visitForStmt(dcc::ast::ForStmt const* s) override
            {
                if (s && s->range.valid())
                {
                    auto const lo = token_index_at_or_after(s->range.begin.offset);
                    auto const hi = token_index_at_or_after(s->range.end.offset);
                    if (lo != kNoTokenIndex && hi != kNoTokenIndex)
                    {
                        auto const pit = std::lower_bound(paren_positions.begin(), paren_positions.end(), lo);
                        if (pit != paren_positions.end() && *pit < hi)
                            info.for_header_parens.insert(*pit);
                    }
                }
                dcc::ast::RecursiveAstVisitor::visitForStmt(s);
            }

            void visitCallExpr(dcc::ast::CallExpr const* e) override
            {
                if (e && e->callee && e->callee->range.valid() && e->range.valid())
                {
                    auto const callee_end = e->callee->range.end.offset;
                    auto const call_end = e->range.end.offset;
                    if (callee_end < call_end)
                    {
                        auto const first_after = token_index_at_or_after(callee_end);
                        auto const limit = token_index_at_or_after(call_end);
                        if (first_after != kNoTokenIndex && limit != kNoTokenIndex)
                        {
                            auto const pit = std::lower_bound(paren_positions.begin(), paren_positions.end(), first_after);
                            if (pit != paren_positions.end() && *pit < limit)
                            {
                                auto const idx = *pit;
                                info.width_eligible_parens.insert(idx);
                                info.paren_context[idx] = token_index_at_or_after(e->callee->range.begin.offset);
                            }
                        }
                    }
                }
                dcc::ast::RecursiveAstVisitor::visitCallExpr(e);
            }

            void visitFuncDecl(dcc::ast::FuncDecl const* d) override
            {
                if (d && d->name_range.valid() && d->range.valid())
                {
                    auto const first_idx = token_index_at_or_after(d->name_range.end.offset);
                    auto const limit_idx = token_index_at_or_after(d->range.end.offset);
                    if (first_idx != kNoTokenIndex && limit_idx != kNoTokenIndex)
                    {
                        auto const decl_head = token_index_at_or_after(d->range.begin.offset);
                        auto const pit = std::lower_bound(paren_positions.begin(), paren_positions.end(), first_idx);
                        if (pit != paren_positions.end() && *pit < limit_idx)
                        {
                            auto const j = *pit;
                            info.width_eligible_parens.insert(j);
                            info.paren_context[j] = decl_head;
                            ++stats.delimiter_match_lookups;
                            auto const close = j < paren_match.size() ? paren_match[j] : kNoTokenIndex;
                            if (close != kNoTokenIndex && close + 1 < tokens.size() && tokens[close + 1].kind == TokenKind::LParen)
                            {
                                info.width_eligible_parens.insert(close + 1);
                                info.paren_context[close + 1] = decl_head;
                            }
                        }
                    }
                }
                dcc::ast::RecursiveAstVisitor::visitFuncDecl(d);
            }

            void visitNamedType(dcc::ast::NamedType const* t) override
            {
                if (t)
                    mark_first_paren_in_range(t->range);
                dcc::ast::RecursiveAstVisitor::visitNamedType(t);
            }

            void visitAmbiguousStmt(dcc::ast::AmbiguousStmt const* s) override
            {
                if (!s)
                    return;
                if (s->resolution == dcc::ast::AmbiguousStmt::Resolution::AsExpr && s->as_expr)
                {
                    visitExpr(s->as_expr);
                    return;
                }
                if (s->as_decl)
                {
                    visitDecl(s->as_decl);
                    return;
                }
                if (s->as_expr)
                    visitExpr(s->as_expr);
            }

            void visitPointerType(dcc::ast::PointerType const* t) override
            {
                if (t && t->range.valid())
                {
                    auto const idx = token_ending_at(t->range.end.offset);
                    if (idx != kNoTokenIndex && idx < info.pointer_star.size() && tokens[idx].kind == TokenKind::Star)
                        info.pointer_star[idx] = true;
                }
                dcc::ast::RecursiveAstVisitor::visitPointerType(t);
            }

            void visitRestrictedType(dcc::ast::RestrictedType const* t) override
            {
                if (t && t->underlying && t->underlying->range.valid() && t->range.valid())
                {
                    auto const lo = token_index_at_or_after(t->underlying->range.end.offset);
                    auto const hi = token_index_at_or_after(t->range.end.offset);
                    if (lo != kNoTokenIndex && hi != kNoTokenIndex)
                    {
                        auto const bit = std::lower_bound(brace_positions.begin(), brace_positions.end(), lo);
                        if (bit != brace_positions.end() && *bit < hi)
                        {
                            info.ast_compact_braces.insert(*bit);
                            info.ast_restricted_braces.insert(*bit);
                            for (auto idx = *bit; idx < hi; ++idx)
                                info.ast_restricted_tokens.insert(idx);
                        }
                    }
                }
                dcc::ast::RecursiveAstVisitor::visitRestrictedType(t);
            }

            void visitFuncPtrType(dcc::ast::FuncPtrType const* t) override
            {
                if (t && t->return_type && t->return_type->range.valid())
                {
                    auto const lo = token_index_at_or_after(t->return_type->range.end.offset);
                    if (lo != kNoTokenIndex && lo + 2 < tokens.size() && tokens[lo].kind == TokenKind::LParen && tokens[lo + 1].kind == TokenKind::Star &&
                        tokens[lo + 2].kind == TokenKind::RParen)
                        if (lo + 1 < info.pointer_star.size())
                            info.pointer_star[lo + 1] = true;
                }
                dcc::ast::RecursiveAstVisitor::visitFuncPtrType(t);
            }

            void visitBinaryExpr(dcc::ast::BinaryExpr const* e) override
            {
                collect_binary_operator(e);
                dcc::ast::RecursiveAstVisitor::visitBinaryExpr(e);
            }

            void visitStructLiteralExpr(dcc::ast::StructLiteralExpr const* e) override
            {
                if (e && e->range.valid())
                {
                    auto const lo = token_index_at_or_after(e->range.begin.offset);
                    auto const hi = token_index_at_or_after(e->range.end.offset);
                    if (lo != kNoTokenIndex && hi != kNoTokenIndex)
                    {
                        auto const bit = std::lower_bound(brace_positions.begin(), brace_positions.end(), lo);
                        if (bit != brace_positions.end() && *bit < hi)
                        {
                            info.ast_compact_braces.insert(*bit);
                            info.ast_struct_braces.insert(*bit);
                        }
                    }
                }
                dcc::ast::RecursiveAstVisitor::visitStructLiteralExpr(e);
            }

            void visitBlockExpr(dcc::ast::BlockExpr const* e) override
            {
                if (e && e->body.range.valid())
                {
                    auto const idx = token_at(e->body.range.begin.offset);
                    if (idx != kNoTokenIndex && tokens[idx].kind == TokenKind::LBrace)
                    {
                        if (e->body.stmts.empty())
                            info.ast_compact_braces.insert(idx);
                        else
                            info.ast_block_braces.insert(idx);
                    }
                    for (auto const* s : e->body.stmts)
                        if (s)
                            visitStmt(s);
                    if (e->body.tail)
                        visitExpr(e->body.tail);
                    return;
                }
                dcc::ast::RecursiveAstVisitor::visitBlockExpr(e);
            }

            void visitBlock(dcc::ast::Block const& block) override
            {
                if (block.range.valid())
                {
                    auto const idx = token_at(block.range.begin.offset);
                    if (idx != kNoTokenIndex && tokens[idx].kind == TokenKind::LBrace)
                        info.ast_block_braces.insert(idx);
                }
                dcc::ast::RecursiveAstVisitor::visitBlock(block);
            }

            void visitStructDecl(dcc::ast::StructDecl const* d) override
            {
                mark_decl_body_brace(d->range);
                dcc::ast::RecursiveAstVisitor::visitStructDecl(d);
            }

            void visitUnionDecl(dcc::ast::UnionDecl const* d) override
            {
                mark_decl_body_brace(d->range);
                dcc::ast::RecursiveAstVisitor::visitUnionDecl(d);
            }

            void visitEnumDecl(dcc::ast::EnumDecl const* d) override
            {
                mark_decl_body_brace(d->range);
                dcc::ast::RecursiveAstVisitor::visitEnumDecl(d);
            }

            void visitTemplateInstExpr(dcc::ast::TemplateInstExpr const* e) override
            {
                if (e && e->range.valid())
                {
                    if (e->callee && e->callee->range.valid())
                    {
                        auto const bang = token_at(e->callee->range.end.offset);
                        if (bang != kNoTokenIndex && bang < info.template_bang.size() && tokens[bang].kind == TokenKind::Bang)
                            info.template_bang[bang] = true;
                    }
                    mark_first_paren_in_range(e->range);
                }

                dcc::ast::RecursiveAstVisitor::visitTemplateInstExpr(e);
            }

            [[nodiscard]] std::size_t token_index_at_or_after(Offset off) const noexcept
            {
                auto const it = std::lower_bound(begins.begin(), begins.end(), off);
                if (it == begins.end())
                    return kNoTokenIndex;
                return static_cast<std::size_t>(std::distance(begins.begin(), it));
            }

            [[nodiscard]] std::size_t token_ending_at(Offset off) const noexcept
            {
                auto const it = std::lower_bound(begins.begin(), begins.end(), off);
                if (it == begins.begin())
                    return kNoTokenIndex;

                auto const idx = static_cast<std::size_t>(std::distance(begins.begin(), it) - 1);
                if (idx < tokens.size() && tokens[idx].range.end.offset == off)
                    return idx;

                return kNoTokenIndex;
            }

            void mark_first_paren_in_range(dcc::sm::SourceRange const& range)
            {
                if (!range.valid())
                    return;

                auto const lo = token_index_at_or_after(range.begin.offset);
                auto const hi = token_index_at_or_after(range.end.offset);
                if (lo == kNoTokenIndex || hi == kNoTokenIndex)
                    return;

                auto const pit = std::lower_bound(paren_positions.begin(), paren_positions.end(), lo);
                if (pit != paren_positions.end() && *pit < hi)
                {
                    info.width_eligible_parens.insert(*pit);
                    info.paren_context[*pit] = lo;
                }
            }

            [[nodiscard]] std::size_t token_at(Offset off) const noexcept
            {
                auto const it = std::lower_bound(begins.begin(), begins.end(), off);
                if (it == begins.end() || *it != off)
                    return kNoTokenIndex;
                return static_cast<std::size_t>(std::distance(begins.begin(), it));
            }

            void mark_decl_body_brace(dcc::sm::SourceRange const& range)
            {
                if (!range.valid())
                    return;

                auto const lo = token_index_at_or_after(range.begin.offset);
                auto const hi = token_index_at_or_after(range.end.offset);
                if (lo == kNoTokenIndex || hi == kNoTokenIndex)
                    return;
                auto const bit = std::lower_bound(brace_positions.begin(), brace_positions.end(), lo);
                if (bit != brace_positions.end() && *bit < hi)
                    info.ast_block_braces.insert(*bit);
            }

            void collect_binary_operator(dcc::ast::BinaryExpr const* e)
            {
                if (!e || !e->lhs || !e->rhs || !e->lhs->range.valid() || !e->rhs->range.valid())
                    return;

                auto const begin = std::lower_bound(begins.begin(), begins.end(), e->lhs->range.end.offset);
                auto const end = std::lower_bound(begins.begin(), begins.end(), e->rhs->range.begin.offset);
                for (auto it = begin; it != end; ++it)
                {
                    ++stats.range_scan_steps;
                    auto const idx = static_cast<std::size_t>(std::distance(begins.begin(), it));
                    if (idx < info.binary_operator.size() && tokens[idx].kind == e->op)
                    {
                        info.binary_operator[idx] = true;
                        return;
                    }
                }
            }

            void collect_unary_operator(dcc::ast::UnaryExpr const* e)
            {
                if (!e || !e->range.valid())
                    return;

                auto const it = std::lower_bound(begins.begin(), begins.end(), e->range.begin.offset);
                if (it == begins.end() || *it != e->range.begin.offset)
                    return;

                auto const idx = static_cast<std::size_t>(std::distance(begins.begin(), it));
                if (idx < info.unary_operator.size() && tokens[idx].kind == e->op)
                    info.unary_operator[idx] = true;
            }
        };

        [[nodiscard]] std::vector<std::size_t> build_paren_match(std::vector<dcc::lex::Token> const& tokens)
        {
            std::vector<std::size_t> match(tokens.size(), kNoTokenIndex);
            std::vector<std::size_t> stack;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                auto const k = tokens[i].kind;
                if (k == TokenKind::LParen)
                    stack.push_back(i);
                else if (k == TokenKind::RParen)
                {
                    if (!stack.empty())
                    {
                        match[stack.back()] = i;
                        stack.pop_back();
                    }
                }
            }
            return match;
        }

        [[nodiscard]] std::vector<std::size_t> collect_positions(std::vector<dcc::lex::Token> const& tokens, TokenKind kind)
        {
            std::vector<std::size_t> positions;
            for (std::size_t i = 0; i < tokens.size(); ++i)
                if (tokens[i].kind == kind)
                    positions.push_back(i);
            return positions;
        }

        [[nodiscard]] std::unordered_set<std::size_t> collect_for_header_semicolons(std::vector<dcc::lex::Token> const& tokens)
        {
            std::unordered_set<std::size_t> result;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].kind != TokenKind::KwFor)
                    continue;

                if (i + 1 < tokens.size() && tokens[i + 1].kind == TokenKind::LParen)
                    continue;

                int depth = 0;
                std::size_t marked = 0;
                for (std::size_t j = i + 1; j < tokens.size(); ++j)
                {
                    auto const k = tokens[j].kind;
                    if (k == TokenKind::LParen || k == TokenKind::LBracket || k == TokenKind::LBrace)
                    {
                        ++depth;
                        continue;
                    }
                    if (k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace)
                    {
                        if (depth > 0)
                            --depth;
                        continue;
                    }
                    if (depth != 0)
                        continue;

                    if (k == TokenKind::KwIn)
                        break;

                    if (k == TokenKind::Semicolon)
                    {
                        result.insert(j);
                        if (++marked == 2)
                            break;
                    }
                }
            }
            return result;
        }

        [[nodiscard]] StructuralInfo analyze_structure(std::vector<dcc::lex::Token> const& tokens, dcc::si::string_interner& interner,
                                                       std::string_view src_text, protocol::FormattingOptions const& options, Trivia const& trivia,
                                                       FormatAnalysisStats& stats)
        {
            StructuralInfo info;
            info.template_bang.assign(tokens.size(), false);
            info.binary_operator.assign(tokens.size(), false);
            info.unary_operator.assign(tokens.size(), false);
            info.pointer_star.assign(tokens.size(), false);
            info.tab_size = static_cast<int>(options.tabSize);
            stats.token_count = tokens.size();

            std::vector<Offset> begins;
            begins.reserve(tokens.size());
            for (auto const& tok : tokens)
                begins.push_back(tok.range.begin.offset);

            auto const paren_match = build_paren_match(tokens);
            auto const paren_positions = collect_positions(tokens, TokenKind::LParen);
            auto const brace_positions = collect_positions(tokens, TokenKind::LBrace);
            info.for_header_semicolons = collect_for_header_semicolons(tokens);

            try
            {
                dcc::sm::SourceManager sm;
                auto const fid = sm.add_synthetic("format_input.dc", std::string{src_text});
                auto const* parse_sf = sm.get(fid);
                if (!parse_sf)
                    return info;

                dcc::lex::Lexer lexer{*parse_sf, interner};
                dcc::ast::AstContext ast_ctx;
                std::ostringstream diag_sink;
                dcc::diag::DiagnosticEngine diag{sm, diag_sink};
                diag.set_color(false);

                dcc::parser::Parser parser{lexer, ast_ctx, diag, dcc::parser::ParseMode::Interactive};
                auto* tu = parser.parse();
                if (!tu)
                    return info;

                StructureCollector collector{tokens, begins, src_text, info, paren_match, paren_positions, brace_positions, stats};
                collector.visitTranslationUnit(tu);
                info.parsed = true;
                info.groups = analyze_groups(tokens, src_text, trivia, info);
            }
            catch (...)
            {
                return StructuralInfo{.parsed = false,
                                      .template_bang = std::vector<bool>(tokens.size(), false),
                                      .binary_operator = std::vector<bool>(tokens.size(), false),
                                      .unary_operator = std::vector<bool>(tokens.size(), false),
                                      .pointer_star = std::vector<bool>(tokens.size(), false),
                                      .ast_compact_braces = {},
                                      .ast_block_braces = {},
                                      .ast_struct_braces = {},
                                      .ast_restricted_braces = {},
                                      .ast_restricted_tokens = {},
                                      .lambda_pipe = {},
                                      .width_eligible_parens = {},
                                      .paren_context = {},
                                      .reanchor_starts = {},
                                      .for_header_parens = {},
                                      .for_header_semicolons = {},
                                      .tab_size = static_cast<int>(options.tabSize),
                                      .groups = {}};
            }

            return info;
        }

        [[nodiscard]] std::optional<std::string> emit_formatted(dcc::sm::SourceFile const& sf, std::vector<dcc::lex::Token> const& tokens, Trivia const& trivia,
                                                                StructuralInfo const& info, protocol::FormattingOptions const& options,
                                                                FormatAnalysisStats& stats)
        {
            auto const src_text = sf.text();

            std::string result;
            result.reserve(sf.size() + sf.size() / 4);

            std::vector<bool> enum_region(tokens.size(), false);
            {
                bool in_enum = false;
                for (std::size_t i = 0; i < tokens.size(); ++i)
                {
                    enum_region[i] = in_enum;
                    auto const k = tokens[i].kind;
                    if (k == TokenKind::KwEnum)
                        in_enum = true;
                    else if (k == TokenKind::LBrace || k == TokenKind::RBrace || k == TokenKind::Semicolon || k == TokenKind::KwStruct ||
                             k == TokenKind::KwUnion || k == TokenKind::KwModule)
                        in_enum = false;
                }
            }

            struct ActiveGroup
            {
                TokenKind kind{TokenKind::Eof};
                std::size_t open_tok{};
                std::size_t close_tok{};
                std::size_t recovery_tok{};
                bool matched{true};
                bool wrap{false};
                bool compact{false};
                bool block{false};
                bool tight{false};
                bool has_comment_inside{false};
                bool restricted{false};
                int item_indent{};
                int close_indent{};
            };

            std::vector<ActiveGroup> active;
            std::vector<std::size_t> wrap_positions;
            std::size_t wrap_nesting = 0;
            std::size_t compact_count = 0;
            std::size_t restricted_count = 0;

            int indent = 0;
            int paren_depth = 0;
            int bracket_depth = 0;
            TokenKind prev_kind = TokenKind::Eof;
            bool pending_break = false;

            auto pop_active = [&](bool recovery) {
                auto const& g = active.back();
                if (g.compact && compact_count > 0)
                    --compact_count;
                if (g.restricted && restricted_count > 0)
                    --restricted_count;
                if (g.wrap)
                {
                    if (g.kind != TokenKind::LBrace && wrap_nesting > 0)
                        --wrap_nesting;
                    wrap_positions.pop_back();
                }

                if (recovery)
                {
                    if (g.kind == TokenKind::LParen && paren_depth > 0)
                        --paren_depth;
                    else if (g.kind == TokenKind::LBracket && bracket_depth > 0)
                        --bracket_depth;
                }
                active.pop_back();
            };

            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                auto const& tok = tokens[i];
                bool const is_eof = tok.kind == TokenKind::Eof;

                TokenKind next_kind = TokenKind::Eof;
                if (i + 1 < tokens.size())
                    next_kind = tokens[i + 1].kind;

                auto const spelling = spelling_at(src_text, tok);
                if (!is_eof && spelling.empty())
                    return std::nullopt;

                switch (tok.kind)
                {
                    case TokenKind::LParen:
                        ++paren_depth;
                        break;
                    case TokenKind::RParen:
                        if (paren_depth > 0)
                            --paren_depth;
                        break;
                    case TokenKind::LBracket:
                        ++bracket_depth;
                        break;
                    case TokenKind::RBracket:
                        if (bracket_depth > 0)
                            --bracket_depth;
                        break;
                    default:
                        break;
                }

                if (tok.kind == TokenKind::RBrace && indent > 0)
                    --indent;

                while (!active.empty() && !active.back().matched && active.back().recovery_tok == i)
                {
                    if (active.back().kind == TokenKind::LBrace && tok.kind != TokenKind::RBrace && indent > 0)
                        --indent;
                    pop_active(true);
                }

                bool const is_open = tok.kind == TokenKind::LParen || tok.kind == TokenKind::LBracket || tok.kind == TokenKind::LBrace;
                bool const is_close = tok.kind == TokenKind::RParen || tok.kind == TokenKind::RBracket || tok.kind == TokenKind::RBrace;
                bool const closes_here = is_close && !active.empty() && active.back().matched && active.back().close_tok == i;
                DelimitedGroup const* opening = nullptr;
                if (is_open)
                    if (auto it = info.groups.find(i); it != info.groups.end())
                        opening = &it->second;

                int want_indent = indent;
                bool inside_wrapping_group = false;
                if (!wrap_positions.empty())
                {
                    auto const& top = active[wrap_positions.back()];
                    inside_wrapping_group = true;
                    if (i == top.close_tok)
                    {
                        want_indent = top.close_indent;
                    }
                    else
                    {
                        std::size_t extra = active.size() - 1 - wrap_positions.back();
                        if (closes_here && !active.back().wrap)
                            --extra;

                        bool const item_start = (i == top.open_tok + 1) ||
                                                (i > 0 && tokens[i - 1].kind == TokenKind::Comma && !active.empty() && active.back().open_tok == top.open_tok);
                        want_indent = item_start ? top.item_indent : top.item_indent + static_cast<int>(extra);
                    }
                }

                int need_newlines = 0;
                if (i > 0)
                {
                    int const src_nl = restricted_count > 0 ? 0 : gap_newline_count(trivia, i);
                    int base = pending_break ? 1 : 0;

                    bool const closing_wrap = closes_here && active.back().wrap;
                    bool const closing_block = closes_here && active.back().block && prev_kind != TokenKind::LBrace;

                    if (closing_wrap || closing_block)
                    {
                        need_newlines = std::max(base, 1);
                    }
                    else
                    {
                        if (tok.kind == TokenKind::Semicolon && prev_kind == TokenKind::RBrace)
                            base = 0;
                        if (tok.kind == TokenKind::Comma && prev_kind == TokenKind::RBrace)
                            base = 0;
                        if (tok.kind == TokenKind::KwElse && prev_kind == TokenKind::RBrace)
                            base = 0;

                        if (inside_wrapping_group)
                        {
                            if (src_nl > 0)
                                need_newlines = 1;
                            else
                                need_newlines = base;
                        }
                        else
                        {
                            if (src_nl > 0)
                            {
                                need_newlines = desired_newlines(src_nl, indent, tok.kind, prev_kind);
                                if (need_newlines < base)
                                    need_newlines = base;
                            }
                            else if (base > 0)
                                need_newlines = base;
                            else
                                need_newlines = 0;
                        }

                        if (need_newlines >= 2 && tok.kind == TokenKind::KwImport && prev_kind == TokenKind::Semicolon && i >= 2)
                        {
                            for (std::size_t k = i - 1; k > 0; --k)
                            {
                                if (tokens[k - 1].kind == TokenKind::KwImport)
                                {
                                    need_newlines = 1;
                                    break;
                                }
                                if (tokens[k - 1].kind == TokenKind::Semicolon || tokens[k - 1].kind == TokenKind::RBrace ||
                                    tokens[k - 1].kind == TokenKind::KwModule || tokens[k - 1].kind == TokenKind::KwStruct ||
                                    tokens[k - 1].kind == TokenKind::KwEnum || tokens[k - 1].kind == TokenKind::KwUnion)
                                    break;
                            }
                        }
                    }
                }

                bool const has_comments = gap_has_comment(trivia, i);
                if (is_eof && !has_comments && need_newlines > 1)
                    need_newlines = 1;
                if (has_comments)
                {
                    auto const& gap = trivia.gaps[i];
                    int nl = 0;
                    bool break_after_comment = pending_break;
                    bool emitted_comment = false;
                    for (auto const& piece : gap)
                    {
                        if (piece.kind == TriviaKind::Whitespace)
                        {
                            nl += static_cast<int>(piece.newlines);
                            continue;
                        }

                        emitted_comment = true;
                        if (nl == 0 && i > 0)
                        {
                            result += ' ';
                            result += piece.text;
                            if (piece.kind == TriviaKind::LineComment)
                                break_after_comment = true;
                        }
                        else
                        {
                            int n = nl;
                            if (n < 1)
                                n = (i == 0) ? 0 : 1;
                            if (n > 2)
                                n = 2;
                            result.append(static_cast<std::size_t>(n), '\n');
                            result += make_indent(want_indent, options);
                            result += piece.text;
                            break_after_comment = true;
                        }
                        nl = 0;
                    }

                    int final_nl = nl;
                    if (break_after_comment && final_nl == 0)
                        final_nl = 1;
                    if (need_newlines > 0 && final_nl == 0)
                        final_nl = 1;
                    if (final_nl > 2)
                        final_nl = 2;

                    if (final_nl > 0)
                    {
                        result.append(static_cast<std::size_t>(final_nl), '\n');
                        if (!is_eof)
                            result += make_indent(want_indent, options);
                    }
                    else if (emitted_comment)
                    {
                        switch (tok.kind)
                        {
                            case TokenKind::Comma:
                            case TokenKind::Semicolon:
                            case TokenKind::RParen:
                            case TokenKind::RBracket:
                            case TokenKind::RBrace:
                            case TokenKind::Dot:
                            case TokenKind::ColonColon:
                            case TokenKind::DotDot:
                            case TokenKind::Ellipsis:
                            case TokenKind::Colon:
                                break;
                            default:
                                result += ' ';
                                break;
                        }
                    }
                    else if (i > 0 && !is_eof && space_before_token(tokens, i, info))
                    {
                        result += ' ';
                    }
                }
                else
                {
                    if (need_newlines > 0)
                    {
                        result.append(static_cast<std::size_t>(need_newlines), '\n');
                        if (!is_eof)
                            result += make_indent(want_indent, options);
                    }
                    else if (i > 0 && !is_eof && space_before_token(tokens, i, info))
                    {
                        result += ' ';
                    }
                }
                pending_break = false;

                if (is_eof)
                    break;

                if (tok.kind == TokenKind::RBrace && closes_here && !active.empty() && active.back().compact)
                {
                    bool const tight_close = active.back().tight;
                    if (tight_close)
                    {
                        if (prev_kind != TokenKind::LBrace && active.back().has_comment_inside)
                            result += ' ';
                    }
                    else if (prev_kind != TokenKind::LBrace || gap_has_comment(trivia, i))
                        result += ' ';
                }

                if (tok.kind == TokenKind::Colon && i > 0 && need_newlines == 0)
                {
                    ++stats.enum_region_lookups;
                    if (i < enum_region.size() && enum_region[i])
                        result += ' ';
                }

                result += spelling;

                if (is_open && opening)
                {
                    if (opening->wrap)
                    {
                        int const wbase = indent + static_cast<int>(wrap_nesting);
                        active.push_back(ActiveGroup{.kind = tok.kind,
                                                     .open_tok = i,
                                                     .close_tok = opening->close_tok,
                                                     .recovery_tok = opening->recovery_tok,
                                                     .matched = opening->matched,
                                                     .wrap = true,
                                                     .item_indent = wbase + 1,
                                                     .close_indent = wbase});
                        wrap_positions.push_back(active.size() - 1);
                        if (tok.kind != TokenKind::LBrace)
                            ++wrap_nesting;
                        pending_break = true;
                    }
                    else
                    {
                        active.push_back(ActiveGroup{.kind = tok.kind,
                                                     .open_tok = i,
                                                     .close_tok = opening->close_tok,
                                                     .recovery_tok = opening->recovery_tok,
                                                     .matched = opening->matched,
                                                     .compact = opening->compact,
                                                     .block = opening->block,
                                                     .tight = opening->tight,
                                                     .has_comment_inside = opening->has_comment_inside,
                                                     .restricted = info.ast_restricted_braces.contains(i)});
                        if (opening->block)
                            pending_break = true;
                        else if (opening->compact && !opening->tight && next_kind != TokenKind::RBrace && next_kind != TokenKind::Eof &&
                                 !gap_has_comment(trivia, i + 1))
                            result += ' ';
                        if (opening->compact)
                            ++compact_count;
                        if (info.ast_restricted_braces.contains(i))
                            ++restricted_count;
                    }
                }

                if (tok.kind == TokenKind::LBrace)
                {
                    ++indent;
                }
                else if (tok.kind == TokenKind::RBrace)
                {
                    bool const was_compact = compact_count > 0;
                    bool const was_empty = prev_kind == TokenKind::LBrace;
                    if (!was_compact && !was_empty && next_kind != TokenKind::Semicolon && next_kind != TokenKind::Comma && next_kind != TokenKind::KwElse &&
                        next_kind != TokenKind::RParen && next_kind != TokenKind::RBracket)
                        pending_break = true;
                }
                else if (tok.kind == TokenKind::Semicolon && paren_depth <= 0 && bracket_depth <= 0 && compact_count == 0 &&
                         !info.for_header_semicolons.contains(i))
                {
                    pending_break = true;
                }
                else if (tok.kind == TokenKind::Comma && !active.empty() && active.back().wrap)
                {
                    pending_break = true;
                }

                if (closes_here)
                    pop_active(false);

                prev_kind = tok.kind;
            }

            if (result.empty() || result.back() != '\n')
                result += '\n';

            return result;
        }

        [[nodiscard]] std::string apply_output_options(std::string result, protocol::FormattingOptions const& options)
        {
            if (options.trim_trailing_whitespace())
            {
                std::size_t line_start = 0;
                while (line_start <= result.size())
                {
                    auto const nl = result.find('\n', line_start);
                    auto const line_end = (nl == std::string::npos) ? result.size() : nl;

                    auto trim = line_end;
                    while (trim > line_start && (result[trim - 1] == ' ' || result[trim - 1] == '\t'))
                        --trim;
                    if (trim < line_end)
                        result.erase(trim, line_end - trim);

                    if (nl == std::string::npos)
                        break;
                    line_start = nl + 1;
                }
            }

            if (options.trim_final_newlines())
            {
                while (!result.empty() && result.back() == '\n')
                    result.pop_back();
                if (options.insert_final_newline())
                    result += '\n';
            }
            else if (!options.insert_final_newline())
                if (!result.empty() && result.back() == '\n')
                    result.pop_back();

            return result;
        }

        [[nodiscard]] bool is_utf8_boundary(std::string_view s, std::size_t off) noexcept
        {
            if (off == 0 || off == s.size())
                return true;

            return (static_cast<unsigned char>(s[off]) & 0xC0u) != 0x80u;
        }

        [[nodiscard]] std::size_t snap_boundary_down(std::string_view s, std::size_t off) noexcept
        {
            while (off > 0 && !is_utf8_boundary(s, off))
                --off;
            return off;
        }

        [[nodiscard]] std::size_t snap_boundary_up(std::string_view s, std::size_t off) noexcept
        {
            while (off < s.size() && !is_utf8_boundary(s, off))
                ++off;
            return off;
        }

        [[nodiscard]] std::optional<std::string> format_source_text(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                                    protocol::FormattingOptions const& options, FormatAnalysisStats& stats)
        {
            auto const src_text = sf.text();

            dcc::lex::Lexer lexer{sf, interner};
            std::vector<dcc::lex::Token> tokens;

            while (true)
            {
                auto tok = lexer.next();
                if (tok.kind == TokenKind::Invalid)
                    return std::nullopt;

                tokens.push_back(tok);
                if (tok.kind == TokenKind::Eof)
                    break;
            }

            auto const trivia = build_trivia(src_text, tokens);

            auto const structure = analyze_structure(tokens, interner, src_text, options, trivia, stats);

            if (!structure.parsed)
                return std::nullopt;

            auto formatted = emit_formatted(sf, tokens, trivia, structure, options, stats);
            if (!formatted.has_value())
                return std::nullopt;

            return apply_output_options(std::move(*formatted), options);
        }

        [[nodiscard]] std::optional<std::pair<dcc::sm::SourceRange, std::string>> derive_minimal_edit(std::string_view orig, std::string_view formatted)
        {
            std::size_t prefix = 0;
            while (prefix < orig.size() && prefix < formatted.size() && orig[prefix] == formatted[prefix])
                ++prefix;
            prefix = snap_boundary_down(orig, prefix);

            std::size_t const max_suffix = std::min(orig.size() - prefix, formatted.size() - prefix);
            std::size_t suffix = 0;
            while (suffix < max_suffix && orig[orig.size() - 1 - suffix] == formatted[formatted.size() - 1 - suffix])
                ++suffix;

            auto const r_begin = prefix;
            auto const r_end = snap_boundary_up(orig, orig.size() - suffix);
            auto const fmt_end = formatted.size() - (orig.size() - r_end);
            if (fmt_end < prefix)
                return std::nullopt;

            if (r_begin == r_end && fmt_end == prefix)
                return std::nullopt;

            dcc::sm::SourceRange range;
            range.begin.offset = static_cast<dcc::sm::Offset>(r_begin);
            range.end.offset = static_cast<dcc::sm::Offset>(r_end);
            return std::make_pair(range, std::string{formatted.substr(prefix, fmt_end - prefix)});
        }

    } // anonymous namespace

    std::optional<protocol::TextEdit> format_document(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                      protocol::FormattingOptions const& options, dcc::sm::PositionEncoding position_encoding)
    {
        FormatAnalysisStats stats;
        return format_document(sf, interner, options, stats, position_encoding);
    }

    std::optional<protocol::TextEdit> format_document(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                      protocol::FormattingOptions const& options, FormatAnalysisStats& out_stats,
                                                      dcc::sm::PositionEncoding position_encoding)
    {
        auto formatted = format_source_text(sf, interner, options, out_stats);
        if (!formatted.has_value())
            return std::nullopt;

        auto const end_offset = static_cast<dcc::sm::Offset>(sf.size());
        auto end_pos = sf.lsp_position(end_offset, position_encoding);
        if (!end_pos)
            return std::nullopt;

        protocol::TextEdit edit;
        edit.range.start.line = 0;
        edit.range.start.character = 0;
        edit.range.end.line = end_pos->line;
        edit.range.end.character = end_pos->character;
        edit.newText = std::move(*formatted);

        return edit;
    }

    std::vector<protocol::TextEdit> format_range(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner, protocol::FormattingOptions const& options,
                                                 protocol::LspRange const& range, dcc::sm::PositionEncoding position_encoding)
    {
        FormatAnalysisStats stats;
        return format_range(sf, interner, options, range, stats, position_encoding);
    }

    std::vector<protocol::TextEdit> format_range(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner, protocol::FormattingOptions const& options,
                                                 protocol::LspRange const& range, FormatAnalysisStats& out_stats, dcc::sm::PositionEncoding position_encoding)
    {
        auto start_off = sf.offset_at_lsp_position(range.start.line, range.start.character, position_encoding);
        auto end_off = sf.offset_at_lsp_position(range.end.line, range.end.character, position_encoding);
        if (!start_off || !end_off)
            return {};

        if (*start_off > *end_off || *end_off > static_cast<dcc::sm::Offset>(sf.size()))
            return {};

        auto formatted = format_source_text(sf, interner, options, out_stats);
        if (!formatted.has_value())
            return {};

        auto minimal = derive_minimal_edit(sf.text(), *formatted);
        if (!minimal)
            return {};

        auto const r_begin = static_cast<std::size_t>(minimal->first.begin.offset);
        auto const r_end = static_cast<std::size_t>(minimal->first.end.offset);
        if (r_begin < static_cast<std::size_t>(*start_off) || r_end > static_cast<std::size_t>(*end_off))
            return {};

        auto begin_pos = sf.lsp_position(minimal->first.begin.offset, position_encoding);
        auto end_pos = sf.lsp_position(minimal->first.end.offset, position_encoding);
        if (!begin_pos || !end_pos)
            return {};

        protocol::TextEdit edit;
        edit.range.start.line = begin_pos->line;
        edit.range.start.character = begin_pos->character;
        edit.range.end.line = end_pos->line;
        edit.range.end.character = end_pos->character;
        edit.newText = std::move(minimal->second);

        return {std::move(edit)};
    }

    std::vector<protocol::TextEdit> format_on_type(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                   protocol::FormattingOptions const& options, std::string_view trigger, protocol::LspPosition const& position,
                                                   dcc::sm::PositionEncoding position_encoding)
    {
        FormatAnalysisStats stats;
        return format_on_type(sf, interner, options, trigger, position, stats, position_encoding);
    }

    std::vector<protocol::TextEdit> format_on_type(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                   protocol::FormattingOptions const& options, std::string_view trigger, protocol::LspPosition const& position,
                                                   FormatAnalysisStats& out_stats, dcc::sm::PositionEncoding position_encoding)
    {
        if (trigger.empty())
            return {};

        auto pos_off = sf.offset_at_lsp_position(position.line, position.character, position_encoding);
        if (!pos_off)
            return {};

        auto const text = sf.text();
        auto const pos = static_cast<std::size_t>(*pos_off);
        if (pos < trigger.size())
            return {};
        if (text.substr(pos - trigger.size(), trigger.size()) != trigger)
            return {};

        std::size_t line_start = pos;
        while (line_start > 0 && text[line_start - 1] != '\n')
            --line_start;

        auto line_end = text.find('\n', line_start);
        if (line_end == std::string::npos)
            line_end = text.size();
        if (line_end > line_start && text[line_end - 1] == '\r')
            --line_end;

        auto line_start_pos = sf.lsp_position(static_cast<dcc::sm::Offset>(line_start), position_encoding);
        auto line_end_pos = sf.lsp_position(static_cast<dcc::sm::Offset>(line_end), position_encoding);
        if (!line_start_pos || !line_end_pos)
            return {};

        protocol::LspRange line_range;
        line_range.start.line = line_start_pos->line;
        line_range.start.character = line_start_pos->character;
        line_range.end.line = line_end_pos->line;
        line_range.end.character = line_end_pos->character;

        return format_range(sf, interner, options, line_range, out_stats, position_encoding);
    }

} // namespace dccd::format
