export module dcc.parser;

import std;
import dcc.ast;
import dcc.lex;
import dcc.lex.tokens;
import dcc.diag;
import dcc.si;
import dcc.sm;

namespace dcc::parser
{
    using TK = lex::TokenKind;
}

export namespace dcc::parser
{
    struct ParamShape;

    enum class ParseMode : std::uint8_t
    {
        Batch,
        Interactive,
    };

    class Parser
    {
    public:
        Parser(lex::Lexer& lexer, ast::AstContext& ctx, diag::DiagnosticEngine& diag, ParseMode mode = ParseMode::Batch) noexcept
            : m_lexer(lexer), m_ctx(ctx), m_diag(diag), m_tokens(ctx.allocator()), m_mode(mode)
        {
        }

        Parser(Parser const&) = delete;
        Parser& operator=(Parser const&) = delete;

        [[nodiscard]] ast::TranslationUnit* parse() { return parse_translation_unit(); }

    private:
        struct ParamShape
        {
            sm::SourceRange range;
            std::string_view name;
            sm::SourceRange name_range;
            ast::TypeExpr* type{};
            bool is_pack{};
        };

        class Speculation
        {
        public:
            explicit Speculation(Parser& p) noexcept : m_p(p), m_pos(p.m_pos), m_prev_end(p.m_prev_end), m_suppressed_error_count(p.m_suppressed_error_count)
            {
                ++m_p.m_silent_depth;
            }

            ~Speculation() noexcept
            {
                if (m_silent)
                    --m_p.m_silent_depth;

                if (!m_committed)
                {
                    m_p.m_pos = m_pos;
                    m_p.m_prev_end = m_prev_end;
                }
            }

            Speculation(Speculation const&) = delete;
            Speculation& operator=(Speculation const&) = delete;

            void commit() noexcept
            {
                if (m_silent)
                {
                    --m_p.m_silent_depth;
                    m_silent = false;
                }

                m_committed = true;
            }

            [[nodiscard]] bool had_suppressed_errors() const noexcept { return m_p.m_suppressed_error_count != m_suppressed_error_count; }

        private:
            Parser& m_p;
            std::size_t m_pos;
            sm::Location m_prev_end;
            std::uint32_t m_suppressed_error_count;
            bool m_committed{false};
            bool m_silent{true};
        };

        friend class Speculation;

        lex::Token const& peek(std::size_t ahead = 0)
        {
            while (m_pos + ahead >= m_tokens.size())
                m_tokens.push_back(m_lexer.next());

            return m_tokens[m_pos + ahead];
        }

        lex::Token const& advance()
        {
            auto const& tok = peek();
            m_prev_end = tok.range.end;
            ++m_pos;

            return m_tokens[m_pos - 1];
        }

        lex::Token const& previous() const noexcept { return m_tokens[m_pos - 1]; }

        bool check(TK k) { return peek().kind == k; }
        bool check_at(std::size_t n, TK k) { return peek(n).kind == k; }
        bool eof() { return peek().kind == TK::Eof; }

        bool match(TK k)
        {
            if (!check(k))
                return false;

            advance();
            return true;
        }

        lex::Token const& expect(TK k, std::string_view ctx)
        {
            if (check(k))
                return advance();

            error_at(single_range(), std::format("expected '{}' {}, found '{}'", lex::to_string(k), ctx, lex::to_string(peek().kind)));
            return peek();
        }

        sm::Location loc() { return peek().range.begin; }
        sm::SourceRange range_from(sm::Location begin) const noexcept { return {begin, m_prev_end}; }
        sm::SourceRange single_range() { return peek().range; }

        [[nodiscard]] bool same_error_range(sm::SourceRange range) const noexcept
        {
            return m_last_error_range && m_last_error_range->begin == range.begin && m_last_error_range->end == range.end;
        }

        bool silent() const noexcept { return m_silent_depth > 0; }

        void note_suppressed_error() noexcept { ++m_suppressed_error_count; }

        void emit(diag::Diagnostic d)
        {
            if (silent())
                return;

            m_diag.emit(std::move(d));
        }

        void error_at(sm::SourceRange range, std::string msg)
        {
            if (same_error_range(range) || silent())
            {
                note_suppressed_error();
                return;
            }

            m_last_error_range = range;
            emit(diag::Diagnostic(diag::Severity::Error, std::move(msg)).primary(range));
        }

        void error_at_with_help(sm::SourceRange range, std::string msg, std::string help_text)
        {
            if (same_error_range(range) || silent())
            {
                note_suppressed_error();
                return;
            }

            m_last_error_range = range;
            emit(diag::Diagnostic(diag::Severity::Error, std::move(msg)).primary(range).help(std::move(help_text)));
        }

        void error_unexpected(std::string_view ctx) { error_at(single_range(), std::format("unexpected '{}' {}", lex::to_string(peek().kind), ctx)); }

        void synchronize_to_decl()
        {
            while (!eof())
                switch (peek().kind)
                {
                    case TK::KwModule:
                    case TK::KwImport:
                    case TK::KwUsing:
                    case TK::KwStruct:
                    case TK::KwEnum:
                    case TK::KwUnion:
                    case TK::KwPublic:
                    case TK::KwExtern:
                    case TK::At:
                        return;
                    case TK::Semicolon:
                        advance();
                        return;
                    default:
                        advance();
                }
        }

        void synchronize_to_stmt()
        {
            int brace_depth = 0;
            while (!eof())
            {
                if (brace_depth == 0)
                {
                    switch (peek().kind)
                    {
                        case TK::Semicolon:
                            advance();
                            return;
                        case TK::RBrace:
                            return;
                        case TK::KwReturn:
                        case TK::KwBreak:
                        case TK::KwContinue:
                        case TK::KwIf:
                        case TK::KwWhile:
                        case TK::KwDo:
                        case TK::KwFor:
                        case TK::KwMatch:
                        case TK::KwDefer:
                        case TK::KwStatic:
                            return;
                        default:
                            break;
                    }
                }

                if (check(TK::LBrace))
                    ++brace_depth;

                else if (check(TK::RBrace))
                {
                    if (brace_depth == 0)
                        return;

                    --brace_depth;
                }

                advance();
            }
        }

        ast::TranslationUnit* parse_translation_unit()
        {
            auto start = loc();
            auto* tu = m_ctx.make<ast::TranslationUnit>();

            tu->module_decl = parse_module_decl();

            while (!eof())
            {
                auto* item = parse_top_level_item();
                if (!item)
                {
                    synchronize_to_decl();
                    continue;
                }

                if (item->kind == ast::DeclKind::Import)
                    tu->imports.push_back(item);
                else
                    tu->decls.push_back(item);

                for (auto* ed : m_extra_imports)
                    tu->imports.push_back(ed);
                m_extra_imports.clear();
            }

            tu->range = range_from(start);
            return tu;
        }

        ast::Decl* parse_module_decl()
        {
            if (!check(TK::KwModule))
                return nullptr;

            auto start = loc();
            advance();

            auto path = parse_path();
            expect(TK::Semicolon, "after module path");

            auto nr = !path.segments.empty() ? path.segments.back().range : sm::SourceRange{};
            auto* decl = m_ctx.make<ast::ModuleDecl>(range_from(start), std::move(path), nr);
            return decl;
        }

        ast::Decl* parse_top_level_item()
        {
            auto start = loc();

            auto attrs = parse_attributes();

            bool is_public = match(TK::KwPublic);

            if (check(TK::KwImport))
            {
                advance();
                auto* d = parse_import_decl_body(is_public, start);
                if (d)
                    d->attrs = std::move(attrs);

                return d;
            }

            if (check(TK::KwUsing))
            {
                advance();
                bool is_spill = match(TK::KwPublic);
                if (is_spill)
                    is_public = true;

                auto* d = parse_using_decl_body(is_public, is_spill, start);
                if (d)
                    d->attrs = std::move(attrs);

                return d;
            }

            switch (peek().kind)
            {
                case TK::KwStruct:
                    return parse_struct_decl(std::move(attrs), is_public, start);
                case TK::KwEnum:
                    return parse_enum_decl(std::move(attrs), is_public, start);
                case TK::KwUnion:
                    return parse_union_decl(std::move(attrs), is_public, start);
                default: {
                    bool is_extern = match(TK::KwExtern);
                    return parse_func_or_var_decl(std::move(attrs), is_public, is_extern, start);
                }
            }
        }

        ast::Path parse_path()
        {
            auto start = loc();
            ast::Path p(m_ctx.allocator());

            auto first = expect(TK::Identifier, "in path");
            if (first.kind == TK::Identifier)
                p.segments.push_back({first.interned, first.range});

            while (check(TK::ColonColon))
            {
                if (check_at(1, TK::Identifier))
                {
                    advance();
                    auto seg = advance();
                    p.segments.push_back({seg.interned, seg.range});
                }
                else if (m_mode == ParseMode::Interactive)
                {
                    if (check_at(1, TK::LBrace) || check_at(1, TK::Star))
                        break;

                    advance();
                    error_at(single_range(), "expected identifier after '::'");
                    auto delim_end = m_prev_end;
                    p.segments.push_back({std::string_view{}, sm::SourceRange{delim_end, delim_end}});
                }
                else
                    break;
            }

            p.range = range_from(start);
            return p;
        }

        std::pair<ast::Path, bool> parse_path_or_wildcard()
        {
            auto start = loc();
            ast::Path p(m_ctx.allocator());
            bool wildcard = false;

            auto first = expect(TK::Identifier, "in path");
            if (first.kind == TK::Identifier)
                p.segments.push_back({first.interned, first.range});

            while (check(TK::ColonColon))
            {
                advance();
                if (match(TK::Star))
                {
                    wildcard = true;
                    break;
                }

                if (check(TK::Identifier))
                {
                    auto seg = advance();
                    if (seg.kind == TK::Identifier)
                        p.segments.push_back({seg.interned, seg.range});
                }
                else if (m_mode == ParseMode::Interactive)
                {
                    error_at(single_range(), "expected identifier or '*' after '::'");
                    auto delim_end = m_prev_end;
                    p.segments.push_back({std::string_view{}, sm::SourceRange{delim_end, delim_end}});
                }
                else
                {
                    auto seg = expect(TK::Identifier, "after '::'");
                    if (seg.kind == TK::Identifier)
                        p.segments.push_back({seg.interned, seg.range});
                }
            }

            p.range = range_from(start);
            return {std::move(p), wildcard};
        }

