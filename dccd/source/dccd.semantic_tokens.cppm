export module dccd.semantic_tokens;

import std;
import dcc.sm;
import dcc.ast;
import dcc.sema;
import dcc.sema.type_helpers;
import dcc.ast.visitor;

export namespace dccd::semantic_tokens
{
    inline constexpr int kMaxSkippedLogs = 5;

    enum class TokenType : std::uint32_t
    {
        Namespace,
        Type,
        Class,
        Enum,
        Interface,
        Struct,
        TypeParameter,
        Parameter,
        Variable,
        Property,
        EnumMember,
        Function,
        Method,
        Macro,
        Keyword,
        Modifier,
        Comment,
        String,
        Number,
        Operator,
    };

    enum class TokenModifier : std::uint32_t
    {
        Declaration,
        Readonly,
        Static,
        Deprecated,
        DefaultLibrary,
    };

    constexpr std::array token_types = {
        "namespace",  "type",     "class",  "enum",  "interface", "struct",   "typeParameter", "parameter", "variable", "property",
        "enumMember", "function", "method", "macro", "keyword",   "modifier", "comment",       "string",    "number",   "operator",
    };

    constexpr std::array token_modifiers = {
        "declaration", "readonly", "static", "deprecated", "defaultLibrary",
    };

    struct RawToken
    {
        std::uint32_t line{};
        std::uint32_t character{};
        std::uint32_t length{};
        std::uint32_t type{};
        std::uint32_t modifiers{};

        [[nodiscard]] auto operator<=>(RawToken const&) const = default;
    };

    [[nodiscard]] std::vector<std::uint32_t> delta_encode(std::vector<RawToken> tokens);
    [[nodiscard]] std::vector<std::uint32_t> collect_tokens(dcc::sm::SourceManager const& sm, dcc::ast::TranslationUnit const* tu,
                                                            dcc::sm::FileId requested_file = dcc::sm::FileId::Invalid);

} // namespace dccd::semantic_tokens

module :private;

namespace dccd::semantic_tokens
{
    namespace
    {
        struct Collector : dcc::ast::RecursiveAstVisitor
        {
            dcc::sm::SourceManager const& sm;
            std::vector<RawToken> tokens;
            dcc::sm::FileId requested_file;
            int skipped_log_count{0};

            explicit Collector(dcc::sm::SourceManager const& s, dcc::sm::FileId req_file = dcc::sm::FileId::Invalid) : sm{s}, requested_file{req_file} {}

            void emit(dcc::sm::SourceRange range, TokenType type, std::uint32_t modifiers = 0)
            {
                if (!range.valid())
                    return;

                if (range.byte_length() == 0)
                    return;

                if (requested_file != dcc::sm::FileId::Invalid)
                    if (range.begin.fileId != requested_file || range.end.fileId != requested_file)
                        return;

                auto const* sf = sm.get(range.begin.fileId);
                if (!sf)
                {
                    log_skipped("source file not found", range);
                    return;
                }

                auto const file_size = static_cast<dcc::sm::Offset>(sf->size());
                if (range.begin.offset > file_size || range.end.offset > file_size)
                {
                    log_skipped("offset exceeds file size", range);
                    return;
                }

                if (range.begin.offset > range.end.offset)
                {
                    log_skipped("begin offset > end offset", range);
                    return;
                }

                auto start_pos = sm.location_to_lsp_position(range.begin);
                if (!start_pos)
                {
                    log_skipped("location_to_lsp_position failed for begin", range);
                    return;
                }

                auto end_pos = sm.location_to_lsp_position(range.end);
                if (!end_pos)
                {
                    log_skipped("location_to_lsp_position failed for end", range);
                    return;
                }

                if (start_pos->line != end_pos->line)
                    return;

                if (end_pos->character <= start_pos->character)
                    return;

                RawToken t;
                t.line = start_pos->line;
                t.character = start_pos->character;
                t.length = end_pos->character - start_pos->character;
                t.type = static_cast<std::uint32_t>(type);
                t.modifiers = modifiers;
                tokens.push_back(t);
            }

