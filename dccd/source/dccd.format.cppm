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
    [[nodiscard]] std::optional<protocol::TextEdit> format_document(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                                    protocol::FormattingOptions const& options);

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

        [[nodiscard]] bool source_contains_comments(std::string_view src) noexcept
        {
            bool in_string = false;
            bool in_char = false;
            std::size_t i = 0;
            while (i < src.size())
            {
                char c = src[i];

                if (in_string)
                {
                    if (c == '\\')
                    {
                        i += 2;
                        continue;
                    }

                    if (c == '"')
                        in_string = false;
                }
                else if (in_char)
                {
                    if (c == '\\')
                    {
                        i += 2;
                        continue;
                    }

                    if (c == '\'')
                        in_char = false;
                }
                else
                {
                    if (c == '/' && i + 1 < src.size())
                    {
                        if (src[i + 1] == '/')
                            return true;
                        if (src[i + 1] == '*')
                            return true;
                    }

                    if (c == '"')
                        in_string = true;
                    else if (c == '\'')
                        in_char = true;

                    else if (c == 'u' && i + 1 < src.size())
                    {
                        if (src[i + 1] == '"')
                        {
                            in_string = true;
                            ++i;
                        }
                        else if (src[i + 1] == '\'')
                        {
                            in_char = true;
                            ++i;
                        }
                    }
                }
                ++i;
            }
            return false;
        }

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

        struct CallShape
        {
            std::size_t lparen_tok{};
            std::size_t rparen_tok{};
            std::size_t callee_first_tok{};
        };

        struct StructuralInfo
        {
            bool parsed{false};
            std::vector<bool> template_bang;
            std::vector<bool> binary_operator;
            std::vector<bool> unary_operator;
            std::vector<bool> block_brace;
            std::unordered_set<std::size_t> ast_compact_braces;
            std::unordered_set<std::size_t> ast_block_braces;
            int tab_size{4};
            std::unordered_map<std::size_t, CallShape> wrapping_calls;
        };

        [[nodiscard]] bool space_before_token(std::vector<dcc::lex::Token> const& tokens, std::size_t i, StructuralInfo const& info) noexcept
        {
            if (i == 0)
                return false;

            auto const cur = tokens[i].kind;
            auto const prev = tokens[i - 1].kind;

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

        [[nodiscard]] int count_source_newlines(std::string_view src, Offset begin, Offset end) noexcept
        {
            int n = 0;
            auto const limit = std::min(static_cast<std::size_t>(end), src.size());
            for (auto off = static_cast<std::size_t>(begin); off < limit; ++off)
                if (src[off] == '\n')
                    ++n;

            return n;
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

        [[nodiscard]] std::size_t flat_brace_content_width(std::vector<dcc::lex::Token> const& tokens, std::size_t lbrace_tok, std::string_view src,
                                                           StructuralInfo const& info) noexcept
        {
            std::size_t width = 0;
            int depth = 0;
            for (std::size_t j = lbrace_tok + 1; j < tokens.size(); ++j)
            {
                if (tokens[j].kind == TokenKind::LBrace)
                {
                    ++depth;
                    continue;
                }
                if (tokens[j].kind == TokenKind::RBrace)
                {
                    if (depth == 0)
                        break;
                    --depth;
                    continue;
                }
                width += spelling_at(src, tokens[j]).size();
                if (space_before_token(tokens, j, info))
                    ++width;
            }
            return width;
        }

        [[nodiscard]] bool brace_has_direct_newline(std::string_view src, std::vector<dcc::lex::Token> const& tokens, std::size_t lbrace_tok) noexcept
        {
            int depth = 0;
            int paren_depth = 0;
            int bracket_depth = 0;
            auto prev_end = tokens[lbrace_tok].range.end.offset;

            for (std::size_t j = lbrace_tok + 1; j < tokens.size(); ++j)
            {
                if (depth == 0 && paren_depth == 0 && bracket_depth == 0)
                    if (count_source_newlines(src, prev_end, tokens[j].range.begin.offset) > 0)
                        return true;

                switch (tokens[j].kind)
                {
                    case TokenKind::LBrace:
                        ++depth;
                        break;
                    case TokenKind::RBrace:
                        if (depth == 0)
                            return false;
                        --depth;
                        break;
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
                prev_end = tokens[j].range.end.offset;
            }
            return false;
        }

        void classify_braces(std::vector<dcc::lex::Token> const& tokens, std::string_view src, StructuralInfo& info)
        {
            info.block_brace.assign(tokens.size(), true);

            std::vector<std::size_t> lbraces;
            for (std::size_t i = 0; i < tokens.size(); ++i)
                if (tokens[i].kind == TokenKind::LBrace)
                    lbraces.push_back(i);

            for (std::size_t n = lbraces.size(); n-- > 0;)
            {
                auto const i = lbraces[n];

                if (i + 1 < tokens.size() && tokens[i + 1].kind == TokenKind::RBrace)
                {
                    info.block_brace[i] = false;
                    continue;
                }

                bool want_compact = false;
                if (info.parsed && info.ast_compact_braces.contains(i))
                {
                    want_compact = true;
                }
                else if (!info.parsed || !info.ast_block_braces.contains(i))
                    if (!brace_is_block_context(tokens, i) && flat_brace_content_width(tokens, i, src, info) <= kMaxLineWidth)
                        want_compact = true;

                if (want_compact)
                {
                    if (brace_has_direct_newline(src, tokens, i))
                        want_compact = false;

                    if (want_compact)
                    {
                        int inner_depth = 0;
                        for (std::size_t j = i + 1; j < tokens.size(); ++j)
                        {
                            if (tokens[j].kind == TokenKind::LBrace)
                            {
                                ++inner_depth;
                                if (info.block_brace[j])
                                {
                                    want_compact = false;
                                    break;
                                }
                                continue;
                            }
                            if (tokens[j].kind == TokenKind::RBrace)
                            {
                                if (inner_depth == 0)
                                    break;
                                --inner_depth;
                                continue;
                            }
                        }
                    }
                }

                info.block_brace[i] = !want_compact;
            }
        }

        struct StructureCollector : dcc::ast::RecursiveAstVisitor
        {
            std::vector<dcc::lex::Token> const& tokens;
            std::vector<Offset> const& begins;
            std::string_view src;
            StructuralInfo& info;
            std::vector<CallShape> raw_calls;

            StructureCollector(std::vector<dcc::lex::Token> const& toks, std::vector<Offset> const& token_begins, std::string_view source,
                               StructuralInfo& structural)
                : tokens{toks}, begins{token_begins}, src{source}, info{structural}
            {
            }

            void visitCallExpr(dcc::ast::CallExpr const* e) override
            {
                collect_call(e);
                dcc::ast::RecursiveAstVisitor::visitCallExpr(e);
            }

            void visitUnaryExpr(dcc::ast::UnaryExpr const* e) override
            {
                collect_unary_operator(e);
                dcc::ast::RecursiveAstVisitor::visitUnaryExpr(e);
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
                    auto lo = std::lower_bound(begins.begin(), begins.end(), e->range.begin.offset);
                    auto hi = std::lower_bound(begins.begin(), begins.end(), e->range.end.offset);
                    for (auto it = lo; it != hi; ++it)
                    {
                        auto const idx = static_cast<std::size_t>(std::distance(begins.begin(), it));
                        if (tokens[idx].kind == TokenKind::LBrace)
                        {
                            info.ast_compact_braces.insert(idx);
                            break;
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
                    auto lo = std::lower_bound(begins.begin(), begins.end(), e->range.begin.offset);
                    auto hi = std::lower_bound(begins.begin(), begins.end(), e->range.end.offset);
                    for (auto it = lo; it != hi; ++it)
                    {
                        auto const idx = static_cast<std::size_t>(std::distance(begins.begin(), it));
                        if (idx < info.template_bang.size() && tokens[idx].kind == TokenKind::Bang)
                            info.template_bang[idx] = true;
                    }
                }

                dcc::ast::RecursiveAstVisitor::visitTemplateInstExpr(e);
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

                auto lo = std::lower_bound(begins.begin(), begins.end(), range.begin.offset);
                auto hi = std::lower_bound(begins.begin(), begins.end(), range.end.offset);
                for (auto it = lo; it != hi; ++it)
                {
                    auto const idx = static_cast<std::size_t>(std::distance(begins.begin(), it));
                    if (tokens[idx].kind == TokenKind::LBrace)
                    {
                        info.ast_block_braces.insert(idx);
                        return;
                    }
                }
            }

            void collect_call(dcc::ast::CallExpr const* e)
            {
                if (!e || !e->range.valid() || !e->callee || !e->callee->range.valid())
                    return;

                auto const callee_end = e->callee->range.end.offset;
                auto const call_end = e->range.end.offset;
                if (callee_end >= call_end)
                    return;

                auto const first_callee = std::lower_bound(begins.begin(), begins.end(), e->callee->range.begin.offset);
                if (first_callee == begins.end())
                    return;

                auto const first_after_callee = std::lower_bound(begins.begin(), begins.end(), callee_end);
                if (first_after_callee == begins.end())
                    return;

                auto const callee_first_tok = static_cast<std::size_t>(std::distance(begins.begin(), first_callee));
                auto const search_start = static_cast<std::size_t>(std::distance(begins.begin(), first_after_callee));

                for (std::size_t i = search_start; i < tokens.size(); ++i)
                {
                    if (tokens[i].range.begin.offset >= call_end)
                        break;

                    if (tokens[i].kind != TokenKind::LParen)
                        continue;

                    std::size_t depth = 0;
                    std::size_t rparen = kNoTokenIndex;
                    for (std::size_t j = i; j < tokens.size(); ++j)
                    {
                        if (tokens[j].kind == TokenKind::LParen)
                            ++depth;
                        else if (tokens[j].kind == TokenKind::RParen)
                        {
                            if (depth == 0)
                                break;

                            --depth;
                            if (depth == 0)
                            {
                                rparen = j;
                                break;
                            }
                        }
                    }

                    if (rparen == kNoTokenIndex)
                        return;

                    raw_calls.push_back(CallShape{.lparen_tok = i, .rparen_tok = rparen, .callee_first_tok = callee_first_tok});
                    return;
                }
            }

            void collect_binary_operator(dcc::ast::BinaryExpr const* e)
            {
                if (!e || !e->lhs || !e->rhs || !e->lhs->range.valid() || !e->rhs->range.valid())
                    return;

                auto const begin = std::lower_bound(begins.begin(), begins.end(), e->lhs->range.end.offset);
                auto const end = std::lower_bound(begins.begin(), begins.end(), e->rhs->range.begin.offset);
                for (auto it = begin; it != end; ++it)
                {
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

            void finalize() const
            {
                std::vector<int> brace_depth_at(tokens.size(), 0);
                {
                    int depth = 0;
                    for (std::size_t k = 0; k < tokens.size(); ++k)
                    {
                        brace_depth_at[k] = depth;
                        if (tokens[k].kind == TokenKind::LBrace)
                            ++depth;
                        else if (tokens[k].kind == TokenKind::RBrace && depth > 0)
                            --depth;
                    }
                }

                std::vector<bool> is_nested(raw_calls.size(), false);
                for (std::size_t n = 0; n < raw_calls.size(); ++n)
                    for (std::size_t m = 0; m < raw_calls.size(); ++m)
                        if (m != n && raw_calls[m].lparen_tok < raw_calls[n].lparen_tok && raw_calls[n].rparen_tok < raw_calls[m].rparen_tok)
                        {
                            is_nested[n] = true;
                            break;
                        }

                for (std::size_t n = 0; n < raw_calls.size(); ++n)
                {
                    auto const& call = raw_calls[n];

                    bool wrap = count_source_newlines(src, tokens[call.lparen_tok].range.end.offset, tokens[call.rparen_tok].range.begin.offset) > 0;

                    if (!wrap)
                        for (std::size_t k = call.lparen_tok + 1; k < call.rparen_tok; ++k)
                            if (tokens[k].kind == TokenKind::LBrace && k < info.block_brace.size() && info.block_brace[k])
                            {
                                wrap = true;
                                break;
                            }

                    if (!wrap)
                    {
                        std::size_t width = 0;
                        for (std::size_t k = call.callee_first_tok; k <= call.rparen_tok; ++k)
                        {
                            auto const spelling = spelling_at(src, tokens[k]);
                            width += spelling.size();
                            if (k > call.callee_first_tok && space_before_token(tokens, k, info))
                                ++width;
                        }

                        if (!is_nested[n])
                        {
                            std::size_t prefix = 0;
                            for (std::size_t k = call.callee_first_tok; k-- > 0;)
                            {
                                auto const kk = tokens[k].kind;
                                if (kk == TokenKind::Semicolon || kk == TokenKind::RBrace || kk == TokenKind::LBrace)
                                    break;
                                prefix += spelling_at(src, tokens[k]).size();
                                if (space_before_token(tokens, k + 1, info))
                                    ++prefix;
                            }
                            width += prefix + static_cast<std::size_t>(brace_depth_at[call.callee_first_tok]) * static_cast<std::size_t>(info.tab_size);
                        }

                        if (width > kMaxLineWidth)
                            wrap = true;
                    }

                    if (wrap)
                        info.wrapping_calls.emplace(call.lparen_tok, call);
                }
            }
        };

        [[nodiscard]] StructuralInfo analyze_structure(std::vector<dcc::lex::Token> const& tokens, dcc::si::string_interner& interner,
                                                       std::string_view src_text, protocol::FormattingOptions const& options)
        {
            StructuralInfo info;
            info.template_bang.assign(tokens.size(), false);
            info.binary_operator.assign(tokens.size(), false);
            info.unary_operator.assign(tokens.size(), false);
            info.tab_size = static_cast<int>(options.tabSize);

            std::vector<Offset> begins;
            begins.reserve(tokens.size());
            for (auto const& tok : tokens)
                begins.push_back(tok.range.begin.offset);

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
                dcc::parser::Parser parser{lexer, ast_ctx, diag};
                auto* tu = parser.parse();
                if (!tu || diag.has_errors())
                    return info;

                StructureCollector collector{tokens, begins, src_text, info};
                collector.visitTranslationUnit(tu);
                info.parsed = true;
                classify_braces(tokens, src_text, info);
                collector.finalize();
            }
            catch (...)
            {
                return StructuralInfo{.parsed = false,
                                      .template_bang = std::vector<bool>(tokens.size(), false),
                                      .binary_operator = std::vector<bool>(tokens.size(), false),
                                      .unary_operator = std::vector<bool>(tokens.size(), false),
                                      .block_brace = std::vector<bool>(tokens.size(), true),
                                      .ast_compact_braces = {},
                                      .ast_block_braces = {},
                                      .tab_size = static_cast<int>(options.tabSize),
                                      .wrapping_calls = {}};
            }

            return info;
        }

        [[nodiscard]] std::optional<std::string> emit_formatted(dcc::sm::SourceFile const& sf, std::vector<dcc::lex::Token> const& tokens,
                                                                StructuralInfo const& info, protocol::FormattingOptions const& options)
        {
            auto const src_text = sf.text();

            std::string result;
            result.reserve(sf.size() + sf.size() / 4);

            int indent = 0;
            int paren_depth = 0;
            int bracket_depth = 0;
            int brace_depth = 0;
            TokenKind prev_kind = TokenKind::Eof;
            Offset prev_end_offset = 0;
            int prev_post_nl = 0;
            int compact_brace_depth = 0;

            struct ActiveCall
            {
                std::size_t lparen_tok{};
                std::size_t rparen_tok{};
                int entry_paren_depth{};
                int entry_brace_depth{};
                int arg_indent{};
                int close_indent{};
            };

            std::vector<ActiveCall> call_stack;

            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                auto const& tok = tokens[i];
                if (tok.kind == TokenKind::Eof)
                    continue;

                TokenKind next_kind = TokenKind::Eof;
                if (i + 1 < tokens.size())
                    next_kind = tokens[i + 1].kind;

                auto const spelling = spelling_at(src_text, tok);
                if (spelling.empty())
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
                    case TokenKind::LBrace:
                        ++brace_depth;
                        break;
                    case TokenKind::RBrace:
                        if (brace_depth > 0)
                            --brace_depth;
                        break;
                    default:
                        break;
                }

                if (tok.kind == TokenKind::RBrace && indent > 0)
                    --indent;

                bool opened_wrapping_call = false;
                if (tok.kind == TokenKind::LParen)
                {
                    if (auto it = info.wrapping_calls.find(i); it != info.wrapping_calls.end())
                    {
                        auto const continuation_base = indent + static_cast<int>(call_stack.size());
                        call_stack.push_back(ActiveCall{.lparen_tok = i,
                                                        .rparen_tok = it->second.rparen_tok,
                                                        .entry_paren_depth = paren_depth,
                                                        .entry_brace_depth = brace_depth,
                                                        .arg_indent = continuation_base + 1,
                                                        .close_indent = continuation_base});
                        opened_wrapping_call = true;
                    }
                }

                int need_newlines = 0;
                if (i > 0)
                {
                    int const src_nl = count_source_newlines(src_text, prev_end_offset, tok.range.begin.offset);
                    int base = prev_post_nl;

                    if (tok.kind == TokenKind::RBrace && prev_kind != TokenKind::LBrace)
                        if (src_nl == 0 && base == 0 && compact_brace_depth == 0)
                            base = 1;

                    if (tok.kind == TokenKind::Semicolon && prev_kind == TokenKind::RBrace)
                        base = 0;

                    if (tok.kind == TokenKind::Comma && prev_kind == TokenKind::RBrace)
                        base = 0;

                    if (tok.kind == TokenKind::KwElse && prev_kind == TokenKind::RBrace)
                        base = 0;

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

                    if (!call_stack.empty() && tok.kind == TokenKind::RParen && i == call_stack.back().rparen_tok)
                        if (need_newlines < 1)
                            need_newlines = 1;
                }

                for (int j = prev_post_nl; j < need_newlines; ++j)
                    result += '\n';

                if (need_newlines > 0)
                {
                    int emit_indent = indent;
                    auto const active_count = call_stack.size() - (opened_wrapping_call ? 1u : 0u);
                    if (active_count > 0)
                    {
                        auto const& top = call_stack[active_count - 1];
                        if (tok.kind == TokenKind::RParen && i == top.rparen_tok)
                            emit_indent = top.close_indent;
                        else if (i > 0 && tokens[i - 1].kind == TokenKind::LParen && i - 1 == top.lparen_tok)
                            emit_indent = top.arg_indent;
                        else if (i > 0 && tokens[i - 1].kind == TokenKind::Comma && paren_depth == top.entry_paren_depth &&
                                 brace_depth == top.entry_brace_depth)
                            emit_indent = top.arg_indent;
                        else if (paren_depth >= top.entry_paren_depth || brace_depth > top.entry_brace_depth)
                        {
                            int const tok_brace_depth = (tok.kind == TokenKind::LBrace) ? brace_depth - 1 : brace_depth;
                            int const paren_extra = paren_depth >= top.entry_paren_depth ? paren_depth - top.entry_paren_depth : 0;
                            int const brace_extra = tok_brace_depth >= top.entry_brace_depth ? tok_brace_depth - top.entry_brace_depth : 0;
                            emit_indent = top.arg_indent + paren_extra + brace_extra;
                        }
                    }

                    result += make_indent(emit_indent, options);
                }
                else if (i > 0 && space_before_token(tokens, i, info))
                {
                    result += ' ';
                }

                if (tok.kind == TokenKind::Colon && i > 0 && need_newlines == 0)
                {
                    bool is_enum = false;
                    for (std::size_t k = i; k > 0; --k)
                    {
                        auto const pk = tokens[k - 1].kind;
                        if (pk == TokenKind::KwEnum)
                        {
                            is_enum = true;
                            break;
                        }
                        if (pk == TokenKind::LBrace || pk == TokenKind::RBrace || pk == TokenKind::Semicolon || pk == TokenKind::KwStruct ||
                            pk == TokenKind::KwUnion || pk == TokenKind::KwModule)
                            break;
                    }
                    if (is_enum)
                        result += ' ';
                }

                if (tok.kind == TokenKind::RBrace && compact_brace_depth > 0 && prev_kind != TokenKind::LBrace)
                    result += ' ';

                result += spelling;

                prev_post_nl = 0;

                if (tok.kind == TokenKind::LBrace)
                {
                    ++indent;
                    bool compact_brace = (next_kind == TokenKind::RBrace);
                    if (!compact_brace && i < info.block_brace.size())
                        compact_brace = !info.block_brace[i];

                    if (compact_brace)
                    {
                        if (next_kind != TokenKind::RBrace)
                        {
                            ++compact_brace_depth;
                            result += ' ';
                        }
                    }
                    else
                    {
                        result += '\n';
                        prev_post_nl = 1;
                    }
                }
                else if (tok.kind == TokenKind::RBrace)
                {
                    bool const was_empty = (prev_kind == TokenKind::LBrace);
                    bool const was_compact = compact_brace_depth > 0;
                    if (was_compact && !was_empty)
                        --compact_brace_depth;

                    if (!was_compact && !was_empty && next_kind != TokenKind::Semicolon && next_kind != TokenKind::Comma && next_kind != TokenKind::KwElse &&
                        next_kind != TokenKind::RParen && next_kind != TokenKind::RBracket)
                    {
                        result += '\n';
                        prev_post_nl = 1;
                    }
                }
                else if (tok.kind == TokenKind::Semicolon && paren_depth <= 0 && bracket_depth <= 0 && compact_brace_depth == 0)
                {
                    result += '\n';
                    prev_post_nl = 1;
                }
                else if (tok.kind == TokenKind::Comma && !call_stack.empty() && paren_depth == call_stack.back().entry_paren_depth &&
                         brace_depth == call_stack.back().entry_brace_depth)
                {
                    result += '\n';
                    prev_post_nl = 1;
                }
                else if (opened_wrapping_call)
                {
                    result += '\n';
                    prev_post_nl = 1;
                }

                if (tok.kind == TokenKind::RParen && !call_stack.empty() && i == call_stack.back().rparen_tok)
                    call_stack.pop_back();

                prev_kind = tok.kind;
                prev_end_offset = tok.range.end.offset;
            }

            if (result.empty() || result.back() != '\n')
                result += '\n';

            return result;
        }

    } // anonymous namespace

    std::optional<protocol::TextEdit> format_document(dcc::sm::SourceFile const& sf, dcc::si::string_interner& interner,
                                                      protocol::FormattingOptions const& options)
    {
        auto const src_text = sf.text();
        if (source_contains_comments(src_text))
            return std::nullopt;

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

        auto const structure = analyze_structure(tokens, interner, src_text, options);

        if (!structure.parsed)
            return std::nullopt;

        auto formatted = emit_formatted(sf, tokens, structure, options);
        if (!formatted.has_value())
            return std::nullopt;

        auto const end_offset = static_cast<dcc::sm::Offset>(sf.size());
        auto end_pos = sf.lsp_position(end_offset);
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

} // namespace dccd::format