        std::pmr::vector<ast::Attribute> parse_attributes()
        {
            std::pmr::vector<ast::Attribute> result(m_ctx.allocator());

            while (check(TK::At))
            {
                advance();

                if (match(TK::LBracket))
                {
                    if (!check(TK::RBracket))
                        do
                            result.push_back(parse_single_attribute());
                        while (match(TK::Comma));

                    expect(TK::RBracket, "to close attribute list");
                }
                else
                    result.push_back(parse_single_attribute());
            }

            return result;
        }

        struct AttributeNameResult
        {
            std::string_view name;
            sm::SourceRange range;
        };

        AttributeNameResult parse_attribute_name()
        {
            if (check(TK::KwImport))
            {
                auto tok = advance();
                return {tok.interned, tok.range};
            }

            auto tok = expect(TK::Identifier, "in attribute");
            return {tok.interned, tok.range};
        }

        ast::Attribute parse_single_attribute()
        {
            auto start = loc();
            ast::Attribute attr(m_ctx.allocator());

            auto name_res = parse_attribute_name();
            attr.name = name_res.name;
            attr.range = name_res.range;

            if (match(TK::LParen))
            {
                if (!check(TK::RParen))
                {
                    auto* first = parse_expr();
                    if (first)
                        attr.args.push_back(first);
                    while (match(TK::Comma))
                    {
                        auto* arg = parse_expr();
                        if (arg)
                            attr.args.push_back(arg);
                        else if (m_mode == ParseMode::Interactive)
                            break;
                    }
                }

                expect(TK::RParen, "to close attribute arguments");
            }

            attr.range = range_from(start);
            validate_attribute(attr);
            return attr;
        }

        static constexpr bool is_known_attr_name(std::string_view name) noexcept
        {
            return name == "packed" || name == "align" || name == "nominal" || name == "import" || name == "export" || name == "nomangle" || name == "inline" ||
                   name == "noinline" || name == "section" || name == "calling_conv" || name == "deprecated" || name == "implicit_construction" ||
                   name == "intrinsic";
        }

        void validate_attribute(ast::Attribute const& attr)
        {
            if (!is_known_attr_name(attr.name))
            {
                error_at(attr.range, std::format("unknown attribute `@{}`", attr.name));
                return;
            }

            bool needs_one_arg = (attr.name == "align" || attr.name == "section" || attr.name == "calling_conv" || attr.name == "deprecated");
            bool needs_zero_args = !needs_one_arg;

            if (needs_zero_args && !attr.args.empty())
                error_at(attr.range, std::format("attribute `@{}` does not take arguments", attr.name));

            if (needs_one_arg && attr.args.empty())
                error_at(attr.range, std::format("attribute `@{}` requires an argument", attr.name));

            if (needs_one_arg && attr.args.size() > 1)
                error_at(attr.range, std::format("attribute `@{}` takes at most one argument", attr.name));

            if (attr.name == "section" && attr.args.size() == 1)
                if (!ast::node_cast<ast::StringLiteralExpr>(attr.args[0]))
                    error_at(attr.range, "attribute `@section` requires a string literal argument");

            if (attr.name == "calling_conv" && attr.args.size() == 1)
                if (!ast::node_cast<ast::StringLiteralExpr>(attr.args[0]))
                    error_at(attr.range, "attribute `@calling_conv` requires a string literal argument");

            if (attr.name == "deprecated" && attr.args.size() == 1)
                if (!ast::node_cast<ast::StringLiteralExpr>(attr.args[0]))
                    error_at(attr.range, "attribute `@deprecated` requires a string literal argument");
        }

        ast::Decl* parse_import_decl_body(bool is_public, sm::Location start)
        {
            auto path = parse_path();

            if (check(TK::ColonColon) && check_at(1, TK::LBrace))
            {
                advance();
                advance();

                auto prefix = std::move(path);
                std::pmr::vector<ast::Decl*> group_imports(m_ctx.allocator());

                while (!check(TK::RBrace) && !eof())
                {
                    auto name_tok = expect(TK::Identifier, "in import group");
                    if (name_tok.kind != TK::Identifier)
                        break;

                    ast::Path combined(m_ctx.allocator());
                    for (auto const& seg : prefix.segments)
                        combined.segments.push_back(seg);

                    combined.segments.push_back({name_tok.interned, name_tok.range});
                    if (!combined.segments.empty())
                        combined.range = sm::SourceRange{combined.segments.front().range.begin, combined.segments.back().range.end};

                    auto nr = name_tok.range;
                    auto* d = m_ctx.make<ast::ImportDecl>(range_from(start), std::move(combined), nr);
                    d->is_public = is_public;
                    group_imports.push_back(d);

                    if (!check(TK::Comma))
                        break;

                    advance();
                }

                expect(TK::RBrace, "to close import group");
                expect(TK::Semicolon, "after import");

                if (group_imports.empty())
                {
                    error_at(range_from(start), "empty import group is not allowed");
                    return nullptr;
                }

                for (std::size_t i = 1; i < group_imports.size(); ++i)
                    m_extra_imports.push_back(group_imports[i]);

                return group_imports[0];
            }

            expect(TK::Semicolon, "after import path");

            auto nr = !path.segments.empty() ? path.segments.back().range : sm::SourceRange{};
            auto* d = m_ctx.make<ast::ImportDecl>(range_from(start), std::move(path), nr);
            d->is_public = is_public;
            return d;
        }

        void flatten_using_item(ast::Path const& prefix, ast::UsingItem const* item, std::pmr::vector<ast::Path>& out)
        {
            if (item->children.empty())
            {
                ast::Path full{m_ctx.allocator()};
                for (auto const& seg : prefix.segments)
                    full.segments.push_back(seg);
                for (auto const& seg : item->path.segments)
                    full.segments.push_back(seg);
                if (!full.segments.empty())
                    full.range = sm::SourceRange{.begin = full.segments.front().range.begin, .end = full.segments.back().range.end};
                out.push_back(std::move(full));
            }
            else
            {
                ast::Path new_prefix{m_ctx.allocator()};
                for (auto const& seg : prefix.segments)
                    new_prefix.segments.push_back(seg);
                for (auto const& seg : item->path.segments)
                    new_prefix.segments.push_back(seg);
                if (!new_prefix.segments.empty())
                    new_prefix.range = sm::SourceRange{.begin = new_prefix.segments.front().range.begin, .end = new_prefix.segments.back().range.end};

                for (auto* child : item->children)
                    flatten_using_item(new_prefix, child, out);
            }
        }

        ast::UsingItem* parse_using_item()
        {
            auto start = loc();
            auto* item = m_ctx.make<ast::UsingItem>();
            item->path = parse_path();

            if (check(TK::ColonColon) && check_at(1, TK::LBrace))
            {
                advance();
                advance();

                if (check(TK::RBrace))
                    error_at(range_from(start), "empty using group is not allowed");
                else
                {
                    while (true)
                    {
                        if (check(TK::RBrace))
                            break;
                        item->children.push_back(parse_using_item());
                        if (!check(TK::Comma))
                            break;
                        advance();
                    }
                }

                expect(TK::RBrace, "to close using group");
                item->range = range_from(start);
            }
            else
                item->range = item->path.range;

            return item;
        }

        ast::Decl* parse_using_decl_body(bool is_public, bool is_spill, sm::Location start)
        {
            auto* d = m_ctx.make<ast::UsingDecl>(range_from(start));
            d->is_public = is_public;
            d->is_spill = is_spill;

            if (check(TK::LBrace))
            {
                advance();
                d->using_kind = ast::UsingKind::List;
                if (check(TK::RBrace))
                    error_at(range_from(start), "empty using list is not allowed");
                else
                {
                    while (true)
                    {
                        if (check(TK::RBrace))
                            break;

                        auto* item = parse_using_item();
                        ast::Path empty_prefix{m_ctx.allocator()};
                        flatten_using_item(empty_prefix, item, d->target_list);
                        d->target_items.push_back(item);
                        if (!check(TK::Comma))
                            break;

                        advance();
                    }
                }

                expect(TK::RBrace, "to close using list");
                expect(TK::Semicolon, "after using declaration");
                d->range = range_from(start);
                return d;
            }

            auto lhs = parse_path();

            if (check(TK::ColonColon) && check_at(1, TK::LBrace))
            {
                advance();
                advance();
                d->using_kind = ast::UsingKind::List;
                d->target_path = std::move(lhs);
                d->name_range = !d->target_path.segments.empty() ? d->target_path.segments.back().range : sm::SourceRange{};
                if (check(TK::RBrace))
                    error_at(range_from(start), "empty using list is not allowed");
                else
                {
                    while (true)
                    {
                        if (check(TK::RBrace))
                            break;

                        auto* item = parse_using_item();
                        ast::Path empty_prefix{m_ctx.allocator()};
                        flatten_using_item(empty_prefix, item, d->target_list);
                        d->target_items.push_back(item);
                        if (!check(TK::Comma))
                            break;

                        advance();
                    }
                }

                expect(TK::RBrace, "to close using list");
                expect(TK::Semicolon, "after using declaration");
                d->range = range_from(start);
                return d;
            }

            if (check(TK::ColonColon) && check_at(1, TK::Star))
            {
                advance();
                advance();
                d->using_kind = ast::UsingKind::Wildcard;
                d->target_path = std::move(lhs);
                d->name_range = !d->target_path.segments.empty() ? d->target_path.segments.back().range : sm::SourceRange{};
                expect(TK::Semicolon, "after using declaration");
                d->range = range_from(start);
                return d;
            }

            if (check(TK::LParen))
            {
                d->name_range = !lhs.segments.empty() ? lhs.segments.back().range : sm::SourceRange{};
                d->alias_path = std::move(lhs);
                d->using_kind = ast::UsingKind::Concept;
                d->template_params = parse_template_param_list();
                expect(TK::Eq, "after concept template parameters");
                d->target_expr = parse_expr();
                expect(TK::Semicolon, "after using declaration");
                d->range = range_from(start);
                return d;
            }

            if (match(TK::Eq))
            {
                d->name_range = !lhs.segments.empty() ? lhs.segments.back().range : sm::SourceRange{};
                d->alias_path = std::move(lhs);

                if (check(TK::LBrace))
                {
                    advance();
                    d->using_kind = ast::UsingKind::List;
                    if (check(TK::RBrace))
                        error_at(range_from(start), "empty using list is not allowed");
                    else
                    {
                        while (true)
                        {
                            if (check(TK::RBrace))
                                break;

                            auto* item = parse_using_item();
                            ast::Path empty_prefix{m_ctx.allocator()};
                            flatten_using_item(empty_prefix, item, d->target_list);
                            d->target_items.push_back(item);
                            if (!check(TK::Comma))
                                break;

                            advance();
                        }
                    }

                    expect(TK::RBrace, "to close using list");
                    expect(TK::Semicolon, "after using declaration");
                    d->range = range_from(start);
                    return d;
                }

                {
                    Speculation spec(*this);
                    if (peek().kind == TK::Identifier)
                    {
                        auto [path, wildcard] = parse_path_or_wildcard();
                        if (wildcard && !spec.had_suppressed_errors())
                        {
                            spec.commit();
                            d->using_kind = ast::UsingKind::Wildcard;
                            d->target_path = std::move(path);
                            expect(TK::Semicolon, "after using declaration");
                            d->range = range_from(start);
                            return d;
                        }
                    }
                }

                {
                    Speculation spec(*this);
                    auto* alias_type = parse_type();
                    if (alias_type && !spec.had_suppressed_errors() && check(TK::Semicolon))
                    {
                        spec.commit();
                        d->using_kind = ast::UsingKind::Alias;
                        d->target_type = alias_type;
                        expect(TK::Semicolon, "after using declaration");
                        d->range = range_from(start);
                        return d;
                    }
                }

                if (check(TK::KwCompiles))
                {
                    d->using_kind = ast::UsingKind::Concept;
                    d->target_expr = parse_compiles_expr();
                    expect(TK::Semicolon, "after using declaration");
                    d->range = range_from(start);
                    return d;
                }

                d->using_kind = ast::UsingKind::Concept;
                d->target_expr = parse_expr();
                expect(TK::Semicolon, "after using declaration");
                d->range = range_from(start);
                return d;
            }

            d->using_kind = ast::UsingKind::BareImport;
            d->target_path = std::move(lhs);
            d->name_range = !d->target_path.segments.empty() ? d->target_path.segments.back().range : sm::SourceRange{};
            expect(TK::Semicolon, "after using declaration");
            d->range = range_from(start);
            return d;
        }