            void log_skipped(std::string_view reason, dcc::sm::SourceRange const& range)
            {
                if (skipped_log_count < kMaxSkippedLogs)
                {
                    std::cerr << "[dccd.semantic_tokens] skipping token: " << reason << " (fileId=" << static_cast<std::uint32_t>(range.begin.fileId)
                              << ", offset=" << range.begin.offset << "-" << range.end.offset << ")" << std::endl;
                    ++skipped_log_count;
                    if (skipped_log_count >= kMaxSkippedLogs)
                        std::cerr << "[dccd.semantic_tokens] (further skipped tokens suppressed)" << std::endl;
                }
            }

            void log_expr_emit(dcc::sm::SourceRange range, TokenType type) const
            {
                if (type != TokenType::Function && type != TokenType::Method)
                    return;

                auto text_opt = sm.text(range);
                std::cerr << "[dccd.semantic_tokens] emitting " << (type == TokenType::Function ? "function" : "method") << " token: '"
                          << (text_opt ? *text_opt : std::string_view{}) << "'" << std::endl;
            }

            void emit_decl_name(dcc::sm::SourceRange range, TokenType type) { emit(range, type, 1u << static_cast<std::uint32_t>(TokenModifier::Declaration)); }

            [[nodiscard]] dcc::sm::SourceRange find_name_in_range(dcc::sm::SourceRange range, std::string_view name) const
            {
                if (!range.valid() || name.empty())
                    return {};

                auto search_end = range.begin.offset + std::min(range.byte_length(), dcc::sm::Offset{512});
                auto search_range = dcc::sm::SourceRange{range.begin, dcc::sm::Location{range.begin.fileId, search_end}};

                auto text_opt = sm.text(search_range);
                if (!text_opt)
                    return {};

                auto text = *text_opt;
                auto pos = text.find(name);
                if (pos == std::string_view::npos)
                    return {};

                if (pos > 0)
                {
                    char prev = text[pos - 1];
                    if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')
                        return {};
                }

                if (pos + name.size() < text.size())
                {
                    char next = text[pos + name.size()];
                    if (std::isalnum(static_cast<unsigned char>(next)) || next == '_')
                        return {};
                }

                dcc::sm::Offset name_start = range.begin.offset + static_cast<dcc::sm::Offset>(pos);
                dcc::sm::Offset name_end = name_start + static_cast<dcc::sm::Offset>(name.size());

                return dcc::sm::SourceRange{dcc::sm::Location{range.begin.fileId, name_start}, dcc::sm::Location{range.begin.fileId, name_end}};
            }

            TokenType classify_expr_name(dcc::ast::Expr const* expr) const
            {
                if (!expr)
                    return TokenType::Variable;

                if (expr->sema.ufcs_callee)
                    return TokenType::Method;

                if (expr->sema.resolved_specialization)
                    return TokenType::Function;

                if (auto* decl = expr->sema.resolved_decl)
                {
                    switch (decl->kind)
                    {
                        case dcc::ast::DeclKind::Func:
                            return TokenType::Function;
                        case dcc::ast::DeclKind::Var:
                            if (decl->sema.storage == dcc::ast::StorageClass::Param)
                                return TokenType::Parameter;
                            return TokenType::Variable;
                        case dcc::ast::DeclKind::Struct:
                            return TokenType::Struct;
                        case dcc::ast::DeclKind::Union:
                            return TokenType::Struct;
                        case dcc::ast::DeclKind::Enum:
                            return TokenType::Enum;
                        case dcc::ast::DeclKind::Using:
                        case dcc::ast::DeclKind::Module:
                        case dcc::ast::DeclKind::Import:
                            return TokenType::Namespace;
                        case dcc::ast::DeclKind::StaticIfGroup:
                            return TokenType::Variable;
                    }
                }

                return TokenType::Variable;
            }

            TokenType classify_type_name(dcc::ast::TypeExpr const* type_expr) const
            {
                if (!type_expr)
                    return TokenType::Type;

                if (auto* decl = type_expr->sema.resolved_decl)
                {
                    switch (decl->kind)
                    {
                        case dcc::ast::DeclKind::Struct:
                            return TokenType::Struct;
                        case dcc::ast::DeclKind::Union:
                            return TokenType::Struct;
                        case dcc::ast::DeclKind::Enum:
                            return TokenType::Enum;
                        case dcc::ast::DeclKind::Using:
                            return TokenType::Type;
                        case dcc::ast::DeclKind::Module:
                        case dcc::ast::DeclKind::Import:
                            return TokenType::Namespace;
                        default:
                            break;
                    }
                }

                return TokenType::Type;
            }

