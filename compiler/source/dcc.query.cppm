export module dcc.query;

import std;
import dcc.sm;
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

        sm::SourceRange resolved_definition_range{};
        ast::FuncParam const* resolved_param{};

        ast::CallExpr const* enclosing_call{nullptr};
        std::uint32_t active_argument_index{0};

        [[nodiscard]] bool has_ast_node() const noexcept { return hovered_decl || stmt || expr || type_expr; }

        [[nodiscard]] bool has_semantic_target() const noexcept
        {
            return resolved_type || resolved_decl || resolved_specialization || ufcs_callee || resolved_field || resolved_param ||
                   resolved_definition_range.valid();
        }
    };

    [[nodiscard]] std::optional<NodeAtLocation> find_node_at(session::CompilerSession const& session, sm::FileId file, sm::Position position,
                                                             QueryOptions opts = {});

    [[nodiscard]] std::optional<NodeAtLocation> find_node_at(session::CompilerSession const& session, sm::Location location, QueryOptions opts = {});

    [[nodiscard]] sm::SourceRange decl_name_range(ast::Decl const* decl);

    [[nodiscard]] sm::SourceRange field_name_range(ast::FieldDecl const* fd);

    [[nodiscard]] std::vector<sm::SourceRange> find_references(session::CompilerSession const& session, ast::Decl const* target_decl);

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

        void surface_expr_sema(ast::Expr const* expr, NodeAtLocation& result)
        {
            if (!expr)
                return;

            result.ufcs_callee = nullptr;

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
                std::cerr << "[dcc.query] walk_block: no child matched by range, falling back to walk all " << block.stmts.size() << " stmts (block range "
                          << block.range.begin.offset << ".." << block.range.end.offset << " target=" << target.offset << ")" << '\n';

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
                if (tp.value_type && range_contains_or_touches_end(tp.value_type->range, target))
                    walk_type_expr(tp.value_type, result, target, opts);
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
                        if (sym && sym->decl)
                            result.resolved_decl = sym->via_using ? sym->via_using : sym->decl;
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
                    for (auto* p : t->params)
                        if (p && range_contains_or_touches_end(p->range, target))
                            walk_type_expr(p, result, target, opts);
                    break;
                }
                case ast::TypeKind::Qualified: {
                    auto* t = static_cast<ast::QualifiedType const*>(type_expr);
                    if (t->inner && range_contains_or_touches_end(t->inner->range, target))
                        walk_type_expr(t->inner, result, target, opts);
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
                        {
                            result.enclosing_call = e;
                            std::uint32_t active_idx = 0;
                            for (std::size_t i = 0; i < e->args.size(); ++i)
                            {
                                if (e->args[i] && range_contains(e->args[i]->range, target))
                                {
                                    active_idx = static_cast<std::uint32_t>(i);
                                    break;
                                }
                                if (e->args[i] && target.offset > e->args[i]->range.end.offset)
                                    active_idx = static_cast<std::uint32_t>(i + 1);
                                else if (e->args[i] && target.offset <= e->args[i]->range.begin.offset)
                                    break;
                            }
                            result.active_argument_index = active_idx;
                        }
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
            }
        }

        void walk_decl(ast::Decl const* decl, NodeAtLocation& result, sm::Location target, QueryOptions const& opts)
        {
            if (!decl)
                return;

            if (decl->kind == ast::DeclKind::Func && range_contains_or_touches_end(decl->range, target))
                result.enclosing_decl = decl;

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
                            std::cerr << "[dcc.query] walk_decl Func " << d->name << ": body range (" << d->body->range.begin.offset << ".."
                                      << d->body->range.end.offset << ") does not contain target " << target.offset << " but decl range ("
                                      << decl->range.begin.offset << ".." << decl->range.end.offset << ") does; falling back to walk body" << std::endl;
                            walk_block(*d->body, result, target, opts);
                        }
                        else
                        {
                            std::cerr << "[dcc.query] walk_decl Func " << d->name << ": skipping body — decl range (" << decl->range.begin.offset << ".."
                                      << decl->range.end.offset << ") and body range (" << d->body->range.begin.offset << ".." << d->body->range.end.offset
                                      << ") both exclude target " << target.offset << std::endl;
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

            std::cerr << "[dcc.query] borrow_sema: spec walk found expr=" << (spec_result.expr ? 1 : 0) << " stmt=" << (spec_result.stmt ? 1 : 0)
                      << " resolved_type=" << (spec_result.resolved_type ? 1 : 0) << " resolved_decl=" << (spec_result.resolved_decl ? 1 : 0)
                      << " resolved_specialization=" << (spec_result.resolved_specialization ? 1 : 0) << " ufcs_callee=" << (spec_result.ufcs_callee ? 1 : 0)
                      << " resolved_field=" << (spec_result.resolved_field ? 1 : 0) << " hovered_decl=" << (spec_result.hovered_decl ? 1 : 0) << std::endl;

            if (spec_result.resolved_type && !result.resolved_type)
            {
                result.resolved_type = spec_result.resolved_type;
                std::cerr << "[dcc.query] borrow_sema: borrowing resolved_type for " << generic_fn.name << std::endl;
            }
            if (spec_result.resolved_decl && !result.resolved_decl)
            {
                result.resolved_decl = spec_result.resolved_decl;
                std::cerr << "[dcc.query] borrow_sema: borrowing resolved_decl" << std::endl;
            }
            if (spec_result.resolved_specialization && !result.resolved_specialization)
            {
                result.resolved_specialization = spec_result.resolved_specialization;
                std::cerr << "[dcc.query] borrow_sema: borrowing resolved_specialization" << std::endl;
            }
            if (spec_result.ufcs_callee && !result.ufcs_callee)
            {
                result.ufcs_callee = spec_result.ufcs_callee;
                std::cerr << "[dcc.query] borrow_sema: borrowing ufcs_callee" << std::endl;
            }
            if (spec_result.resolved_field && !result.resolved_field)
            {
                result.resolved_field = spec_result.resolved_field;
                result.resolved_field_parent = spec_result.resolved_field_parent;
                std::cerr << "[dcc.query] borrow_sema: borrowing resolved_field" << std::endl;
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
                        std::cerr << "[dcc.query] borrow_sema: borrowing resolved_type from spec param[" << param_idx << "] for " << generic_fn.name
                                  << std::endl;
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
                        std::cerr << "[dcc.query] borrow_sema: borrowing resolved_type from spec cloned VarDecl for " << generic_fn.name << std::endl;
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
                std::cerr << "[dcc.query] borrow_sema: borrowing hovered VarDecl from spec for " << generic_fn.name << '\n';
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
                        {
                            result.enclosing_call = e;
                            std::uint32_t active_idx = 0;
                            for (std::size_t i = 0; i < e->args.size(); ++i)
                            {
                                if (e->args[i] && range_contains(e->args[i]->range, target))
                                {
                                    active_idx = static_cast<std::uint32_t>(i);
                                    break;
                                }
                                if (e->args[i] && target.offset > e->args[i]->range.end.offset)
                                    active_idx = static_cast<std::uint32_t>(i + 1);
                                else if (e->args[i] && target.offset <= e->args[i]->range.begin.offset)
                                    break;
                            }
                            result.active_argument_index = active_idx;
                        }
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
                        if (sym && sym->decl)
                            result.resolved_decl = sym->via_using ? sym->via_using : sym->decl;
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
            std::cerr << "[dcc.query] find_node_at: FileId is Invalid" << std::endl;
            return std::nullopt;
        }

        auto const* sf = session.source_manager().get(file);
        if (!sf)
        {
            std::cerr << "[dcc.query] find_node_at: no SourceFile for file_id=" << static_cast<std::uint32_t>(file) << std::endl;
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

    std::optional<NodeAtLocation> find_node_at(session::CompilerSession const& session, sm::Location location, QueryOptions opts)
    {
        auto* sema_ctx = session.sema_context();
        if (!sema_ctx)
        {
            std::cerr << "[dcc.query] find_node_at: no sema_context" << std::endl;
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
            std::cerr << "[dcc.query] find_node_at: no ModuleInfo for file_id=" << static_cast<std::uint32_t>(location.fileId) << " offset=" << location.offset
                      << "; known module file_ids:";

            for (auto const& mod : graph.all())
            {
                auto const* sf = session.source_manager().get(mod->file_id);
                std::cerr << " " << static_cast<std::uint32_t>(mod->file_id);
                if (sf)
                    std::cerr << "(\"" << sf->path().string() << "\" uri=\"" << sf->uri() << "\")";
            }
            std::cerr << std::endl;
            return std::nullopt;
        }

        if (!module->tu)
        {
            std::cerr << "[dcc.query] find_node_at: ModuleInfo::tu is null for file_id=" << static_cast<std::uint32_t>(location.fileId) << std::endl;
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
            std::cerr << "[dcc.query] strict walk found no node at offset " << location.offset << "; trying conservative full walk" << std::endl;

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
                            std::cerr << "[dcc.query] struct fallback: resolved param '" << name << "' in template " << fd->name
                                      << " range=" << result.resolved_definition_range.begin.offset << ".." << result.resolved_definition_range.end.offset
                                      << std::endl;
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
                            std::cerr << "[dcc.query] struct fallback: resolved local var '" << name << "' in template " << fd->name
                                      << " name_range=" << local->name_range.begin.offset << ".." << local->name_range.end.offset << std::endl;
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
                        std::cerr << "[dcc.query] borrowing sema from specialization of " << fd->name << " (" << specs.size() << " specialization(s) available)"
                                  << std::endl;
                        borrow_sema_from_spec(result, *spec, location, opts, *fd);
                    }
                }
            }
        }

        if (result.has_ast_node())
        {
            std::cerr << "[dcc.query] successfully found node at offset " << location.offset << ": hovered_decl=" << (result.hovered_decl ? 1 : 0)
                      << " enclosing_decl_kind=" << (result.enclosing_decl ? static_cast<int>(result.enclosing_decl->kind) : -1)
                      << " Stmt=" << (result.stmt ? static_cast<int>(result.stmt->kind) : -1)
                      << " Expr=" << (result.expr ? static_cast<int>(result.expr->kind) : -1)
                      << " TypeExpr=" << (result.type_expr ? static_cast<int>(result.type_expr->kind) : -1);
            std::cerr << " resolved_decl=" << (result.resolved_decl ? 1 : 0);
            std::cerr << " resolved_specialization=" << (result.resolved_specialization ? 1 : 0);
            std::cerr << " ufcs_callee=" << (result.ufcs_callee ? 1 : 0);
            std::cerr << " resolved_field=" << (result.resolved_field ? 1 : 0);
            std::cerr << " resolved_param=" << (result.resolved_param ? 1 : 0);
            std::cerr << " enclosing_call=" << (result.enclosing_call ? 1 : 0);
            std::cerr << " active_arg=" << result.active_argument_index;
            std::cerr << std::endl;
        }
        else
            std::cerr << "[dcc.query] no AST node found at offset " << location.offset << std::endl;

        return result;
    }

    namespace
    {
        [[nodiscard]] bool decl_or_spec_matches(ast::Decl const* candidate, ast::Decl const* target)
        {
            return candidate == target;
        }

        [[nodiscard]] bool callee_or_spec_matches(ast::Expr const* expr, ast::Decl const* target)
        {
            if (!expr)
                return false;
            if (expr->sema.resolved_decl && decl_or_spec_matches(expr->sema.resolved_decl, target))
                return true;
            if (expr->sema.resolved_specialization && decl_or_spec_matches(expr->sema.resolved_specialization, target))
                return true;
            if (expr->sema.ufcs_callee && decl_or_spec_matches(expr->sema.ufcs_callee, target))
                return true;
            return false;
        }

        struct ReferenceCollector : ast::RecursiveAstVisitor
        {
            std::vector<sm::SourceRange>& out;
            ast::Decl const* target;
            sema::Scope const* scope;

            ReferenceCollector(std::vector<sm::SourceRange>& o, ast::Decl const* t, sema::Scope const* s) : out{o}, target{t}, scope{s} {}

            void visitExpr(ast::Expr const* expr) override
            {
                if (!expr)
                    return;

                switch (expr->kind)
                {
                    case ast::ExprKind::Ident: {
                        if (callee_or_spec_matches(expr, target))
                            out.push_back(expr->range);
                        break;
                    }
                    case ast::ExprKind::PathExpr: {
                        auto* e = static_cast<ast::PathExpr const*>(expr);
                        if (callee_or_spec_matches(expr, target))
                        {
                            if (!e->path.segments.empty())
                                out.push_back(e->path.segments.back().range);
                            else
                                out.push_back(expr->range);
                        }

                        for (auto const& ta : e->explicit_enum_args)
                        {
                            if (ta.type)
                                visitTypeExpr(ta.type);
                            if (ta.expr)
                                visitExpr(ta.expr);
                        }
                        break;
                    }
                    case ast::ExprKind::Call: {
                        auto* e = static_cast<ast::CallExpr const*>(expr);
                        if (callee_or_spec_matches(expr, target) && e->callee)
                        {
                            auto* ce = e->callee;
                            while (ce && ce->kind == ast::ExprKind::TemplateInst)
                                ce = static_cast<ast::TemplateInstExpr const*>(ce)->callee;

                            if (ce)
                            {
                                if (ce->kind == ast::ExprKind::Ident)
                                    out.push_back(ce->range);
                                else if (ce->kind == ast::ExprKind::PathExpr)
                                {
                                    auto* pe = static_cast<ast::PathExpr const*>(ce);
                                    if (!pe->path.segments.empty())
                                        out.push_back(pe->path.segments.back().range);
                                }
                            }
                            visitExpr(e->callee);
                        }
                        else if (e->callee)
                        {
                            visitExpr(e->callee);
                        }
                        for (auto* a : e->args)
                            visitExpr(a);
                        break;
                    }
                    case ast::ExprKind::TemplateInst: {
                        auto* e = static_cast<ast::TemplateInstExpr const*>(expr);
                        if (callee_or_spec_matches(expr, target) && e->callee)
                        {
                            auto* ce = e->callee;
                            while (ce && ce->kind == ast::ExprKind::TemplateInst)
                                ce = static_cast<ast::TemplateInstExpr const*>(ce)->callee;

                            if (ce)
                            {
                                if (ce->kind == ast::ExprKind::Ident)
                                    out.push_back(ce->range);
                                else if (ce->kind == ast::ExprKind::PathExpr)
                                {
                                    auto* pe = static_cast<ast::PathExpr const*>(ce);
                                    if (!pe->path.segments.empty())
                                        out.push_back(pe->path.segments.back().range);
                                }
                            }
                            visitExpr(e->callee);
                        }
                        else if (e->callee)
                        {
                            visitExpr(e->callee);
                        }
                        for (auto const& ta : e->template_args)
                        {
                            if (ta.type)
                                visitTypeExpr(ta.type);
                            if (ta.expr)
                                visitExpr(ta.expr);
                        }
                        break;
                    }
                    default:
                        ast::RecursiveAstVisitor::visitExpr(expr);
                        break;
                }
            }

            void visitTypeExpr(ast::TypeExpr const* type_expr) override
            {
                if (!type_expr)
                    return;

                if (type_expr->kind == ast::TypeKind::Named)
                {
                    auto* t = static_cast<ast::NamedType const*>(type_expr);
                    if (t->sema.resolved_decl && decl_or_spec_matches(t->sema.resolved_decl, target))
                    {
                        if (!t->path.segments.empty())
                            out.push_back(t->path.segments.back().range);
                    }
                    else if (scope && !t->path.is_empty() && !t->path.segments.empty())
                    {
                        auto const* sym = sema::resolve_type_path(*scope, t->path);
                        if (sym && sym->decl && decl_or_spec_matches(sym->decl, target))
                            out.push_back(t->path.segments.back().range);
                    }

                    for (auto const& ta : t->template_args)
                    {
                        if (ta.type)
                            visitTypeExpr(ta.type);
                        if (ta.expr)
                            visitExpr(ta.expr);
                    }
                }
                else
                {
                    ast::RecursiveAstVisitor::visitTypeExpr(type_expr);
                }
            }
        };

    } // anonymous namespace

    std::vector<sm::SourceRange> find_references(session::CompilerSession const& session, ast::Decl const* target_decl)
    {
        std::vector<sm::SourceRange> ranges;

        if (!target_decl)
        {
            std::cerr << "[dcc.query] find_references: null target_decl" << std::endl;
            return ranges;
        }

        auto* sema_ctx = session.sema_context();
        if (!sema_ctx)
        {
            std::cerr << "[dcc.query] find_references: no sema context" << std::endl;
            return ranges;
        }

        auto& graph = const_cast<sema::SemaContext*>(sema_ctx)->graph();

        for (auto const& mod : graph.all())
        {
            if (!mod || !mod->tu)
                continue;

            auto const* scope = mod->own_scope;
            ReferenceCollector collector(ranges, target_decl, scope);
            collector.visitTranslationUnit(mod->tu);
        }

        std::ranges::sort(ranges, [](sm::SourceRange const& a, sm::SourceRange const& b) {
            auto fid_a = static_cast<std::uint32_t>(a.begin.fileId);
            auto fid_b = static_cast<std::uint32_t>(b.begin.fileId);
            if (fid_a != fid_b)
                return fid_a < fid_b;
            if (a.begin.offset != b.begin.offset)
                return a.begin.offset < b.begin.offset;
            return a.end.offset < b.end.offset;
        });

        auto [first, last] = std::ranges::unique(ranges, [](sm::SourceRange const& a, sm::SourceRange const& b) {
            return a.begin.fileId == b.begin.fileId && a.begin.offset == b.begin.offset && a.end.offset == b.end.offset;
        });

        ranges.erase(first, last);

        std::cerr << "[dcc.query] find_references: found " << ranges.size() << " references" << '\n';

        return ranges;
    }

} // namespace dcc::query