        ast::TypeExpr* parse_type()
        {
            auto start = loc();
            auto prefix_quals = parse_qualifiers();

            auto* base = parse_type_atom();
            if (!base)
                return nullptr;

            if (prefix_quals != ast::Qual::None)
            {
                auto range = range_from(start);
                base = m_ctx.make<ast::QualifiedType>(range, prefix_quals, base);
            }

            base = parse_type_suffix(base);

            return base;
        }

    public:
        ast::Qual parse_qualifiers()
        {
            ast::Qual q = ast::Qual::None;
            for (;;)
            {
                if (match(TK::KwConst))
                    q |= ast::Qual::Const;
                else if (match(TK::KwVolatile))
                    q |= ast::Qual::Volatile;
                else if (match(TK::KwRestrict))
                    q |= ast::Qual::Restrict;
                else
                    break;
            }

            return q;
        }

        ast::TypeExpr* parse_type_atom()
        {
            auto start = loc();

            if (check(TK::LBracket) && check_at(1, TK::RBracket))
            {
                advance();
                advance();
                auto* el = parse_type();
                if (!el)
                    return nullptr;

                return m_ctx.make<ast::SliceType>(range_from(start), el);
            }

            if (ast::is_primitive_type(peek().kind))
            {
                auto tok = advance();
                return m_ctx.make<ast::PrimitiveType>(tok.range, tok.kind);
            }

            if (match(TK::LParen))
            {
                auto* inner = parse_type();
                expect(TK::RParen, "after grouped type");
                return inner;
            }

            if (check(TK::Identifier))
            {
                auto path = parse_path();
                auto* nt = m_ctx.make<ast::NamedType>(range_from(start), std::move(path));

                if (check(TK::LParen))
                {
                    advance();
                    if (!check(TK::RParen))
                        do
                            nt->template_args.push_back(parse_template_arg());
                        while (match(TK::Comma));

                    expect(TK::RParen, "to close template arguments");
                    nt->range = range_from(start);
                }

                return nt;
            }

            error_at(single_range(), std::format("expected type, found '{}'", lex::to_string(peek().kind)));
            return nullptr;
        }

        ast::TypeExpr* parse_type_suffix(ast::TypeExpr* base)
        {
            auto start = base->range.begin;

            for (;;)
            {
                if (check(TK::Dot) && check_at(1, TK::IntLiteral))
                {
                    advance();
                    auto tok = advance();
                    std::int64_t iv = 0;
                    if (tok.value)
                        if (auto* vp = std::get_if<std::intmax_t>(&*tok.value))
                            iv = static_cast<std::int64_t>(*vp);

                    auto idx = m_ctx.make<ast::IntLiteralExpr>(tok.range, iv, tok.interned);
                    base = m_ctx.make<ast::PackIndexType>(range_from(start), base, idx);
                    continue;
                }

                if (check(TK::LBracket))
                {
                    advance();
                    if (match(TK::RBracket))
                    {
                        base = m_ctx.make<ast::FamType>(range_from(start), base);
                        continue;
                    }

                    auto* size = parse_expr();
                    expect(TK::RBracket, "to close array size");
                    base = m_ctx.make<ast::ArrayType>(range_from(start), base, size);
                    continue;
                }

                if (check(TK::LParen) && check_at(1, TK::Star) && check_at(2, TK::RParen))
                {
                    advance();
                    advance();
                    advance();
                    expect(TK::LParen, "in function pointer parameter list");

                    auto* fp = m_ctx.make<ast::FuncPtrType>(range_from(start), base);

                    if (!check(TK::RParen))
                        do
                            fp->params.push_back(parse_type());
                        while (match(TK::Comma));

                    expect(TK::RParen, "to close function pointer parameter list");
                    fp->range = range_from(start);
                    base = fp;
                    continue;
                }

                if (match(TK::Star))
                {
                    base = m_ctx.make<ast::PointerType>(range_from(start), base);
                    continue;
                }

                if (ast::is_qualifier(peek().kind))
                {
                    auto q = parse_qualifiers();
                    base = m_ctx.make<ast::QualifiedType>(range_from(start), q, base);
                    continue;
                }

                break;
            }

            return base;
        }

        ast::TemplateArg parse_template_arg()
        {
            auto start = loc();
            ast::TemplateArg arg;

            if (ast::is_type_start(peek().kind))
            {
                Speculation spec(*this);
                auto* type = parse_type();
                if (type && !spec.had_suppressed_errors() && (check(TK::Comma) || check(TK::RParen)))
                {
                    spec.commit();
                    arg.type = type;
                    arg.range = range_from(start);
                    return arg;
                }
            }

            arg.expr = parse_expr();
            arg.range = range_from(start);
            return arg;
        }

        std::pmr::vector<ast::TemplateParam> parse_template_param_list(bool allow_pack = true)
        {
            std::pmr::vector<ast::TemplateParam> params(m_ctx.allocator());
            expect(TK::LParen, "to begin template parameter list");
            if (match(TK::RParen))
                return params;

            do
            {
                auto start = loc();
                ast::TemplateParam tp;

                if (check(TK::Identifier) && (check_at(1, TK::Comma) || check_at(1, TK::RParen) || check_at(1, TK::Ellipsis)))
                {
                    auto tok = advance();
                    tp.name = tok.interned;
                    tp.range = tok.range;
                    if (match(TK::Ellipsis))
                    {
                        tp.is_pack = true;
                        if (!allow_pack)
                            error_at(tp.range, "variadic template parameters are not allowed on struct/enum declarations");
                    }
                }
                else
                {
                    tp.value_type = parse_type();
                    auto name = expect(TK::Identifier, "in template value parameter");
                    if (name.kind == TK::Identifier)
                    {
                        tp.name = name.interned;
                        tp.range = range_from(start);
                        if (match(TK::Ellipsis))
                        {
                            tp.is_pack = true;
                            if (!allow_pack)
                                error_at(tp.range, "variadic template parameters are not allowed on struct/enum declarations");
                        }
                    }
                }
                params.push_back(std::move(tp));
            } while (match(TK::Comma));

            bool seen_pack = false;
            for (auto const& p : params)
            {
                if (seen_pack && !p.is_pack)
                    error_at(p.range, "non-pack template parameter after pack parameter");

                if (p.is_pack) // TODO loosen.
                    seen_pack = true;
            }

            expect(TK::RParen, "to close template parameter list");
            return params;
        }

        ast::Decl* parse_func_or_var_decl(std::pmr::vector<ast::Attribute> attrs, bool is_public, bool is_extern, sm::Location start)
        {
            if (!ast::is_type_start(peek().kind))
            {
                error_unexpected("at top level");
                return nullptr;
            }

            auto* type = parse_type();
            if (!type)
                return nullptr;

            auto name_tok = expect(TK::Identifier, "in declaration");
            if (name_tok.kind != TK::Identifier)
                return nullptr;

            if (check(TK::Eq) || check(TK::Semicolon))
            {
                auto* var = m_ctx.make<ast::VarDecl>(sm::SourceRange{}, name_tok.interned, name_tok.range);
                var->type = type;
                var->is_public = is_public;
                var->is_extern = is_extern;
                var->attrs = std::move(attrs);

                if (match(TK::Eq))
                    var->init = parse_expr();

                expect(TK::Semicolon, "after variable declaration");
                var->range = range_from(start);
                return var;
            }

            if (check(TK::LParen))
            {
                auto* func = m_ctx.make<ast::FuncDecl>(sm::SourceRange{}, name_tok.interned, name_tok.range);
                func->return_type = type;
                func->is_public = is_public;
                func->is_extern = is_extern;
                func->attrs = std::move(attrs);

                auto first = parse_param_shape_list();

                if (check(TK::LParen))
                {
                    for (auto& shape : first)
                    {
                        ast::TemplateParam tp;
                        tp.name = shape.name;
                        tp.range = shape.range;
                        tp.value_type = shape.type;
                        tp.is_pack = shape.is_pack;
                        func->template_params.push_back(std::move(tp));
                    }

                    auto second = parse_param_shape_list();
                    for (auto& shape : second)
                    {
                        if (!shape.type)
                        {
                            error_at(shape.range, "function parameter must have a type");
                            continue;
                        }
                        ast::FuncParam fp;
                        fp.name = shape.name;
                        fp.range = shape.range;
                        fp.type = shape.type;
                        fp.is_pack = shape.is_pack;
                        func->params.push_back(std::move(fp));
                    }
                }
                else
                {
                    for (auto& shape : first)
                    {
                        if (!shape.type)
                        {
                            error_at_with_help(shape.range, "function parameter must have a type",
                                               "to introduce a template parameter, add a second '()' for the regular parameter list");
                            continue;
                        }

                        ast::FuncParam fp;
                        fp.name = shape.name;
                        fp.range = shape.range;
                        fp.type = shape.type;
                        fp.is_pack = shape.is_pack;
                        func->params.push_back(std::move(fp));
                    }
                }

                if (match(TK::KwIf))
                    func->constraint = parse_expr(0, true);

                if (match(TK::Semicolon))
                    ;
                else if (check(TK::LBrace))
                    func->body = parse_block();
                else
                    error_at(single_range(), "expected '{' for function body or ';' for forward declaration");

                func->range = range_from(start);
                return func;
            }

            error_at(single_range(), std::format("expected '(', '=', or ';' after declarator name, found '{}'", lex::to_string(peek().kind)));
            return nullptr;
        }