            void visitDecl(dcc::ast::Decl const* decl) override;
            void visitStmt(dcc::ast::Stmt const* stmt) override;
            void visitExpr(dcc::ast::Expr const* expr) override;
            void visitTypeExpr(dcc::ast::TypeExpr const* type_expr) override;
            void visitPattern(dcc::ast::Pattern const* pat) override;
            void visitMatchArm(dcc::ast::MatchArm const& arm) override;
            void visitTemplateArgs(std::pmr::vector<dcc::ast::TemplateArg> const& args) override;
            void visitTemplateParams(std::pmr::vector<dcc::ast::TemplateParam> const& params) override;
            void visitAttrs(std::pmr::vector<dcc::ast::Attribute> const& attrs) override;

            void emit_using_item(dcc::ast::UsingItem const* item);
        };

        void Collector::visitAttrs(std::pmr::vector<dcc::ast::Attribute> const& attrs)
        {
            for (auto const& a : attrs)
                for (auto* arg : a.args)
                    if (arg)
                        visitExpr(arg);
        }

        void Collector::visitTemplateParams(std::pmr::vector<dcc::ast::TemplateParam> const& params)
        {
            for (auto const& tp : params)
            {
                auto name_range = find_name_in_range(tp.range, tp.name);
                if (name_range.valid())
                    emit_decl_name(name_range, TokenType::TypeParameter);
                if (tp.value_type)
                    visitTypeExpr(tp.value_type);
            }
        }

        void Collector::visitTemplateArgs(std::pmr::vector<dcc::ast::TemplateArg> const& args)
        {
            for (auto const& a : args)
            {
                if (a.type)
                    visitTypeExpr(a.type);

                if (a.expr)
                    visitExpr(a.expr);
            }
        }

        void Collector::visitMatchArm(dcc::ast::MatchArm const& arm)
        {
            if (arm.pattern)
                visitPattern(arm.pattern);
            if (arm.type_pattern)
                visitTypeExpr(arm.type_pattern);
            if (arm.guard)
                visitExpr(arm.guard);
            if (arm.body)
                visitExpr(arm.body);
        }

        void Collector::visitPattern(dcc::ast::Pattern const* pat)
        {
            if (!pat)
                return;

            switch (pat->kind)
            {
                case dcc::ast::PatternKind::Binding: {
                    auto* p = static_cast<dcc::ast::BindingPattern const*>(pat);
                    auto name_range = find_name_in_range(p->range, p->name);
                    if (name_range.valid())
                        emit(name_range, TokenType::Parameter);
                    break;
                }
                case dcc::ast::PatternKind::EnumDestructure: {
                    auto* p = static_cast<dcc::ast::EnumDestructurePattern const*>(pat);
                    for (auto const& seg : p->variant_path.segments)
                        emit(seg.range, TokenType::Namespace);
                    for (auto* sub : p->payload)
                        if (sub)
                            visitPattern(sub);
                    break;
                }
                case dcc::ast::PatternKind::StructDestructure: {
                    auto* p = static_cast<dcc::ast::StructDestructurePattern const*>(pat);
                    for (auto const& seg : p->type_path.segments)
                        emit(seg.range, TokenType::Namespace);
                    for (auto const& f : p->fields)
                        if (f.pattern)
                            visitPattern(f.pattern);
                    break;
                }
                default:
                    dcc::ast::RecursiveAstVisitor::visitPattern(pat);
                    break;
            }
        }

        void Collector::visitTypeExpr(dcc::ast::TypeExpr const* type_expr)
        {
            if (!type_expr)
                return;

            if (type_expr->kind == dcc::ast::TypeKind::Named)
            {
                auto* t = static_cast<dcc::ast::NamedType const*>(type_expr);
                auto const& segs = t->path.segments;
                for (std::size_t i = 0; i < segs.size(); ++i)
                {
                    if (i == segs.size() - 1)
                    {
                        auto tt = classify_type_name(type_expr);
                        emit(segs[i].range, tt);
                    }
                    else
                    {
                        emit(segs[i].range, TokenType::Namespace);
                    }
                }
                visitTemplateArgs(t->template_args);
            }
            else
            {
                dcc::ast::RecursiveAstVisitor::visitTypeExpr(type_expr);
            }
        }

