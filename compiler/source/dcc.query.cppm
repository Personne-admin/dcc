export module dcc.query;

import std;
import dcc.sm;
import dcc.si;
import dcc.lex;
import dcc.ast;
import dcc.types;
import dcc.sema;
import dcc.sema.type_helpers;
import dcc.session;
import dcc.ast.visitor;

export namespace dcc::query
{
    struct QueryOptions
    {
        bool include_decls{true};
        bool include_stmts{true};
        bool include_exprs{true};
        bool include_type_exprs{true};
    };

    struct NodeAtLocation
    {
        sm::FileId file{sm::FileId::Invalid};
        sm::Position position{};
        sm::Location location{};

        sema::ModuleInfo const* module{};
        sema::Scope const* scope{};

        ast::Decl const* decl{};
        ast::Decl const* enclosing_decl{};
        ast::Decl const* hovered_decl{};
        ast::Stmt const* stmt{};
        ast::Expr const* expr{};
        ast::TypeExpr const* type_expr{};

        types::Type const* resolved_type{};
        ast::Decl const* resolved_decl{};
        ast::FuncDecl const* resolved_specialization{};
        ast::Decl const* ufcs_callee{};
        ast::FieldDecl const* resolved_field{};

        ast::Decl const* resolved_field_parent{};
        ast::UsingDecl const* resolved_via_using{};

        ast::EnumVariant const* resolved_variant{};
        ast::EnumDecl const* resolved_variant_owner{};

        ast::TemplateParam const* resolved_tparam{};
        ast::Decl const* resolved_tparam_owner{};

        sm::SourceRange resolved_definition_range{};
        ast::FuncParam const* resolved_param{};
        ast::LambdaExpr const* resolved_lambda{};

        ast::CallExpr const* enclosing_call{nullptr};

        [[nodiscard]] bool has_ast_node() const noexcept { return hovered_decl || stmt || expr || type_expr; }

        [[nodiscard]] bool has_semantic_target() const noexcept
        {
            return resolved_type || resolved_decl || resolved_specialization || ufcs_callee || resolved_field || resolved_param || resolved_variant ||
                   resolved_tparam || resolved_definition_range.valid();
        }
    };

    enum class LocalSymbolKind : std::uint8_t
    {
        Variable,
        Parameter,
        TemplateParam,
    };

    struct LocalSymbolInfo
    {
        std::string_view name;
        LocalSymbolKind kind{LocalSymbolKind::Variable};
        std::uint32_t depth{0};
        ast::VarDecl const* var_decl{};
        ast::FuncDecl const* param_owner{};
        std::uint32_t param_index{0};
        types::Type const* type{};
        sm::Offset name_offset{0};
        bool is_pack : 1 {};
    };

    struct LocalContext
    {
        sm::Location location{};
        sema::ModuleInfo const* module{};
        sema::Scope const* scope{};
        ast::Decl const* enclosing_decl{};
        ast::FuncDecl const* enclosing_func{};
        ast::Block const* enclosing_block{};
        std::vector<LocalSymbolInfo> locals;

        [[nodiscard]] types::Type const* local_type(std::string_view name) const
        {
            for (auto const& l : locals)
                if (l.name == name)
                    return l.type;

            return nullptr;
        }
    };

    struct ActiveCallInfo
    {
        ast::CallExpr const* call{nullptr};
        ast::Expr const* callee_expr{nullptr};
        std::string callee_name;
        bool ufcs{false};
        bool in_call_arguments{false};
        std::uint32_t active_parameter{0};
        std::uint32_t explicit_argument_count{0};
        bool current_argument_has_tokens{false};
        sm::Offset open_paren_offset{0};
    };

    struct SourceRegion
    {
        bool in_string : 1 {};
        bool in_comment : 1 {};

        [[nodiscard]] bool in_string_or_comment() const noexcept { return in_string || in_comment; }
    };

    [[nodiscard]] std::optional<NodeAtLocation> find_node_at(session::CompilerSession const& session, sm::FileId file, sm::Position position,
                                                             QueryOptions opts = {});

    [[nodiscard]] std::optional<NodeAtLocation> find_node_at(session::CompilerSession const& session, sm::Location location, QueryOptions opts = {});

    [[nodiscard]] LocalContext collect_local_context(session::CompilerSession const& session, sm::FileId file, sm::Location location);

    [[nodiscard]] std::optional<ActiveCallInfo> find_active_call(session::CompilerSession const& session, sm::FileId file, sm::Location location);

    [[nodiscard]] SourceRegion source_region_at(session::CompilerSession const& session, sm::FileId file, sm::Offset offset);

    [[nodiscard]] sm::SourceRange decl_name_range(ast::Decl const* decl);

    [[nodiscard]] sm::SourceRange field_name_range(ast::FieldDecl const* fd);

    [[nodiscard]] bool file_in_module_graph(session::CompilerSession const& session, sm::FileId file);

    enum class SymbolKind : std::uint8_t
    {
        Declaration,
        Field,
        FuncParam,
        TemplateParam,
        EnumVariant,
        ImportAlias,
        UsingAlias,
        Module,
        Unknown,
    };

    struct SymbolId
    {
        sm::FileId file{sm::FileId::Invalid};
        sm::Offset name_offset{0};
        SymbolKind kind{SymbolKind::Unknown};
        sm::FileId owner_file{sm::FileId::Invalid};
        sm::Offset owner_offset{0};
        std::uint32_t sub_index{0};

        [[nodiscard]] bool valid() const noexcept { return file != sm::FileId::Invalid && kind != SymbolKind::Unknown; }

        [[nodiscard]] friend bool operator==(SymbolId const&, SymbolId const&) = default;
    };

    struct ResolveOptions
    {
        bool include_decls{true};
        bool include_stmts{true};
        bool include_exprs{true};
        bool include_type_exprs{true};
    };

    struct ResolvedSymbol
    {
        SymbolId id;
        SymbolKind kind{SymbolKind::Unknown};
        std::string_view name;
        sm::SourceRange name_range;
        sm::SourceRange definition_range;

        ast::Decl const* decl{nullptr};
        ast::FuncDecl const* specialization{nullptr};
        ast::Decl const* owner_decl{nullptr};
        ast::UsingDecl const* via_using{nullptr};
        ast::ImportDecl const* via_import{nullptr};
        std::uint32_t sub_index{0};

        bool is_module : 1 {};
        bool is_ambiguous : 1 {};
        bool is_external_alias : 1 {};
        bool from_specialization : 1 {};

        [[nodiscard]] bool has_target() const noexcept { return id.valid(); }

        [[nodiscard]] bool is_renameable() const noexcept { return id.valid() && !is_module && !is_ambiguous && !is_external_alias && name_range.valid(); }
    };

    [[nodiscard]] std::optional<ResolvedSymbol> resolve_symbol_at(session::CompilerSession const& session, sm::FileId file, sm::Position position,
                                                                  ResolveOptions opts = {});

    [[nodiscard]] std::optional<ResolvedSymbol> resolve_symbol_at(session::CompilerSession const& session, sm::Location location, ResolveOptions opts = {});

    [[nodiscard]] std::vector<sm::SourceRange> find_symbol_references(session::CompilerSession const& session, ResolvedSymbol const& target,
                                                                      bool include_declaration = false);

    [[nodiscard]] sm::SourceRange symbol_name_range(ResolvedSymbol const& symbol) noexcept;

    [[nodiscard]] std::string symbol_display_name(ResolvedSymbol const& symbol);

    [[nodiscard]] bool can_rename_symbol(ResolvedSymbol const& symbol) noexcept;

    struct IndexedSymbolRecord
    {
        SymbolId id;
        SymbolKind kind{SymbolKind::Unknown};
        std::string name;
        sm::SourceRange name_range;
        sm::SourceRange definition_range;
        std::string container;
        std::string module_path;
        std::uint8_t decl_kind{0xFF};
        bool is_module : 1 {};
        bool is_renameable : 1 {};
    };

    struct ReferenceOccurrence
    {
        SymbolId target;
        sm::SourceRange range;
    };

    struct ExtractedModule
    {
        std::string canonical_path;
        std::string file_path;
        sm::FileId file_id{sm::FileId::Invalid};
        std::uint64_t content_revision{0};
        bool skipped{false};
        std::vector<std::string> imports;
        std::vector<IndexedSymbolRecord> symbols;
        std::vector<ReferenceOccurrence> references;
    };

    struct WorkspaceExtraction
    {
        std::vector<ExtractedModule> modules;
    };

    [[nodiscard]] WorkspaceExtraction extract_workspace(session::CompilerSession const& session,
                                                        std::unordered_map<std::string, std::uint64_t> const& skip_revisions = {});

    [[nodiscard]] std::vector<IndexedSymbolRecord> extract_file_symbols(session::CompilerSession& session, sm::FileId file);

} // namespace dcc::query

module :private;

namespace dcc::query
{
    namespace
    {
        [[nodiscard]] constexpr bool range_contains(sm::SourceRange const& range, sm::Location target) noexcept
        {
            return range.begin.fileId == target.fileId && range.begin.offset <= target.offset && range.end.offset > target.offset;
        }

        [[nodiscard]] constexpr bool range_contains_or_touches_end(sm::SourceRange const& range, sm::Location target) noexcept
        {
            if (!range.valid())
                return false;
            return range.begin.fileId == target.fileId && range.begin.offset <= target.offset && range.end.offset >= target.offset;
        }

        [[nodiscard]] bool is_target_on_decl_name(ast::Decl const* decl, sm::Location target)
        {
            auto nr = decl_name_range(decl);
            return nr.valid() && range_contains(nr, target);
        }