        std::pmr::vector<ParamShape> parse_param_shape_list()
        {
            std::pmr::vector<ParamShape> result(m_ctx.allocator());
            expect(TK::LParen, "to begin parameter list");
            if (match(TK::RParen))
                return result;

            do
            {
                auto start = loc();
                ParamShape shape;

                if (check(TK::Identifier) && (check_at(1, TK::Comma) || check_at(1, TK::RParen) || check_at(1, TK::Ellipsis)))
                {
                    auto tok = advance();
                    shape.name = tok.interned;
                    shape.name_range = tok.range;
                    shape.range = tok.range;
                    if (match(TK::Ellipsis))
                        shape.is_pack = true;
                }
                else
                {
                    shape.type = parse_type();
                    auto name = expect(TK::Identifier, "in parameter declaration");
                    if (name.kind == TK::Identifier)
                    {
                        shape.name = name.interned;
                        shape.name_range = name.range;
                        if (match(TK::Ellipsis))
                            shape.is_pack = true;
                    }

                    shape.range = range_from(start);
                }
                result.push_back(std::move(shape));
            } while (match(TK::Comma));

            bool seen_pack = false; // TODO loosen and improve deductions.
            for (auto const& p : result)
            {
                if (seen_pack && !p.is_pack)
                    error_at(p.range, "non-pack parameter after pack parameter");

                if (p.is_pack)
                    seen_pack = true;
            }

            expect(TK::RParen, "to close parameter list");
            return result;
        }

        ast::Decl* parse_struct_decl(std::pmr::vector<ast::Attribute> attrs, bool is_public, sm::Location start)
        {
            advance();
            auto name = expect(TK::Identifier, "in struct declaration");
            if (name.kind != TK::Identifier)
                return nullptr;

            auto* d = m_ctx.make<ast::StructDecl>(sm::SourceRange{}, name.interned, name.range);
            d->is_public = is_public;
            d->attrs = std::move(attrs);

            if (check(TK::LParen))
                d->template_params = parse_template_param_list(false);

            if (match(TK::KwIf))
                d->constraint = parse_expr(0, true);

            expect(TK::LBrace, "to begin struct body");
            while (!check(TK::RBrace) && !eof())
            {
                auto prev_pos = m_pos;
                d->fields.push_back(parse_field_decl());
                if (m_pos == prev_pos && !check(TK::RBrace) && !eof())
                    advance();
            }

            expect(TK::RBrace, "to close struct body");

            d->range = range_from(start);
            return d;
        }

        ast::Decl* parse_union_decl(std::pmr::vector<ast::Attribute> attrs, bool is_public, sm::Location start)
        {
            advance();
            auto name = expect(TK::Identifier, "in union declaration");
            if (name.kind != TK::Identifier)
                return nullptr;

            auto* d = m_ctx.make<ast::UnionDecl>(sm::SourceRange{}, name.interned, name.range);
            d->is_public = is_public;
            d->attrs = std::move(attrs);

            expect(TK::LBrace, "to begin union body");
            while (!check(TK::RBrace) && !eof())
            {
                auto prev_pos = m_pos;
                d->fields.push_back(parse_field_decl());
                if (m_pos == prev_pos && !check(TK::RBrace) && !eof())
                    advance();
            }

            expect(TK::RBrace, "to close union body");

            d->range = range_from(start);
            return d;
        }

        ast::FieldDecl parse_field_decl()
        {
            auto start = loc();
            ast::FieldDecl f;

            auto saved_pos = m_pos;
            f.type = parse_type();
            auto name = expect(TK::Identifier, "in field declaration");
            if (name.kind == TK::Identifier)
            {
                f.name = name.interned;
                f.name_range = name.range;
            }

            expect(TK::Semicolon, "after field declaration");
            f.range = range_from(start);

            if (m_pos == saved_pos && !check(TK::Semicolon) && !check(TK::RBrace) && !eof())
            {
                while (!check(TK::Semicolon) && !check(TK::RBrace) && !eof())
                    advance();
                if (check(TK::Semicolon))
                    advance();
            }

            return f;
        }

        ast::Decl* parse_enum_decl(std::pmr::vector<ast::Attribute> attrs, bool is_public, sm::Location start)
        {
            advance();
            auto name = expect(TK::Identifier, "in enum declaration");
            if (name.kind != TK::Identifier)
                return nullptr;

            auto* d = m_ctx.make<ast::EnumDecl>(sm::SourceRange{}, name.interned, name.range);
            d->is_public = is_public;
            d->attrs = std::move(attrs);

            if (check(TK::LParen))
                d->template_params = parse_template_param_list(false);

            if (match(TK::Colon))
                d->backing_type = parse_type();

            if (match(TK::KwIf))
                d->constraint = parse_expr(0, true);

            expect(TK::LBrace, "to begin enum body");
            bool any_payload = false;
            while (!check(TK::RBrace) && !eof())
            {
                auto vstart = loc();
                ast::EnumVariant variant(m_ctx.allocator());
                variant.attrs = parse_attributes();

                auto vname = expect(TK::Identifier, "in enum variant");
                if (vname.kind == TK::Identifier)
                {
                    variant.name = vname.interned;
                    variant.range = vname.range;
                }

                if (match(TK::LParen))
                {
                    any_payload = true;
                    if (!check(TK::RParen))
                        do
                            variant.payload.push_back(parse_type());
                        while (match(TK::Comma));

                    expect(TK::RParen, "to close variant payload");
                }

                if (match(TK::Eq))
                    variant.explicit_value = parse_expr();

                variant.range = range_from(vstart);
                d->variants.push_back(std::move(variant));

                if (!match(TK::Comma))
                    break;
            }
            expect(TK::RBrace, "to close enum body");
            d->is_tagged = any_payload;
            d->range = range_from(start);
            return d;
        }

        ast::Stmt* parse_stmt()
        {
            auto start = loc();

            if (match(TK::Semicolon))
                return m_ctx.make<ast::ExprStmt>(range_from(start), nullptr);

            switch (peek().kind)
            {
                case TK::KwReturn:
                    return parse_return_stmt();
                case TK::KwBreak:
                    advance();
                    expect(TK::Semicolon, "after 'break'");
                    return m_ctx.make<ast::BreakStmt>(range_from(start), m_ctx.allocator());
                case TK::KwContinue:
                    advance();
                    expect(TK::Semicolon, "after 'continue'");
                    return m_ctx.make<ast::ContinueStmt>(range_from(start), m_ctx.allocator());
                case TK::KwWhile:
                    return parse_while_stmt();
                case TK::KwDo:
                    return parse_do_while_stmt();
                case TK::KwFor:
                    return parse_for_stmt();
                case TK::KwDefer:
                    return parse_defer_stmt();
                case TK::KwStatic:
                    if (check_at(1, TK::KwIf))
                        return parse_static_if_stmt();

                    if (check_at(1, TK::KwMatch))
                        return parse_static_match_stmt();

                    if (check_at(1, TK::KwFor))
                        return parse_static_for_stmt();

                    error_at(single_range(), "'static' must be followed by 'if', 'match', or 'for'");
                    advance();
                    return nullptr;
                case TK::At: {
                    auto attrs = parse_attributes();
                    if (ast::is_type_start(peek().kind))
                    {
                        auto var_start = loc();

                        Speculation spec(*this);
                        auto* var = speculate_var_decl(var_start);
                        if (var && !spec.had_suppressed_errors())
                        {
                            spec.commit();
                            var->attrs = std::move(attrs);
                            var->range = range_from(start);
                            return m_ctx.make<ast::DeclStmt>(var->range, var);
                        }
                    }
                    error_at(single_range(), "expected variable declaration after attributes");
                    synchronize_to_stmt();
                    return nullptr;
                }
                default:
                    break;
            }

            if (auto* stmt = try_parse_decl_or_expr_stmt())
                return stmt;

            auto* expr = parse_expr();
            if (!expr)
                return nullptr;

            if (match(TK::Semicolon) || is_block_like_expr(expr))
            {
                auto* es = m_ctx.make<ast::ExprStmt>(range_from(start), expr);
                return es;
            }

            error_at(single_range(), "expected ';' after expression statement");
            return m_ctx.make<ast::ExprStmt>(range_from(start), expr);
        }

        ast::Decl* speculate_var_decl(sm::Location start)
        {
            auto* type = parse_type();
            if (!type)
                return nullptr;

            if (!check(TK::Identifier))
                return nullptr;

            if (peek(1).kind != TK::Eq && peek(1).kind != TK::Semicolon)
                return nullptr;

            auto name_tok = advance();
            auto* var = m_ctx.make<ast::VarDecl>(sm::SourceRange{}, name_tok.interned, name_tok.range);
            var->type = type;

            if (match(TK::Eq))
            {
                var->init = parse_expr();
                if (!var->init)
                    return nullptr;
            }

            if (!match(TK::Semicolon))
                return nullptr;

            var->range = range_from(start);
            return var;
        }

        ast::Stmt* try_parse_decl_or_expr_stmt()
        {
            if (!ast::is_type_start(peek().kind))
                return nullptr;

            auto start = loc();
            auto save_prev_end = m_prev_end;

            struct Attempt
            {
                std::size_t end_pos{};
                sm::Location end_prev{};
                bool had_suppressed_error{};
                bool ok{false};
            };

            ast::Decl* decl_alt = nullptr;
            Attempt da;
            {
                Speculation spec(*this);
                decl_alt = speculate_var_decl(start);
                if (decl_alt)
                {
                    da.had_suppressed_error = spec.had_suppressed_errors();
                    da.end_pos = m_pos;
                    da.end_prev = m_prev_end;
                    da.ok = !da.had_suppressed_error;
                }
            }

            ast::Expr* expr_alt = nullptr;
            Attempt ea;
            {
                Speculation spec(*this);
                auto* e = parse_expr();
                if (e && match(TK::Semicolon))
                {
                    ea.had_suppressed_error = spec.had_suppressed_errors();
                    expr_alt = e;
                    ea.end_pos = m_pos;
                    ea.end_prev = m_prev_end;
                    ea.ok = !ea.had_suppressed_error;
                }
            }

            auto adopt = [&](Attempt const& a) noexcept {
                m_pos = a.end_pos;
                m_prev_end = a.end_prev;
            };

            if (da.ok && ea.ok && da.end_pos == ea.end_pos)
            {
                adopt(da);
                return m_ctx.make<ast::AmbiguousStmt>(range_from(start), decl_alt, expr_alt);
            }

            if (da.ok && (!ea.ok || da.end_pos > ea.end_pos))
            {
                adopt(da);
                return m_ctx.make<ast::DeclStmt>(decl_alt->range, decl_alt);
            }

            if (ea.ok)
            {
                adopt(ea);
                return m_ctx.make<ast::ExprStmt>(range_from(start), expr_alt);
            }

            m_prev_end = save_prev_end;
            return nullptr;
        }