        void Collector::visitExpr(dcc::ast::Expr const* expr)
        {
            if (!expr)
                return;

            switch (expr->kind)
            {
                case dcc::ast::ExprKind::IntLiteral: {
                    emit(expr->range, TokenType::Number);
                    break;
                }
                case dcc::ast::ExprKind::FloatLiteral: {
                    emit(expr->range, TokenType::Number);
                    break;
                }
                case dcc::ast::ExprKind::StringLiteral: {
                    emit(expr->range, TokenType::String);
                    break;
                }
                case dcc::ast::ExprKind::U16StringLiteral: {
                    emit(expr->range, TokenType::String);
                    break;
                }
                case dcc::ast::ExprKind::CharLiteral: {
                    emit(expr->range, TokenType::String);
                    break;
                }
                case dcc::ast::ExprKind::U16CharLiteral: {
                    emit(expr->range, TokenType::String);
                    break;
                }
                case dcc::ast::ExprKind::BoolLiteral:
                case dcc::ast::ExprKind::NullLiteral:
                    break;

                case dcc::ast::ExprKind::Ident: {
                    auto* e = static_cast<dcc::ast::IdentExpr const*>(expr);
                    auto tt = classify_expr_name(expr);
                    emit(e->range, tt);
                    log_expr_emit(e->range, tt);
                    break;
                }
                case dcc::ast::ExprKind::PathExpr: {
                    auto* e = static_cast<dcc::ast::PathExpr const*>(expr);
                    auto const& segs = e->path.segments;
                    for (std::size_t i = 0; i < segs.size(); ++i)
                    {
                        if (i == segs.size() - 1)
                        {
                            auto tt = classify_expr_name(expr);
                            emit(segs[i].range, tt);
                            log_expr_emit(segs[i].range, tt);
                        }
                        else
                            emit(segs[i].range, TokenType::Namespace);
                    }
                    visitTemplateArgs(e->explicit_enum_args);
                    break;
                }
                case dcc::ast::ExprKind::Call: {
                    auto* e = static_cast<dcc::ast::CallExpr const*>(expr);

                    if (e->sema.ufcs_callee || (e->callee && e->callee->kind == dcc::ast::ExprKind::FieldAccess))
                    {
                        if (e->callee)
                            visitExpr(e->callee);
                    }
                    else if (e->callee)
                    {
                        dcc::ast::Decl const* resolved =
                            e->sema.resolved_specialization ? static_cast<dcc::ast::Decl const*>(e->sema.resolved_specialization) : e->sema.resolved_decl;

                        if (resolved && resolved->kind == dcc::ast::DeclKind::Func)
                        {
                            switch (e->callee->kind)
                            {
                                case dcc::ast::ExprKind::Ident: {
                                    auto* ident = static_cast<dcc::ast::IdentExpr const*>(e->callee);
                                    {
                                        auto text_opt = sm.text(ident->range);
                                        std::cerr << "[dccd.semantic_tokens] emitting function token from CallExpr: "
                                                  << (text_opt ? *text_opt : std::string_view{}) << std::endl;
                                    }
                                    emit(ident->range, TokenType::Function);
                                    break;
                                }
                                case dcc::ast::ExprKind::PathExpr: {
                                    auto* path_expr = static_cast<dcc::ast::PathExpr const*>(e->callee);
                                    auto const& segs = path_expr->path.segments;
                                    for (std::size_t i = 0; i < segs.size(); ++i)
                                    {
                                        if (i == segs.size() - 1)
                                        {
                                            {
                                                auto text_opt = sm.text(segs[i].range);
                                                std::cerr << "[dccd.semantic_tokens] emitting function token from CallExpr: "
                                                          << (text_opt ? *text_opt : std::string_view{}) << std::endl;
                                            }
                                            emit(segs[i].range, TokenType::Function);
                                        }
                                        else
                                            emit(segs[i].range, TokenType::Namespace);
                                    }
                                    visitTemplateArgs(path_expr->explicit_enum_args);
                                    break;
                                }
                                case dcc::ast::ExprKind::TemplateInst: {
                                    auto* ti = static_cast<dcc::ast::TemplateInstExpr const*>(e->callee);
                                    if (ti->callee)
                                    {
                                        if (ti->callee->kind == dcc::ast::ExprKind::Ident)
                                        {
                                            auto* inner_ident = static_cast<dcc::ast::IdentExpr const*>(ti->callee);
                                            {
                                                auto text_opt = sm.text(inner_ident->range);
                                                std::cerr << "[dccd.semantic_tokens] emitting function token from CallExpr: "
                                                          << (text_opt ? *text_opt : std::string_view{}) << std::endl;
                                            }

                                            emit(inner_ident->range, TokenType::Function);
                                        }
                                        else if (ti->callee->kind == dcc::ast::ExprKind::PathExpr)
                                        {
                                            auto* inner_path = static_cast<dcc::ast::PathExpr const*>(ti->callee);
                                            auto const& segs = inner_path->path.segments;
                                            for (std::size_t i = 0; i < segs.size(); ++i)
                                            {
                                                if (i == segs.size() - 1)
                                                {
                                                    {
                                                        auto text_opt = sm.text(segs[i].range);
                                                        std::cerr << "[dccd.semantic_tokens] emitting function token from CallExpr: "
                                                                  << (text_opt ? *text_opt : std::string_view{}) << std::endl;
                                                    }

                                                    emit(segs[i].range, TokenType::Function);
                                                }
                                                else
                                                    emit(segs[i].range, TokenType::Namespace);
                                            }
                                            visitTemplateArgs(inner_path->explicit_enum_args);
                                        }
                                        else
                                            visitExpr(ti->callee);
                                    }
                                    visitTemplateArgs(ti->template_args);
                                    break;
                                }
                                default:
                                    visitExpr(e->callee);
                                    break;
                            }
                        }
                        else
                            visitExpr(e->callee);
                    }

                    for (auto* a : e->args)
                        if (a)
                            visitExpr(a);

                    break;
                }
                case dcc::ast::ExprKind::FieldAccess: {
                    auto* e = static_cast<dcc::ast::FieldAccessExpr const*>(expr);
                    if (e->object)
                        visitExpr(e->object);

                    TokenType tt = TokenType::Property;
                    if (e->sema.ufcs_callee)
                        tt = TokenType::Method;
                    else if (auto* decl = e->sema.resolved_decl)
                        if (decl->kind == dcc::ast::DeclKind::Func)
                            tt = TokenType::Function;

                    emit(e->field_range, tt);
                    log_expr_emit(e->field_range, tt);
                    break;
                }
                case dcc::ast::ExprKind::StructLiteral: {
                    auto* e = static_cast<dcc::ast::StructLiteralExpr const*>(expr);
                    if (e->type)
                        visitTypeExpr(e->type);
                    for (auto const& f : e->fields)
                    {
                        emit(f.name_range, TokenType::Property);
                        if (f.value)
                            visitExpr(f.value);
                    }
                    break;
                }
                case dcc::ast::ExprKind::Compiles: {
                    auto* e = static_cast<dcc::ast::CompilesExpr const*>(expr);
                    for (auto const& p : e->params)
                    {
                        auto name_range = find_name_in_range(p.range, p.name);
                        if (name_range.valid())
                            emit(name_range, TokenType::Parameter);
                        if (p.type)
                            visitTypeExpr(p.type);
                    }
                    visitBlock(e->body);
                    break;
                }
                default:
                    dcc::ast::RecursiveAstVisitor::visitExpr(expr);
                    break;
            }
        }