        void walk_decl(ast::Decl const* decl, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_stmt(ast::Stmt const* stmt, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_expr(ast::Expr const* expr, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_type_expr(ast::TypeExpr const* type_expr, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_pattern(ast::Pattern const* pat, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_block(ast::Block const& block, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_match_arm(ast::MatchArm const& arm, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_template_arg(ast::TemplateArg const& arg, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_template_args(std::pmr::vector<ast::TemplateArg> const& args, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_template_params(std::pmr::vector<ast::TemplateParam> const& params, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);
        void walk_attrs(std::pmr::vector<ast::Attribute> const& attrs, NodeAtLocation& result, sm::Location target, QueryOptions const& opts);

        [[nodiscard]] sm::SourceRange func_param_name_range(ast::FuncParam const& param) noexcept;
        [[nodiscard]] sm::SourceRange template_param_name_range(ast::TemplateParam const& tp) noexcept;
        [[nodiscard]] sm::SourceRange enum_variant_name_range(ast::EnumVariant const& v) noexcept;
        [[nodiscard]] std::optional<std::size_t> find_param_name_index(ast::FuncDecl const* fd, sm::Location target);
        [[nodiscard]] std::optional<std::size_t> find_tparam_name_index(std::span<ast::TemplateParam const> params, sm::Location target);
        void check_param_and_tparam_names(ast::Decl const* decl, NodeAtLocation& result, sm::Location target);

        void surface_expr_sema(ast::Expr const* expr, NodeAtLocation& result)
        {
            if (!expr)
                return;

            result.ufcs_callee = nullptr;
            result.resolved_variant = nullptr;

            if (const auto* t = sema::get_resolved_type(expr->sema))
                result.resolved_type = t;
            if (expr->sema.resolved_decl)
                result.resolved_decl = expr->sema.resolved_decl;
            if (expr->sema.resolved_specialization)
                result.resolved_specialization = expr->sema.resolved_specialization;
            else
                result.resolved_specialization = nullptr;
            if (expr->sema.ufcs_callee)
                result.ufcs_callee = expr->sema.ufcs_callee;
            if (expr->sema.constructed_variant && !result.resolved_variant)
                result.resolved_variant = expr->sema.constructed_variant;
        }

        void surface_type_sema(ast::TypeExpr const* type_expr, NodeAtLocation& result)
        {
            if (!type_expr)
                return;

            result.resolved_type = nullptr;
            result.resolved_decl = nullptr;

            if (const auto* t = sema::get_canonical(type_expr->sema))
                result.resolved_type = t;
            if (type_expr->sema.resolved_decl)
                result.resolved_decl = type_expr->sema.resolved_decl;
        }

        void apply_resolved_type_symbol(NodeAtLocation& result, sema::Symbol const* sym)
        {
            if (!sym || !sym->decl)
                return;

            result.resolved_decl = sym->decl;

            if (sym->via_using && sym->via_using->using_kind == ast::UsingKind::Alias && static_cast<ast::Decl const*>(sym->via_using) != sym->decl)
                result.resolved_via_using = sym->via_using;
        }

        void walk_block(ast::Block const& block, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            bool any_stmt_matched = false;
            for (auto* s : block.stmts)
            {
                if (!s)
                    continue;

                if (range_contains_or_touches_end(s->range, target))
                {
                    walk_stmt(s, result, target, opts);
                    any_stmt_matched = true;
                }
            }

            bool tail_matched = false;
            if (block.tail && range_contains_or_touches_end(block.tail->range, target))
            {
                walk_expr(block.tail, result, target, opts);
                tail_matched = true;
            }

            if (!any_stmt_matched && !tail_matched && range_contains(block.range, target))
            {
                for (auto* s : block.stmts)
                {
                    if (!s)
                        continue;
                    walk_stmt(s, result, target, opts);
                }
                if (block.tail)
                    walk_expr(block.tail, result, target, opts);
            }
        }

        void walk_match_arm(ast::MatchArm const& arm, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (arm.pattern && range_contains_or_touches_end(arm.pattern->range, target))
                walk_pattern(arm.pattern, result, target, opts);

            if (arm.type_pattern && range_contains_or_touches_end(arm.type_pattern->range, target))
                walk_type_expr(arm.type_pattern, result, target, opts);

            if (arm.guard && range_contains_or_touches_end(arm.guard->range, target))
                walk_expr(arm.guard, result, target, opts);

            if (arm.body && range_contains_or_touches_end(arm.body->range, target))
                walk_expr(arm.body, result, target, opts);
        }

        void walk_template_arg(ast::TemplateArg const& arg, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (arg.type && range_contains_or_touches_end(arg.type->range, target))
                walk_type_expr(arg.type, result, target, opts);

            if (arg.expr && range_contains_or_touches_end(arg.expr->range, target))
                walk_expr(arg.expr, result, target, opts);
        }

        void walk_template_args(std::pmr::vector<ast::TemplateArg> const& args, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            for (auto const& a : args)
                walk_template_arg(a, result, target, opts);
        }

        void walk_template_params(std::pmr::vector<ast::TemplateParam> const& params, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            for (auto const& tp : params)
            {
                if (tp.value_type && range_contains_or_touches_end(tp.value_type->range, target))
                    walk_type_expr(tp.value_type, result, target, opts);
                if (tp.default_type && range_contains_or_touches_end(tp.default_type->range, target))
                    walk_type_expr(tp.default_type, result, target, opts);
                if (tp.default_value && range_contains_or_touches_end(tp.default_value->range, target))
                    walk_expr(tp.default_value, result, target, opts);
            }
        }

        void walk_attrs(std::pmr::vector<ast::Attribute> const& attrs, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            for (auto const& a : attrs)
                for (auto* arg : a.args)
                    if (arg && range_contains_or_touches_end(arg->range, target))
                        walk_expr(arg, result, target, opts);
        }

        void walk_pattern(ast::Pattern const* pat, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (!pat)
                return;

            switch (pat->kind)
            {
                case ast::PatternKind::Literal: {
                    const auto* p = static_cast<ast::LiteralPattern const*>(pat);
                    if (p->value && range_contains_or_touches_end(p->value->range, target))
                        walk_expr(p->value, result, target, opts);
                    break;
                }
                case ast::PatternKind::Binding:
                case ast::PatternKind::Wildcard:
                    break;
                case ast::PatternKind::Ref: {
                    const auto* p = static_cast<ast::RefPattern const*>(pat);
                    if (p->inner && range_contains_or_touches_end(p->inner->range, target))
                        walk_pattern(p->inner, result, target, opts);
                    break;
                }
                case ast::PatternKind::EnumDestructure: {
                    const auto* p = static_cast<ast::EnumDestructurePattern const*>(pat);
                    for (auto* sub : p->payload)
                        if (sub && range_contains_or_touches_end(sub->range, target))
                            walk_pattern(sub, result, target, opts);
                    break;
                }
                case ast::PatternKind::StructDestructure: {
                    const auto* p = static_cast<ast::StructDestructurePattern const*>(pat);
                    for (auto const& f : p->fields)
                        if (f.pattern && range_contains_or_touches_end(f.pattern->range, target))
                            walk_pattern(f.pattern, result, target, opts);
                    break;
                }
                case ast::PatternKind::Range: {
                    const auto* p = static_cast<ast::RangePattern const*>(pat);
                    if (p->start && range_contains_or_touches_end(p->start->range, target))
                        walk_expr(p->start, result, target, opts);
                    if (p->end && range_contains_or_touches_end(p->end->range, target))
                        walk_expr(p->end, result, target, opts);
                    break;
                }
                case ast::PatternKind::Or: {
                    const auto* p = static_cast<ast::OrPattern const*>(pat);
                    for (auto* alt : p->alternatives)
                        if (alt && range_contains_or_touches_end(alt->range, target))
                            walk_pattern(alt, result, target, opts);
                    break;
                }
            }
        }

        void walk_type_expr(ast::TypeExpr const* type_expr, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (!type_expr)
                return;

            if (opts.include_type_exprs && range_contains(type_expr->range, target))
            {
                result.type_expr = type_expr;
                surface_type_sema(type_expr, result);
            }

            switch (type_expr->kind)
            {
                case ast::TypeKind::Primitive:
                    break;
                case ast::TypeKind::Named: {
                    auto* t = static_cast<ast::NamedType const*>(type_expr);
                    walk_template_args(t->template_args, result, target, opts);

                    if (!result.resolved_decl && result.scope && !t->path.is_empty())
                    {
                        auto const* sym = sema::resolve_type_path(*result.scope, t->path);
                        apply_resolved_type_symbol(result, sym);
                    }
                    break;
                }
                case ast::TypeKind::Pointer: {
                    auto* t = static_cast<ast::PointerType const*>(type_expr);
                    if (t->pointee && range_contains_or_touches_end(t->pointee->range, target))
                        walk_type_expr(t->pointee, result, target, opts);
                    break;
                }
                case ast::TypeKind::Array: {
                    auto* t = static_cast<ast::ArrayType const*>(type_expr);
                    if (t->element && range_contains_or_touches_end(t->element->range, target))
                        walk_type_expr(t->element, result, target, opts);
                    if (t->size && range_contains_or_touches_end(t->size->range, target))
                        walk_expr(t->size, result, target, opts);
                    break;
                }
                case ast::TypeKind::Slice: {
                    auto* t = static_cast<ast::SliceType const*>(type_expr);
                    if (t->element && range_contains_or_touches_end(t->element->range, target))
                        walk_type_expr(t->element, result, target, opts);
                    break;
                }
                case ast::TypeKind::Fam: {
                    auto* t = static_cast<ast::FamType const*>(type_expr);
                    if (t->element && range_contains_or_touches_end(t->element->range, target))
                        walk_type_expr(t->element, result, target, opts);
                    break;
                }
                case ast::TypeKind::FuncPtr: {
                    auto* t = static_cast<ast::FuncPtrType const*>(type_expr);
                    if (t->return_type && range_contains_or_touches_end(t->return_type->range, target))
                        walk_type_expr(t->return_type, result, target, opts);
                    for (auto const& p : t->params)
                        if (p.type && range_contains_or_touches_end(p.type->range, target))
                            walk_type_expr(p.type, result, target, opts);
                    break;
                }
                case ast::TypeKind::Qualified: {
                    auto* t = static_cast<ast::QualifiedType const*>(type_expr);
                    if (t->inner && range_contains_or_touches_end(t->inner->range, target))
                        walk_type_expr(t->inner, result, target, opts);
                    break;
                }
                case ast::TypeKind::Restricted: {
                    auto* t = static_cast<ast::RestrictedType const*>(type_expr);
                    if (t->underlying && range_contains_or_touches_end(t->underlying->range, target))
                        walk_type_expr(t->underlying, result, target, opts);
                    for (auto const* element : t->elements)
                        if (element && range_contains_or_touches_end(element->range, target))
                            walk_expr(element, result, target, opts);
                    break;
                }
                case ast::TypeKind::PackIndex: {
                    auto* t = static_cast<ast::PackIndexType const*>(type_expr);
                    if (t->base && range_contains_or_touches_end(t->base->range, target))
                        walk_type_expr(t->base, result, target, opts);
                    if (t->index && range_contains_or_touches_end(t->index->range, target))
                        walk_expr(t->index, result, target, opts);
                    break;
                }
            }
        }

        void walk_expr(ast::Expr const* expr, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (!expr)
                return;

            if (opts.include_exprs && range_contains(expr->range, target))
            {
                result.expr = expr;
                surface_expr_sema(expr, result);
            }

            switch (expr->kind)
            {
                case ast::ExprKind::IntLiteral:
                case ast::ExprKind::FloatLiteral:
                case ast::ExprKind::StringLiteral:
                case ast::ExprKind::U16StringLiteral:
                case ast::ExprKind::CharLiteral:
                case ast::ExprKind::U16CharLiteral:
                case ast::ExprKind::BoolLiteral:
                case ast::ExprKind::NullLiteral:
                case ast::ExprKind::Ident:
                    break;

                case ast::ExprKind::PathExpr: {
                    auto* e = static_cast<ast::PathExpr const*>(expr);
                    walk_template_args(e->explicit_enum_args, result, target, opts);
                    break;
                }
                case ast::ExprKind::Unary: {
                    auto* e = static_cast<ast::UnaryExpr const*>(expr);
                    if (e->operand && range_contains_or_touches_end(e->operand->range, target))
                        walk_expr(e->operand, result, target, opts);
                    break;
                }
                case ast::ExprKind::Postfix: {
                    auto* e = static_cast<ast::PostfixExpr const*>(expr);
                    if (e->operand && range_contains_or_touches_end(e->operand->range, target))
                        walk_expr(e->operand, result, target, opts);
                    break;
                }
                case ast::ExprKind::Binary: {
                    auto* e = static_cast<ast::BinaryExpr const*>(expr);
                    if (e->lhs && range_contains_or_touches_end(e->lhs->range, target))
                        walk_expr(e->lhs, result, target, opts);
                    if (e->rhs && range_contains_or_touches_end(e->rhs->range, target))
                        walk_expr(e->rhs, result, target, opts);
                    break;
                }
                case ast::ExprKind::Call: {
                    auto* e = static_cast<ast::CallExpr const*>(expr);
                    if (e->callee && range_contains_or_touches_end(e->callee->range, target))
                    {
                        walk_expr(e->callee, result, target, opts);

                        bool target_on_method_id = true;
                        if (e->callee->kind == ast::ExprKind::FieldAccess)
                        {
                            auto const* fa = static_cast<ast::FieldAccessExpr const*>(e->callee);
                            target_on_method_id = range_contains(fa->field_range, target);
                        }
                        if (target_on_method_id && expr->sema.resolved_specialization)
                            result.resolved_specialization = expr->sema.resolved_specialization;
                    }

                    if (range_contains(expr->range, target))
                    {
                        bool in_args = !e->callee || target.offset > e->callee->range.end.offset;
                        if (in_args)
                            result.enclosing_call = e;
                    }

                    for (auto* a : e->args)
                        if (a && range_contains_or_touches_end(a->range, target))
                            walk_expr(a, result, target, opts);
                    break;
                }
                case ast::ExprKind::FieldAccess: {
                    auto* e = static_cast<ast::FieldAccessExpr const*>(expr);
                    if (e->object && range_contains_or_touches_end(e->object->range, target))
                        walk_expr(e->object, result, target, opts);

                    if (range_contains(e->field_range, target))
                    {
                        result.expr = expr;
                        surface_expr_sema(expr, result);

                        if (!result.resolved_field)
                        {
                            auto const* nominal = expr->sema.resolved_decl;
                            if (nominal)
                            {
                                result.resolved_field_parent = nominal;
                                auto const& field_name = e->field;
                                if (nominal->kind == ast::DeclKind::Struct)
                                {
                                    auto const* sd = static_cast<ast::StructDecl const*>(nominal);
                                    for (auto const& f : sd->fields)
                                        if (f.name == field_name)
                                        {
                                            result.resolved_field = &f;
                                            break;
                                        }
                                }
                                else if (nominal->kind == ast::DeclKind::Union)
                                {
                                    auto const* ud = static_cast<ast::UnionDecl const*>(nominal);
                                    for (auto const& f : ud->fields)
                                        if (f.name == field_name)
                                        {
                                            result.resolved_field = &f;
                                            break;
                                        }
                                }
                            }
                        }
                    }
                    break;
                }
                case ast::ExprKind::Index: {
                    auto* e = static_cast<ast::IndexExpr const*>(expr);
                    if (e->object && range_contains_or_touches_end(e->object->range, target))
                        walk_expr(e->object, result, target, opts);
                    if (e->index && range_contains_or_touches_end(e->index->range, target))
                        walk_expr(e->index, result, target, opts);
                    break;
                }
                case ast::ExprKind::PackAccess: {
                    auto* e = static_cast<ast::PackAccessExpr const*>(expr);
                    if (e->object && range_contains_or_touches_end(e->object->range, target))
                        walk_expr(e->object, result, target, opts);
                    if (e->index && range_contains_or_touches_end(e->index->range, target))
                        walk_expr(e->index, result, target, opts);
                    break;
                }
                case ast::ExprKind::Cast: {
                    auto* e = static_cast<ast::CastExpr const*>(expr);
                    if (e->operand && range_contains_or_touches_end(e->operand->range, target))
                        walk_expr(e->operand, result, target, opts);
                    if (e->target && range_contains_or_touches_end(e->target->range, target))
                        walk_type_expr(e->target, result, target, opts);
                    break;
                }
                case ast::ExprKind::Block: {
                    auto* e = static_cast<ast::BlockExpr const*>(expr);
                    if (range_contains_or_touches_end(e->body.range, target))
                        walk_block(e->body, result, target, opts);
                    else if (range_contains(expr->range, target))
                        walk_block(e->body, result, target, opts);
                    break;
                }
                case ast::ExprKind::If: {
                    auto* e = static_cast<ast::IfExpr const*>(expr);
                    if (e->condition && range_contains_or_touches_end(e->condition->range, target))
                        walk_expr(e->condition, result, target, opts);
                    if (range_contains_or_touches_end(e->then_block.range, target))
                        walk_block(e->then_block, result, target, opts);
                    if (e->else_branch && range_contains_or_touches_end(e->else_branch->range, target))
                        walk_expr(e->else_branch, result, target, opts);
                    break;
                }
                case ast::ExprKind::Match: {
                    auto* e = static_cast<ast::MatchExpr const*>(expr);
                    if (e->operand && range_contains_or_touches_end(e->operand->range, target))
                        walk_expr(e->operand, result, target, opts);
                    for (auto const& arm : e->arms)
                        if (range_contains_or_touches_end(arm.range, target))
                            walk_match_arm(arm, result, target, opts);
                    break;
                }
                case ast::ExprKind::StructLiteral: {
                    auto* e = static_cast<ast::StructLiteralExpr const*>(expr);
                    if (e->type && range_contains_or_touches_end(e->type->range, target))
                        walk_type_expr(e->type, result, target, opts);
                    for (auto const& f : e->fields)
                        if (f.value && range_contains_or_touches_end(f.value->range, target))
                            walk_expr(f.value, result, target, opts);
                    break;
                }
                case ast::ExprKind::Sizeof: {
                    auto* e = static_cast<ast::SizeofExpr const*>(expr);
                    if (e->target && range_contains_or_touches_end(e->target->range, target))
                        walk_type_expr(e->target, result, target, opts);
                    break;
                }
                case ast::ExprKind::Alignof: {
                    auto* e = static_cast<ast::AlignofExpr const*>(expr);
                    if (e->target && range_contains_or_touches_end(e->target->range, target))
                        walk_type_expr(e->target, result, target, opts);
                    break;
                }
                case ast::ExprKind::Offsetof: {
                    auto* e = static_cast<ast::OffsetofExpr const*>(expr);
                    if (e->target && range_contains_or_touches_end(e->target->range, target))
                        walk_type_expr(e->target, result, target, opts);
                    break;
                }
                case ast::ExprKind::Compiles: {
                    auto* e = static_cast<ast::CompilesExpr const*>(expr);
                    for (auto const& p : e->params)
                        if (p.type && range_contains_or_touches_end(p.type->range, target))
                            walk_type_expr(p.type, result, target, opts);
                    if (range_contains_or_touches_end(e->body.range, target))
                        walk_block(e->body, result, target, opts);
                    break;
                }
                case ast::ExprKind::Range: {
                    auto* e = static_cast<ast::RangeExpr const*>(expr);
                    if (e->start && range_contains_or_touches_end(e->start->range, target))
                        walk_expr(e->start, result, target, opts);
                    if (e->end && range_contains_or_touches_end(e->end->range, target))
                        walk_expr(e->end, result, target, opts);
                    break;
                }
                case ast::ExprKind::TypeAST: {
                    auto* e = static_cast<ast::TypeASTExpr const*>(expr);
                    if (e->type_node && range_contains_or_touches_end(e->type_node->range, target))
                        walk_type_expr(e->type_node, result, target, opts);
                    break;
                }
                case ast::ExprKind::TemplateInst: {
                    auto* e = static_cast<ast::TemplateInstExpr const*>(expr);
                    if (e->callee && range_contains_or_touches_end(e->callee->range, target))
                    {
                        walk_expr(e->callee, result, target, opts);
                        bool target_on_method_id = true;
                        if (e->callee->kind == ast::ExprKind::FieldAccess)
                        {
                            auto const* fa = static_cast<ast::FieldAccessExpr const*>(e->callee);
                            target_on_method_id = range_contains(fa->field_range, target);
                        }
                        if (target_on_method_id && expr->sema.resolved_specialization)
                            result.resolved_specialization = expr->sema.resolved_specialization;
                    }
                    walk_template_args(e->template_args, result, target, opts);
                    break;
                }
                case ast::ExprKind::SizeofPack:
                case ast::ExprKind::PackExpansion:
                    break;
                case ast::ExprKind::Asm: {
                    auto* e = static_cast<ast::AsmExpr const*>(expr);
                    for (auto& op : e->operands)
                    {
                        if (op.expr && range_contains_or_touches_end(op.expr->range, target))
                            walk_expr(op.expr, result, target, opts);
                    }
                    break;
                }
                case ast::ExprKind::Lambda: {
                    auto* e = static_cast<ast::LambdaExpr const*>(expr);
                    for (auto const& p : e->params)
                    {
                        if (p.type && range_contains_or_touches_end(p.type->range, target))
                            walk_type_expr(p.type, result, target, opts);
                        auto nr = func_param_name_range(p);
                        if (nr.valid() && range_contains(nr, target) && !result.resolved_param)
                        {
                            result.resolved_param = &p;
                            result.resolved_lambda = e;
                            result.resolved_definition_range = nr;
                        }
                    }
                    if (e->body && range_contains_or_touches_end(e->body->range, target))
                        walk_expr(e->body, result, target, opts);
                    break;
                }
            }
        }

        void walk_stmt(ast::Stmt const* stmt, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (!stmt)
                return;

            if (opts.include_stmts && range_contains(stmt->range, target))
                result.stmt = stmt;

            switch (stmt->kind)
            {
                case ast::StmtKind::Expr: {
                    auto* s = static_cast<ast::ExprStmt const*>(stmt);
                    if (s->expr && range_contains_or_touches_end(s->expr->range, target))
                        walk_expr(s->expr, result, target, opts);
                    break;
                }
                case ast::StmtKind::DeclStmt: {
                    auto* s = static_cast<ast::DeclStmt const*>(stmt);
                    if (s->decl && range_contains_or_touches_end(s->decl->range, target))
                        walk_decl(s->decl, result, target, opts);
                    break;
                }
                case ast::StmtKind::Return: {
                    auto* s = static_cast<ast::ReturnStmt const*>(stmt);
                    if (s->value && range_contains_or_touches_end(s->value->range, target))
                        walk_expr(s->value, result, target, opts);
                    break;
                }
                case ast::StmtKind::Break:
                case ast::StmtKind::Continue:
                    break;
                case ast::StmtKind::While: {
                    auto* s = static_cast<ast::WhileStmt const*>(stmt);
                    if (s->condition && range_contains_or_touches_end(s->condition->range, target))
                        walk_expr(s->condition, result, target, opts);
                    if (range_contains_or_touches_end(s->body.range, target))
                        walk_block(s->body, result, target, opts);
                    break;
                }
                case ast::StmtKind::DoWhile: {
                    auto* s = static_cast<ast::DoWhileStmt const*>(stmt);
                    if (range_contains_or_touches_end(s->body.range, target))
                        walk_block(s->body, result, target, opts);
                    if (s->condition && range_contains_or_touches_end(s->condition->range, target))
                        walk_expr(s->condition, result, target, opts);
                    break;
                }
                case ast::StmtKind::For: {
                    auto* s = static_cast<ast::ForStmt const*>(stmt);
                    if (s->init && range_contains_or_touches_end(s->init->range, target))
                        walk_stmt(s->init, result, target, opts);
                    if (s->cond && range_contains_or_touches_end(s->cond->range, target))
                        walk_expr(s->cond, result, target, opts);
                    if (s->update && range_contains_or_touches_end(s->update->range, target))
                        walk_expr(s->update, result, target, opts);
                    if (range_contains_or_touches_end(s->body.range, target))
                        walk_block(s->body, result, target, opts);
                    break;
                }
                case ast::StmtKind::ForIn: {
                    auto* s = static_cast<ast::ForInStmt const*>(stmt);
                    if (s->item_type && range_contains_or_touches_end(s->item_type->range, target))
                        walk_type_expr(s->item_type, result, target, opts);
                    if (s->iterable && range_contains_or_touches_end(s->iterable->range, target))
                        walk_expr(s->iterable, result, target, opts);
                    if (range_contains_or_touches_end(s->body.range, target))
                        walk_block(s->body, result, target, opts);
                    break;
                }
                case ast::StmtKind::Defer: {
                    auto* s = static_cast<ast::DeferStmt const*>(stmt);
                    if (s->body && range_contains_or_touches_end(s->body->range, target))
                        walk_stmt(s->body, result, target, opts);
                    break;
                }
                case ast::StmtKind::StaticIf: {
                    auto* s = static_cast<ast::StaticIfStmt const*>(stmt);
                    if (s->condition && range_contains_or_touches_end(s->condition->range, target))
                        walk_expr(s->condition, result, target, opts);
                    if (range_contains_or_touches_end(s->then_block.range, target))
                        walk_block(s->then_block, result, target, opts);
                    if (s->else_branch && range_contains_or_touches_end(s->else_branch->range, target))
                        walk_stmt(s->else_branch, result, target, opts);
                    break;
                }
                case ast::StmtKind::StaticMatch: {
                    auto* s = static_cast<ast::StaticMatchStmt const*>(stmt);
                    if (s->operand && range_contains_or_touches_end(s->operand->range, target))
                        walk_expr(s->operand, result, target, opts);
                    for (auto const& arm : s->arms)
                        if (range_contains_or_touches_end(arm.range, target))
                            walk_match_arm(arm, result, target, opts);
                    break;
                }
                case ast::StmtKind::StaticFor: {
                    auto* s = static_cast<ast::StaticForStmt const*>(stmt);
                    if (s->pack_expr && range_contains_or_touches_end(s->pack_expr->range, target))
                        walk_expr(s->pack_expr, result, target, opts);
                    break;
                }
                case ast::StmtKind::Ambiguous: {
                    auto* s = static_cast<ast::AmbiguousStmt const*>(stmt);
                    if (s->as_decl && range_contains_or_touches_end(s->as_decl->range, target))
                        walk_decl(s->as_decl, result, target, opts);
                    if (s->as_expr && range_contains_or_touches_end(s->as_expr->range, target))
                        walk_expr(s->as_expr, result, target, opts);
                    break;
                }
                case ast::StmtKind::Asm: {
                    auto* s = static_cast<ast::AsmStmt const*>(stmt);
                    for (auto& op : s->operands)
                    {
                        if (op.expr && range_contains_or_touches_end(op.expr->range, target))
                            walk_expr(op.expr, result, target, opts);
                    }
                    break;
                }
            }
        }

        void walk_decl(ast::Decl const* decl, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (!decl)
                return;

            if (decl->kind == ast::DeclKind::Func && range_contains_or_touches_end(decl->range, target))
                result.enclosing_decl = decl;

            check_param_and_tparam_names(decl, result, target);

            if (opts.include_decls)
            {
                if (is_target_on_decl_name(decl, target))
                {
                    result.hovered_decl = decl;
                    result.decl = decl;
                }
            }

            walk_attrs(decl->attrs, result, target, opts);

            switch (decl->kind)
            {
                case ast::DeclKind::Module:
                case ast::DeclKind::Import:
                case ast::DeclKind::StaticIfGroup:
                    break;
                case ast::DeclKind::Using: {
                    auto* d = static_cast<ast::UsingDecl const*>(decl);
                    walk_template_params(d->template_params, result, target, opts);
                    if (d->target_type && range_contains_or_touches_end(d->target_type->range, target))
                        walk_type_expr(d->target_type, result, target, opts);
                    if (d->target_expr && range_contains_or_touches_end(d->target_expr->range, target))
                        walk_expr(d->target_expr, result, target, opts);
                    break;
                }
                case ast::DeclKind::Struct: {
                    auto* d = static_cast<ast::StructDecl const*>(decl);
                    walk_template_params(d->template_params, result, target, opts);
                    for (auto const& f : d->fields)
                        if (f.type && range_contains_or_touches_end(f.type->range, target))
                            walk_type_expr(f.type, result, target, opts);
                    break;
                }
                case ast::DeclKind::Union: {
                    auto* d = static_cast<ast::UnionDecl const*>(decl);
                    for (auto const& f : d->fields)
                        if (f.type && range_contains_or_touches_end(f.type->range, target))
                            walk_type_expr(f.type, result, target, opts);
                    break;
                }
                case ast::DeclKind::Enum: {
                    auto* d = static_cast<ast::EnumDecl const*>(decl);
                    walk_template_params(d->template_params, result, target, opts);
                    if (d->backing_type && range_contains_or_touches_end(d->backing_type->range, target))
                        walk_type_expr(d->backing_type, result, target, opts);
                    for (auto const& v : d->variants)
                    {
                        for (auto* t : v.payload)
                            if (t && range_contains_or_touches_end(t->range, target))
                                walk_type_expr(t, result, target, opts);
                        if (v.explicit_value && range_contains_or_touches_end(v.explicit_value->range, target))
                            walk_expr(v.explicit_value, result, target, opts);
                    }
                    break;
                }
                case ast::DeclKind::Func: {
                    auto* d = static_cast<ast::FuncDecl const*>(decl);
                    if (d->return_type && range_contains_or_touches_end(d->return_type->range, target))
                        walk_type_expr(d->return_type, result, target, opts);
                    walk_template_params(d->template_params, result, target, opts);
                    for (auto const& p : d->params)
                        if (p.type && range_contains_or_touches_end(p.type->range, target))
                            walk_type_expr(p.type, result, target, opts);
                    if (d->constraint && range_contains_or_touches_end(d->constraint->range, target))
                        walk_expr(d->constraint, result, target, opts);
                    if (d->body.has_value())
                    {
                        if (range_contains_or_touches_end(d->body->range, target))
                        {
                            walk_block(*d->body, result, target, opts);
                        }
                        else if (range_contains(decl->range, target))
                        {
                            walk_block(*d->body, result, target, opts);
                        }
                    }
                    break;
                }
                case ast::DeclKind::Var: {
                    auto* d = static_cast<ast::VarDecl const*>(decl);
                    if (d->type && range_contains_or_touches_end(d->type->range, target))
                        walk_type_expr(d->type, result, target, opts);
                    if (d->init && range_contains_or_touches_end(d->init->range, target))
                        walk_expr(d->init, result, target, opts);
                    break;
                }
            }
        }

        void walk_translation_unit(ast::TranslationUnit const* tu, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (!tu)
                return;

            if (tu->module_decl && range_contains_or_touches_end(tu->module_decl->range, target))
                walk_decl(tu->module_decl, result, target, opts);

            for (auto* d : tu->imports)
                if (d && range_contains_or_touches_end(d->range, target))
                    walk_decl(d, result, target, opts);

            for (auto* d : tu->decls)
                if (d && range_contains_or_touches_end(d->range, target))
                    walk_decl(d, result, target, opts);
        }

        [[nodiscard]] sm::SourceRange func_param_name_range(ast::FuncParam const& param) noexcept
        {
            if (!param.range.valid() || param.name.empty())
                return {};

            sm::SourceRange nr;
            nr.begin.fileId = param.range.begin.fileId;
            auto name_len = static_cast<sm::Offset>(param.name.size());
            if (param.range.end.offset > name_len)
                nr.begin.offset = param.range.end.offset - name_len;
            else
                nr.begin.offset = param.range.begin.offset;

            nr.end = param.range.end;
            return nr;
        }

        [[nodiscard]] sm::SourceRange template_param_name_range(ast::TemplateParam const& tp) noexcept
        {
            if (!tp.range.valid() || tp.name.empty())
                return {};

            sm::SourceRange nr;
            nr.begin.fileId = tp.range.begin.fileId;
            auto name_len = static_cast<sm::Offset>(tp.name.size());
            if (tp.range.end.offset >= name_len && tp.range.byte_length() >= name_len)
                nr.begin.offset = tp.range.end.offset - name_len;
            else
                nr.begin.offset = tp.range.begin.offset;

            nr.end = tp.range.end;
            return nr;
        }

        [[nodiscard]] sm::SourceRange enum_variant_name_range(ast::EnumVariant const& v) noexcept
        {
            if (v.name.empty() || !v.range.valid())
                return {};

            sm::SourceRange nr;
            nr.begin.fileId = v.range.begin.fileId;
            auto name_len = static_cast<sm::Offset>(v.name.size());
            if (v.range.byte_length() >= name_len)
                nr.begin.offset = v.range.begin.offset;
            else
                nr.begin.offset = v.range.end.offset - name_len;

            nr.end.offset = nr.begin.offset + name_len;
            nr.end.fileId = nr.begin.fileId;
            return nr;
        }

        [[nodiscard]] std::optional<std::size_t> find_param_name_index(ast::FuncDecl const* fd, sm::Location target)
        {
            if (!fd)
                return std::nullopt;
            for (std::size_t i = 0; i < fd->params.size(); ++i)
            {
                auto nr = func_param_name_range(fd->params[i]);
                if (nr.valid() && range_contains(nr, target))
                    return i;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::size_t> find_tparam_name_index(std::span<ast::TemplateParam const> params, sm::Location target)
        {
            for (std::size_t i = 0; i < params.size(); ++i)
            {
                auto nr = template_param_name_range(params[i]);
                if (nr.valid() && range_contains(nr, target))
                    return i;
            }
            return std::nullopt;
        }

        void check_param_and_tparam_names(ast::Decl const* decl, NodeAtLocation& result, sm::Location target)
        {
            if (!decl)
                return;

            auto check_tparams = [&](std::span<ast::TemplateParam const> tparams) {
                if (result.resolved_tparam)
                    return;
                if (auto idx = find_tparam_name_index(tparams, target))
                {
                    result.resolved_tparam = &tparams[*idx];
                    result.resolved_tparam_owner = decl;
                }
            };

            switch (decl->kind)
            {
                case ast::DeclKind::Func: {
                    auto const* fd = static_cast<ast::FuncDecl const*>(decl);
                    check_tparams(fd->template_params);
                    if (result.resolved_param)
                        return;
                    if (auto idx = find_param_name_index(fd, target))
                    {
                        result.resolved_param = &fd->params[*idx];
                        result.resolved_definition_range = func_param_name_range(fd->params[*idx]);
                    }
                    break;
                }
                case ast::DeclKind::Struct: {
                    auto const* sd = static_cast<ast::StructDecl const*>(decl);
                    check_tparams(sd->template_params);
                    if (result.resolved_field)
                        return;
                    for (auto const& f : sd->fields)
                    {
                        auto nr = field_name_range(&f);
                        if (nr.valid() && range_contains(nr, target))
                        {
                            result.resolved_field = &f;
                            result.resolved_field_parent = decl;
                            break;
                        }
                    }
                    break;
                }
                case ast::DeclKind::Union: {
                    auto const* ud = static_cast<ast::UnionDecl const*>(decl);
                    if (result.resolved_field)
                        return;
                    for (auto const& f : ud->fields)
                    {
                        auto nr = field_name_range(&f);
                        if (nr.valid() && range_contains(nr, target))
                        {
                            result.resolved_field = &f;
                            result.resolved_field_parent = decl;
                            break;
                        }
                    }
                    break;
                }
                case ast::DeclKind::Enum: {
                    auto const* ed = static_cast<ast::EnumDecl const*>(decl);
                    check_tparams(ed->template_params);
                    if (result.resolved_variant)
                        return;
                    for (auto const& v : ed->variants)
                    {
                        auto nr = enum_variant_name_range(v);
                        if (nr.valid() && range_contains(nr, target))
                        {
                            result.resolved_variant = &v;
                            result.resolved_variant_owner = ed;
                            break;
                        }
                    }
                    break;
                }
                case ast::DeclKind::Using:
                    check_tparams(static_cast<ast::UsingDecl const*>(decl)->template_params);
                    break;
                default:
                    break;
            }
        }

        [[nodiscard]] ast::VarDecl const* find_local_var_in_block(ast::Block const& block, std::string_view name, sm::Location target)
        {
            ast::VarDecl const* best = nullptr;
            sm::Offset best_dist = 0;

            auto search = [&](auto& self, ast::Block const& b) -> void {
                for (auto* s : b.stmts)
                {
                    if (!s)
                        continue;

                    if (s->kind == ast::StmtKind::DeclStmt)
                    {
                        auto* ds = static_cast<ast::DeclStmt const*>(s);
                        if (ds->decl && ds->decl->kind == ast::DeclKind::Var)
                        {
                            auto* vd = static_cast<ast::VarDecl const*>(ds->decl);
                            if (vd->name == name && vd->name_range.valid() && vd->name_range.begin.offset < target.offset)
                            {
                                auto dist = target.offset - vd->name_range.begin.offset;
                                if (!best || dist < best_dist)
                                {
                                    best = vd;
                                    best_dist = dist;
                                }
                            }
                        }
                    }

                    if (s->kind == ast::StmtKind::While)
                        self(self, static_cast<ast::WhileStmt const*>(s)->body);
                    else if (s->kind == ast::StmtKind::DoWhile)
                        self(self, static_cast<ast::DoWhileStmt const*>(s)->body);
                    else if (s->kind == ast::StmtKind::For)
                    {
                        auto* fs = static_cast<ast::ForStmt const*>(s);
                        if (fs->init && fs->init->kind == ast::StmtKind::DeclStmt)
                        {
                            auto* ids = static_cast<ast::DeclStmt const*>(fs->init);
                            if (ids->decl && ids->decl->kind == ast::DeclKind::Var)
                            {
                                auto* vd = static_cast<ast::VarDecl const*>(ids->decl);
                                if (vd->name == name && vd->name_range.valid() && vd->name_range.begin.offset < target.offset)
                                {
                                    auto dist = target.offset - vd->name_range.begin.offset;
                                    if (!best || dist < best_dist)
                                    {
                                        best = vd;
                                        best_dist = dist;
                                    }
                                }
                            }
                        }
                        self(self, fs->body);
                    }
                    else if (s->kind == ast::StmtKind::ForIn)
                        self(self, static_cast<ast::ForInStmt const*>(s)->body);
                    else if (s->kind == ast::StmtKind::StaticIf)
                        self(self, static_cast<ast::StaticIfStmt const*>(s)->then_block);
                }
            };

            search(search, block);
            return best;
        }

        [[nodiscard]] std::string_view expr_simple_name(ast::Expr const* expr)
        {
            if (!expr)
                return {};

            if (expr->kind == ast::ExprKind::Ident)
                return static_cast<ast::IdentExpr const*>(expr)->name;

            if (expr->kind == ast::ExprKind::PathExpr)
            {
                auto const* pe = static_cast<ast::PathExpr const*>(expr);
                if (pe->path.is_simple())
                    return pe->path.simple_name();
            }

            return {};
        }

        void borrow_sema_from_spec(NodeAtLocation& result, ast::FuncDecl const& spec, sm::Location target, QueryOptions const& opts,
                                   ast::FuncDecl const& generic_fn)
        {
            NodeAtLocation spec_result;
            spec_result.file = result.file;
            spec_result.location = result.location;
            spec_result.position = result.position;
            spec_result.module = result.module;
            spec_result.scope = result.scope;

            walk_block(*spec.body, spec_result, target, opts);

            if (spec_result.resolved_type && !result.resolved_type)
            {
                result.resolved_type = spec_result.resolved_type;
            }
            if (spec_result.resolved_decl && !result.resolved_decl)
            {
                result.resolved_decl = spec_result.resolved_decl;
            }
            if (spec_result.resolved_specialization && !result.resolved_specialization)
            {
                result.resolved_specialization = spec_result.resolved_specialization;
            }
            if (spec_result.ufcs_callee && !result.ufcs_callee)
            {
                result.ufcs_callee = spec_result.ufcs_callee;
            }
            if (spec_result.resolved_field && !result.resolved_field)
            {
                result.resolved_field = spec_result.resolved_field;
                result.resolved_field_parent = spec_result.resolved_field_parent;
            }

            if (!result.resolved_type && result.resolved_param)
            {
                auto param_idx = static_cast<std::size_t>(result.resolved_param - generic_fn.params.data());
                if (param_idx < generic_fn.params.size() && param_idx < spec.params.size())
                {
                    auto const& sp = spec.params[param_idx];
                    if (sp.type && sp.type->sema.canonical)
                    {
                        result.resolved_type = sema::get_canonical(sp.type->sema);
                    }
                }
            }

            if (!result.resolved_type && result.resolved_decl && result.resolved_decl->kind == ast::DeclKind::Var)
            {
                if (spec_result.hovered_decl && spec_result.hovered_decl->kind == ast::DeclKind::Var)
                {
                    auto const* spec_vd = static_cast<ast::VarDecl const*>(spec_result.hovered_decl);
                    if (spec_vd->type && spec_vd->type->sema.canonical)
                    {
                        result.resolved_type = sema::get_canonical(spec_vd->type->sema);
                    }

                    result.resolved_decl = spec_vd;
                }
            }

            if (!result.resolved_decl && spec_result.hovered_decl && result.hovered_decl && result.hovered_decl->kind == ast::DeclKind::Var &&
                spec_result.hovered_decl->kind == ast::DeclKind::Var &&
                static_cast<ast::VarDecl const*>(result.hovered_decl)->name == static_cast<ast::VarDecl const*>(spec_result.hovered_decl)->name)
            {
                result.resolved_decl = spec_result.hovered_decl;
                auto const* spec_vd = static_cast<ast::VarDecl const*>(spec_result.hovered_decl);
                if (!result.resolved_type && spec_vd->type && spec_vd->type->sema.canonical)
                {
                    result.resolved_type = sema::get_canonical(spec_vd->type->sema);
                }
            }
        }

        struct QueryAllVisitor : ast::RecursiveAstVisitor
        {
            NodeAtLocation& result;
            sm::Location target;
            QueryOptions const& opts;

            QueryAllVisitor(NodeAtLocation& r, sm::Location t, QueryOptions const& o) : result{r}, target{t}, opts{o} {}

            void visitDecl(ast::Decl const* decl) override
            {
                if (!decl)
                    return;

                if (decl->kind == ast::DeclKind::Func && range_contains_or_touches_end(decl->range, target))
                    result.enclosing_decl = decl;

                check_param_and_tparam_names(decl, result, target);

                if (opts.include_decls)
                {
                    if (is_target_on_decl_name(decl, target))
                    {
                        result.hovered_decl = decl;
                        result.decl = decl;
                    }
                }

                ast::RecursiveAstVisitor::visitDecl(decl);
            }

            void visitStmt(ast::Stmt const* stmt) override
            {
                if (!stmt)
                    return;

                if (opts.include_stmts && range_contains(stmt->range, target))
                    result.stmt = stmt;

                ast::RecursiveAstVisitor::visitStmt(stmt);
            }

            void visitExpr(ast::Expr const* expr) override
            {
                if (!expr)
                    return;

                if (opts.include_exprs && range_contains(expr->range, target))
                {
                    result.expr = expr;
                    surface_expr_sema(expr, result);
                }

                if (expr->kind == ast::ExprKind::Call)
                {
                    auto* e = static_cast<ast::CallExpr const*>(expr);
                    if (e->callee)
                    {
                        ast::RecursiveAstVisitor::visitExpr(e->callee);
                    }

                    if (range_contains_or_touches_end(expr->range, target))
                    {
                        bool in_args = !e->callee || target.offset > e->callee->range.end.offset;
                        if (in_args)
                            result.enclosing_call = e;
                    }

                    for (auto* a : e->args)
                        if (a)
                            ast::RecursiveAstVisitor::visitExpr(a);
                    return;
                }

                if (expr->kind == ast::ExprKind::FieldAccess)
                {
                    auto* e = static_cast<ast::FieldAccessExpr const*>(expr);
                    if (e->object)
                        ast::RecursiveAstVisitor::visitExpr(e->object);

                    if (opts.include_exprs && range_contains(e->field_range, target))
                    {
                        result.expr = expr;
                        surface_expr_sema(expr, result);
                    }
                    if (!result.resolved_field && range_contains(e->field_range, target))
                    {
                        auto const* nominal = expr->sema.resolved_decl;
                        if (nominal)
                        {
                            result.resolved_field_parent = nominal;
                            auto const& field_name = e->field;
                            if (nominal->kind == ast::DeclKind::Struct)
                            {
                                auto const* sd = static_cast<ast::StructDecl const*>(nominal);
                                for (auto const& f : sd->fields)
                                    if (f.name == field_name)
                                    {
                                        result.resolved_field = &f;
                                        break;
                                    }
                            }
                            else if (nominal->kind == ast::DeclKind::Union)
                            {
                                auto const* ud = static_cast<ast::UnionDecl const*>(nominal);
                                for (auto const& f : ud->fields)
                                    if (f.name == field_name)
                                    {
                                        result.resolved_field = &f;
                                        break;
                                    }
                            }
                        }
                    }
                    return;
                }

                ast::RecursiveAstVisitor::visitExpr(expr);
            }

            void visitTypeExpr(ast::TypeExpr const* type_expr) override
            {
                if (!type_expr)
                    return;

                if (opts.include_type_exprs && range_contains(type_expr->range, target))
                {
                    result.type_expr = type_expr;
                    surface_type_sema(type_expr, result);
                }

                if (type_expr->kind == ast::TypeKind::Named)
                {
                    auto* t = static_cast<ast::NamedType const*>(type_expr);
                    if (!result.resolved_decl && result.scope && !t->path.is_empty())
                    {
                        auto const* sym = sema::resolve_type_path(*result.scope, t->path);
                        apply_resolved_type_symbol(result, sym);
                    }
                }

                ast::RecursiveAstVisitor::visitTypeExpr(type_expr);
            }
        };

    } // anonymous namespace

    sm::SourceRange decl_name_range(ast::Decl const* decl)
    {
        if (!decl)
            return {};
        switch (decl->kind)
        {
            case ast::DeclKind::Func:
                return static_cast<ast::FuncDecl const*>(decl)->name_range;
            case ast::DeclKind::Var:
                return static_cast<ast::VarDecl const*>(decl)->name_range;
            case ast::DeclKind::Struct:
                return static_cast<ast::StructDecl const*>(decl)->name_range;
            case ast::DeclKind::Union:
                return static_cast<ast::UnionDecl const*>(decl)->name_range;
            case ast::DeclKind::Enum:
                return static_cast<ast::EnumDecl const*>(decl)->name_range;
            case ast::DeclKind::Module:
                return static_cast<ast::ModuleDecl const*>(decl)->name_range;
            case ast::DeclKind::Import:
                return static_cast<ast::ImportDecl const*>(decl)->name_range;
            case ast::DeclKind::Using:
                return static_cast<ast::UsingDecl const*>(decl)->name_range;
            case ast::DeclKind::StaticIfGroup:
                return decl->range;
        }
        return {};
    }

    sm::SourceRange field_name_range(ast::FieldDecl const* fd)
    {
        if (!fd || fd->name.empty())
            return {};

        if (fd->name_range.valid())
            return fd->name_range;

        if (!fd->range.valid())
            return {};

        auto name_len = static_cast<sm::Offset>(fd->name.size());
        if (name_len > fd->range.byte_length())
            return fd->range;

        sm::SourceRange nr;
        nr.begin.fileId = fd->range.begin.fileId;
        nr.begin.offset = fd->range.end.offset - name_len;
        nr.end = fd->range.end;
        return nr;
    }

    std::optional<NodeAtLocation> find_node_at(session::CompilerSession const& session, sm::FileId file, sm::Position position, QueryOptions opts)
    {
        if (file == sm::FileId::Invalid)
        {
            return std::nullopt;
        }

        auto const* sf = session.source_manager().get(file);
        if (!sf)
        {
            return std::nullopt;
        }

        auto loc_result = session.source_manager().lsp_position_to_location(file, position);

        if (!loc_result)
            return std::nullopt;

        auto result = find_node_at(session, *loc_result, opts);
        if (result)
            result->position = position;

        return result;
    }

    bool file_in_module_graph(session::CompilerSession const& session, sm::FileId file)
    {
        auto const* sema_ctx = session.sema_context();
        if (!sema_ctx)
            return false;

        for (auto const& mod : const_cast<sema::SemaContext*>(sema_ctx)->graph().all())
            if (mod->file_id == file && mod->tu)
                return true;

        return false;
    }

    std::optional<NodeAtLocation> find_node_at(session::CompilerSession const& session, sm::Location location, QueryOptions opts)
    {
        auto* sema_ctx = session.sema_context();
        if (!sema_ctx)
        {
            return std::nullopt;
        }

        auto& graph = const_cast<sema::SemaContext*>(sema_ctx)->graph();

        sema::ModuleInfo const* module = nullptr;
        for (auto const& mod : graph.all())
        {
            if (mod->file_id == location.fileId)
            {
                module = mod.get();
                break;
            }
        }

        if (!module)
        {
            return std::nullopt;
        }

        if (!module->tu)
        {
            return std::nullopt;
        }

        sm::Position result_position{};
        auto pos_result = session.source_manager().location_to_lsp_position(location);
        if (pos_result)
            result_position = *pos_result;

        NodeAtLocation result;
        result.file = location.fileId;
        result.position = result_position;
        result.location = location;
        result.module = module;
        result.scope = module->own_scope;

        if (range_contains(module->tu->range, location))
            walk_translation_unit(module->tu, result, location, opts);

        if (!result.has_ast_node() && range_contains(module->tu->range, location))
        {
            QueryAllVisitor all_visitor(result, location, opts);

            if (module->tu->module_decl)
                all_visitor.visitDecl(module->tu->module_decl);

            for (auto* d : module->tu->imports)
                if (d)
                    all_visitor.visitDecl(d);

            for (auto* d : module->tu->decls)
                if (d)
                    all_visitor.visitDecl(d);
        }

        if (result.has_ast_node() && !result.resolved_decl && !result.hovered_decl && result.expr)
        {
            auto const* fd = result.enclosing_decl && result.enclosing_decl->kind == ast::DeclKind::Func
                                 ? static_cast<ast::FuncDecl const*>(result.enclosing_decl)
                                 : nullptr;

            if (fd && !fd->template_params.empty() && fd->body.has_value())
            {
                auto name = expr_simple_name(result.expr);
                if (!name.empty())
                {
                    for (auto const& p : fd->params)
                    {
                        if (p.name == name)
                        {
                            result.resolved_param = &p;
                            result.resolved_definition_range = func_param_name_range(p);
                            break;
                        }
                    }

                    if (!result.resolved_param && !result.resolved_decl)
                    {
                        auto const* local = find_local_var_in_block(*fd->body, name, location);
                        if (local)
                        {
                            result.resolved_decl = local;
                            result.resolved_definition_range = local->name_range;
                        }
                    }
                }
            }
        }

        if (result.has_ast_node())
        {
            auto const* fd = result.enclosing_decl && result.enclosing_decl->kind == ast::DeclKind::Func
                                 ? static_cast<ast::FuncDecl const*>(result.enclosing_decl)
                                 : nullptr;

            if (fd && !fd->template_params.empty())
            {
                auto& spec_reg = const_cast<sema::SemaContext*>(sema_ctx)->spec_registry();
                auto specs = spec_reg.specializations_of(fd);
                if (!specs.empty())
                {
                    auto const* spec = specs.front();
                    if (spec && spec->body.has_value())
                    {
                        borrow_sema_from_spec(result, *spec, location, opts, *fd);
                    }
                }
            }
        }

        return result;
    }

    namespace
    {
        struct DeclAnchor
        {
            sm::FileId file{sm::FileId::Invalid};
            sm::Offset name_offset{0};

            [[nodiscard]] bool valid() const noexcept { return file != sm::FileId::Invalid; }
        };

        [[nodiscard]] DeclAnchor decl_anchor(ast::Decl const* d)
        {
            if (!d)
                return {};

            auto nr = decl_name_range(d);
            if (nr.valid())
                return {nr.begin.fileId, nr.begin.offset};

            if (d->range.valid())
                return {d->range.begin.fileId, d->range.begin.offset};

            return {};
        }

        [[nodiscard]] SymbolId decl_symbol_id(ast::Decl const* d, SymbolKind kind)
        {
            SymbolId id;
            id.kind = kind;
            auto anchor = decl_anchor(d);
            id.file = anchor.file;
            id.name_offset = anchor.name_offset;
            return id;
        }

        [[nodiscard]] ast::Decl const* query_nominal_decl(types::Type const* ty)
        {
            if (!ty)
                return nullptr;

            switch (ty->kind)
            {
                case types::TypeKind::Struct: {
                    auto const* d = static_cast<types::StructType const*>(ty)->decl;
                    return reinterpret_cast<ast::Decl const*>(d);
                }
                case types::TypeKind::Union: {
                    auto const* d = static_cast<types::UnionType const*>(ty)->decl;
                    return reinterpret_cast<ast::Decl const*>(d);
                }
                case types::TypeKind::Enum: {
                    auto const* d = static_cast<types::EnumType const*>(ty)->decl;
                    return reinterpret_cast<ast::Decl const*>(d);
                }
                case types::TypeKind::Pointer:
                    return query_nominal_decl(static_cast<types::PointerType const*>(ty)->pointee);
                case types::TypeKind::Nominal:
                    return query_nominal_decl(static_cast<types::NominalType const*>(ty)->underlying);
                default:
                    return nullptr;
            }
        }

        [[nodiscard]] ast::FieldDecl const* query_find_field(ast::Decl const* d, std::string_view name)
        {
            if (auto const* sd = ast::node_cast<ast::StructDecl>(d))
            {
                for (auto const& f : sd->fields)
                    if (f.name == name)
                        return &f;
            }
            else if (auto const* ud = ast::node_cast<ast::UnionDecl>(d))
            {
                for (auto const& f : ud->fields)
                    if (f.name == name)
                        return &f;
            }
            return nullptr;
        }

        [[nodiscard]] std::uint32_t field_index_in(ast::Decl const* owner, ast::FieldDecl const* fd)
        {
            if (!owner || !fd)
                return 0;

            if (auto const* sd = ast::node_cast<ast::StructDecl>(owner))
            {
                for (std::size_t i = 0; i < sd->fields.size(); ++i)
                    if (&sd->fields[i] == fd)
                        return static_cast<std::uint32_t>(i);
            }
            else if (auto const* ud = ast::node_cast<ast::UnionDecl>(owner))
            {
                for (std::size_t j = 0; j < ud->fields.size(); ++j)
                    if (&ud->fields[j] == fd)
                        return static_cast<std::uint32_t>(j);
            }

            return 0;
        }

        [[nodiscard]] std::uint32_t variant_index_in(ast::EnumDecl const* owner, ast::EnumVariant const* v)
        {
            if (!owner || !v)
                return 0;

            for (std::size_t i = 0; i < owner->variants.size(); ++i)
                if (&owner->variants[i] == v)
                    return static_cast<std::uint32_t>(i);

            return 0;
        }

        [[nodiscard]] SymbolId field_symbol_id(ast::FieldDecl const* fd, ast::Decl const* owner)
        {
            SymbolId id;
            id.kind = SymbolKind::Field;
            if (fd && field_name_range(fd).valid())
            {
                id.file = field_name_range(fd).begin.fileId;
                id.name_offset = field_name_range(fd).begin.offset;
            }
            else if (fd && fd->range.valid())
            {
                id.file = fd->range.begin.fileId;
                id.name_offset = fd->range.begin.offset;
            }

            auto anchor = decl_anchor(owner);
            id.owner_file = anchor.file;
            id.owner_offset = anchor.name_offset;
            id.sub_index = field_index_in(owner, fd);
            return id;
        }

        [[nodiscard]] SymbolId variant_symbol_id(ast::EnumVariant const* v, ast::EnumDecl const* owner)
        {
            SymbolId id;
            id.kind = SymbolKind::EnumVariant;
            if (v && enum_variant_name_range(*v).valid())
            {
                id.file = enum_variant_name_range(*v).begin.fileId;
                id.name_offset = enum_variant_name_range(*v).begin.offset;
            }

            auto anchor = decl_anchor(owner);
            id.owner_file = anchor.file;
            id.owner_offset = anchor.name_offset;
            id.sub_index = variant_index_in(owner, v);
            return id;
        }

        [[nodiscard]] SymbolId param_symbol_id(ast::FuncDecl const* owner, std::size_t param_index)
        {
            SymbolId id;
            id.kind = SymbolKind::FuncParam;
            if (owner && param_index < owner->params.size())
            {
                auto nr = func_param_name_range(owner->params[param_index]);
                if (nr.valid())
                {
                    id.file = nr.begin.fileId;
                    id.name_offset = nr.begin.offset;
                }
            }

            auto anchor = decl_anchor(owner);
            id.owner_file = anchor.file;
            id.owner_offset = anchor.name_offset;
            id.sub_index = static_cast<std::uint32_t>(param_index);
            return id;
        }

        [[nodiscard]] SymbolId tparam_symbol_id(ast::Decl const* owner, ast::TemplateParam const* tp)
        {
            SymbolId id;
            id.kind = SymbolKind::TemplateParam;
            if (tp && template_param_name_range(*tp).valid())
            {
                id.file = template_param_name_range(*tp).begin.fileId;
                id.name_offset = template_param_name_range(*tp).begin.offset;
            }

            auto anchor = decl_anchor(owner);
            id.owner_file = anchor.file;
            id.owner_offset = anchor.name_offset;

            if (owner && tp)
            {
                auto index_of = [&](std::span<ast::TemplateParam const> params) {
                    for (std::size_t i = 0; i < params.size(); ++i)
                        if (&params[i] == tp)
                        {
                            id.sub_index = static_cast<std::uint32_t>(i);
                            return;
                        }
                };

                switch (owner->kind)
                {
                    case ast::DeclKind::Func:
                        index_of(static_cast<ast::FuncDecl const*>(owner)->template_params);
                        break;
                    case ast::DeclKind::Struct:
                        index_of(static_cast<ast::StructDecl const*>(owner)->template_params);
                        break;
                    case ast::DeclKind::Enum:
                        index_of(static_cast<ast::EnumDecl const*>(owner)->template_params);
                        break;
                    case ast::DeclKind::Using:
                        index_of(static_cast<ast::UsingDecl const*>(owner)->template_params);
                        break;
                    default:
                        break;
                }
            }

            return id;
        }

        [[nodiscard]] std::optional<std::size_t> param_index_for_synthetic(ast::FuncDecl const* owner, ast::Decl const* decl)
        {
            if (!owner || !decl || decl->kind != ast::DeclKind::Var)
                return std::nullopt;

            auto const* vd = static_cast<ast::VarDecl const*>(decl);
            for (std::size_t i = 0; i < owner->params.size(); ++i)
                if (owner->params[i].synthetic_decl == vd)
                    return i;

            return std::nullopt;
        }

        [[nodiscard]] ast::FuncDecl const* source_decl_of_spec(session::CompilerSession const& session, ast::FuncDecl const* spec)
        {
            if (!spec)
                return nullptr;

            auto* sema_ctx = const_cast<sema::SemaContext*>(session.sema_context());
            if (!sema_ctx)
                return spec;

            auto const* source = sema_ctx->spec_registry().source_decl_of(spec);
            return source ? source : spec;
        }

        struct LambdaParamRef
        {
            ast::LambdaExpr const* lambda{nullptr};
            std::size_t index{0};
        };

        [[nodiscard]] std::optional<LambdaParamRef> find_lambda_param_for_synthetic(session::CompilerSession const& session, ast::VarDecl const* vd)
        {
            if (!vd)
                return std::nullopt;

            auto* sema_ctx = const_cast<sema::SemaContext*>(session.sema_context());
            if (!sema_ctx)
                return std::nullopt;

            auto& graph = const_cast<sema::SemaContext*>(sema_ctx)->graph();
            for (auto const& mod : graph.all())
                if (mod)
                    for (auto* lf : mod->lambda_funcs)
                        if (lf && lf->lambda_source)
                            for (std::size_t i = 0; i < lf->params.size(); ++i)
                                if (lf->params[i].synthetic_decl == vd)
                                    return LambdaParamRef{lf->lambda_source, i};
            return std::nullopt;
        }

        [[nodiscard]] ast::FieldDecl const* resolve_field_access(ast::FieldAccessExpr const* fa, ast::Decl const** out_owner)
        {
            if (out_owner)
                *out_owner = nullptr;

            if (!fa)
                return nullptr;

            ast::Decl const* owner = fa->sema.resolved_decl;
            if (!owner && fa->object)
                owner = query_nominal_decl(sema::get_resolved_type(fa->object->sema));

            if (!owner)
                return nullptr;

            auto* field = query_find_field(owner, fa->field);
            if (field && out_owner)
                *out_owner = owner;

            return field;
        }

        [[nodiscard]] std::string_view expr_name_at(ast::Expr const* expr, sm::Location target)
        {
            if (!expr)
                return {};

            if (expr->kind == ast::ExprKind::Ident)
                return static_cast<ast::IdentExpr const*>(expr)->name;

            if (expr->kind == ast::ExprKind::PathExpr)
            {
                auto const* pe = static_cast<ast::PathExpr const*>(expr);
                for (auto const& seg : pe->path.segments)
                    if (seg.range.valid() && seg.range.begin.fileId == target.fileId && seg.range.begin.offset <= target.offset &&
                        target.offset < seg.range.end.offset)
                        return seg.name;
            }

            return {};
        }

        [[nodiscard]] sm::SourceRange expr_name_range_at(ast::Expr const* expr, sm::Location target)
        {
            if (!expr)
                return {};

            if (expr->kind == ast::ExprKind::Ident)
                return expr->range;

            if (expr->kind == ast::ExprKind::PathExpr)
            {
                auto const* pe = static_cast<ast::PathExpr const*>(expr);
                for (auto const& seg : pe->path.segments)
                    if (seg.range.valid() && seg.range.begin.fileId == target.fileId && seg.range.begin.offset <= target.offset &&
                        target.offset < seg.range.end.offset)
                        return seg.range;
            }

            return expr->range;
        }

        [[nodiscard]] bool expr_decl_matches(session::CompilerSession const& session, ast::Expr const* expr, ResolvedSymbol const& target)
        {
            if (!expr)
                return false;

            if (target.kind != SymbolKind::Declaration)
                return false;

            if (expr->sema.resolved_decl && expr->sema.resolved_decl == target.decl)
                return true;
            if (expr->sema.ufcs_callee && static_cast<ast::Decl const*>(expr->sema.ufcs_callee) == target.decl)
                return true;
            if (expr->sema.resolved_specialization)
            {
                auto const* source = source_decl_of_spec(session, expr->sema.resolved_specialization);
                if (source && static_cast<ast::Decl const*>(source) == target.decl)
                    return true;
            }

            return false;
        }

        [[nodiscard]] ast::EnumDecl const* find_variant_owner(session::CompilerSession const& session, ast::EnumVariant const* variant)
        {
            if (!variant)
                return nullptr;

            auto* sema_ctx = const_cast<sema::SemaContext*>(session.sema_context());
            if (!sema_ctx)
                return nullptr;

            auto& graph = const_cast<sema::SemaContext*>(sema_ctx)->graph();

            auto scan_decl = [&](auto& self, ast::Decl const* decl) -> ast::EnumDecl const* {
                if (!decl)
                    return nullptr;

                if (decl->kind == ast::DeclKind::Enum)
                {
                    auto const* ed = static_cast<ast::EnumDecl const*>(decl);
                    for (auto const& v : ed->variants)
                        if (&v == variant)
                            return ed;
                }

                if (decl->kind == ast::DeclKind::StaticIfGroup)
                {
                    auto const* sg = static_cast<ast::StaticIfGroup const*>(decl);
                    for (auto* d : sg->then_decls)
                        if (auto* found = self(self, d))
                            return found;
                }

                return nullptr;
            };

            for (auto const& mod : graph.all())
            {
                if (!mod || !mod->tu)
                    continue;

                for (auto* d : mod->tu->decls)
                    if (auto* found = scan_decl(scan_decl, d))
                        return found;
            }

            return nullptr;
        }

        [[nodiscard]] std::string_view decl_name_of(ast::Decl const* decl)
        {
            if (!decl)
                return {};

            switch (decl->kind)
            {
                case ast::DeclKind::Func:
                    return static_cast<ast::FuncDecl const*>(decl)->name;
                case ast::DeclKind::Var:
                    return static_cast<ast::VarDecl const*>(decl)->name;
                case ast::DeclKind::Struct:
                    return static_cast<ast::StructDecl const*>(decl)->name;
                case ast::DeclKind::Union:
                    return static_cast<ast::UnionDecl const*>(decl)->name;
                case ast::DeclKind::Enum:
                    return static_cast<ast::EnumDecl const*>(decl)->name;
                case ast::DeclKind::Using: {
                    auto const* ud = static_cast<ast::UsingDecl const*>(decl);
                    return ud->alias_path.is_empty() ? std::string_view{} : ud->alias_path.tail_name();
                }
                case ast::DeclKind::Module:
                    return static_cast<ast::ModuleDecl const*>(decl)->module_path.is_empty()
                               ? std::string_view{}
                               : static_cast<ast::ModuleDecl const*>(decl)->module_path.tail_name();
                case ast::DeclKind::Import:
                    return static_cast<ast::ImportDecl const*>(decl)->module_path.is_empty()
                               ? std::string_view{}
                               : static_cast<ast::ImportDecl const*>(decl)->module_path.tail_name();
                case ast::DeclKind::StaticIfGroup:
                    return {};
            }
            return {};
        }

        [[nodiscard]] bool range_less(sm::SourceRange const& a, sm::SourceRange const& b)
        {
            auto fa = static_cast<std::uint32_t>(a.begin.fileId);
            auto fb = static_cast<std::uint32_t>(b.begin.fileId);
            if (fa != fb)
                return fa < fb;
            if (a.begin.offset != b.begin.offset)
                return a.begin.offset < b.begin.offset;
            return a.end.offset < b.end.offset;
        }

        [[nodiscard]] bool range_equal(sm::SourceRange const& a, sm::SourceRange const& b)
        {
            return a.begin.fileId == b.begin.fileId && a.begin.offset == b.begin.offset && a.end.fileId == b.end.fileId && a.end.offset == b.end.offset;
        }

        void sort_dedup_ranges(std::vector<sm::SourceRange>& ranges)
        {
            std::ranges::sort(ranges, range_less);
            auto [first, last] = std::ranges::unique(ranges, range_equal);
            ranges.erase(first, last);
        }

        [[nodiscard]] bool is_legal_identifier(std::string_view name) noexcept
        {
            if (name.empty())
                return false;

            auto is_ident_start = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            auto is_ident_cont = [&](char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); };

            if (!is_ident_start(name.front()))
                return false;
            for (char c : name)
                if (!is_ident_cont(c))
                    return false;

            return dcc::lex::classify_identifier(name) == dcc::lex::TokenKind::Identifier;
        }

        [[nodiscard]] ast::FieldDecl const* symbol_field_of(ResolvedSymbol const& symbol)
        {
            if (symbol.kind != SymbolKind::Field || !symbol.owner_decl)
                return nullptr;

            if (auto const* sd = ast::node_cast<ast::StructDecl>(symbol.owner_decl))
            {
                if (symbol.sub_index < sd->fields.size())
                    return &sd->fields[symbol.sub_index];
            }
            else if (auto const* ud = ast::node_cast<ast::UnionDecl>(symbol.owner_decl))
            {
                if (symbol.sub_index < ud->fields.size())
                    return &ud->fields[symbol.sub_index];
            }

            return nullptr;
        }

    } // anonymous namespace

    std::optional<ResolvedSymbol> resolve_symbol_at(session::CompilerSession const& session, sm::FileId file, sm::Position position, ResolveOptions opts)
    {
        if (file == sm::FileId::Invalid)
            return std::nullopt;

        auto loc_result = session.source_manager().lsp_position_to_location(file, position);
        if (!loc_result)
            return std::nullopt;

        auto result = resolve_symbol_at(session, *loc_result, opts);
        return result;
    }

    std::optional<ResolvedSymbol> resolve_symbol_at(session::CompilerSession const& session, sm::Location location, ResolveOptions opts)
    {
        auto region = source_region_at(session, location.fileId, location.offset);
        if (region.in_string_or_comment())
            return std::nullopt;

        QueryOptions qopts;
        qopts.include_decls = opts.include_decls;
        qopts.include_stmts = opts.include_stmts;
        qopts.include_exprs = opts.include_exprs;
        qopts.include_type_exprs = opts.include_type_exprs;

        auto node = find_node_at(session, location, qopts);
        if (!node)
            return std::nullopt;

        ResolvedSymbol out;
        out.name_range = {};
        out.definition_range = {};

        if (node->resolved_field)
        {
            ast::Decl const* owner = node->resolved_field_parent;
            if (!owner && node->expr && node->expr->kind == ast::ExprKind::FieldAccess)
                owner = resolve_field_access(static_cast<ast::FieldAccessExpr const*>(node->expr), &owner) ? owner : nullptr;

            if (owner)
            {
                out.kind = SymbolKind::Field;
                out.name = node->resolved_field->name;
                out.definition_range = field_name_range(node->resolved_field);
                out.decl = owner;
                out.owner_decl = owner;
                out.sub_index = field_index_in(owner, node->resolved_field);
                out.id = field_symbol_id(node->resolved_field, owner);

                if (node->expr && node->expr->kind == ast::ExprKind::FieldAccess)
                    out.name_range = static_cast<ast::FieldAccessExpr const*>(node->expr)->field_range;
                else
                    out.name_range = field_name_range(node->resolved_field);

                return out;
            }
        }

        if (node->resolved_param)
        {
            auto const* fd =
                node->enclosing_decl && node->enclosing_decl->kind == ast::DeclKind::Func ? static_cast<ast::FuncDecl const*>(node->enclosing_decl) : nullptr;

            std::size_t param_index = 0;
            bool found = false;
            if (fd)
            {
                for (std::size_t i = 0; i < fd->params.size(); ++i)
                    if (&fd->params[i] == node->resolved_param)
                    {
                        param_index = i;
                        found = true;
                        break;
                    }
            }

            out.kind = SymbolKind::FuncParam;
            out.name = node->resolved_param->name;
            if (found && node->expr)
                out.name_range = expr_name_range_at(node->expr, location);
            else
                out.name_range = func_param_name_range(*node->resolved_param);
            out.definition_range = node->resolved_definition_range.valid() ? node->resolved_definition_range : out.name_range;

            if (found)
            {
                out.owner_decl = fd;
                out.sub_index = static_cast<std::uint32_t>(param_index);
                out.id = param_symbol_id(fd, param_index);
            }
            else if (node->resolved_lambda)
            {
                std::size_t lambda_param_index = 0;
                for (std::size_t i = 0; i < node->resolved_lambda->params.size(); ++i)
                    if (&node->resolved_lambda->params[i] == node->resolved_param)
                    {
                        lambda_param_index = i;
                        break;
                    }
                out.owner_decl = nullptr;
                out.sub_index = static_cast<std::uint32_t>(lambda_param_index);
                SymbolId id;
                id.kind = SymbolKind::FuncParam;
                if (out.name_range.valid())
                {
                    id.file = out.name_range.begin.fileId;
                    id.name_offset = out.name_range.begin.offset;
                }
                if (node->resolved_lambda->range.valid())
                {
                    id.owner_file = node->resolved_lambda->range.begin.fileId;
                    id.owner_offset = node->resolved_lambda->range.begin.offset;
                }
                id.sub_index = static_cast<std::uint32_t>(lambda_param_index);
                out.id = id;
            }
            return out;
        }

        if (node->resolved_tparam)
        {
            out.kind = SymbolKind::TemplateParam;
            out.name = node->resolved_tparam->name;
            out.name_range = template_param_name_range(*node->resolved_tparam);
            out.definition_range = out.name_range;
            out.decl = node->resolved_tparam_owner;
            out.owner_decl = node->resolved_tparam_owner;
            out.id = tparam_symbol_id(node->resolved_tparam_owner, node->resolved_tparam);
            return out;
        }

        if (node->resolved_variant)
        {
            ast::EnumDecl const* owner = node->resolved_variant_owner;
            if (!owner && node->module && node->module->tu)
                owner = find_variant_owner(session, node->resolved_variant);

            if (owner)
            {
                out.kind = SymbolKind::EnumVariant;
                out.name = node->resolved_variant->name;
                out.definition_range = enum_variant_name_range(*node->resolved_variant);
                out.decl = owner;
                out.owner_decl = owner;
                out.sub_index = variant_index_in(owner, node->resolved_variant);
                out.id = variant_symbol_id(node->resolved_variant, owner);

                if (node->expr)
                    out.name_range = expr_name_range_at(node->expr, location);
                else
                    out.name_range = enum_variant_name_range(*node->resolved_variant);

                return out;
            }
        }

        if (node->hovered_decl)
        {
            switch (node->hovered_decl->kind)
            {
                case ast::DeclKind::Using: {
                    auto const* ud = static_cast<ast::UsingDecl const*>(node->hovered_decl);
                    out.kind = SymbolKind::UsingAlias;
                    out.name = ud->alias_path.is_empty() ? std::string_view{} : ud->alias_path.tail_name();
                    out.name_range = decl_name_range(ud);
                    out.definition_range = out.name_range;
                    out.decl = ud;
                    out.id = decl_symbol_id(ud, SymbolKind::UsingAlias);
                    out.is_external_alias = true;
                    return out;
                }
                case ast::DeclKind::Import: {
                    auto const* id = static_cast<ast::ImportDecl const*>(node->hovered_decl);
                    out.kind = SymbolKind::ImportAlias;
                    out.name = id->module_path.is_empty() ? std::string_view{} : id->module_path.tail_name();
                    out.name_range = decl_name_range(id);
                    out.definition_range = out.name_range;
                    out.decl = id;
                    out.via_import = id;
                    out.is_module = true;
                    out.id = decl_symbol_id(id, SymbolKind::ImportAlias);
                    return out;
                }
                case ast::DeclKind::Module: {
                    auto const* md = static_cast<ast::ModuleDecl const*>(node->hovered_decl);
                    out.kind = SymbolKind::Module;
                    out.name = md->module_path.is_empty() ? std::string_view{} : md->module_path.tail_name();
                    out.name_range = decl_name_range(md);
                    out.definition_range = out.name_range;
                    out.decl = md;
                    out.is_module = true;
                    out.id = decl_symbol_id(md, SymbolKind::Module);
                    return out;
                }
                case ast::DeclKind::Struct:
                case ast::DeclKind::Union:
                case ast::DeclKind::Enum:
                case ast::DeclKind::Func:
                case ast::DeclKind::Var: {
                    out.kind = SymbolKind::Declaration;
                    out.name = decl_name_of(node->hovered_decl);
                    out.name_range = decl_name_range(node->hovered_decl);
                    out.definition_range = out.name_range;
                    out.decl = node->hovered_decl;
                    out.id = decl_symbol_id(node->hovered_decl, SymbolKind::Declaration);
                    return out;
                }
                default:
                    break;
            }
        }

        if (node->has_ast_node() && node->type_expr && node->type_expr->kind == ast::TypeKind::Named && node->resolved_decl)
        {
            auto const* nt = static_cast<ast::NamedType const*>(node->type_expr);
            sm::SourceRange seg_range = nt->range;
            for (auto const& seg : nt->path.segments)
            {
                if (seg.range.valid() && seg.range.begin.fileId == location.fileId && seg.range.begin.offset <= location.offset &&
                    location.offset < seg.range.end.offset)
                {
                    seg_range = seg.range;
                    break;
                }
            }

            auto const* target_type = node->resolved_decl;

            if (!out.is_external_alias && node->scope)
            {
                auto const* tsym = sema::resolve_type_path(*node->scope, nt->path);
                if (tsym && tsym->via_using && tsym->decl && tsym->decl == target_type)
                {
                    out.via_using = tsym->via_using;
                    out.is_external_alias = true;
                }
            }

            out.kind = SymbolKind::Declaration;
            out.name = decl_name_of(target_type);
            out.name_range = seg_range;
            out.decl = target_type;
            out.definition_range = decl_name_range(target_type);
            if (!out.definition_range.valid())
                out.definition_range = target_type->range;
            out.id = decl_symbol_id(target_type, SymbolKind::Declaration);
            return out;
        }

        if (node->has_ast_node() && node->expr)
        {
            if (node->resolved_variant)
            {
                ast::EnumDecl const* owner = node->resolved_variant_owner;
                if (!owner && node->module && node->module->tu)
                    owner = find_variant_owner(session, node->resolved_variant);
                if (owner)
                {
                    out.kind = SymbolKind::EnumVariant;
                    out.name = node->resolved_variant->name;
                    out.name_range = expr_name_range_at(node->expr, location);
                    out.definition_range = enum_variant_name_range(*node->resolved_variant);
                    out.decl = owner;
                    out.owner_decl = owner;
                    out.sub_index = variant_index_in(owner, node->resolved_variant);
                    out.id = variant_symbol_id(node->resolved_variant, owner);
                    return out;
                }
            }

            ast::Decl const* target = nullptr;
            ast::FuncDecl const* spec = nullptr;

            if (node->resolved_specialization)
            {
                auto const* source = source_decl_of_spec(session, node->resolved_specialization);
                if (source)
                {
                    target = source;
                    spec = node->resolved_specialization;
                    out.from_specialization = true;
                }
            }
            else if (node->ufcs_callee)
                target = node->ufcs_callee;
            else if (node->resolved_decl)
                target = node->resolved_decl;

            if (target && target->kind == ast::DeclKind::Var && static_cast<ast::VarDecl const*>(target)->sema.storage == ast::StorageClass::Param &&
                node->enclosing_decl && node->enclosing_decl->kind == ast::DeclKind::Func)
            {
                auto const* fd = static_cast<ast::FuncDecl const*>(node->enclosing_decl);
                if (auto idx = param_index_for_synthetic(fd, target))
                {
                    out.kind = SymbolKind::FuncParam;
                    out.name = fd->params[*idx].name;
                    out.name_range = expr_name_range_at(node->expr, location);
                    out.definition_range = func_param_name_range(fd->params[*idx]);
                    out.decl = fd;
                    out.owner_decl = fd;
                    out.sub_index = static_cast<std::uint32_t>(*idx);
                    out.id = param_symbol_id(fd, *idx);
                    return out;
                }
            }

            if (target && target->kind == ast::DeclKind::Var && static_cast<ast::VarDecl const*>(target)->sema.storage == ast::StorageClass::Param)
            {
                if (auto lpr = find_lambda_param_for_synthetic(session, static_cast<ast::VarDecl const*>(target)))
                {
                    auto const* l = lpr->lambda;
                    auto const& lp = l->params[lpr->index];
                    auto nr = func_param_name_range(lp);
                    if (!nr.valid())
                        nr = lp.range;
                    out.kind = SymbolKind::FuncParam;
                    out.name = lp.name;
                    out.name_range = expr_name_range_at(node->expr, location);
                    if (!out.name_range.valid())
                        out.name_range = nr;
                    out.definition_range = nr;
                    out.sub_index = static_cast<std::uint32_t>(lpr->index);
                    SymbolId id;
                    id.kind = SymbolKind::FuncParam;
                    id.file = nr.begin.fileId;
                    id.name_offset = nr.begin.offset;
                    if (l->range.valid())
                    {
                        id.owner_file = l->range.begin.fileId;
                        id.owner_offset = l->range.begin.offset;
                    }
                    id.sub_index = static_cast<std::uint32_t>(lpr->index);
                    out.id = id;
                    return out;
                }
            }

            if (target)
            {
                out.kind = SymbolKind::Declaration;
                out.name = decl_name_of(target);
                out.name_range = expr_name_range_at(node->expr, location);
                if (!out.name_range.valid())
                    out.name_range = node->expr->range;

                out.decl = target;
                out.specialization = spec;
                out.definition_range = decl_name_range(target);
                if (!out.definition_range.valid())
                    out.definition_range = target->range;
                out.id = decl_symbol_id(target, SymbolKind::Declaration);

                if (!out.is_external_alias && node->scope)
                {
                    auto name = expr_name_at(node->expr, location);
                    if (!name.empty())
                    {
                        ast::Path path(node->scope->allocator());
                        ast::PathSegment seg;
                        seg.name = name;
                        seg.range = out.name_range;
                        path.segments.push_back(seg);
                        path.range = out.name_range;

                        auto vs = sema::resolve_value_overloads(*node->scope, path);
                        for (auto const& sym : vs)
                        {
                            if (sym.via_using && sym.decl && sym.decl == target)
                            {
                                out.via_using = sym.via_using;
                                out.is_external_alias = true;
                                break;
                            }
                        }
                        if (!out.is_external_alias && target->kind != ast::DeclKind::Func && target->kind != ast::DeclKind::Var)
                        {
                            auto const* tsym = sema::resolve_type_path(*node->scope, path);
                            if (tsym && tsym->via_using && tsym->decl && tsym->decl == target)
                            {
                                out.via_using = tsym->via_using;
                                out.is_external_alias = true;
                            }
                        }
                    }
                }

                return out;
            }

            if (node->enclosing_decl && node->enclosing_decl->kind == ast::DeclKind::Func)
            {
                auto const* fd = static_cast<ast::FuncDecl const*>(node->enclosing_decl);

                if (node->resolved_param)
                {
                    std::size_t param_index = 0;
                    for (std::size_t i = 0; i < fd->params.size(); ++i)
                        if (&fd->params[i] == node->resolved_param)
                        {
                            param_index = i;
                            break;
                        }
                    out.kind = SymbolKind::FuncParam;
                    out.name = node->resolved_param->name;
                    out.name_range = expr_name_range_at(node->expr, location);
                    out.definition_range = func_param_name_range(*node->resolved_param);
                    out.owner_decl = fd;
                    out.sub_index = static_cast<std::uint32_t>(param_index);
                    out.id = param_symbol_id(fd, param_index);
                    return out;
                }

                auto name = expr_name_at(node->expr, location);
                if (!name.empty())
                {
                    for (std::size_t i = 0; i < fd->template_params.size(); ++i)
                        if (fd->template_params[i].name == name)
                        {
                            out.kind = SymbolKind::TemplateParam;
                            out.name = name;
                            out.name_range = expr_name_range_at(node->expr, location);
                            out.definition_range = template_param_name_range(fd->template_params[i]);
                            out.owner_decl = fd;
                            out.decl = fd;
                            out.id = tparam_symbol_id(fd, &fd->template_params[i]);
                            return out;
                        }

                    auto const* local = find_local_var_in_block(*fd->body, name, location);
                    if (local)
                    {
                        out.kind = SymbolKind::Declaration;
                        out.name = local->name;
                        out.name_range = expr_name_range_at(node->expr, location);
                        out.definition_range = local->name_range;
                        out.decl = local;
                        out.id = decl_symbol_id(local, SymbolKind::Declaration);
                        return out;
                    }
                }
            }
        }

        return std::nullopt;
    }

    namespace
    {
        struct SymbolReferenceCollector : ast::RecursiveAstVisitor
        {
            std::vector<sm::SourceRange>& out;
            ResolvedSymbol const& target;
            session::CompilerSession const& session;
            ast::Decl const* current_template_decl{nullptr};
            std::unordered_map<ast::VarDecl const*, SymbolId> m_lambda_param_ids;

            SymbolReferenceCollector(std::vector<sm::SourceRange>& o, ResolvedSymbol const& t, session::CompilerSession const& s)
                : out{o}, target{t}, session{s}
            {
            }

            void visitLambdaExpr(ast::LambdaExpr const* e) override
            {
                if (!e)
                    return;

                std::unordered_map<ast::VarDecl const*, SymbolId> local_ids;
                if (e->synthesized_func && target.kind == SymbolKind::FuncParam)
                {
                    auto const& synth = *e->synthesized_func;
                    for (std::size_t i = 0; i < e->params.size() && i < synth.params.size(); ++i)
                    {
                        auto const& lp = e->params[i];
                        auto nr = func_param_name_range(lp);
                        if (!nr.valid())
                            nr = lp.range;
                        SymbolId id;
                        id.kind = SymbolKind::FuncParam;
                        id.file = nr.begin.fileId;
                        id.name_offset = nr.begin.offset;
                        if (e->range.valid())
                        {
                            id.owner_file = e->range.begin.fileId;
                            id.owner_offset = e->range.begin.offset;
                        }
                        id.sub_index = static_cast<std::uint32_t>(i);
                        if (id == target.id && nr.valid())
                            out.push_back(nr);
                        if (auto* syn = synth.params[i].synthetic_decl)
                            local_ids.emplace(syn, id);
                    }
                }

                auto saved = std::move(m_lambda_param_ids);
                m_lambda_param_ids = std::move(local_ids);

                if (e->body)
                    visitExpr(e->body);

                m_lambda_param_ids = std::move(saved);
            }

            void visitDecl(ast::Decl const* decl) override
            {
                if (!decl)
                    return;

                auto* prev = current_template_decl;
                bool has_tparams = false;
                switch (decl->kind)
                {
                    case ast::DeclKind::Func:
                        has_tparams = !static_cast<ast::FuncDecl const*>(decl)->template_params.empty();
                        break;
                    case ast::DeclKind::Struct:
                        has_tparams = !static_cast<ast::StructDecl const*>(decl)->template_params.empty();
                        break;
                    case ast::DeclKind::Enum:
                        has_tparams = !static_cast<ast::EnumDecl const*>(decl)->template_params.empty();
                        break;
                    case ast::DeclKind::Using:
                        has_tparams = !static_cast<ast::UsingDecl const*>(decl)->template_params.empty();
                        break;
                    default:
                        break;
                }
                if (has_tparams)
                    current_template_decl = decl;

                ast::RecursiveAstVisitor::visitDecl(decl);

                current_template_decl = prev;
            }

            void visitPattern(ast::Pattern const* pat) override
            {
                if (!pat)
                    return;

                switch (pat->kind)
                {
                    case ast::PatternKind::EnumDestructure: {
                        auto const* p = static_cast<ast::EnumDestructurePattern const*>(pat);
                        if (target.kind == SymbolKind::EnumVariant && p->resolved_variant && target.owner_decl)
                        {
                            auto const* owner = ast::node_cast<ast::EnumDecl>(target.owner_decl);
                            if (owner)
                            {
                                std::size_t idx = 0;
                                for (std::size_t i = 0; i < owner->variants.size(); ++i)
                                    if (&owner->variants[i] == p->resolved_variant)
                                    {
                                        idx = i;
                                        break;
                                    }
                                if (target.sub_index == idx && target.id == variant_symbol_id(p->resolved_variant, owner) && !p->variant_path.segments.empty())
                                    out.push_back(p->variant_path.segments.back().range);
                            }
                        }
                        break;
                    }
                    case ast::PatternKind::StructDestructure: {
                        auto const* p = static_cast<ast::StructDestructurePattern const*>(pat);
                        if (target.kind == SymbolKind::Field && target.owner_decl)
                        {
                            for (auto const& f : p->fields)
                                if (f.range.valid() && f.resolved_field_index == target.sub_index)
                                    out.push_back(f.range);
                        }
                        break;
                    }
                    default:
                        break;
                }

                ast::RecursiveAstVisitor::visitPattern(pat);
            }

            void visitTypeExpr(ast::TypeExpr const* type_expr) override
            {
                if (!type_expr)
                    return;

                if (type_expr->kind == ast::TypeKind::Named)
                {
                    auto* t = static_cast<ast::NamedType const*>(type_expr);

                    if (target.kind == SymbolKind::TemplateParam)
                    {
                        if (target.owner_decl && current_template_decl == target.owner_decl && !t->path.is_empty() && t->path.is_simple() &&
                            t->path.simple_name() == target.name)
                        {
                            if (!t->path.segments.empty())
                                out.push_back(t->path.segments.back().range);
                        }
                    }

                    if (target.kind == SymbolKind::Declaration)
                    {
                        if (t->sema.resolved_decl && t->sema.resolved_decl == target.decl && !t->path.segments.empty())
                            out.push_back(t->path.segments.back().range);
                    }

                    for (auto const& ta : t->template_args)
                    {
                        if (ta.type)
                            visitTypeExpr(ta.type);
                        if (ta.expr)
                            visitExpr(ta.expr);
                    }
                    return;
                }

                ast::RecursiveAstVisitor::visitTypeExpr(type_expr);
            }

            void visitExpr(ast::Expr const* expr) override
            {
                if (!expr)
                    return;

                switch (expr->kind)
                {
                    case ast::ExprKind::Ident: {
                        if (target.kind == SymbolKind::Declaration && expr_decl_matches(session, expr, target))
                            out.push_back(expr->range);
                        else if (target.kind == SymbolKind::FuncParam)
                        {
                            auto const* fd = target.owner_decl && target.owner_decl->kind == ast::DeclKind::Func
                                                 ? static_cast<ast::FuncDecl const*>(target.owner_decl)
                                                 : nullptr;
                            if (fd)
                            {
                                auto idx = param_index_for_synthetic(fd, expr->sema.resolved_decl);
                                if (idx && *idx == target.sub_index)
                                    out.push_back(expr->range);
                            }
                            else if (expr->sema.resolved_decl && expr->sema.resolved_decl->kind == ast::DeclKind::Var)
                            {
                                auto lit = m_lambda_param_ids.find(static_cast<ast::VarDecl const*>(expr->sema.resolved_decl));
                                if (lit != m_lambda_param_ids.end() && lit->second == target.id)
                                    out.push_back(expr->range);
                            }
                        }
                        else if (target.kind == SymbolKind::TemplateParam && current_template_decl == target.owner_decl &&
                                 static_cast<ast::IdentExpr const*>(expr)->name == target.name)
                        {
                            out.push_back(expr->range);
                        }
                        break;
                    }
                    case ast::ExprKind::PathExpr: {
                        auto* e = static_cast<ast::PathExpr const*>(expr);
                        if (target.kind == SymbolKind::EnumVariant && e->sema.constructed_variant && target.owner_decl)
                        {
                            auto const* owner = ast::node_cast<ast::EnumDecl>(target.owner_decl);
                            if (owner)
                            {
                                std::size_t idx = 0;
                                for (std::size_t i = 0; i < owner->variants.size(); ++i)
                                    if (&owner->variants[i] == e->sema.constructed_variant)
                                    {
                                        idx = i;
                                        break;
                                    }
                                if (target.sub_index == idx && !e->path.segments.empty())
                                    out.push_back(e->path.segments.back().range);
                            }
                        }
                        else if (target.kind == SymbolKind::Declaration && expr_decl_matches(session, expr, target) && !e->path.segments.empty())
                        {
                            out.push_back(e->path.segments.back().range);
                        }

                        for (auto const& ta : e->explicit_enum_args)
                        {
                            if (ta.type)
                                visitTypeExpr(ta.type);
                            if (ta.expr)
                                visitExpr(ta.expr);
                        }
                        return;
                    }
                    case ast::ExprKind::Call: {
                        auto* e = static_cast<ast::CallExpr const*>(expr);
                        if (e->callee)
                        {
                            if (expr_decl_matches(session, expr, target))
                            {
                                auto* ce = e->callee;
                                while (ce && ce->kind == ast::ExprKind::TemplateInst)
                                    ce = static_cast<ast::TemplateInstExpr const*>(ce)->callee;

                                if (ce)
                                    push_callee_range(ce);
                            }
                            visitExpr(e->callee);
                        }
                        for (auto* a : e->args)
                            visitExpr(a);
                        return;
                    }
                    case ast::ExprKind::TemplateInst: {
                        auto* e = static_cast<ast::TemplateInstExpr const*>(expr);
                        if (e->callee)
                        {
                            if (expr_decl_matches(session, expr, target))
                            {
                                auto* ce = e->callee;
                                while (ce && ce->kind == ast::ExprKind::TemplateInst)
                                    ce = static_cast<ast::TemplateInstExpr const*>(ce)->callee;

                                if (ce)
                                    push_callee_range(ce);
                            }
                            visitExpr(e->callee);
                        }
                        for (auto const& ta : e->template_args)
                        {
                            if (ta.type)
                                visitTypeExpr(ta.type);
                            if (ta.expr)
                                visitExpr(ta.expr);
                        }
                        return;
                    }
                    case ast::ExprKind::FieldAccess: {
                        auto* e = static_cast<ast::FieldAccessExpr const*>(expr);
                        if (e->object)
                            visitExpr(e->object);

                        if (target.kind == SymbolKind::Field)
                        {
                            ast::Decl const* owner = nullptr;
                            auto const* field = resolve_field_access(e, &owner);
                            if (field && owner && target.owner_decl == owner)
                            {
                                auto id = field_symbol_id(field, owner);
                                if (id == target.id)
                                    out.push_back(e->field_range);
                            }
                        }
                        else if (target.kind == SymbolKind::Declaration && expr_decl_matches(session, expr, target))
                        {
                            out.push_back(e->field_range);
                        }
                        return;
                    }
                    case ast::ExprKind::StructLiteral: {
                        auto* e = static_cast<ast::StructLiteralExpr const*>(expr);
                        if (e->type)
                            visitTypeExpr(e->type);
                        if (target.kind == SymbolKind::Field)
                        {
                            for (auto const& f : e->fields)
                            {
                                if (f.name_range.valid() && f.resolved_field_index == target.sub_index && f.name == target.name)
                                {
                                    ast::Decl const* owner = nullptr;
                                    if (e->sema.resolved_decl)
                                        owner = e->sema.resolved_decl;
                                    else if (e->type)
                                        owner = query_nominal_decl(sema::get_canonical(e->type->sema));

                                    if (target.owner_decl == owner)
                                        out.push_back(f.name_range);
                                }
                            }
                        }
                        for (auto const& f : e->fields)
                            if (f.value)
                                visitExpr(f.value);
                        return;
                    }
                    case ast::ExprKind::Binary: {
                        auto* e = static_cast<ast::BinaryExpr const*>(expr);
                        if (e->lhs)
                            visitExpr(e->lhs);
                        if (e->rhs)
                            visitExpr(e->rhs);
                        return;
                    }
                    default:
                        ast::RecursiveAstVisitor::visitExpr(expr);
                        break;
                }
            }

            void push_callee_range(ast::Expr const* ce)
            {
                if (ce->kind == ast::ExprKind::Ident)
                    out.push_back(ce->range);
                else if (ce->kind == ast::ExprKind::PathExpr)
                {
                    auto* pe = static_cast<ast::PathExpr const*>(ce);
                    if (!pe->path.segments.empty())
                        out.push_back(pe->path.segments.back().range);
                }
                else if (ce->kind == ast::ExprKind::FieldAccess)
                {
                    auto* fa = static_cast<ast::FieldAccessExpr const*>(ce);
                    out.push_back(fa->field_range);
                }
            }
        };

    } // anonymous namespace

    std::vector<sm::SourceRange> find_symbol_references(session::CompilerSession const& session, ResolvedSymbol const& target, bool include_declaration)
    {
        std::vector<sm::SourceRange> ranges;

        if (!target.has_target())
        {
            return ranges;
        }

        auto* sema_ctx = session.sema_context();
        if (!sema_ctx)
        {
            return ranges;
        }

        auto& graph = const_cast<sema::SemaContext*>(sema_ctx)->graph();

        for (auto const& mod : graph.all())
        {
            if (!mod || !mod->tu)
                continue;

            SymbolReferenceCollector collector(ranges, target, session);
            collector.visitTranslationUnit(mod->tu);
        }

        if (include_declaration)
        {
            if (target.definition_range.valid())
                ranges.push_back(target.definition_range);
        }

        sort_dedup_ranges(ranges);

        return ranges;
    }

    namespace
    {
        struct ExtractionSink
        {
            std::vector<IndexedSymbolRecord>& symbols;
            std::vector<ReferenceOccurrence>& references;

            void add_reference(SymbolId target, sm::SourceRange range)
            {
                if (!target.valid() || !range.valid())
                    return;
                references.push_back(ReferenceOccurrence{target, range});
            }
        };

        [[nodiscard]] ast::Decl const* bulk_resolved_decl(session::CompilerSession const& session, ast::Expr const* expr)
        {
            if (!expr)
                return nullptr;

            if (expr->sema.resolved_decl)
                return expr->sema.resolved_decl;
            if (expr->sema.ufcs_callee)
                return static_cast<ast::Decl const*>(expr->sema.ufcs_callee);
            if (expr->sema.resolved_specialization)
                return static_cast<ast::Decl const*>(source_decl_of_spec(session, expr->sema.resolved_specialization));

            return nullptr;
        }

        [[nodiscard]] bool bulk_is_synthetic_param(ast::FuncDecl const* current_func, ast::Expr const* expr)
        {
            return current_func && expr && expr->sema.resolved_decl && param_index_for_synthetic(current_func, expr->sema.resolved_decl).has_value();
        }

        void collect_variant_ids(ast::Decl const* decl, std::unordered_map<ast::EnumVariant const*, SymbolId>& out)
        {
            if (!decl)
                return;

            if (decl->kind == ast::DeclKind::Enum)
            {
                auto const* ed = static_cast<ast::EnumDecl const*>(decl);
                for (auto const& v : ed->variants)
                    out.emplace(&v, variant_symbol_id(&v, ed));
            }
            else if (decl->kind == ast::DeclKind::StaticIfGroup)
            {
                auto const* sg = static_cast<ast::StaticIfGroup const*>(decl);
                for (auto* d : sg->then_decls)
                    collect_variant_ids(d, out);
            }
        }

        [[nodiscard]] SymbolId template_param_id_by_name(ast::Decl const* owner, std::string_view name)
        {
            if (!owner || name.empty())
                return {};

            auto index_of = [&](std::span<ast::TemplateParam const> params) -> SymbolId {
                for (std::size_t i = 0; i < params.size(); ++i)
                    if (params[i].name == name)
                        return tparam_symbol_id(owner, &params[i]);
                return {};
            };

            switch (owner->kind)
            {
                case ast::DeclKind::Func:
                    return index_of(static_cast<ast::FuncDecl const*>(owner)->template_params);
                case ast::DeclKind::Struct:
                    return index_of(static_cast<ast::StructDecl const*>(owner)->template_params);
                case ast::DeclKind::Enum:
                    return index_of(static_cast<ast::EnumDecl const*>(owner)->template_params);
                case ast::DeclKind::Using:
                    return index_of(static_cast<ast::UsingDecl const*>(owner)->template_params);
                default:
                    return {};
            }
        }

        struct BulkReferenceCollector : ast::RecursiveAstVisitor
        {
            ExtractionSink& sink;
            session::CompilerSession const& session;
            sema::Scope const* module_scope{nullptr};
            ast::Decl const* current_template_decl{nullptr};
            ast::FuncDecl const* current_func{nullptr};
            std::unordered_map<ast::EnumVariant const*, SymbolId> const& variant_ids;
            std::unordered_map<ast::VarDecl const*, SymbolId> m_lambda_param_ids;

            BulkReferenceCollector(ExtractionSink& s, session::CompilerSession const& sess, sema::Scope const* scope,
                                   std::unordered_map<ast::EnumVariant const*, SymbolId> const& vids)
                : sink{s}, session{sess}, module_scope{scope}, variant_ids{vids}
            {
            }

            void visitDecl(ast::Decl const* decl) override
            {
                if (!decl)
                    return;

                auto* prev_tdecl = current_template_decl;
                auto* prev_func = current_func;

                bool has_tparams = false;
                switch (decl->kind)
                {
                    case ast::DeclKind::Func:
                        has_tparams = !static_cast<ast::FuncDecl const*>(decl)->template_params.empty();
                        break;
                    case ast::DeclKind::Struct:
                        has_tparams = !static_cast<ast::StructDecl const*>(decl)->template_params.empty();
                        break;
                    case ast::DeclKind::Enum:
                        has_tparams = !static_cast<ast::EnumDecl const*>(decl)->template_params.empty();
                        break;
                    case ast::DeclKind::Using:
                        has_tparams = !static_cast<ast::UsingDecl const*>(decl)->template_params.empty();
                        break;
                    default:
                        break;
                }
                if (has_tparams)
                    current_template_decl = decl;
                if (decl->kind == ast::DeclKind::Func)
                    current_func = static_cast<ast::FuncDecl const*>(decl);

                ast::RecursiveAstVisitor::visitDecl(decl);

                current_template_decl = prev_tdecl;
                current_func = prev_func;
            }

            void visitPattern(ast::Pattern const* pat) override
            {
                if (!pat)
                    return;

                switch (pat->kind)
                {
                    case ast::PatternKind::EnumDestructure: {
                        auto const* p = static_cast<ast::EnumDestructurePattern const*>(pat);
                        if (p->resolved_variant && !p->variant_path.segments.empty())
                        {
                            auto it = variant_ids.find(p->resolved_variant);
                            if (it != variant_ids.end())
                                sink.add_reference(it->second, p->variant_path.segments.back().range);
                        }
                        break;
                    }
                    case ast::PatternKind::StructDestructure: {
                        auto const* p = static_cast<ast::StructDestructurePattern const*>(pat);
                        if (module_scope && !p->type_path.is_empty())
                        {
                            auto const* sym = sema::resolve_type_path(*module_scope, p->type_path);
                            ast::Decl const* owner = sym ? sym->decl : nullptr;
                            if (owner && owner->kind != ast::DeclKind::Struct && owner->kind != ast::DeclKind::Union)
                                owner = nullptr;

                            if (owner)
                            {
                                auto add_fields = [&](auto const* fields) {
                                    for (auto const& f : p->fields)
                                    {
                                        if (!f.range.valid() || f.resolved_field_index >= fields->size())
                                            continue;
                                        auto const& fd = (*fields)[f.resolved_field_index];
                                        if (fd.name == f.field_name)
                                            sink.add_reference(field_symbol_id(&fd, owner), f.range);
                                    }
                                };
                                if (auto const* sd = ast::node_cast<ast::StructDecl>(owner))
                                    add_fields(&sd->fields);
                                else if (auto const* ud = ast::node_cast<ast::UnionDecl>(owner))
                                    add_fields(&ud->fields);
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }

                ast::RecursiveAstVisitor::visitPattern(pat);
            }

            void visitTypeExpr(ast::TypeExpr const* type_expr) override
            {
                if (!type_expr)
                    return;

                if (type_expr->kind == ast::TypeKind::Named)
                {
                    auto* t = static_cast<ast::NamedType const*>(type_expr);

                    if (current_template_decl && !t->path.is_empty() && t->path.is_simple())
                    {
                        auto tp_id = template_param_id_by_name(current_template_decl, t->path.simple_name());
                        if (tp_id.valid() && !t->path.segments.empty())
                            sink.add_reference(tp_id, t->path.segments.back().range);
                    }

                    if (t->sema.resolved_decl && !t->path.segments.empty())
                        sink.add_reference(decl_symbol_id(t->sema.resolved_decl, SymbolKind::Declaration), t->path.segments.back().range);

                    for (auto const& ta : t->template_args)
                    {
                        if (ta.type)
                            visitTypeExpr(ta.type);
                        if (ta.expr)
                            visitExpr(ta.expr);
                    }
                    return;
                }

                ast::RecursiveAstVisitor::visitTypeExpr(type_expr);
            }

            void visitExpr(ast::Expr const* expr) override
            {
                if (!expr)
                    return;

                switch (expr->kind)
                {
                    case ast::ExprKind::Ident: {
                        auto const* id = static_cast<ast::IdentExpr const*>(expr);

                        if (expr->sema.resolved_decl && expr->sema.resolved_decl->kind == ast::DeclKind::Var)
                        {
                            auto lit = m_lambda_param_ids.find(static_cast<ast::VarDecl const*>(expr->sema.resolved_decl));
                            if (lit != m_lambda_param_ids.end())
                            {
                                sink.add_reference(lit->second, expr->range);
                                break;
                            }
                        }

                        if (!bulk_is_synthetic_param(current_func, expr))
                        {
                            if (auto const* d = bulk_resolved_decl(session, expr))
                                sink.add_reference(decl_symbol_id(d, SymbolKind::Declaration), expr->range);
                        }

                        if (current_func && expr->sema.resolved_decl)
                        {
                            if (auto idx = param_index_for_synthetic(current_func, expr->sema.resolved_decl))
                                sink.add_reference(param_symbol_id(current_func, *idx), expr->range);
                        }

                        if (current_template_decl)
                        {
                            auto tp_id = template_param_id_by_name(current_template_decl, id->name);
                            if (tp_id.valid())
                                sink.add_reference(tp_id, expr->range);
                        }
                        break;
                    }
                    case ast::ExprKind::PathExpr: {
                        auto* e = static_cast<ast::PathExpr const*>(expr);

                        if (e->sema.constructed_variant && !e->path.segments.empty())
                        {
                            auto it = variant_ids.find(e->sema.constructed_variant);
                            if (it != variant_ids.end())
                                sink.add_reference(it->second, e->path.segments.back().range);
                        }

                        if (!bulk_is_synthetic_param(current_func, expr))
                        {
                            if (auto const* d = bulk_resolved_decl(session, expr))
                                if (!e->path.segments.empty())
                                    sink.add_reference(decl_symbol_id(d, SymbolKind::Declaration), e->path.segments.back().range);
                        }

                        for (auto const& ta : e->explicit_enum_args)
                        {
                            if (ta.type)
                                visitTypeExpr(ta.type);
                            if (ta.expr)
                                visitExpr(ta.expr);
                        }
                        return;
                    }
                    case ast::ExprKind::Call: {
                        auto* e = static_cast<ast::CallExpr const*>(expr);
                        if (e->callee)
                        {
                            if (!bulk_is_synthetic_param(current_func, expr))
                            {
                                if (auto const* d = bulk_resolved_decl(session, expr))
                                {
                                    auto* ce = e->callee;
                                    while (ce && ce->kind == ast::ExprKind::TemplateInst)
                                        ce = static_cast<ast::TemplateInstExpr const*>(ce)->callee;

                                    if (ce)
                                        push_callee_range(ce, d);
                                }
                            }
                            visitExpr(e->callee);
                        }
                        for (auto* a : e->args)
                            visitExpr(a);
                        return;
                    }
                    case ast::ExprKind::TemplateInst: {
                        auto* e = static_cast<ast::TemplateInstExpr const*>(expr);
                        if (e->callee)
                        {
                            if (!bulk_is_synthetic_param(current_func, expr))
                            {
                                if (auto const* d = bulk_resolved_decl(session, expr))
                                {
                                    auto* ce = e->callee;
                                    while (ce && ce->kind == ast::ExprKind::TemplateInst)
                                        ce = static_cast<ast::TemplateInstExpr const*>(ce)->callee;

                                    if (ce)
                                        push_callee_range(ce, d);
                                }
                            }
                            visitExpr(e->callee);
                        }
                        for (auto const& ta : e->template_args)
                        {
                            if (ta.type)
                                visitTypeExpr(ta.type);
                            if (ta.expr)
                                visitExpr(ta.expr);
                        }
                        return;
                    }
                    case ast::ExprKind::FieldAccess: {
                        auto* e = static_cast<ast::FieldAccessExpr const*>(expr);
                        if (e->object)
                            visitExpr(e->object);

                        ast::Decl const* owner = nullptr;
                        auto const* field = resolve_field_access(e, &owner);
                        if (field && owner)
                            sink.add_reference(field_symbol_id(field, owner), e->field_range);

                        if (!bulk_is_synthetic_param(current_func, expr))
                        {
                            if (auto const* d = bulk_resolved_decl(session, expr))
                                sink.add_reference(decl_symbol_id(d, SymbolKind::Declaration), e->field_range);
                        }
                        return;
                    }
                    case ast::ExprKind::StructLiteral: {
                        auto* e = static_cast<ast::StructLiteralExpr const*>(expr);
                        if (e->type)
                            visitTypeExpr(e->type);

                        ast::Decl const* owner = e->sema.resolved_decl;
                        if (!owner && e->type)
                            owner = query_nominal_decl(sema::get_canonical(e->type->sema));

                        if (owner)
                        {
                            auto add_fields = [&](auto const* fields) {
                                for (auto const& f : e->fields)
                                {
                                    if (!f.name_range.valid() || f.resolved_field_index >= fields->size())
                                        continue;
                                    auto const& fd = (*fields)[f.resolved_field_index];
                                    if (fd.name == f.name)
                                        sink.add_reference(field_symbol_id(&fd, owner), f.name_range);
                                }
                            };
                            if (auto const* sd = ast::node_cast<ast::StructDecl>(owner))
                                add_fields(&sd->fields);
                            else if (auto const* ud = ast::node_cast<ast::UnionDecl>(owner))
                                add_fields(&ud->fields);
                        }

                        for (auto const& f : e->fields)
                            if (f.value)
                                visitExpr(f.value);
                        return;
                    }
                    case ast::ExprKind::Binary: {
                        auto* e = static_cast<ast::BinaryExpr const*>(expr);
                        if (e->lhs)
                            visitExpr(e->lhs);
                        if (e->rhs)
                            visitExpr(e->rhs);
                        return;
                    }
                    default:
                        ast::RecursiveAstVisitor::visitExpr(expr);
                        break;
                }
            }

            void visitLambdaExpr(ast::LambdaExpr const* e) override
            {
                if (!e)
                    return;

                std::unordered_map<ast::VarDecl const*, SymbolId> local_ids;
                if (e->synthesized_func)
                {
                    auto const& synth = *e->synthesized_func;
                    for (std::size_t i = 0; i < e->params.size() && i < synth.params.size(); ++i)
                    {
                        auto const& lp = e->params[i];
                        auto nr = func_param_name_range(lp);
                        if (!nr.valid())
                            nr = lp.range;
                        SymbolId id;
                        id.kind = SymbolKind::FuncParam;
                        id.file = nr.begin.fileId;
                        id.name_offset = nr.begin.offset;
                        if (e->range.valid())
                        {
                            id.owner_file = e->range.begin.fileId;
                            id.owner_offset = e->range.begin.offset;
                        }
                        id.sub_index = static_cast<std::uint32_t>(i);
                        if (nr.valid())
                            sink.add_reference(id, nr);
                        if (auto* syn = synth.params[i].synthetic_decl)
                            local_ids.emplace(syn, id);
                    }
                }

                auto saved_lambda_params = std::move(m_lambda_param_ids);
                m_lambda_param_ids = std::move(local_ids);

                if (e->body)
                    visitExpr(e->body);

                m_lambda_param_ids = std::move(saved_lambda_params);
            }

            void push_callee_range(ast::Expr const* ce, ast::Decl const* d)
            {
                auto id = decl_symbol_id(d, SymbolKind::Declaration);
                if (ce->kind == ast::ExprKind::Ident)
                    sink.add_reference(id, ce->range);
                else if (ce->kind == ast::ExprKind::PathExpr)
                {
                    auto* pe = static_cast<ast::PathExpr const*>(ce);
                    if (!pe->path.segments.empty())
                        sink.add_reference(id, pe->path.segments.back().range);
                }
                else if (ce->kind == ast::ExprKind::FieldAccess)
                {
                    auto* fa = static_cast<ast::FieldAccessExpr const*>(ce);
                    sink.add_reference(id, fa->field_range);
                }
            }
        };

        void add_decl_record(ast::Decl const* d, std::string_view container, std::string_view module_path, std::vector<IndexedSymbolRecord>& out)
        {
            auto name = decl_name_of(d);
            if (name.empty())
                return;

            auto nr = decl_name_range(d);
            if (!nr.valid())
                nr = d->range;

            IndexedSymbolRecord rec;
            rec.kind = (d->kind == ast::DeclKind::Using) ? SymbolKind::UsingAlias : SymbolKind::Declaration;
            rec.id = decl_symbol_id(d, rec.kind);
            rec.name = std::string{name};
            rec.name_range = nr;
            rec.definition_range = nr;
            rec.container = std::string{container};
            rec.module_path = std::string{module_path};
            rec.decl_kind = static_cast<std::uint8_t>(d->kind);
            rec.is_renameable = is_legal_identifier(name) && rec.kind != SymbolKind::UsingAlias;
            out.push_back(std::move(rec));
        }

        void add_field_record(ast::FieldDecl const& f, ast::Decl const* owner, std::string_view container, std::string_view module_path,
                              std::vector<IndexedSymbolRecord>& out)
        {
            if (f.name.empty())
                return;

            auto nr = field_name_range(&f);
            if (!nr.valid())
                return;

            IndexedSymbolRecord rec;
            rec.kind = SymbolKind::Field;
            rec.id = field_symbol_id(&f, owner);
            rec.name = std::string{f.name};
            rec.name_range = nr;
            rec.definition_range = nr;
            rec.container = std::string{container};
            rec.module_path = std::string{module_path};
            rec.is_renameable = is_legal_identifier(f.name);
            out.push_back(std::move(rec));
        }

        void add_variant_record(ast::EnumVariant const& v, ast::EnumDecl const* owner, std::string_view container, std::string_view module_path,
                                std::vector<IndexedSymbolRecord>& out)
        {
            if (v.name.empty())
                return;

            auto nr = enum_variant_name_range(v);
            if (!nr.valid())
                return;

            IndexedSymbolRecord rec;
            rec.kind = SymbolKind::EnumVariant;
            rec.id = variant_symbol_id(&v, owner);
            rec.name = std::string{v.name};
            rec.name_range = nr;
            rec.definition_range = nr;
            rec.container = std::string{container};
            rec.module_path = std::string{module_path};
            rec.is_renameable = is_legal_identifier(v.name);
            out.push_back(std::move(rec));
        }

        void collect_module_symbols(ast::TranslationUnit const* tu, std::string_view module_path, std::vector<IndexedSymbolRecord>& out)
        {
            for (auto* d : tu->decls)
            {
                if (!d)
                    continue;

                switch (d->kind)
                {
                    case ast::DeclKind::Struct:
                    case ast::DeclKind::Union:
                    case ast::DeclKind::Enum:
                    case ast::DeclKind::Func:
                    case ast::DeclKind::Var:
                    case ast::DeclKind::Using: {
                        auto decl_name = decl_name_of(d);
                        add_decl_record(d, module_path, module_path, out);
                        (void)decl_name;

                        if (d->kind == ast::DeclKind::Struct || d->kind == ast::DeclKind::Union)
                        {
                            auto add_fields = [&](auto const* fields) {
                                for (auto const& f : *fields)
                                    add_field_record(f, d, decl_name, module_path, out);
                            };
                            if (auto const* sd = ast::node_cast<ast::StructDecl>(d))
                                add_fields(&sd->fields);
                            else if (auto const* ud = ast::node_cast<ast::UnionDecl>(d))
                                add_fields(&ud->fields);
                        }
                        else if (d->kind == ast::DeclKind::Enum)
                        {
                            auto const* ed = static_cast<ast::EnumDecl const*>(d);
                            for (auto const& v : ed->variants)
                                add_variant_record(v, ed, decl_name, module_path, out);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        [[nodiscard]] std::string module_display_name(sema::ModuleInfo const& mod)
        {
            if (mod.tu && mod.tu->module_decl)
            {
                auto const* md = static_cast<ast::ModuleDecl const*>(mod.tu->module_decl);
                std::string s;
                for (std::size_t i = 0; i < md->module_path.segments.size(); ++i)
                {
                    if (i > 0)
                        s += "::";
                    s += md->module_path.segments[i].name;
                }
                if (!s.empty())
                    return s;
            }
            return mod.canonical_path.str();
        }

    } // anonymous namespace

    WorkspaceExtraction extract_workspace(session::CompilerSession const& session, std::unordered_map<std::string, std::uint64_t> const& skip_revisions)
    {
        WorkspaceExtraction out;

        auto* sema_ctx = const_cast<sema::SemaContext*>(session.sema_context());
        if (!sema_ctx)
            return out;

        auto& graph = const_cast<sema::SemaContext*>(sema_ctx)->graph();
        auto& sm = const_cast<sm::SourceManager&>(session.source_manager());

        std::unordered_map<ast::EnumVariant const*, SymbolId> variant_ids;
        for (auto const& mod : graph.all())
            if (mod && mod->tu)
                for (auto* d : mod->tu->decls)
                    collect_variant_ids(d, variant_ids);

        for (auto const& mod : graph.all())
        {
            if (!mod)
                continue;

            ExtractedModule em;
            em.canonical_path = mod->canonical_path.str();
            em.file_path = mod->file_path.string();
            em.file_id = mod->file_id;
            em.content_revision = sm.content_revision(mod->file_id);

            for (auto const& binding : mod->imports)
                if (binding.target)
                    em.imports.push_back(binding.target->canonical_path.str());

            auto skip_it = skip_revisions.find(em.canonical_path);
            if (skip_it != skip_revisions.end() && skip_it->second == em.content_revision)
            {
                em.skipped = true;
                out.modules.push_back(std::move(em));
                continue;
            }

            if (mod->tu)
            {
                auto container = module_display_name(*mod);
                ExtractionSink sink{em.symbols, em.references};
                BulkReferenceCollector collector{sink, session, mod->own_scope, variant_ids};
                collector.visitTranslationUnit(mod->tu);

                collect_module_symbols(mod->tu, container, em.symbols);

                std::ranges::sort(em.references, [](ReferenceOccurrence const& a, ReferenceOccurrence const& b) {
                    auto fa = static_cast<std::uint32_t>(a.target.file);
                    auto fb = static_cast<std::uint32_t>(b.target.file);
                    if (fa != fb)
                        return fa < fb;
                    if (a.target.name_offset != b.target.name_offset)
                        return a.target.name_offset < b.target.name_offset;
                    return range_less(a.range, b.range);
                });
                auto [first, last] = std::ranges::unique(em.references, [](ReferenceOccurrence const& a, ReferenceOccurrence const& b) {
                    return a.target == b.target && range_equal(a.range, b.range);
                });
                em.references.erase(first, last);
            }

            out.modules.push_back(std::move(em));
        }

        return out;
    }

    std::vector<IndexedSymbolRecord> extract_file_symbols(session::CompilerSession& session, sm::FileId file)
    {
        std::vector<IndexedSymbolRecord> out;

        auto const* sf = session.source_manager().get(file);
        if (!sf)
            return out;

        auto* tu = session.parse_file(file);
        if (!tu)
            return out;

        std::string container;
        if (tu->module_decl)
        {
            auto const* md = static_cast<ast::ModuleDecl const*>(tu->module_decl);
            for (std::size_t i = 0; i < md->module_path.segments.size(); ++i)
            {
                if (i > 0)
                    container += "::";
                container += md->module_path.segments[i].name;
            }
        }
        if (container.empty())
            container = sf->path().filename().string();

        collect_module_symbols(tu, container, out);
        return out;
    }

    sm::SourceRange symbol_name_range(ResolvedSymbol const& symbol) noexcept
    {
        return symbol.name_range;
    }

    bool can_rename_symbol(ResolvedSymbol const& symbol) noexcept
    {
        if (!symbol.is_renameable())
            return false;

        return is_legal_identifier(symbol.name);
    }

    std::string symbol_display_name(ResolvedSymbol const& symbol)
    {
        switch (symbol.kind)
        {
            case SymbolKind::Field: {
                auto const* fd = symbol_field_of(symbol);
                if (!fd)
                    return std::string{symbol.name};
                if (fd->type && fd->type->sema.canonical)
                    return std::format("{} {}", sema::format_dcc_type(sema::get_canonical(fd->type->sema)), fd->name);
                return std::string{fd->name};
            }
            case SymbolKind::FuncParam: {
                auto const* fd =
                    symbol.owner_decl && symbol.owner_decl->kind == ast::DeclKind::Func ? static_cast<ast::FuncDecl const*>(symbol.owner_decl) : nullptr;
                if (!fd || symbol.sub_index >= fd->params.size())
                    return std::string{symbol.name};
                auto const& p = fd->params[symbol.sub_index];
                if (p.type && p.type->sema.canonical)
                    return std::format("{} {}", sema::format_dcc_type(sema::get_canonical(p.type->sema)), p.name);
                if (p.name.empty())
                    return "<unresolved>";
                return std::string{p.name};
            }
            case SymbolKind::TemplateParam:
                return std::format("template parameter {}", symbol.name.empty() ? std::string_view{"<unnamed>"} : symbol.name);
            case SymbolKind::EnumVariant: {
                std::string_view enum_name = "<enum>";
                if (symbol.owner_decl && symbol.owner_decl->kind == ast::DeclKind::Enum)
                    enum_name = static_cast<ast::EnumDecl const*>(symbol.owner_decl)->name;
                return std::format("{}::{}", enum_name, symbol.name);
            }
            case SymbolKind::UsingAlias: {
                auto const* ud = symbol.decl ? ast::node_cast<ast::UsingDecl>(symbol.decl) : nullptr;
                if (!ud)
                    return std::string{symbol.name};
                std::string target;
                if (ud->target_type && ud->target_type->sema.canonical)
                    target = sema::format_dcc_type(sema::get_canonical(ud->target_type->sema));
                else if (!ud->target_path.is_empty())
                {
                    for (std::size_t i = 0; i < ud->target_path.segments.size(); ++i)
                    {
                        if (i > 0)
                            target += "::";
                        target += ud->target_path.segments[i].name;
                    }
                }
                else
                    target = "<unknown>";
                return std::format("using {} = {}", symbol.name, target);
            }
            case SymbolKind::Module:
            case SymbolKind::ImportAlias:
                return std::format("module {}", symbol.name);
            case SymbolKind::Declaration:
                break;
            default:
                return std::string{symbol.name};
        }

        auto const* target = symbol.decl;
        if (!target)
            return std::string{symbol.name};

        switch (target->kind)
        {
            case ast::DeclKind::Func: {
                auto const* fd = static_cast<ast::FuncDecl const*>(target);
                std::string ret = "void";
                if (fd->return_type && fd->return_type->sema.canonical)
                    ret = sema::format_dcc_type(sema::get_canonical(fd->return_type->sema));
                std::string sig = std::format("{} {}(", ret, fd->name);
                for (std::size_t i = 0; i < fd->params.size(); ++i)
                {
                    if (i > 0)
                        sig += ", ";
                    if (fd->params[i].type && fd->params[i].type->sema.canonical)
                        sig += std::format("{} {}", sema::format_dcc_type(sema::get_canonical(fd->params[i].type->sema)), fd->params[i].name);
                    else if (!fd->params[i].name.empty())
                        sig += fd->params[i].name;
                }
                sig += ")";
                return sig;
            }
            case ast::DeclKind::Var: {
                auto const* vd = static_cast<ast::VarDecl const*>(target);
                if (vd->type && vd->type->sema.canonical)
                    return std::format("{} {}", sema::format_dcc_type(sema::get_canonical(vd->type->sema)), vd->name);
                return std::string{vd->name};
            }
            case ast::DeclKind::Struct:
                return std::format("struct {}", static_cast<ast::StructDecl const*>(target)->name);
            case ast::DeclKind::Union:
                return std::format("union {}", static_cast<ast::UnionDecl const*>(target)->name);
            case ast::DeclKind::Enum:
                return std::format("enum {}", static_cast<ast::EnumDecl const*>(target)->name);
            default:
                return std::string{symbol.name};
        }
    }

    namespace
    {
        struct LocalCollector
        {
            session::CompilerSession const& session;
            sm::Location target;
            LocalContext& out;
            std::uint32_t enclosing_block_depth{0};

            [[nodiscard]] bool contains(sm::SourceRange const& range) const noexcept
            {
                if (!range.valid())
                    return false;

                if (range.begin.fileId != target.fileId)
                    return false;

                return range.begin.offset <= target.offset && target.offset <= range.end.offset;
            }

            void run()
            {
                auto* sema_ctx = const_cast<sema::SemaContext*>(session.sema_context());
                if (!sema_ctx)
                    return;

                auto& graph = const_cast<sema::SemaContext*>(sema_ctx)->graph();
                for (auto const& mod : graph.all())
                    if (mod->file_id == target.fileId)
                    {
                        out.module = mod.get();
                        break;
                    }

                if (!out.module || !out.module->tu)
                    return;

                out.scope = out.module->own_scope;

                ast::FuncDecl const* enclosing = nullptr;
                std::size_t enclosing_len = 0;
                for (auto* d : out.module->tu->decls)
                    consider_decl(d, enclosing, enclosing_len);

                if (enclosing)
                {
                    out.enclosing_decl = enclosing;
                    out.enclosing_func = enclosing;
                    add_params(enclosing);
                    if (enclosing->body.has_value())
                        visit_block(*enclosing->body, 0);
                }

                std::ranges::stable_sort(out.locals, [](LocalSymbolInfo const& a, LocalSymbolInfo const& b) {
                    if (a.depth != b.depth)
                        return a.depth > b.depth;
                    return a.name_offset > b.name_offset;
                });
            }

            void consider_decl(ast::Decl const* decl, ast::FuncDecl const*& enclosing, std::size_t& enclosing_len)
            {
                if (!decl)
                    return;

                if (decl->kind == ast::DeclKind::Func)
                {
                    auto const* fd = static_cast<ast::FuncDecl const*>(decl);
                    if (fd->body.has_value() && contains(fd->body->range))
                    {
                        auto len = static_cast<std::size_t>(fd->body->range.end.offset - fd->body->range.begin.offset);
                        if (!enclosing || len < enclosing_len)
                        {
                            enclosing = fd;
                            enclosing_len = len;
                        }
                    }
                }
                else if (decl->kind == ast::DeclKind::StaticIfGroup)
                {
                    auto const* sg = static_cast<ast::StaticIfGroup const*>(decl);
                    for (auto* d : sg->then_decls)
                        consider_decl(d, enclosing, enclosing_len);
                    if (sg->else_group)
                        consider_decl(sg->else_group, enclosing, enclosing_len);
                }
            }

            void add_local(std::string_view name, LocalSymbolKind kind, std::uint32_t depth, sm::Offset name_offset, types::Type const* ty,
                           ast::VarDecl const* vd = nullptr, ast::FuncDecl const* param_owner = nullptr, std::uint32_t param_index = 0, bool is_pack = false)
            {
                if (name.empty())
                    return;

                LocalSymbolInfo li;
                li.name = name;
                li.kind = kind;
                li.depth = depth;
                li.var_decl = vd;
                li.param_owner = param_owner;
                li.param_index = param_index;
                li.type = ty;
                li.name_offset = name_offset;
                li.is_pack = is_pack;
                out.locals.push_back(std::move(li));
            }

            void add_var(ast::VarDecl const* vd, std::uint32_t depth)
            {
                types::Type const* ty = nullptr;
                if (vd->type && vd->type->sema.canonical)
                    ty = sema::get_canonical(vd->type->sema);
                else if (vd->init && vd->init->sema.resolved_type)
                    ty = sema::get_resolved_type(vd->init->sema);

                auto name_offset = vd->name_range.valid() ? vd->name_range.begin.offset : vd->range.begin.offset;
                add_local(vd->name, LocalSymbolKind::Variable, depth, name_offset, ty, vd);
            }

            void add_params(ast::FuncDecl const* fd)
            {
                for (std::size_t i = 0; i < fd->params.size(); ++i)
                {
                    auto const& p = fd->params[i];
                    if (p.name.empty())
                        continue;

                    types::Type const* ty = nullptr;
                    if (p.type && p.type->sema.canonical)
                        ty = sema::get_canonical(p.type->sema);

                    sm::Offset name_offset = 0;
                    if (p.range.valid() && p.range.end.offset > static_cast<sm::Offset>(p.name.size()))
                        name_offset = p.range.end.offset - static_cast<sm::Offset>(p.name.size());
                    else if (p.range.valid())
                        name_offset = p.range.begin.offset;

                    add_local(p.name, LocalSymbolKind::Parameter, 0, name_offset, ty, nullptr, fd, static_cast<std::uint32_t>(i), p.is_pack);
                }

                for (auto const& tp : fd->template_params)
                {
                    if (tp.name.empty())
                        continue;

                    add_local(tp.name, LocalSymbolKind::TemplateParam, 0, tp.range.valid() ? tp.range.begin.offset : 0, nullptr, nullptr, fd, 0, tp.is_pack);
                }
            }

            void visit_block(ast::Block const& block, std::uint32_t depth)
            {
                if (!contains(block.range))
                    return;

                if (!out.enclosing_block || depth > enclosing_block_depth)
                {
                    out.enclosing_block = &block;
                    enclosing_block_depth = depth;
                }

                for (auto* s : block.stmts)
                    visit_stmt(s, depth);
                if (block.tail)
                    visit_expr_locals(block.tail, depth);
            }

            void visit_stmt(ast::Stmt const* stmt, std::uint32_t depth)
            {
                if (!stmt)
                    return;

                using ast::StmtKind;
                switch (stmt->kind)
                {
                    case StmtKind::DeclStmt: {
                        auto const* ds = static_cast<ast::DeclStmt const*>(stmt);
                        if (ds->decl && ds->decl->kind == ast::DeclKind::Var)
                        {
                            auto const* vd = static_cast<ast::VarDecl const*>(ds->decl);
                            if (!vd->name.empty() && (!vd->name_range.valid() || vd->name_range.begin.offset < target.offset))
                                add_var(vd, depth);
                        }
                        break;
                    }
                    case StmtKind::Expr: {
                        auto const* s = static_cast<ast::ExprStmt const*>(stmt);
                        if (s->expr)
                            visit_expr_locals(s->expr, depth);
                        break;
                    }
                    case StmtKind::While: {
                        auto const* s = static_cast<ast::WhileStmt const*>(stmt);
                        visit_block(s->body, depth + 1);
                        break;
                    }
                    case StmtKind::DoWhile: {
                        auto const* s = static_cast<ast::DoWhileStmt const*>(stmt);
                        visit_block(s->body, depth + 1);
                        break;
                    }
                    case StmtKind::For: {
                        auto const* s = static_cast<ast::ForStmt const*>(stmt);
                        if (s->init)
                            visit_stmt(s->init, depth);
                        visit_block(s->body, depth + 1);
                        break;
                    }
                    case StmtKind::ForIn: {
                        auto const* s = static_cast<ast::ForInStmt const*>(stmt);
                        if (!s->item_name.empty())
                        {
                            auto name_offset = s->name_range.valid() ? s->name_range.begin.offset : s->range.begin.offset;
                            add_local(s->item_name, LocalSymbolKind::Variable, depth + 1, name_offset,
                                      reinterpret_cast<types::Type const*>(s->resolved_item_type));
                        }
                        visit_block(s->body, depth + 1);
                        break;
                    }
                    case StmtKind::Defer: {
                        auto const* s = static_cast<ast::DeferStmt const*>(stmt);
                        if (s->body)
                            visit_stmt(s->body, depth);
                        break;
                    }
                    case StmtKind::StaticIf: {
                        auto const* s = static_cast<ast::StaticIfStmt const*>(stmt);
                        visit_block(s->then_block, depth + 1);
                        if (s->else_branch)
                            visit_stmt(s->else_branch, depth);
                        break;
                    }
                    case StmtKind::StaticMatch: {
                        auto const* s = static_cast<ast::StaticMatchStmt const*>(stmt);
                        for (auto const& arm : s->arms)
                            if (arm.body)
                                visit_expr_locals(arm.body, depth);
                        break;
                    }
                    case StmtKind::Ambiguous: {
                        auto const* s = static_cast<ast::AmbiguousStmt const*>(stmt);
                        if (s->as_decl && s->as_decl->kind == ast::DeclKind::Var)
                        {
                            auto const* vd = static_cast<ast::VarDecl const*>(s->as_decl);
                            if (!vd->name.empty() && (!vd->name_range.valid() || vd->name_range.begin.offset < target.offset))
                                add_var(vd, depth);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            void visit_expr_locals(ast::Expr const* expr, std::uint32_t depth)
            {
                if (!expr)
                    return;

                using ast::ExprKind;
                if (expr->kind == ExprKind::Block)
                {
                    auto const* b = static_cast<ast::BlockExpr const*>(expr);
                    visit_block(b->body, depth + 1);
                }
                else if (expr->kind == ExprKind::If)
                {
                    auto const* e = static_cast<ast::IfExpr const*>(expr);
                    visit_block(e->then_block, depth + 1);
                    if (e->else_branch)
                        visit_expr_locals(e->else_branch, depth);
                }
                else if (expr->kind == ExprKind::Match)
                {
                    auto const* e = static_cast<ast::MatchExpr const*>(expr);
                    for (auto const& arm : e->arms)
                        if (arm.body)
                            visit_expr_locals(arm.body, depth);
                }
            }
        };

    } // anonymous namespace

    LocalContext collect_local_context(session::CompilerSession const& session, sm::FileId file, sm::Location location)
    {
        LocalContext out;
        out.location = location;

        auto const* sf = session.source_manager().get(file);
        if (!sf)
            return out;

        LocalCollector collector{session, location, out};
        collector.run();
        return out;
    }

    namespace
    {
        [[nodiscard]] std::vector<lex::Token> lex_up_to(lex::Lexer& lexer, std::uint32_t cursor)
        {
            std::vector<lex::Token> out;
            for (;;)
            {
                auto tok = lexer.next();
                if (tok.kind == lex::TokenKind::Eof)
                    break;
                if (tok.range.begin.offset >= cursor)
                    break;
                out.push_back(tok);
                if (tok.range.end.offset >= cursor)
                    break;
            }
            return out;
        }

        [[nodiscard]] std::optional<std::size_t> find_call_open_paren(std::vector<lex::Token> const& tokens, ast::CallExpr const* call,
                                                                      std::uint32_t callee_end)
        {
            std::uint32_t upper = static_cast<std::uint32_t>(call->range.end.offset);
            if (!call->args.empty() && call->args.front())
            {
                auto first_arg_begin = static_cast<std::uint32_t>(call->args.front()->range.begin.offset);
                upper = std::min(first_arg_begin, upper);
            }

            std::optional<std::size_t> result;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].kind == lex::TokenKind::LParen && tokens[i].range.begin.offset >= callee_end && tokens[i].range.begin.offset < upper)
                    result = i;
            }
            return result;
        }

        [[nodiscard]] std::optional<std::size_t> innermost_open_paren(std::vector<lex::Token> const& tokens)
        {
            std::vector<std::size_t> stack;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].kind == lex::TokenKind::LParen)
                    stack.push_back(i);
                else if (tokens[i].kind == lex::TokenKind::RParen)
                {
                    if (!stack.empty())
                        stack.pop_back();
                }
            }
            if (stack.empty())
                return std::nullopt;
            return stack.back();
        }

        struct TokenCallee
        {
            std::string name;
            bool ufcs{false};
            bool found{false};
        };

        [[nodiscard]] TokenCallee callee_from_tokens(std::vector<lex::Token> const& tokens, std::size_t open_paren_index)
        {
            TokenCallee out;
            if (open_paren_index == 0)
                return out;

            std::size_t i = open_paren_index - 1;
            auto const& prev = tokens[i];

            if (prev.kind == lex::TokenKind::Identifier)
            {
                out.name = std::string{prev.interned};
                out.found = true;
                if (i >= 1 && tokens[i - 1].kind == lex::TokenKind::Dot)
                    out.ufcs = true;
                return out;
            }

            if (prev.kind == lex::TokenKind::RParen)
            {
                int depth = 0;
                std::size_t j = i;
                for (;;)
                {
                    if (tokens[j].kind == lex::TokenKind::RParen)
                        ++depth;
                    else if (tokens[j].kind == lex::TokenKind::LParen)
                    {
                        if (--depth <= 0)
                            break;
                    }
                    if (j == 0)
                        return out;
                    --j;
                }
                if (j >= 2 && tokens[j - 1].kind == lex::TokenKind::Bang && tokens[j - 2].kind == lex::TokenKind::Identifier)
                {
                    out.name = std::string{tokens[j - 2].interned};
                    out.found = true;
                }
                return out;
            }

            return out;
        }

        struct ParenSlots
        {
            std::uint32_t top_level_commas{0};
            bool current_arg_has_tokens{false};
        };

        [[nodiscard]] ParenSlots scan_paren_slots(std::vector<lex::Token> const& tokens, std::size_t open_paren_index, std::uint32_t cursor)
        {
            ParenSlots out;
            int depth = 0;
            std::optional<std::size_t> last_comma_index;

            for (std::size_t i = open_paren_index + 1; i < tokens.size(); ++i)
            {
                auto const& tok = tokens[i];
                if (tok.range.begin.offset >= cursor)
                    break;

                switch (tok.kind)
                {
                    case lex::TokenKind::LParen:
                    case lex::TokenKind::LBracket:
                    case lex::TokenKind::LBrace:
                        ++depth;
                        break;
                    case lex::TokenKind::RParen:
                    case lex::TokenKind::RBracket:
                    case lex::TokenKind::RBrace:
                        if (depth > 0)
                            --depth;
                        break;
                    case lex::TokenKind::Comma:
                        if (depth == 0)
                        {
                            ++out.top_level_commas;
                            last_comma_index = i;
                        }
                        break;
                    default:
                        break;
                }
            }

            std::uint32_t slot_start = tokens[open_paren_index].range.end.offset;
            if (last_comma_index)
                slot_start = tokens[*last_comma_index].range.end.offset;

            for (std::size_t i = open_paren_index + 1; i < tokens.size(); ++i)
            {
                auto const& tok = tokens[i];
                if (tok.range.begin.offset >= cursor)
                    break;
                if (tok.range.begin.offset >= slot_start)
                {
                    out.current_arg_has_tokens = true;
                    break;
                }
            }
            return out;
        }

    } // anonymous namespace

    std::optional<ActiveCallInfo> find_active_call(session::CompilerSession const& session, sm::FileId file, sm::Location location)
    {
        auto const* sf = session.source_manager().get(file);
        if (!sf)
            return std::nullopt;

        auto text = sf->text();
        if (location.offset > static_cast<sm::Offset>(text.size()))
            return std::nullopt;

        auto cursor = static_cast<std::uint32_t>(location.offset);

        lex::Lexer lexer{*sf, const_cast<si::string_interner&>(session.interner())};
        auto tokens = lex_up_to(lexer, cursor);

        ActiveCallInfo info;
        auto node = find_node_at(session, location, QueryOptions{});
        ast::CallExpr const* call = (node && node->enclosing_call) ? node->enclosing_call : nullptr;

        std::optional<std::size_t> open_paren_index;

        if (call)
        {
            info.call = call;
            info.callee_expr = call->callee;

            ast::Expr const* callee = call->callee;
            while (callee && callee->kind == ast::ExprKind::TemplateInst)
                callee = static_cast<ast::TemplateInstExpr const*>(callee)->callee;

            if (callee)
            {
                if (callee->kind == ast::ExprKind::Ident)
                    info.callee_name = std::string{static_cast<ast::IdentExpr const*>(callee)->name};
                else if (callee->kind == ast::ExprKind::PathExpr)
                {
                    auto const* pe = static_cast<ast::PathExpr const*>(callee);
                    if (!pe->path.segments.empty())
                        info.callee_name = std::string{pe->path.segments.back().name};
                }
                else if (callee->kind == ast::ExprKind::FieldAccess)
                {
                    auto const* fa = static_cast<ast::FieldAccessExpr const*>(callee);
                    info.callee_name = std::string{fa->field};
                    info.ufcs = true;
                }
            }

            info.ufcs = info.ufcs || call->sema.ufcs_callee != nullptr;
            std::uint32_t callee_end = callee ? static_cast<std::uint32_t>(callee->range.end.offset) : static_cast<std::uint32_t>(call->range.begin.offset);
            open_paren_index = find_call_open_paren(tokens, call, callee_end);
        }

        if (!open_paren_index)
        {
            if (node && node->enclosing_decl && node->enclosing_decl->kind == ast::DeclKind::Func)
            {
                auto const* fd = static_cast<ast::FuncDecl const*>(node->enclosing_decl);
                bool in_body = fd->body.has_value() && fd->body->range.valid() && fd->body->range.begin.fileId == location.fileId &&
                               fd->body->range.begin.offset <= cursor && cursor <= fd->body->range.end.offset;
                if (!in_body)
                    return std::nullopt;
            }

            open_paren_index = innermost_open_paren(tokens);
            if (open_paren_index)
            {
                auto callee_info = callee_from_tokens(tokens, *open_paren_index);
                if (callee_info.found)
                {
                    if (info.callee_name.empty())
                    {
                        info.callee_name = std::move(callee_info.name);
                        info.ufcs = callee_info.ufcs;
                    }
                }
                else if (info.callee_name.empty())
                    return std::nullopt;
            }
        }

        if (!open_paren_index)
            return std::nullopt;

        auto const& open_tok = tokens[*open_paren_index];
        if (cursor <= open_tok.range.begin.offset)
            return std::nullopt;

        info.open_paren_offset = open_tok.range.begin.offset;
        auto slots = scan_paren_slots(tokens, *open_paren_index, cursor);
        info.active_parameter = info.ufcs ? slots.top_level_commas + 1 : slots.top_level_commas;
        info.explicit_argument_count = slots.top_level_commas + (slots.current_arg_has_tokens ? 1u : 0u);
        info.current_argument_has_tokens = slots.current_arg_has_tokens;
        info.in_call_arguments = true;
        return info;
    }

    namespace
    {
        enum class SourceLexMode : std::uint8_t
        {
            Code,
            LineComment,
            BlockComment,
            String,
            Char,
        };

        struct SourceScan
        {
            SourceLexMode mode{SourceLexMode::Code};

            void feed(std::string_view text, std::size_t limit)
            {
                for (std::size_t i = 0; i < limit; ++i)
                {
                    char c = text[i];
                    switch (mode)
                    {
                        case SourceLexMode::Code:
                            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
                            {
                                mode = SourceLexMode::LineComment;
                                ++i;
                            }
                            else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
                            {
                                mode = SourceLexMode::BlockComment;
                                ++i;
                            }
                            else if (c == '"')
                                mode = SourceLexMode::String;
                            else if (c == '\'')
                                mode = SourceLexMode::Char;
                            break;
                        case SourceLexMode::LineComment:
                            if (c == '\n')
                                mode = SourceLexMode::Code;
                            break;
                        case SourceLexMode::BlockComment:
                            if (c == '*' && i + 1 < text.size() && text[i + 1] == '/')
                            {
                                mode = SourceLexMode::Code;
                                ++i;
                            }
                            break;
                        case SourceLexMode::String:
                            if (c == '\\')
                                ++i;
                            else if (c == '"')
                                mode = SourceLexMode::Code;
                            break;
                        case SourceLexMode::Char:
                            if (c == '\\')
                                ++i;
                            else if (c == '\'')
                                mode = SourceLexMode::Code;
                            break;
                    }
                }
            }
        };

    } // anonymous namespace

    SourceRegion source_region_at(session::CompilerSession const& session, sm::FileId file, sm::Offset offset)
    {
        SourceRegion region;
        auto const* sf = session.source_manager().get(file);
        if (!sf)
            return region;

        auto text = sf->text();
        if (offset > static_cast<sm::Offset>(text.size()))
            return region;

        SourceScan scan;
        scan.feed(text, static_cast<std::size_t>(offset));
        switch (scan.mode)
        {
            case SourceLexMode::String:
            case SourceLexMode::Char:
                region.in_string = true;
                break;
            case SourceLexMode::LineComment:
            case SourceLexMode::BlockComment:
                region.in_comment = true;
                break;
            case SourceLexMode::Code:
                break;
        }
        return region;
    }

} // namespace dcc::query