        ast::Stmt* parse_return_stmt()
        {
            auto start = loc();
            advance();
            auto* stmt = m_ctx.make<ast::ReturnStmt>(sm::SourceRange{}, m_ctx.allocator());
            if (!check(TK::Semicolon))
                stmt->value = parse_expr();

            expect(TK::Semicolon, "after 'return'");
            stmt->range = range_from(start);
            return stmt;
        }

        ast::Stmt* parse_while_stmt()
        {
            auto start = loc();
            advance();
            auto* cond = parse_expr(0, true);
            auto body = parse_block();
            auto* s = m_ctx.make<ast::WhileStmt>(range_from(start), cond, std::move(body));
            return s;
        }

        ast::Stmt* parse_do_while_stmt()
        {
            auto start = loc();
            advance();
            auto body = parse_block();
            expect(TK::KwWhile, "after 'do' block");
            auto* cond = parse_expr();
            expect(TK::Semicolon, "after 'while' condition");
            return m_ctx.make<ast::DoWhileStmt>(range_from(start), std::move(body), cond);
        }

        ast::Stmt* parse_for_stmt()
        {
            auto start = loc();
            advance();

            bool by_ref = match(TK::Amp);

            if (check(TK::Identifier) && check_at(1, TK::KwIn))
            {
                auto name_tok = advance();
                advance();
                auto* iter = parse_expr(0, true);
                auto body = parse_block();
                auto* s = m_ctx.make<ast::ForInStmt>(range_from(start), std::move(body));
                s->item_name = name_tok.interned;
                s->name_range = name_tok.range;
                s->iterable = iter;
                s->by_reference = by_ref;
                return s;
            }

            if (ast::is_type_start(peek().kind))
            {
                Speculation spec(*this);
                auto* type = parse_type();

                bool type_ref = match(TK::Amp);
                if (type && !spec.had_suppressed_errors() && check(TK::Identifier) && check_at(1, TK::KwIn))
                {
                    auto name_tok = advance();
                    advance();
                    spec.commit();
                    auto* iter = parse_expr(0, true);
                    auto body = parse_block();
                    auto* s = m_ctx.make<ast::ForInStmt>(range_from(start), std::move(body));
                    s->item_type = type;
                    s->item_name = name_tok.interned;
                    s->name_range = name_tok.range;
                    s->iterable = iter;
                    s->by_reference = by_ref || type_ref;
                    return s;
                }
            }

            bool has_parens = false;
            if (match(TK::LParen))
                has_parens = true;

            ast::StmtPtr init = nullptr;
            if (check(TK::Semicolon))
                advance();
            else
                init = parse_stmt();

            ast::Expr* cond = nullptr;
            if (!check(TK::Semicolon))
                cond = parse_expr();

            expect(TK::Semicolon, "after for-condition");

            ast::Expr* update = nullptr;
            if (!check(TK::LBrace) && !(has_parens && check(TK::RParen)))
                update = parse_expr(0, true);

            if (has_parens)
                expect(TK::RParen, "after for-update or for-condition");

            auto body = parse_block();
            auto* s = m_ctx.make<ast::ForStmt>(range_from(start), std::move(body));
            s->init = init;
            s->cond = cond;
            s->update = update;
            return s;
        }

        ast::Stmt* parse_defer_stmt()
        {
            auto start = loc();
            advance();
            auto* body = parse_stmt();
            return m_ctx.make<ast::DeferStmt>(range_from(start), body);
        }

        ast::Stmt* parse_static_if_stmt()
        {
            auto start = loc();
            advance();
            advance();

            return finish_static_if(start);
        }

        ast::StaticIfStmt* finish_static_if(sm::Location start)
        {
            auto* cond = parse_expr(0, true);
            auto then_block = parse_block();
            auto* s = m_ctx.make<ast::StaticIfStmt>(range_from(start), cond, std::move(then_block));

            if (auto* bin = ast::node_cast<ast::BinaryExpr>(cond))
            {
                if (bin->op == TK::EqEq && ast::node_cast<ast::IdentExpr>(bin->lhs))
                    s->is_type_if = true;
            }

            if (match(TK::KwElse))
            {
                if (check(TK::KwStatic) && check_at(1, TK::KwIf))
                {
                    auto chain_start = loc();
                    advance();
                    advance();
                    s->else_branch = finish_static_if(chain_start);
                }
                else if (match(TK::KwIf))
                {
                    auto chain_start = previous().range.begin;
                    s->else_branch = finish_static_if(chain_start);
                }
                else
                {
                    auto block_start = loc();
                    auto block = parse_block();
                    auto* be = m_ctx.make<ast::BlockExpr>(range_from(block_start), std::move(block));
                    s->else_branch = m_ctx.make<ast::ExprStmt>(range_from(block_start), be);
                }
            }

            s->range = range_from(start);
            return s;
        }

        ast::MatchArm parse_type_match_arm()
        {
            auto start = loc();
            ast::MatchArm arm;

            if (check(TK::Identifier) && peek().interned == "_")
            {
                advance();
                arm.is_wildcard = true;
            }
            else
                arm.type_pattern = parse_type();

            if (match(TK::KwIf))
                arm.guard = parse_expr(0, true);

            expect(TK::FatArrow, "in type match arm");
            arm.body = parse_expr();
            arm.range = range_from(start);
            return arm;
        }

        [[nodiscard]] static bool token_starts_type(lex::TokenKind k) noexcept
        {
            switch (k)
            {
                case lex::TokenKind::Kwu8:
                case lex::TokenKind::Kwi8:
                case lex::TokenKind::Kwu16:
                case lex::TokenKind::Kwi16:
                case lex::TokenKind::Kwu32:
                case lex::TokenKind::Kwi32:
                case lex::TokenKind::Kwu64:
                case lex::TokenKind::Kwi64:
                case lex::TokenKind::Kwf32:
                case lex::TokenKind::Kwf64:
                case lex::TokenKind::KwChar:
                case lex::TokenKind::KwBool:
                case lex::TokenKind::KwVoid:
                case lex::TokenKind::KwConst:
                case lex::TokenKind::KwVolatile:
                case lex::TokenKind::KwRestrict:
                    return true;
                default:
                    return false;
            }
        }

        ast::Stmt* parse_static_match_stmt()
        {
            auto start = loc();
            advance();
            advance();

            auto* operand = parse_expr(0, true);
            auto* s = m_ctx.make<ast::StaticMatchStmt>(range_from(start), operand);

            expect(TK::LBrace, "to begin static match body");

            s->is_type_match = token_starts_type(peek().kind);

            while (!check(TK::RBrace) && !eof())
            {
                if (s->is_type_match)
                    s->arms.push_back(parse_type_match_arm());
                else
                    s->arms.push_back(parse_match_arm());

                if (!match(TK::Comma))
                    break;
            }

            expect(TK::RBrace, "to close static match body");
            s->range = range_from(start);
            return s;
        }

        ast::Stmt* parse_static_for_stmt()
        {
            auto start = loc();
            advance();
            advance();

            if (check(TK::Identifier) && check_at(1, TK::KwIn))
            {
                auto name_tok = advance();
                advance();
                auto* pack = parse_expr(0, true);
                auto body = parse_block();
                auto* s = m_ctx.make<ast::StaticForStmt>(range_from(start), std::move(body));
                s->item_name = name_tok.interned;
                s->name_range = name_tok.range;
                s->pack_expr = pack;
                if (ast::node_cast<ast::RangeExpr>(pack))
                    s->is_range_for = true;
                return s;
            }

            if (ast::is_type_start(peek().kind))
            {
                Speculation spec(*this);
                auto* type = parse_type();
                if (type && !spec.had_suppressed_errors() && check(TK::Identifier) && check_at(1, TK::KwIn))
                {
                    auto name_tok = advance();
                    advance();
                    spec.commit();
                    auto* pack = parse_expr(0, true);
                    auto body = parse_block();
                    auto* s = m_ctx.make<ast::StaticForStmt>(range_from(start), std::move(body));
                    s->item_name = name_tok.interned;
                    s->name_range = name_tok.range;
                    s->pack_expr = pack;
                    s->is_type_for = true;
                    return s;
                }
            }

            error_at(single_range(), "expected 'identifier in <pack>' after 'static for'");
            auto body = parse_block();
            return m_ctx.make<ast::StaticForStmt>(range_from(start), std::move(body));
        }

        ast::Block parse_block()
        {
            auto start = loc();
            expect(TK::LBrace, "to begin block");

            ast::Block block(m_ctx.allocator());
            block.range.begin = start;

            while (!check(TK::RBrace) && !eof())
            {
                bool is_stmt_keyword = false;
                switch (peek().kind)
                {
                    case TK::KwReturn:
                    case TK::KwBreak:
                    case TK::KwContinue:
                    case TK::KwWhile:
                    case TK::KwDo:
                    case TK::KwFor:
                    case TK::KwDefer:
                    case TK::KwStatic:
                    case TK::At:
                    case TK::Semicolon:
                        is_stmt_keyword = true;
                        break;
                    default:
                        break;
                }

                if (is_stmt_keyword)
                {
                    auto* s = parse_stmt();
                    if (s)
                        block.stmts.push_back(s);
                    else
                        synchronize_to_stmt();

                    continue;
                }

                if (auto* s = try_parse_decl_or_expr_stmt())
                {
                    block.stmts.push_back(s);
                    continue;
                }

                auto stmt_start = loc();
                auto* expr = parse_expr();
                if (!expr)
                {
                    synchronize_to_stmt();
                    continue;
                }

                if (match(TK::Semicolon))
                    block.stmts.push_back(m_ctx.make<ast::ExprStmt>(range_from(stmt_start), expr));
                else if (check(TK::RBrace))
                {
                    block.tail = expr;
                    break;
                }
                else if (is_block_like_expr(expr))
                    block.stmts.push_back(m_ctx.make<ast::ExprStmt>(range_from(stmt_start), expr));
                else
                {
                    error_at(single_range(), "expected ';' or '}' after expression");
                    synchronize_to_stmt();
                }
            }

            expect(TK::RBrace, "to close block");
            block.range = range_from(start);
            return block;
        }