        void Collector::visitStmt(dcc::ast::Stmt const* stmt)
        {
            if (!stmt)
                return;

            if (stmt->kind == dcc::ast::StmtKind::ForIn)
            {
                auto* s = static_cast<dcc::ast::ForInStmt const*>(stmt);
                if (s->item_type)
                    visitTypeExpr(s->item_type);
                emit(s->name_range, TokenType::Parameter);
                if (s->iterable)
                    visitExpr(s->iterable);
                visitBlock(s->body);
            }
            else
            {
                dcc::ast::RecursiveAstVisitor::visitStmt(stmt);
            }
        }

        void Collector::visitDecl(dcc::ast::Decl const* decl)
        {
            if (!decl)
                return;

            visitAttrs(decl->attrs);

            switch (decl->kind)
            {
                case dcc::ast::DeclKind::Module: {
                    auto* d = static_cast<dcc::ast::ModuleDecl const*>(decl);
                    for (auto const& seg : d->module_path.segments)
                        emit(seg.range, TokenType::Namespace);
                    break;
                }
                case dcc::ast::DeclKind::Import: {
                    auto* d = static_cast<dcc::ast::ImportDecl const*>(decl);
                    for (auto const& seg : d->module_path.segments)
                        emit(seg.range, TokenType::Namespace);
                    break;
                }
                case dcc::ast::DeclKind::Using: {
                    auto* d = static_cast<dcc::ast::UsingDecl const*>(decl);
                    visitTemplateParams(d->template_params);
                    for (auto const& seg : d->alias_path.segments)
                        emit_decl_name(seg.range, TokenType::Type);
                    for (auto const& seg : d->target_path.segments)
                        emit(seg.range, TokenType::Namespace);
                    if (d->target_type)
                        visitTypeExpr(d->target_type);
                    if (d->target_expr)
                        visitExpr(d->target_expr);
                    for (auto const* item : d->target_items)
                        emit_using_item(item);
                    for (auto const& p : d->target_list)
                        for (auto const& seg : p.segments)
                            emit(seg.range, TokenType::Namespace);
                    break;
                }
                case dcc::ast::DeclKind::Struct: {
                    auto* d = static_cast<dcc::ast::StructDecl const*>(decl);
                    if (d->name_range.valid())
                        emit_decl_name(d->name_range, TokenType::Struct);
                    visitTemplateParams(d->template_params);
                    for (auto const& f : d->fields)
                    {
                        if (f.type)
                            visitTypeExpr(f.type);
                        if (f.name_range.valid())
                            emit_decl_name(f.name_range, TokenType::Property);
                    }
                    break;
                }
                case dcc::ast::DeclKind::Union: {
                    auto* d = static_cast<dcc::ast::UnionDecl const*>(decl);
                    if (d->name_range.valid())
                        emit_decl_name(d->name_range, TokenType::Struct);
                    for (auto const& f : d->fields)
                    {
                        if (f.type)
                            visitTypeExpr(f.type);
                        if (f.name_range.valid())
                            emit_decl_name(f.name_range, TokenType::Property);
                    }
                    break;
                }
                case dcc::ast::DeclKind::Enum: {
                    auto* d = static_cast<dcc::ast::EnumDecl const*>(decl);
                    if (d->name_range.valid())
                        emit_decl_name(d->name_range, TokenType::Enum);
                    if (d->backing_type)
                        visitTypeExpr(d->backing_type);
                    visitTemplateParams(d->template_params);
                    for (auto const& v : d->variants)
                    {
                        for (auto* t : v.payload)
                            if (t)
                                visitTypeExpr(t);
                        if (v.range.valid() && v.range.byte_length() > 0)
                        {
                            auto variant_name_range = find_name_in_range(v.range, v.name);
                            if (variant_name_range.valid())
                                emit_decl_name(variant_name_range, TokenType::EnumMember);
                        }
                        if (v.explicit_value)
                            visitExpr(v.explicit_value);
                    }
                    break;
                }
                case dcc::ast::DeclKind::Func: {
                    auto* d = static_cast<dcc::ast::FuncDecl const*>(decl);
                    if (d->name_range.valid())
                        emit_decl_name(d->name_range, TokenType::Function);
                    if (d->return_type)
                        visitTypeExpr(d->return_type);
                    visitTemplateParams(d->template_params);
                    for (auto const& p : d->params)
                    {
                        if (p.type)
                            visitTypeExpr(p.type);
                        if (p.range.valid() && p.range.byte_length() > 0)
                        {
                            auto param_name_range = find_name_in_range(p.range, p.name);
                            if (param_name_range.valid())
                                emit(param_name_range, TokenType::Parameter);
                        }
                    }
                    if (d->constraint)
                        visitExpr(d->constraint);
                    if (d->body.has_value())
                        visitBlock(*d->body);
                    break;
                }
                case dcc::ast::DeclKind::Var: {
                    auto* d = static_cast<dcc::ast::VarDecl const*>(decl);
                    if (d->name_range.valid())
                        emit_decl_name(d->name_range, TokenType::Variable);
                    if (d->type)
                        visitTypeExpr(d->type);
                    if (d->init)
                        visitExpr(d->init);
                    break;
                }
                case dcc::ast::DeclKind::StaticIfGroup: {
                    for (auto const* g = static_cast<dcc::ast::StaticIfGroup const*>(decl); g; g = g->else_group)
                    {
                        if (g->condition)
                            visitExpr(g->condition);
                        for (auto* inner : g->then_decls)
                            visitDecl(inner);
                    }
                    break;
                }
            }
        }