        bool is_block_like_expr(ast::Expr* e)
        {
            if (!e)
                return false;

            switch (e->kind)
            {
                case ast::ExprKind::Block:
                case ast::ExprKind::If:
                case ast::ExprKind::Match:
                case ast::ExprKind::StructLiteral:
                case ast::ExprKind::Compiles:
                    return true;
                default:
                    return false;
            }
        }

        int binary_precedence(TK k) noexcept
        {
            switch (k)
            {
                case TK::Eq:
                case TK::PlusEq:
                case TK::MinusEq:
                case TK::StarEq:
                case TK::SlashEq:
                case TK::PercentEq:
                case TK::AmpEq:
                case TK::PipeEq:
                case TK::CaretEq:
                case TK::LtLtEq:
                case TK::GtGtEq:
                    return 1;

                case TK::DotDot:
                    return 2;

                case TK::PipePipe:
                    return 3;
                case TK::AmpAmp:
                    return 4;
                case TK::Pipe:
                    return 5;
                case TK::Caret:
                    return 6;
                case TK::Amp:
                    return 7;
                case TK::EqEq:
                case TK::BangEq:
                    return 8;
                case TK::Lt:
                case TK::Gt:
                case TK::LtEq:
                case TK::GtEq:
                    return 9;
                case TK::LtLt:
                case TK::GtGt:
                    return 10;
                case TK::Plus:
                case TK::Minus:
                    return 11;
                case TK::Star:
                case TK::Slash:
                case TK::Percent:
                    return 12;

                case TK::KwAs:
                    return 13;

                default:
                    return -1;
            }
        }

        bool is_assignment_op(TK k) noexcept { return k == TK::Eq || (k >= TK::PlusEq && k <= TK::GtGtEq); }

        ast::Expr* parse_expr(int min_prec = 0, bool no_struct_lit = false)
        {
            auto* left = parse_unary(no_struct_lit);
            if (!left)
                return nullptr;

            for (;;)
            {
                if (is_block_like_expr(left))
                {
                    auto op = peek().kind;
                    if (op != TK::KwAs && op != TK::AmpAmp && op != TK::PipePipe && op != TK::DotDot)
                        break;
                }

                auto op = peek().kind;
                int prec = binary_precedence(op);
                if (prec < min_prec)
                    break;

                auto op_range = single_range();
                advance();

                if (op == TK::KwAs)
                {
                    auto* type = parse_type();
                    auto range = sm::SourceRange{left->range.begin, m_prev_end};
                    left = m_ctx.make<ast::CastExpr>(range, left, type);
                    continue;
                }

                if (op == TK::DotDot)
                {
                    bool inclusive = match(TK::Eq);
                    auto* right = parse_expr(prec + 1, no_struct_lit);
                    auto range = sm::SourceRange{left->range.begin, m_prev_end};
                    left = m_ctx.make<ast::RangeExpr>(range, left, right, inclusive);
                    continue;
                }

                int next_prec = is_assignment_op(op) ? prec : prec + 1;
                auto* right = parse_expr(next_prec, no_struct_lit);
                if (!right)
                    return left;

                auto range = sm::SourceRange{left->range.begin, m_prev_end};
                left = m_ctx.make<ast::BinaryExpr>(range, left, op, right);
                std::ignore = op_range;
            }

            return left;
        }

        ast::Expr* parse_unary(bool no_struct_lit)
        {
            auto start = loc();
            switch (peek().kind)
            {
                case TK::Minus:
                case TK::Bang:
                case TK::Tilde:
                case TK::Amp:
                case TK::Star:
                case TK::Increment:
                case TK::Decrement: {
                    auto op = advance().kind;
                    auto* operand = parse_unary(no_struct_lit);
                    return m_ctx.make<ast::UnaryExpr>(range_from(start), op, operand);
                }
                default:
                    break;
            }

            auto* primary = parse_primary(no_struct_lit);
            if (!primary)
                return nullptr;

            return parse_postfix(primary, no_struct_lit);
        }

        ast::Expr* parse_postfix(ast::Expr* expr, bool no_struct_lit)
        {
            for (;;)
            {
                auto start = expr->range.begin;
                switch (peek().kind)
                {
                    case TK::Dot: {
                        advance();
                        if (check(TK::IntLiteral))
                        {
                            auto tok = advance();
                            std::int64_t iv = 0;
                            if (tok.value)
                                if (auto* vp = std::get_if<std::intmax_t>(&*tok.value))
                                    iv = static_cast<std::int64_t>(*vp);

                            auto idx = m_ctx.make<ast::IntLiteralExpr>(tok.range, iv, tok.interned);
                            expr = m_ctx.make<ast::PackAccessExpr>(range_from(start), expr, idx);
                            continue;
                        }

                        auto field = expect(TK::Identifier, "after '.'");
                        if (field.kind != TK::Identifier)
                        {
                            if (m_mode == ParseMode::Interactive)
                            {
                                auto dot_end = m_prev_end;
                                expr = m_ctx.make<ast::FieldAccessExpr>(range_from(start), expr, std::string_view{}, sm::SourceRange{dot_end, dot_end});
                            }
                            return expr;
                        }

                        expr = m_ctx.make<ast::FieldAccessExpr>(range_from(start), expr, field.interned, field.range);
                        continue;
                    }
                    case TK::LBracket: {
                        advance();
                        if (check(TK::DotDot))
                        {
                            advance();
                            bool inclusive = match(TK::Eq);
                            ast::ExprPtr end = nullptr;
                            if (!check(TK::RBracket))
                                end = parse_expr();

                            expect(TK::RBracket, "to close index range expression");
                            auto* range = m_ctx.make<ast::RangeExpr>(range_from(start), nullptr, end, inclusive);
                            expr = m_ctx.make<ast::IndexExpr>(range_from(start), expr, range);
                            continue;
                        }

                        auto* idx = parse_expr(3);
                        if (check(TK::DotDot))
                        {
                            advance();
                            bool inclusive = match(TK::Eq);
                            ast::ExprPtr end = nullptr;
                            if (!check(TK::RBracket))
                                end = parse_expr();
                            expect(TK::RBracket, "to close index range expression");
                            auto* range = m_ctx.make<ast::RangeExpr>(range_from(start), idx, end, inclusive);
                            expr = m_ctx.make<ast::IndexExpr>(range_from(start), expr, range);
                            continue;
                        }
                        expect(TK::RBracket, "to close index expression");
                        expr = m_ctx.make<ast::IndexExpr>(range_from(start), expr, idx);
                        continue;
                    }
                    case TK::LParen: {
                        advance();
                        auto* call = m_ctx.make<ast::CallExpr>(sm::SourceRange{}, expr);
                        if (!check(TK::RParen))
                        {
                            auto parse_arg = [&]() -> ast::Expr* {
                                auto as = loc();
                                auto* arg = parse_expr();
                                if (arg && match(TK::Ellipsis))
                                    arg = m_ctx.make<ast::PackExpansionExpr>(range_from(as), arg);
                                return arg;
                            };
                            auto* first = parse_arg();
                            if (first)
                                call->args.push_back(first);
                            while (match(TK::Comma))
                            {
                                auto* arg = parse_arg();
                                if (arg)
                                    call->args.push_back(arg);
                                else if (m_mode == ParseMode::Interactive)
                                    break;
                            }
                        }

                        expect(TK::RParen, "to close call argument list");
                        call->range = range_from(start);
                        expr = call;
                        continue;
                    }
                    case TK::Bang: {
                        Speculation spec(*this);
                        advance();

                        auto* inst = m_ctx.make<ast::TemplateInstExpr>(sm::SourceRange{}, expr);

                        if (match(TK::LParen))
                        {
                            if (!check(TK::RParen))
                                do
                                    inst->template_args.push_back(parse_template_arg());
                                while (match(TK::Comma));

                            expect(TK::RParen, "to close template arguments");
                        }
                        else
                        {
                            auto arg_start = loc();
                            ast::TemplateArg arg;
                            if (ast::is_primitive_type(peek().kind))
                            {
                                auto tok = advance();
                                arg.type = m_ctx.make<ast::PrimitiveType>(tok.range, tok.kind);
                            }
                            else if (check(TK::Identifier))
                            {
                                auto tok = advance();
                                ast::Path p(m_ctx.allocator());
                                p.segments.push_back({tok.interned, tok.range});
                                p.range = tok.range;
                                arg.type = m_ctx.make<ast::NamedType>(tok.range, std::move(p));
                            }
                            else if (peek().kind == TK::IntLiteral || peek().kind == TK::FloatLiteral || peek().kind == TK::StringLiteral ||
                                     peek().kind == TK::U16StringLiteral || peek().kind == TK::CharLiteral || peek().kind == TK::U16CharLiteral ||
                                     peek().kind == TK::KwTrue || peek().kind == TK::KwFalse || peek().kind == TK::KwNull)
                                arg.expr = parse_primary(no_struct_lit);
                            else
                                return expr;

                            arg.range = range_from(arg_start);
                            inst->template_args.push_back(std::move(arg));
                        }

                        spec.commit();

                        inst->range = range_from(start);
                        expr = inst;

                        continue;
                    }
                    case TK::Increment:
                    case TK::Decrement: {
                        auto op = advance().kind;
                        expr = m_ctx.make<ast::PostfixExpr>(range_from(start), expr, op);
                        continue;
                    }
                    default:
                        return expr;
                }
            }
        }

        ast::Expr* parse_primary(bool no_struct_lit)
        {
            auto start = loc();
            switch (peek().kind)
            {
                case TK::IntLiteral: {
                    auto tok = advance();
                    std::int64_t v = 0;
                    if (tok.value)
                        if (auto* iv = std::get_if<std::intmax_t>(&*tok.value))
                            v = static_cast<std::int64_t>(*iv);

                    return m_ctx.make<ast::IntLiteralExpr>(tok.range, v, tok.interned);
                }
                case TK::FloatLiteral: {
                    auto tok = advance();
                    double v = 0.0;
                    if (tok.value)
                        if (auto* dv = std::get_if<double>(&*tok.value))
                            v = *dv;

                    return m_ctx.make<ast::FloatLiteralExpr>(tok.range, v, tok.interned);
                }
                case TK::StringLiteral: {
                    auto tok = advance();
                    std::string_view v;
                    if (tok.value)
                        if (auto* sv = std::get_if<std::string>(&*tok.value))
                            v = *sv;

                    return m_ctx.make<ast::StringLiteralExpr>(tok.range, v, tok.interned);
                }
                case TK::U16StringLiteral: {
                    auto tok = advance();
                    std::u16string_view v;
                    if (tok.value)
                        if (auto* uv = std::get_if<std::u16string>(&*tok.value))
                            v = *uv;

                    return m_ctx.make<ast::U16StringLiteralExpr>(tok.range, v, tok.interned);
                }
                case TK::CharLiteral: {
                    auto tok = advance();
                    std::uint32_t cp = 0;
                    if (tok.value)
                        if (auto* uv = std::get_if<std::uint32_t>(&*tok.value))
                            cp = *uv;

                    return m_ctx.make<ast::CharLiteralExpr>(tok.range, cp);
                }
                case TK::U16CharLiteral: {
                    auto tok = advance();
                    std::uint32_t val = 0;
                    if (tok.value)
                        if (auto* uv = std::get_if<std::uint32_t>(&*tok.value))
                            val = *uv;

                    return m_ctx.make<ast::U16CharLiteralExpr>(tok.range, val);
                }
                case TK::KwTrue:
                    advance();
                    return m_ctx.make<ast::BoolLiteralExpr>(range_from(start), true);
                case TK::KwFalse:
                    advance();
                    return m_ctx.make<ast::BoolLiteralExpr>(range_from(start), false);
                case TK::KwNull:
                    advance();
                    return m_ctx.make<ast::NullLiteralExpr>(range_from(start));

                case TK::LParen: {
                    advance();
                    auto* inner = parse_expr();
                    expect(TK::RParen, "to close grouped expression");
                    return inner;
                }

                case TK::LBrace:
                    return parse_brace_expr(nullptr, start);

                case TK::KwIf:
                    return parse_if_expr();
                case TK::KwMatch:
                    return parse_match_expr();
                case TK::KwSizeof:
                    return parse_sizeof_expr();
                case TK::KwAlignof:
                    return parse_alignof_expr();
                case TK::KwOffsetof:
                    return parse_offsetof_expr();
                case TK::KwCompiles:
                    return parse_compiles_expr();

                case TK::Identifier: {
                    {
                        Speculation spec(*this);
                        auto first = advance();
                        if (match(TK::LParen))
                        {
                            std::pmr::vector<ast::TemplateArg> enum_args(m_ctx.allocator());
                            if (!check(TK::RParen))
                                do
                                    enum_args.push_back(parse_template_arg());
                                while (match(TK::Comma));

                            if (match(TK::RParen) && match(TK::ColonColon))
                            {
                                ast::Path full_path(m_ctx.allocator());
                                full_path.segments.push_back({first.interned, first.range});

                                auto variant_name = expect(TK::Identifier, "after '::' in enum construction");
                                if (variant_name.kind == TK::Identifier)
                                    full_path.segments.push_back({variant_name.interned, variant_name.range});

                                while (check(TK::ColonColon) && check_at(1, TK::Identifier))
                                {
                                    advance();
                                    auto seg = advance();
                                    full_path.segments.push_back({seg.interned, seg.range});
                                }

                                if (!spec.had_suppressed_errors())
                                {
                                    spec.commit();
                                    full_path.range = range_from(start);
                                    auto* pe = m_ctx.make<ast::PathExpr>(full_path.range, std::move(full_path), m_ctx.allocator());
                                    pe->explicit_enum_args = std::move(enum_args);
                                    return pe;
                                }
                            }
                        }
                    }

                    auto path = parse_path();
                    auto path_range = path.range;

                    if (!no_struct_lit && check(TK::LBrace))
                    {
                        auto* nt = m_ctx.make<ast::NamedType>(path_range, std::move(path), m_ctx.allocator());
                        return parse_brace_expr(nt, start);
                    }

                    if (path.segments.size() == 1)
                    {
                        auto seg = path.segments[0];
                        return m_ctx.make<ast::IdentExpr>(path_range, seg.name);
                    }

                    return m_ctx.make<ast::PathExpr>(path_range, std::move(path), m_ctx.allocator());
                }

                default:
                    if (ast::is_primitive_type(peek().kind))
                    {
                        auto* type_node = parse_type();
                        if (!type_node)
                            return nullptr;

                        return m_ctx.make<ast::TypeASTExpr>(range_from(start), type_node);
                    }

                    {
                        auto const& tok = peek();
                        auto msg = std::format("expected expression, found '{}'", lex::to_string(tok.kind));
                        auto d = diag::Diagnostic(diag::Severity::Error, std::move(msg)).primary(tok.range);
                        if (!tok.valid())
                            if (auto em = tok.error_message(); !em.empty())
                                std::move(d).note(std::string{em});

                        emit(std::move(d));
                        m_last_error_range = tok.range;
                    }

                    advance();
                    return nullptr;
            }
        }

        ast::Expr* parse_brace_expr(ast::TypeExpr* type_prefix, sm::Location start)
        {
            expect(TK::LBrace, "to begin block or struct literal");

            if (match(TK::RBrace))
            {
                if (type_prefix)
                {
                    auto* sl = m_ctx.make<ast::StructLiteralExpr>(range_from(start));
                    sl->type = type_prefix;
                    return sl;
                }

                ast::Block b(m_ctx.allocator());
                b.range = range_from(start);
                return m_ctx.make<ast::BlockExpr>(range_from(start), std::move(b));
            }

            if (type_prefix)
                return parse_struct_literal_after_brace(type_prefix, start);

            if (check(TK::Identifier) && check_at(1, TK::Comma))
                return parse_struct_literal_after_brace(nullptr, start);

            if (check(TK::Identifier) && check_at(1, TK::Eq))
            {
                bool is_struct_literal = false;
                {
                    Speculation spec(*this);
                    auto* probe = parse_expr();
                    if (probe && !spec.had_suppressed_errors() && (check(TK::Comma) || check(TK::RBrace)))
                        is_struct_literal = true;
                }

                if (is_struct_literal)
                    return parse_struct_literal_after_brace(nullptr, start);
            }

            if (looks_like_decl())
            {
                ast::Block block(m_ctx.allocator());
                block.range.begin = start;
                while (!check(TK::RBrace) && !eof())
                {
                    if (auto* s = try_parse_decl_or_expr_stmt())
                    {
                        block.stmts.push_back(s);
                        continue;
                    }

                    auto stmt_start = loc();
                    auto* e = parse_expr();
                    if (!e)
                    {
                        synchronize_to_stmt();
                        continue;
                    }
                    if (match(TK::Semicolon))
                        block.stmts.push_back(m_ctx.make<ast::ExprStmt>(range_from(stmt_start), e));
                    else if (check(TK::RBrace))
                    {
                        block.tail = e;
                        break;
                    }
                    else if (is_block_like_expr(e))
                        block.stmts.push_back(m_ctx.make<ast::ExprStmt>(range_from(stmt_start), e));
                    else
                    {
                        error_at(single_range(), "expected ';' or '}' after expression");
                        synchronize_to_stmt();
                    }
                }

                expect(TK::RBrace, "to close block");
                block.range = range_from(start);
                return m_ctx.make<ast::BlockExpr>(range_from(start), std::move(block));
            }

            auto first_start = loc();
            auto* first = parse_expr();
            if (!first)
            {
                expect(TK::RBrace, "to close brace expression");
                ast::Block b(m_ctx.allocator());
                b.range = range_from(start);
                return m_ctx.make<ast::BlockExpr>(range_from(start), std::move(b));
            }

            if (check(TK::Comma))
            {
                auto* sl = m_ctx.make<ast::StructLiteralExpr>(sm::SourceRange{});
                sl->type = nullptr;
                ast::StructLiteralField f;
                f.range = first->range;
                f.value = first;
                sl->fields.push_back(std::move(f));

                while (match(TK::Comma))
                {
                    if (check(TK::RBrace))
                        break;

                    auto fstart = loc();
                    ast::StructLiteralField nf;
                    if (check(TK::Identifier) && check_at(1, TK::Eq))
                    {
                        auto name = advance();
                        nf.name = name.interned;
                        nf.name_range = name.range;
                        advance();
                        nf.value = parse_expr();
                    }
                    else if (check(TK::Identifier) && (check_at(1, TK::Comma) || check_at(1, TK::RBrace)))
                    {
                        auto name = advance();
                        nf.name = name.interned;
                        nf.name_range = name.range;
                        nf.value = m_ctx.make<ast::IdentExpr>(name.range, name.interned);
                    }
                    else
                        nf.value = parse_expr();

                    nf.range = range_from(fstart);
                    sl->fields.push_back(std::move(nf));
                }
                expect(TK::RBrace, "to close struct literal");
                sl->range = range_from(start);
                return sl;
            }

            if (check(TK::RBrace))
            {
                advance();
                ast::Block b(m_ctx.allocator());
                b.tail = first;
                b.range = range_from(start);
                return m_ctx.make<ast::BlockExpr>(range_from(start), std::move(b));
            }

            if (match(TK::Semicolon))
            {
                ast::Block block(m_ctx.allocator());
                block.range.begin = start;
                block.stmts.push_back(m_ctx.make<ast::ExprStmt>(range_from(first_start), first));
                while (!check(TK::RBrace) && !eof())
                {
                    if (auto* s = try_parse_decl_or_expr_stmt())
                    {
                        block.stmts.push_back(s);
                        continue;
                    }

                    auto sstart = loc();
                    auto* e = parse_expr();
                    if (!e)
                    {
                        synchronize_to_stmt();
                        continue;
                    }

                    if (match(TK::Semicolon))
                        block.stmts.push_back(m_ctx.make<ast::ExprStmt>(range_from(sstart), e));
                    else if (check(TK::RBrace))
                    {
                        block.tail = e;
                        break;
                    }
                    else if (is_block_like_expr(e))
                        block.stmts.push_back(m_ctx.make<ast::ExprStmt>(range_from(sstart), e));
                    else
                    {
                        error_at(single_range(), "expected ';' or '}' after expression");
                        synchronize_to_stmt();
                    }
                }

                expect(TK::RBrace, "to close block");
                block.range = range_from(start);
                return m_ctx.make<ast::BlockExpr>(range_from(start), std::move(block));
            }

            error_at(single_range(), "expected ',', ';', or '}' inside braces");

            expect(TK::RBrace, "to close block");
            ast::Block b(m_ctx.allocator());
            b.tail = first;
            b.range = range_from(start);
            return m_ctx.make<ast::BlockExpr>(range_from(start), std::move(b));
        }