        void Collector::emit_using_item(dcc::ast::UsingItem const* item)
        {
            for (auto const& seg : item->path.segments)
                emit(seg.range, TokenType::Namespace);
            for (auto const* child : item->children)
                emit_using_item(child);
        }

    } // anonymous namespace

    std::vector<std::uint32_t> delta_encode(std::vector<RawToken> tokens)
    {
        if (tokens.empty())
        {
            std::cerr << "Returning 0 semantic tokens" << std::endl;
            return {};
        }

        std::ranges::sort(tokens, [](RawToken const& a, RawToken const& b) {
            if (a.line != b.line)
                return a.line < b.line;
            return a.character < b.character;
        });

        std::vector<RawToken> accepted;
        accepted.reserve(tokens.size());

        for (auto const& t : tokens)
        {
            if (t.length == 0)
                continue;

            if (accepted.empty())
            {
                accepted.push_back(t);
                continue;
            }

            auto const& prev = accepted.back();

            if (t.line == prev.line && t.character == prev.character)
                continue;

            if (t.line == prev.line && t.character < prev.character + prev.length)
                continue;

            accepted.push_back(t);
        }

        std::vector<std::uint32_t> data;
        data.reserve(accepted.size() * 5);

        std::uint32_t prev_line = 0;
        std::uint32_t prev_char = 0;
        std::uint32_t encoded_count = 0;

        for (auto const& t : accepted)
        {
            if (encoded_count == 0)
            {
                data.push_back(t.line);
                data.push_back(t.character);
                data.push_back(t.length);
                data.push_back(t.type);
                data.push_back(t.modifiers);

                prev_line = t.line;
                prev_char = t.character;
                ++encoded_count;
                continue;
            }

            if (t.line < prev_line)
            {
                std::cerr << "[dccd] delta_encode: negative delta_line from " << prev_line << " to " << t.line << " at char " << t.character << "; skipping"
                          << std::endl;
                continue;
            }

            if (t.line == prev_line)
            {
                if (t.character < prev_char)
                {
                    std::cerr << "[dccd] delta_encode: negative delta_start_char on line " << t.line << " from " << prev_char << " to " << t.character
                              << "; skipping" << std::endl;
                    continue;
                }

                data.push_back(0);
                data.push_back(t.character - prev_char);
            }
            else
            {
                data.push_back(t.line - prev_line);
                data.push_back(t.character);
            }

            data.push_back(t.length);
            data.push_back(t.type);
            data.push_back(t.modifiers);

            prev_line = t.line;
            prev_char = t.character;
            ++encoded_count;
        }

        std::cerr << "Returning " << encoded_count << " semantic tokens" << '\n';
        return data;
    }

    std::vector<std::uint32_t> collect_tokens(dcc::sm::SourceManager const& sm, dcc::ast::TranslationUnit const* tu, dcc::sm::FileId requested_file)
    {
        if (!tu)
            return {};

        Collector c{sm, requested_file};

        if (tu->module_decl)
            c.visitDecl(tu->module_decl);

        for (auto* d : tu->imports)
            if (d)
                c.visitDecl(d);

        for (auto* d : tu->decls)
            if (d)
                c.visitDecl(d);

        return delta_encode(std::move(c.tokens));
    }

} // namespace dccd::semantic_tokens