        ast::Expr* parse_struct_literal_after_brace(ast::TypeExpr* type_prefix, sm::Location start)
        {
            auto* sl = m_ctx.make<ast::StructLiteralExpr>(sm::SourceRange{});
            sl->type = type_prefix;

            if (match(TK::RBrace))
            {
                sl->range = range_from(start);
                return sl;
            }

            do
            {
                auto fstart = loc();
                ast::StructLiteralField f;

                if (check(TK::Identifier) && check_at(1, TK::Eq))
                {
                    auto name = advance();
                    f.name = name.interned;
                    f.name_range = name.range;
                    advance();
                    f.value = parse_expr();
                }
                else if (check(TK::Identifier) && (check_at(1, TK::Comma) || check_at(1, TK::RBrace)))
                {
                    auto name = advance();
                    f.name = name.interned;
                    f.name_range = name.range;
                    f.value = m_ctx.make<ast::IdentExpr>(name.range, name.interned);
                }
                else
                    f.value = parse_expr();

                f.range = range_from(fstart);
                sl->fields.push_back(std::move(f));
            } while (match(TK::Comma) && !check(TK::RBrace));

            expect(TK::RBrace, "to close struct literal");
            sl->range = range_from(start);
            return sl;
        }

        bool looks_like_decl()
        {
            if (!ast::is_type_start(peek().kind))
                return false;

            Speculation spec(*this);
            auto* type = parse_type();
            if (!type)
                return false;

            if (spec.had_suppressed_errors())
                return false;

            if (peek().kind != TK::Identifier)
                return false;

            if (peek(1).kind != TK::Eq && peek(1).kind != TK::Semicolon)
                return false;

            return true;
        }

        ast::Expr* parse_if_expr()
        {
            auto start = loc();
            advance();
            auto* cond = parse_expr(0, true);
            auto then_block = parse_block();
            auto* e = m_ctx.make<ast::IfExpr>(range_from(start), cond, std::move(then_block));

            if (match(TK::KwElse))
            {
                if (check(TK::KwIf))
                    e->else_branch = parse_if_expr();
                else
                {
                    auto bstart = loc();
                    auto block = parse_block();
                    e->else_branch = m_ctx.make<ast::BlockExpr>(range_from(bstart), std::move(block));
                }
            }
            e->range = range_from(start);
            return e;
        }

        ast::Expr* parse_match_expr()
        {
            auto start = loc();
            advance();
            auto* operand = parse_expr(0, true);
            auto* m = m_ctx.make<ast::MatchExpr>(range_from(start), operand);
            expect(TK::LBrace, "to begin match body");

            while (!check(TK::RBrace) && !eof())
            {
                m->arms.push_back(parse_match_arm());
                if (!match(TK::Comma))
                    break;
            }

            expect(TK::RBrace, "to close match body");
            m->range = range_from(start);
            return m;
        }

        ast::MatchArm parse_match_arm()
        {
            auto start = loc();
            ast::MatchArm arm;
            arm.pattern = parse_pattern();

            if (match(TK::KwIf))
                arm.guard = parse_expr(0, true);

            expect(TK::FatArrow, "in match arm");
            arm.body = parse_expr();
            arm.range = range_from(start);
            return arm;
        }

        ast::Expr* parse_sizeof_expr()
        {
            auto start = loc();
            advance();

            if (match(TK::Ellipsis))
            {
                expect(TK::LParen, "after sizeof...");
                auto name = expect(TK::Identifier, "in sizeof... pack name");
                expect(TK::RParen, "after sizeof... pack name");
                if (name.kind == TK::Identifier)
                    return m_ctx.make<ast::SizeofPackExpr>(range_from(start), name.interned, name.range);

                return m_ctx.make<ast::SizeofExpr>(range_from(start), nullptr);
            }

            expect(TK::LParen, "after 'sizeof'");
            auto* type = parse_type();
            expect(TK::RParen, "after sizeof type");
            return m_ctx.make<ast::SizeofExpr>(range_from(start), type);
        }

        ast::Expr* parse_alignof_expr()
        {
            auto start = loc();
            advance();
            expect(TK::LParen, "after 'alignof'");
            auto* type = parse_type();
            expect(TK::RParen, "after alignof type");
            return m_ctx.make<ast::AlignofExpr>(range_from(start), type);
        }

        ast::Expr* parse_offsetof_expr()
        {
            auto start = loc();
            advance();
            expect(TK::LParen, "after 'offsetof'");
            auto* type = parse_type();
            expect(TK::Comma, "between offsetof type and field");
            auto field = expect(TK::Identifier, "as offsetof field");
            expect(TK::RParen, "after offsetof field");
            std::string_view name = (field.kind == TK::Identifier) ? field.interned : std::string_view{};
            return m_ctx.make<ast::OffsetofExpr>(range_from(start), type, name);
        }

        ast::Expr* parse_compiles_expr()
        {
            auto start = loc();
            advance();

            auto* e = m_ctx.make<ast::CompilesExpr>(sm::SourceRange{}, m_ctx.allocator());

            if (check(TK::LParen))
            {
                advance();
                if (!check(TK::RParen))
                {
                    do
                    {
                        auto pstart = loc();
                        ast::CompilesParam param;
                        param.type = parse_type();
                        auto name = expect(TK::Identifier, "in compiles parameter");
                        if (name.kind == TK::Identifier)
                        {
                            param.name = name.interned;
                            param.range = range_from(pstart);
                            if (match(TK::Ellipsis))
                                param.is_pack = true;
                        }

                        e->params.push_back(std::move(param));
                    } while (match(TK::Comma));
                }
                expect(TK::RParen, "to close compiles parameters");

                bool seen_pack = false;
                for (auto const& p : e->params)
                {
                    if (seen_pack && !p.is_pack)
                        error_at(p.range, "non-pack compiles parameter after pack parameter");

                    if (p.is_pack) // TODO losen.
                        seen_pack = true;
                }

                e->body = parse_block();
            }
            else if (check(TK::LBrace))
                e->body = parse_block();
            else
                error_at(single_range(), "expected '(' or '{' after 'compiles'");

            e->range = range_from(start);
            return e;
        }

        ast::Pattern* parse_pattern()
        {
            auto* first = parse_single_pattern();
            if (!check(TK::Pipe))
                return first;

            auto start = first->range.begin;
            auto* o = m_ctx.make<ast::OrPattern>(sm::SourceRange{}, m_ctx.allocator());
            o->alternatives.push_back(first);
            while (match(TK::Pipe))
                o->alternatives.push_back(parse_single_pattern());

            o->range = range_from(start);
            return o;
        }

        ast::Pattern* parse_single_pattern()
        {
            auto start = loc();

            if (check(TK::Amp))
            {
                advance();
                auto* inner = parse_single_pattern();
                if (!inner)
                    return m_ctx.make<ast::WildcardPattern>(range_from(start));

                if (inner->kind == ast::PatternKind::Binding)
                {
                    auto* bp = static_cast<ast::BindingPattern*>(inner);
                    bp->by_reference = true;
                    bp->range.begin = start;
                    return bp;
                }

                return m_ctx.make<ast::RefPattern>(range_from(start), inner);
            }

            switch (peek().kind)
            {
                case TK::Identifier: {
                    if (check_at(1, TK::ColonColon) || check_at(1, TK::LBrace) || check_at(1, TK::LParen))
                    {
                        auto path = parse_path();
                        if (match(TK::LParen))
                        {
                            auto* p = m_ctx.make<ast::EnumDestructurePattern>(sm::SourceRange{}, std::move(path), m_ctx.allocator());
                            p->has_parens = true;
                            if (!check(TK::RParen))
                                do
                                    p->payload.push_back(parse_pattern());
                                while (match(TK::Comma));

                            expect(TK::RParen, "to close enum payload");
                            p->range = range_from(start);
                            return p;
                        }
                        if (check(TK::LBrace))
                            return parse_struct_destructure_pattern(std::move(path), start);

                        return m_ctx.make<ast::EnumDestructurePattern>(range_from(start), std::move(path), m_ctx.allocator());
                    }
                    {
                        auto tok = advance();
                        if (tok.interned == "_")
                            return m_ctx.make<ast::WildcardPattern>(tok.range);

                        return m_ctx.make<ast::BindingPattern>(tok.range, tok.interned);
                    }
                }
                case TK::IntLiteral:
                case TK::FloatLiteral:
                case TK::StringLiteral:
                case TK::U16StringLiteral:
                case TK::CharLiteral:
                case TK::U16CharLiteral:
                case TK::KwTrue:
                case TK::KwFalse:
                case TK::KwNull:
                case TK::Minus: {
                    auto* lit = parse_unary(true);
                    if (check(TK::DotDot))
                    {
                        advance();
                        bool inclusive = match(TK::Eq);
                        auto* end = parse_unary(true);
                        return m_ctx.make<ast::RangePattern>(range_from(start), lit, end, inclusive);
                    }

                    return m_ctx.make<ast::LiteralPattern>(range_from(start), lit);
                }
                default:
                    error_at(single_range(), std::format("expected pattern, found '{}'", lex::to_string(peek().kind)));
                    advance();
                    return nullptr;
            }
        }

        ast::Pattern* parse_struct_destructure_pattern(ast::Path path, sm::Location start)
        {
            advance();
            auto* p = m_ctx.make<ast::StructDestructurePattern>(sm::SourceRange{}, std::move(path), m_ctx.allocator());

            if (match(TK::RBrace))
            {
                p->range = range_from(start);
                return p;
            }

            do
            {
                if (check(TK::Identifier) && peek().interned == "_")
                {
                    advance();
                    p->has_rest = true;
                    continue;
                }

                auto fstart = loc();
                bool field_ref = match(TK::Amp);
                auto name = expect(TK::Identifier, "in struct pattern");
                if (name.kind != TK::Identifier)
                    break;

                ast::StructPatternField f;
                f.field_name = name.interned;
                f.range = range_from(fstart);
                if (match(TK::FatArrow) || match(TK::Eq))
                    f.pattern = parse_pattern();
                else
                {
                    auto* bp = m_ctx.make<ast::BindingPattern>(name.range, name.interned);
                    bp->by_reference = field_ref;
                    f.pattern = bp;
                }
                if (field_ref && (match(TK::FatArrow) || match(TK::Eq)))
                    ;

                p->fields.push_back(f);
            } while (match(TK::Comma) && !check(TK::RBrace));

            expect(TK::RBrace, "to close struct pattern");
            p->range = range_from(start);
            return p;
        }

        lex::Lexer& m_lexer;
        ast::AstContext& m_ctx;
        diag::DiagnosticEngine& m_diag;

        std::pmr::vector<lex::Token> m_tokens;
        std::size_t m_pos{};
        sm::Location m_prev_end{};
        std::uint32_t m_silent_depth{};
        std::uint32_t m_suppressed_error_count{};
        std::optional<sm::SourceRange> m_last_error_range;
        ParseMode m_mode{ParseMode::Batch};
        std::vector<ast::Decl*> m_extra_imports;
    };

} // namespace dcc::parser
