export module dcc.sema.instantiator;

import std;
import dcc.ast;
import dcc.comptime;
import dcc.types;
import dcc.diag;
import dcc.si;
import dcc.sm;
import dcc.lex.tokens;
import dcc.sema.infer;
import dcc.sema.scope;
import dcc.sema.type_helpers;

namespace dcc::sema
{
    namespace
    {
        class AstCloner
        {
        public:
            explicit AstCloner(ast::AstContext& ctx) : m_ctx(ctx), m_alloc(ctx.allocator()) {}

            ast::TypeExpr* clone_type(ast::TypeExpr const* t);
            ast::Expr* clone_expr(ast::Expr const* e);
            ast::Stmt* clone_stmt(ast::Stmt const* s);
            ast::Decl* clone_decl(ast::Decl const* d);
            ast::Pattern* clone_pattern(ast::Pattern const* p);
            ast::Block clone_block(ast::Block const& b);

            ast::Path clone_path(ast::Path const& p);
            ast::MatchArm clone_match_arm(ast::MatchArm const& arm);
            ast::StructLiteralField clone_struct_literal_field(ast::StructLiteralField const& f);
            ast::StructPatternField clone_struct_pattern_field(ast::StructPatternField const& f);
            ast::FuncParam clone_func_param(ast::FuncParam const& p);
            ast::TemplateArg clone_template_arg(ast::TemplateArg const& a);

        private:
            ast::AstContext& m_ctx;
            ast::Allocator m_alloc;
        };

        ast::TypeExpr* AstCloner::clone_type(ast::TypeExpr const* t)
        {
            if (!t)
                return nullptr;

            switch (t->kind)
            {
                case ast::TypeKind::Primitive: {
                    auto* e = static_cast<ast::PrimitiveType const*>(t);
                    auto* n = m_ctx.make<ast::PrimitiveType>(e->range, e->which);
                    n->sema = e->sema;
                    return n;
                }
                case ast::TypeKind::Named: {
                    auto* e = static_cast<ast::NamedType const*>(t);
                    auto path = clone_path(e->path);
                    auto* n = m_ctx.make<ast::NamedType>(e->range, std::move(path));
                    n->template_args.reserve(e->template_args.size());
                    for (auto const& a : e->template_args)
                        n->template_args.push_back(clone_template_arg(a));

                    n->sema = e->sema;
                    return n;
                }
                case ast::TypeKind::Pointer: {
                    auto* e = static_cast<ast::PointerType const*>(t);
                    auto* n = m_ctx.make<ast::PointerType>(e->range, clone_type(e->pointee));
                    n->sema = e->sema;
                    return n;
                }
                case ast::TypeKind::Array: {
                    auto* e = static_cast<ast::ArrayType const*>(t);
                    auto* n = m_ctx.make<ast::ArrayType>(e->range, clone_type(e->element), e->size ? clone_expr(e->size) : nullptr);
                    n->sema = e->sema;
                    return n;
                }
                case ast::TypeKind::Slice: {
                    auto* e = static_cast<ast::SliceType const*>(t);
                    auto* n = m_ctx.make<ast::SliceType>(e->range, clone_type(e->element));
                    n->sema = e->sema;
                    return n;
                }
                case ast::TypeKind::Fam: {
                    auto* e = static_cast<ast::FamType const*>(t);
                    auto* n = m_ctx.make<ast::FamType>(e->range, clone_type(e->element));
                    n->sema = e->sema;
                    return n;
                }
                case ast::TypeKind::FuncPtr: {
                    auto* e = static_cast<ast::FuncPtrType const*>(t);
                    auto* n = m_ctx.make<ast::FuncPtrType>(e->range, clone_type(e->return_type));
                    n->params.reserve(e->params.size());
                    for (auto* p : e->params)
                        n->params.push_back(clone_type(p));

                    n->sema = e->sema;
                    return n;
                }
                case ast::TypeKind::Qualified: {
                    auto* e = static_cast<ast::QualifiedType const*>(t);
                    auto* n = m_ctx.make<ast::QualifiedType>(e->range, e->quals, clone_type(e->inner));
                    n->sema = e->sema;
                    return n;
                }
                case ast::TypeKind::PackIndex: {
                    auto* e = static_cast<ast::PackIndexType const*>(t);
                    auto* n = m_ctx.make<ast::PackIndexType>(e->range, clone_type(e->base), e->index ? clone_expr(e->index) : nullptr);
                    n->sema = e->sema;
                    return n;
                }
            }

            return nullptr;
        }

        ast::Expr* AstCloner::clone_expr(ast::Expr const* e)
        {
            if (!e)
                return nullptr;

            switch (e->kind)
            {
                case ast::ExprKind::IntLiteral: {
                    auto* ex = static_cast<ast::IntLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::IntLiteralExpr>(ex->range, ex->value, ex->spelling);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::FloatLiteral: {
                    auto* ex = static_cast<ast::FloatLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::FloatLiteralExpr>(ex->range, ex->value, ex->spelling);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::StringLiteral: {
                    auto* ex = static_cast<ast::StringLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::StringLiteralExpr>(ex->range, std::string_view{ex->value}, ex->spelling);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::U16StringLiteral: {
                    auto* ex = static_cast<ast::U16StringLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::U16StringLiteralExpr>(ex->range, std::u16string_view{ex->value}, ex->spelling);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::CharLiteral: {
                    auto* ex = static_cast<ast::CharLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::CharLiteralExpr>(ex->range, ex->codepoint);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::U16CharLiteral: {
                    auto* ex = static_cast<ast::U16CharLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::U16CharLiteralExpr>(ex->range, ex->value);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::BoolLiteral: {
                    auto* ex = static_cast<ast::BoolLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::BoolLiteralExpr>(ex->range, ex->value);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::NullLiteral: {
                    auto* ex = static_cast<ast::NullLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::NullLiteralExpr>(ex->range);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Ident: {
                    auto* ex = static_cast<ast::IdentExpr const*>(e);
                    auto* n = m_ctx.make<ast::IdentExpr>(ex->range, ex->name);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::PathExpr: {
                    auto* ex = static_cast<ast::PathExpr const*>(e);
                    auto path = clone_path(ex->path);
                    auto* n = m_ctx.make<ast::PathExpr>(ex->range, std::move(path), m_ctx.allocator());
                    n->explicit_enum_args.reserve(ex->explicit_enum_args.size());
                    for (auto const& a : ex->explicit_enum_args)
                        n->explicit_enum_args.push_back(clone_template_arg(a));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Unary: {
                    auto* ex = static_cast<ast::UnaryExpr const*>(e);
                    auto* n = m_ctx.make<ast::UnaryExpr>(ex->range, ex->op, clone_expr(ex->operand));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Postfix: {
                    auto* ex = static_cast<ast::PostfixExpr const*>(e);
                    auto* n = m_ctx.make<ast::PostfixExpr>(ex->range, clone_expr(ex->operand), ex->op);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Binary: {
                    auto* ex = static_cast<ast::BinaryExpr const*>(e);
                    auto* n = m_ctx.make<ast::BinaryExpr>(ex->range, clone_expr(ex->lhs), ex->op, clone_expr(ex->rhs));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Call: {
                    auto* ex = static_cast<ast::CallExpr const*>(e);
                    auto* n = m_ctx.make<ast::CallExpr>(ex->range, clone_expr(ex->callee));
                    n->args.reserve(ex->args.size());
                    for (auto* a : ex->args)
                        n->args.push_back(clone_expr(a));

                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::FieldAccess: {
                    auto* ex = static_cast<ast::FieldAccessExpr const*>(e);
                    auto* n = m_ctx.make<ast::FieldAccessExpr>(ex->range, clone_expr(ex->object), ex->field, ex->field_range);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Index: {
                    auto* ex = static_cast<ast::IndexExpr const*>(e);
                    auto* n = m_ctx.make<ast::IndexExpr>(ex->range, clone_expr(ex->object), clone_expr(ex->index));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::PackAccess: {
                    auto* ex = static_cast<ast::PackAccessExpr const*>(e);
                    auto* n = m_ctx.make<ast::PackAccessExpr>(ex->range, clone_expr(ex->object), clone_expr(ex->index));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Cast: {
                    auto* ex = static_cast<ast::CastExpr const*>(e);
                    auto* n = m_ctx.make<ast::CastExpr>(ex->range, clone_expr(ex->operand), clone_type(ex->target));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Block: {
                    auto* ex = static_cast<ast::BlockExpr const*>(e);
                    auto body = clone_block(ex->body);
                    auto* n = m_ctx.make<ast::BlockExpr>(ex->range, std::move(body));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::If: {
                    auto* ex = static_cast<ast::IfExpr const*>(e);
                    auto then_blk = clone_block(ex->then_block);
                    auto* n = m_ctx.make<ast::IfExpr>(ex->range, clone_expr(ex->condition), std::move(then_blk));
                    n->else_branch = clone_expr(ex->else_branch);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Match: {
                    auto* ex = static_cast<ast::MatchExpr const*>(e);
                    auto* n = m_ctx.make<ast::MatchExpr>(ex->range, clone_expr(ex->operand));
                    n->arms.reserve(ex->arms.size());
                    for (auto const& arm : ex->arms)
                        n->arms.push_back(clone_match_arm(arm));

                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::StructLiteral: {
                    auto* ex = static_cast<ast::StructLiteralExpr const*>(e);
                    auto* n = m_ctx.make<ast::StructLiteralExpr>(ex->range);
                    n->type = clone_type(ex->type);
                    n->fields.reserve(ex->fields.size());
                    for (auto const& f : ex->fields)
                        n->fields.push_back(clone_struct_literal_field(f));

                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Sizeof: {
                    auto* ex = static_cast<ast::SizeofExpr const*>(e);
                    auto* n = m_ctx.make<ast::SizeofExpr>(ex->range, clone_type(ex->target));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Alignof: {
                    auto* ex = static_cast<ast::AlignofExpr const*>(e);
                    auto* n = m_ctx.make<ast::AlignofExpr>(ex->range, clone_type(ex->target));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Offsetof: {
                    auto* ex = static_cast<ast::OffsetofExpr const*>(e);
                    auto* n = m_ctx.make<ast::OffsetofExpr>(ex->range, clone_type(ex->target), ex->field);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Compiles: {
                    auto* ex = static_cast<ast::CompilesExpr const*>(e);
                    auto* n = m_ctx.make<ast::CompilesExpr>(ex->range);
                    n->params.reserve(ex->params.size());
                    for (auto const& p : ex->params)
                    {
                        ast::CompilesParam cp;
                        cp.name = p.name;
                        cp.range = p.range;
                        cp.type = clone_type(p.type);
                        n->params.push_back(cp);
                    }
                    n->body = clone_block(ex->body);
                    n->value = ex->value;
                    n->resolved = ex->resolved;
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::TypeAST: {
                    auto* ex = static_cast<ast::TypeASTExpr const*>(e);
                    auto* n = m_ctx.make<ast::TypeASTExpr>(ex->range, clone_type(ex->type_node));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::TemplateInst: {
                    auto* ex = static_cast<ast::TemplateInstExpr const*>(e);
                    auto* n = m_ctx.make<ast::TemplateInstExpr>(ex->range, clone_expr(ex->callee));
                    n->template_args.reserve(ex->template_args.size());
                    for (auto const& a : ex->template_args)
                        n->template_args.push_back(clone_template_arg(a));

                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::SizeofPack: {
                    auto* ex = static_cast<ast::SizeofPackExpr const*>(e);
                    auto* n = m_ctx.make<ast::SizeofPackExpr>(ex->range, ex->pack_name, ex->name_range);
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::PackExpansion: {
                    auto* ex = static_cast<ast::PackExpansionExpr const*>(e);
                    auto* n = m_ctx.make<ast::PackExpansionExpr>(ex->range, clone_expr(ex->operand));
                    n->sema = ex->sema;
                    return n;
                }
                case ast::ExprKind::Range: {
                    auto* ex = static_cast<ast::RangeExpr const*>(e);
                    auto* n = m_ctx.make<ast::RangeExpr>(ex->range, clone_expr(ex->start), clone_expr(ex->end), ex->inclusive);
                    n->sema = ex->sema;
                    return n;
                }
            }

            return nullptr;
        }

        ast::Stmt* AstCloner::clone_stmt(ast::Stmt const* s)
        {
            if (!s)
                return nullptr;

            switch (s->kind)
            {
                case ast::StmtKind::Expr: {
                    auto* st = static_cast<ast::ExprStmt const*>(s);
                    return m_ctx.make<ast::ExprStmt>(st->range, clone_expr(st->expr));
                }
                case ast::StmtKind::DeclStmt: {
                    auto* st = static_cast<ast::DeclStmt const*>(s);
                    return m_ctx.make<ast::DeclStmt>(st->range, clone_decl(st->decl));
                }
                case ast::StmtKind::Return: {
                    auto* st = static_cast<ast::ReturnStmt const*>(s);
                    auto* n = m_ctx.make<ast::ReturnStmt>(st->range);
                    n->value = clone_expr(st->value);
                    n->exit_defers.assign(st->exit_defers.begin(), st->exit_defers.end());
                    return n;
                }
                case ast::StmtKind::Break: {
                    auto* st = static_cast<ast::BreakStmt const*>(s);
                    auto* n = m_ctx.make<ast::BreakStmt>(st->range);
                    n->exit_defers.assign(st->exit_defers.begin(), st->exit_defers.end());
                    return n;
                }
                case ast::StmtKind::Continue: {
                    auto* st = static_cast<ast::ContinueStmt const*>(s);
                    auto* n = m_ctx.make<ast::ContinueStmt>(st->range);
                    n->exit_defers.assign(st->exit_defers.begin(), st->exit_defers.end());
                    return n;
                }
                case ast::StmtKind::While: {
                    auto* st = static_cast<ast::WhileStmt const*>(s);
                    auto body = clone_block(st->body);
                    return m_ctx.make<ast::WhileStmt>(st->range, clone_expr(st->condition), std::move(body));
                }
                case ast::StmtKind::DoWhile: {
                    auto* st = static_cast<ast::DoWhileStmt const*>(s);
                    auto body = clone_block(st->body);
                    return m_ctx.make<ast::DoWhileStmt>(st->range, std::move(body), clone_expr(st->condition));
                }
                case ast::StmtKind::For: {
                    auto* st = static_cast<ast::ForStmt const*>(s);
                    auto body = clone_block(st->body);
                    auto* n = m_ctx.make<ast::ForStmt>(st->range, std::move(body));
                    n->init = clone_stmt(st->init);
                    n->cond = clone_expr(st->cond);
                    n->update = clone_expr(st->update);
                    return n;
                }
                case ast::StmtKind::ForIn: {
                    auto* st = static_cast<ast::ForInStmt const*>(s);
                    auto body = clone_block(st->body);
                    auto* n = m_ctx.make<ast::ForInStmt>(st->range, std::move(body));
                    n->item_type = clone_type(st->item_type);
                    n->item_name = st->item_name;
                    n->name_range = st->name_range;
                    n->iterable = clone_expr(st->iterable);
                    n->resolved_item_type = st->resolved_item_type;
                    n->by_reference = st->by_reference;
                    return n;
                }
                case ast::StmtKind::Defer: {
                    auto* st = static_cast<ast::DeferStmt const*>(s);
                    return m_ctx.make<ast::DeferStmt>(st->range, clone_stmt(st->body));
                }
                case ast::StmtKind::StaticIf: {
                    auto* st = static_cast<ast::StaticIfStmt const*>(s);
                    auto then_blk = clone_block(st->then_block);
                    auto* n = m_ctx.make<ast::StaticIfStmt>(st->range, clone_expr(st->condition), std::move(then_blk));
                    n->else_branch = clone_stmt(st->else_branch);
                    n->taken_branch = st->taken_branch;
                    n->is_type_if = st->is_type_if;
                    return n;
                }
                case ast::StmtKind::StaticMatch: {
                    auto* st = static_cast<ast::StaticMatchStmt const*>(s);
                    auto* n = m_ctx.make<ast::StaticMatchStmt>(st->range, clone_expr(st->operand));
                    n->arms.reserve(st->arms.size());
                    for (auto const& arm : st->arms)
                        n->arms.push_back(clone_match_arm(arm));
                    n->taken_arm = st->taken_arm;
                    n->is_type_match = st->is_type_match;
                    return n;
                }
                case ast::StmtKind::StaticFor: {
                    auto* st = static_cast<ast::StaticForStmt const*>(s);
                    auto body = clone_block(st->body);
                    auto* n = m_ctx.make<ast::StaticForStmt>(st->range, std::move(body));
                    n->item_name = st->item_name;
                    n->name_range = st->name_range;
                    n->pack_expr = clone_expr(st->pack_expr);
                    n->is_type_for = st->is_type_for;
                    n->resolved_pack_type = st->resolved_pack_type;
                    return n;
                }
                case ast::StmtKind::Ambiguous: {
                    auto* st = static_cast<ast::AmbiguousStmt const*>(s);
                    return m_ctx.make<ast::AmbiguousStmt>(st->range, clone_decl(st->as_decl), clone_expr(st->as_expr));
                }
            }

            return nullptr;
        }

        ast::Decl* AstCloner::clone_decl(ast::Decl const* d)
        {
            if (!d)
                return nullptr;

            switch (d->kind)
            {
                case ast::DeclKind::Var: {
                    auto* dd = static_cast<ast::VarDecl const*>(d);
                    auto* n = m_ctx.make<ast::VarDecl>(dd->range, dd->name, dd->name_range);
                    n->type = clone_type(dd->type);
                    n->init = clone_expr(dd->init);
                    n->sema = dd->sema;
                    n->is_public = dd->is_public;
                    n->is_extern = dd->is_extern;
                    return n;
                }
                case ast::DeclKind::Func: {
                    auto* dd = static_cast<ast::FuncDecl const*>(d);
                    auto* n = m_ctx.make<ast::FuncDecl>(dd->range, dd->name, dd->name_range);
                    n->return_type = clone_type(dd->return_type);
                    if (dd->body)
                        n->body = clone_block(*dd->body);

                    n->params.reserve(dd->params.size());
                    for (auto const& p : dd->params)
                        n->params.push_back(clone_func_param(p));

                    n->constraint = clone_expr(dd->constraint);
                    n->sema = dd->sema;
                    n->is_public = dd->is_public;
                    n->is_extern = dd->is_extern;
                    return n;
                }
                default:
                    return nullptr;
            }
        }

        ast::Pattern* AstCloner::clone_pattern(ast::Pattern const* p)
        {
            if (!p)
                return nullptr;

            switch (p->kind)
            {
                case ast::PatternKind::Literal: {
                    auto* pp = static_cast<ast::LiteralPattern const*>(p);
                    auto* n = m_ctx.make<ast::LiteralPattern>(pp->range, clone_expr(pp->value));
                    n->matched_type = pp->matched_type;
                    return n;
                }
                case ast::PatternKind::Binding: {
                    auto* pp = static_cast<ast::BindingPattern const*>(p);
                    auto* n = m_ctx.make<ast::BindingPattern>(pp->range, pp->name);
                    n->matched_type = pp->matched_type;
                    n->by_reference = pp->by_reference;
                    return n;
                }
                case ast::PatternKind::Ref: {
                    auto* pp = static_cast<ast::RefPattern const*>(p);
                    auto* n = m_ctx.make<ast::RefPattern>(pp->range, clone_pattern(pp->inner));
                    n->matched_type = pp->matched_type;
                    return n;
                }
                case ast::PatternKind::Wildcard: {
                    auto* pp = static_cast<ast::WildcardPattern const*>(p);
                    auto* n = m_ctx.make<ast::WildcardPattern>(pp->range);
                    n->matched_type = pp->matched_type;
                    return n;
                }
                case ast::PatternKind::EnumDestructure: {
                    auto* pp = static_cast<ast::EnumDestructurePattern const*>(p);
                    auto vp = clone_path(pp->variant_path);
                    auto* n = m_ctx.make<ast::EnumDestructurePattern>(pp->range, std::move(vp));
                    n->payload.reserve(pp->payload.size());
                    for (auto* sub : pp->payload)
                        n->payload.push_back(clone_pattern(sub));

                    n->resolved_variant = pp->resolved_variant;
                    n->matched_type = pp->matched_type;
                    n->has_parens = pp->has_parens;
                    return n;
                }
                case ast::PatternKind::StructDestructure: {
                    auto* pp = static_cast<ast::StructDestructurePattern const*>(p);
                    auto tp = clone_path(pp->type_path);
                    auto* n = m_ctx.make<ast::StructDestructurePattern>(pp->range, std::move(tp));
                    n->fields.reserve(pp->fields.size());
                    for (auto const& f : pp->fields)
                        n->fields.push_back(clone_struct_pattern_field(f));

                    n->has_rest = pp->has_rest;
                    n->matched_type = pp->matched_type;
                    return n;
                }
                case ast::PatternKind::Range: {
                    auto* pp = static_cast<ast::RangePattern const*>(p);
                    auto* n = m_ctx.make<ast::RangePattern>(pp->range, clone_expr(pp->start), clone_expr(pp->end), pp->inclusive);
                    n->matched_type = pp->matched_type;
                    return n;
                }
                case ast::PatternKind::Or: {
                    auto* pp = static_cast<ast::OrPattern const*>(p);
                    auto* n = m_ctx.make<ast::OrPattern>(pp->range);
                    n->alternatives.reserve(pp->alternatives.size());
                    for (auto* alt : pp->alternatives)
                        n->alternatives.push_back(clone_pattern(alt));

                    n->matched_type = pp->matched_type;
                    return n;
                }
            }

            return nullptr;
        }

        ast::Block AstCloner::clone_block(ast::Block const& b)
        {
            ast::Block result(b.range, m_alloc);
            result.stmts.reserve(b.stmts.size());
            for (auto* stmt : b.stmts)
                result.stmts.push_back(clone_stmt(stmt));

            result.tail = clone_expr(b.tail);
            result.exit_defers.assign(b.exit_defers.begin(), b.exit_defers.end());
            return result;
        }

        ast::Path AstCloner::clone_path(ast::Path const& p)
        {
            ast::Path result(p.range, m_alloc);
            result.segments.assign(p.segments.begin(), p.segments.end());
            return result;
        }

        ast::MatchArm AstCloner::clone_match_arm(ast::MatchArm const& arm)
        {
            ast::MatchArm result;
            result.pattern = clone_pattern(arm.pattern);
            result.type_pattern = clone_type(arm.type_pattern);
            result.range = arm.range;
            result.guard = clone_expr(arm.guard);
            result.body = clone_expr(arm.body);
            result.is_wildcard = arm.is_wildcard;
            return result;
        }

        ast::StructLiteralField AstCloner::clone_struct_literal_field(ast::StructLiteralField const& f)
        {
            ast::StructLiteralField result;
            result.name = f.name;
            result.name_range = f.name_range;
            result.range = f.range;
            result.value = clone_expr(f.value);
            result.resolved_field_index = f.resolved_field_index;
            return result;
        }

        ast::StructPatternField AstCloner::clone_struct_pattern_field(ast::StructPatternField const& f)
        {
            ast::StructPatternField result;
            result.field_name = f.field_name;
            result.range = f.range;
            result.pattern = clone_pattern(f.pattern);
            result.resolved_field_index = f.resolved_field_index;
            return result;
        }

        ast::FuncParam AstCloner::clone_func_param(ast::FuncParam const& p)
        {
            ast::FuncParam result;
            result.name = p.name;
            result.range = p.range;
            result.type = clone_type(p.type);
            result.sema = p.sema;
            result.is_pack = p.is_pack;
            return result;
        }

        ast::TemplateArg AstCloner::clone_template_arg(ast::TemplateArg const& a)
        {
            ast::TemplateArg result;
            result.range = a.range;
            result.type = clone_type(a.type);
            result.expr = clone_expr(a.expr);
            result.resolved_as = a.resolved_as;
            return result;
        }

        class TypeSubstitutor
        {
        public:
            TypeSubstitutor(infer::TemplateBindings const& b, types::TypeContext& tc, si::InternedHashMap<types::TypePtr> const& name_map,
                            std::span<ast::TemplateParam const> tparams = {},
                            diag::DiagnosticEngine* diag_ptr = nullptr)
                : m_bindings(b), m_types(tc), m_name_map(name_map), m_template_params(tparams), m_diag(diag_ptr)
            {
            }

            void substitute_in_type(ast::TypeExpr* t);
            void substitute_in_expr(ast::Expr* e);
            void substitute_in_stmt(ast::Stmt* s);
            void substitute_in_block(ast::Block& b);
            void substitute_in_pattern(ast::Pattern* p);
            void substitute_in_decl(ast::Decl* d);

            [[nodiscard]] types::TypePtr resolve_pack_index_type(ast::PackIndexType const* pi) const;

        private:
            infer::TemplateBindings const& m_bindings;
            types::TypeContext& m_types;
            si::InternedHashMap<types::TypePtr> const& m_name_map;
            std::span<ast::TemplateParam const> m_template_params;
            diag::DiagnosticEngine* m_diag{};

            [[nodiscard]] types::TypePtr deep_substitute(types::TypePtr type) const
            {
                if (!type)
                    return nullptr;

                auto sub = m_bindings.substitute(type);
                if (sub != type)
                    return sub;

                if (auto const* param = types::type_cast<types::TemplateParamType>(type))
                {
                    auto it = m_name_map.find(param->name);
                    if (it != m_name_map.end())
                        return it->second;

                    return type;
                }

                switch (type->kind)
                {
                    case types::TypeKind::Pointer: {
                        auto const* p = static_cast<types::PointerType const*>(static_cast<void const*>(type));
                        auto inner = deep_substitute(p->pointee);
                        if (inner != p->pointee)
                            return m_types.pointer_to(inner, p->pointee_quals);

                        return type;
                    }
                    case types::TypeKind::Array: {
                        auto const* a = static_cast<types::ArrayType const*>(static_cast<void const*>(type));
                        auto inner = deep_substitute(a->element);
                        if (inner != a->element)
                            return m_types.array_t(inner, a->count);

                        return type;
                    }
                    case types::TypeKind::RuntimeArray: {
                        auto const* rt = static_cast<types::RuntimeArrayType const*>(static_cast<void const*>(type));
                        auto inner = deep_substitute(rt->element);
                        if (inner != rt->element)
                            return m_types.runtime_array_t(inner);

                        return type;
                    }
                    case types::TypeKind::Slice: {
                        auto const* s = static_cast<types::SliceType const*>(static_cast<void const*>(type));
                        auto inner = deep_substitute(s->element);
                        if (inner != s->element)
                            return m_types.slice_t(inner, s->element_quals);

                        return type;
                    }
                    case types::TypeKind::Fam: {
                        auto const* f = static_cast<types::FamType const*>(static_cast<void const*>(type));
                        auto inner = deep_substitute(f->element);
                        if (inner != f->element)
                            return m_types.fam_t(inner);

                        return type;
                    }
                    case types::TypeKind::TypePack: {
                        auto const* pt = static_cast<types::TypePackType const*>(type);
                        if (auto const* tpt = types::type_cast<types::TemplateParamType>(pt->element))
                        {
                            auto const* pack = m_bindings.lookup_pack(tpt);
                            if (pack && pt->pack_index < pack->size())
                                return (*pack)[pt->pack_index];
                            return m_types.m_errort();
                        }
                        auto inner = deep_substitute(pt->element);
                        if (inner != pt->element)
                            return m_types.type_pack_t(inner, pt->pack_index);
                        return type;
                    }
                    case types::TypeKind::FuncPtr: {
                        auto const* f = static_cast<types::FuncPtrType const*>(static_cast<void const*>(type));
                        auto ret = deep_substitute(f->return_type);
                        bool changed = (ret != f->return_type);
                        std::vector<types::TypePtr> params;
                        params.reserve(f->params.size());
                        for (auto p : f->params)
                        {
                            auto sp = deep_substitute(p);
                            if (sp != p)
                                changed = true;

                            params.push_back(sp);
                        }
                        if (changed)
                            return m_types.funcptr_t(ret, params);

                        return type;
                    }
                    case types::TypeKind::Nominal: {
                        auto const* t = static_cast<types::NominalType const*>(static_cast<void const*>(type));
                        auto underlying = deep_substitute(t->underlying);
                        if (underlying != t->underlying)
                            return m_types.nominal_alias_t(underlying, t->decl);
                        return type;
                    }
                    default:
                        return type;
                }
            }

            static comptime::Value* to_local_cv(ast::ExprSema const& sema) { return const_cast<comptime::Value*>(sema.const_value); }

            static comptime::Value const* to_local_cv_const(ast::ExprSema const& sema) { return sema.const_value; }
        };

        types::TypePtr TypeSubstitutor::resolve_pack_index_type(ast::PackIndexType const* pi) const
        {
            if (pi->base && !pi->base->sema.canonical)
            {
                if (auto* base_named = ast::node_cast<ast::NamedType>(pi->base))
                {
                    if (base_named->path.is_simple())
                    {
                        std::string_view name = base_named->path.simple_name();
                        std::optional<std::int64_t> idx_val;
                        if (auto* lit = ast::node_cast<ast::IntLiteralExpr>(pi->index))
                            idx_val = lit->value;
                        else if (pi->index && pi->index->sema.const_value)
                            idx_val = pi->index->sema.const_value->const_to_int();

                        if (!idx_val || *idx_val < 0)
                            return m_types.m_errort();

                        std::uint32_t index = static_cast<std::uint32_t>(*idx_val);

                        for (auto const& tp : m_template_params)
                        {
                            if (tp.name == name && tp.is_pack)
                            {
                                auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(&tp), tp.name, 0);
                                if (!param_ty)
                                    continue;
                                auto const* tp_ptr = types::type_cast<types::TemplateParamType>(param_ty);
                                if (!tp_ptr)
                                    continue;
                                auto const* pack = m_bindings.lookup_pack(tp_ptr);
                                if (pack)
                                {
                                    if (index < pack->size())
                                        return (*pack)[index];
                                    if (m_diag)
                                        m_diag->error(pi->range, "pack index {} out of bounds for pack '{}' of length {}", index, name, pack->size());
                                    return m_types.m_errort();
                                }
                                return m_types.m_errort();
                            }

                            if (tp.name == name && m_diag)
                            {
                                m_diag->error(pi->range, "cannot index non-pack type with .N syntax");
                                return m_types.m_errort();
                            }
                        }

                        return m_types.m_errort();
                    }
                }

                return m_types.m_errort();
            }
            return nullptr;
        }

        void TypeSubstitutor::substitute_in_type(ast::TypeExpr* t)
        {
            if (!t)
                return;

            if (t->sema.canonical)
            {
                auto canon = get_canonical(t->sema);
                auto sub = deep_substitute(canon);
                if (sub != canon)
                    set_canonical(t->sema, sub);
            }

            switch (t->kind)
            {
                case ast::TypeKind::Pointer:
                    substitute_in_type(static_cast<ast::PointerType*>(t)->pointee);
                    break;
                case ast::TypeKind::Array: {
                    auto* arr = static_cast<ast::ArrayType*>(t);
                    substitute_in_type(arr->element);
                    if (arr->size)
                        substitute_in_expr(arr->size);

                    break;
                }
                case ast::TypeKind::Slice:
                    substitute_in_type(static_cast<ast::SliceType*>(t)->element);
                    break;
                case ast::TypeKind::Fam:
                    substitute_in_type(static_cast<ast::FamType*>(t)->element);
                    break;
                case ast::TypeKind::FuncPtr: {
                    auto* fp = static_cast<ast::FuncPtrType*>(t);
                    substitute_in_type(fp->return_type);
                    for (auto* p : fp->params)
                        substitute_in_type(p);
                    break;
                }
                case ast::TypeKind::Qualified:
                    substitute_in_type(static_cast<ast::QualifiedType*>(t)->inner);
                    break;
                case ast::TypeKind::Named: {
                    auto* nt = static_cast<ast::NamedType*>(t);
                    if (!t->sema.canonical && nt->path.is_simple() && nt->template_args.empty())
                    {
                        auto it = m_name_map.find(nt->path.simple_name());
                        if (it != m_name_map.end())
                        {
                            set_canonical(t->sema, it->second);
                            break;
                        }
                    }
                    for (auto& ta : nt->template_args)
                    {
                        substitute_in_type(ta.type);
                        if (ta.expr)
                            substitute_in_expr(ta.expr);
                    }
                    break;
                }
                case ast::TypeKind::Primitive:
                    break;
                case ast::TypeKind::PackIndex: {
                    auto* pi = static_cast<ast::PackIndexType*>(t);
                    substitute_in_type(pi->base);
                    if (pi->index)
                        substitute_in_expr(pi->index);

                    auto resolved = resolve_pack_index_type(pi);
                    if (resolved)
                    {
                        set_canonical(t->sema, resolved);
                    }
                    else if (pi->base && pi->base->sema.canonical)
                    {
                        auto base_canon = get_canonical(pi->base->sema);

                        std::optional<std::int64_t> idx_val;
                        if (auto* lit = ast::node_cast<ast::IntLiteralExpr>(pi->index))
                            idx_val = lit->value;
                        else if (pi->index && pi->index->sema.const_value)
                            idx_val = pi->index->sema.const_value->const_to_int();

                        if (!idx_val || *idx_val < 0)
                        {
                            set_canonical(t->sema, m_types.m_errort());
                            break;
                        }

                        std::uint32_t index = static_cast<std::uint32_t>(*idx_val);

                        if (auto const* tpp = types::type_cast<types::TemplateParamType>(base_canon))
                        {
                            auto const* pack = m_bindings.lookup_pack(tpp);
                            if (pack)
                            {
                                if (index < pack->size())
                                {
                                    set_canonical(t->sema, (*pack)[index]);
                                    break;
                                }
                                if (m_diag)
                                    m_diag->error(pi->range, "pack index {} out of bounds for pack '{}' of length {}", index, tpp->name, pack->size());
                            }
                            set_canonical(t->sema, m_types.m_errort());
                            break;
                        }
                        else if (auto const* tpt = types::type_cast<types::TypePackType>(base_canon))
                        {
                            std::uint32_t final_index = tpt->pack_index + index;
                            if (auto const* ept = types::type_cast<types::TemplateParamType>(tpt->element))
                            {
                                auto const* pack = m_bindings.lookup_pack(ept);
                                if (pack)
                                {
                                    if (final_index < pack->size())
                                    {
                                        set_canonical(t->sema, (*pack)[final_index]);
                                        break;
                                    }
                                    if (m_diag)
                                        m_diag->error(pi->range, "pack index {} out of bounds for pack '{}' of length {}", final_index, ept->name, pack->size());
                                }
                            }

                            auto elem_ty = m_types.type_pack_t(tpt->element, final_index);
                            set_canonical(t->sema, elem_ty);
                            break;
                        }

                        if (m_diag)
                            m_diag->error(pi->range, "cannot index non-pack type with .N syntax");
                        set_canonical(t->sema, m_types.m_errort());
                    }
                    break;
                }
            }
        }

        void TypeSubstitutor::substitute_in_expr(ast::Expr* e)
        {
            if (!e)
                return;

            e->sema.is_from_instantiation = true;

            if (e->sema.resolved_type)
            {
                auto canon = get_resolved_type(e->sema);
                auto sub = deep_substitute(canon);
                if (sub != canon)
                    set_resolved_type(e->sema, sub);
            }

            if (e->sema.const_value)
            {
                auto* cv = to_local_cv(e->sema);
                auto sub = deep_substitute(cv->type);
                if (sub != cv->type)
                    cv->type = sub;
            }

            switch (e->kind)
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
                case ast::ExprKind::PathExpr:
                    break;

                case ast::ExprKind::Unary:
                    substitute_in_expr(static_cast<ast::UnaryExpr*>(e)->operand);
                    break;
                case ast::ExprKind::Postfix:
                    substitute_in_expr(static_cast<ast::PostfixExpr*>(e)->operand);
                    break;
                case ast::ExprKind::Binary: {
                    auto* ex = static_cast<ast::BinaryExpr*>(e);
                    substitute_in_expr(ex->lhs);
                    substitute_in_expr(ex->rhs);
                    break;
                }
                case ast::ExprKind::Call: {
                    auto* ex = static_cast<ast::CallExpr*>(e);
                    substitute_in_expr(ex->callee);
                    for (auto* a : ex->args)
                        substitute_in_expr(a);
                    break;
                }
                case ast::ExprKind::FieldAccess:
                    substitute_in_expr(static_cast<ast::FieldAccessExpr*>(e)->object);
                    break;
                case ast::ExprKind::PackAccess: {
                    auto* ex = static_cast<ast::PackAccessExpr*>(e);
                    substitute_in_expr(ex->object);
                    substitute_in_expr(ex->index);
                    break;
                }
                case ast::ExprKind::Index: {
                    auto* ex = static_cast<ast::IndexExpr*>(e);
                    substitute_in_expr(ex->object);
                    substitute_in_expr(ex->index);
                    break;
                }
                case ast::ExprKind::Cast: {
                    auto* ex = static_cast<ast::CastExpr*>(e);
                    substitute_in_expr(ex->operand);
                    substitute_in_type(ex->target);
                    break;
                }
                case ast::ExprKind::Block:
                    substitute_in_block(static_cast<ast::BlockExpr*>(e)->body);
                    break;
                case ast::ExprKind::If: {
                    auto* ex = static_cast<ast::IfExpr*>(e);
                    substitute_in_expr(ex->condition);
                    substitute_in_block(ex->then_block);
                    substitute_in_expr(ex->else_branch);
                    break;
                }
                case ast::ExprKind::Match: {
                    auto* ex = static_cast<ast::MatchExpr*>(e);
                    substitute_in_expr(ex->operand);
                    for (auto& arm : ex->arms)
                    {
                        substitute_in_pattern(arm.pattern);
                        substitute_in_type(arm.type_pattern);
                        substitute_in_expr(arm.guard);
                        substitute_in_expr(arm.body);
                    }
                    break;
                }
                case ast::ExprKind::StructLiteral: {
                    auto* ex = static_cast<ast::StructLiteralExpr*>(e);
                    substitute_in_type(ex->type);
                    for (auto& f : ex->fields)
                        substitute_in_expr(f.value);

                    break;
                }
                case ast::ExprKind::Sizeof:
                    substitute_in_type(static_cast<ast::SizeofExpr*>(e)->target);
                    break;
                case ast::ExprKind::Alignof:
                    substitute_in_type(static_cast<ast::AlignofExpr*>(e)->target);
                    break;
                case ast::ExprKind::Offsetof:
                    substitute_in_type(static_cast<ast::OffsetofExpr*>(e)->target);
                    break;
                case ast::ExprKind::Compiles: {
                    auto* ex = static_cast<ast::CompilesExpr*>(e);
                    for (auto& p : ex->params)
                        substitute_in_type(p.type);
                    substitute_in_block(ex->body);
                    break;
                }
                case ast::ExprKind::TypeAST:
                    substitute_in_type(static_cast<ast::TypeASTExpr*>(e)->type_node);
                    break;
                case ast::ExprKind::TemplateInst: {
                    auto* ex = static_cast<ast::TemplateInstExpr*>(e);
                    substitute_in_expr(ex->callee);
                    for (auto& ta : ex->template_args)
                    {
                        substitute_in_type(ta.type);
                        if (ta.expr)
                            substitute_in_expr(ta.expr);
                    }
                    break;
                }
                case ast::ExprKind::Range: {
                    auto* ex = static_cast<ast::RangeExpr*>(e);
                    substitute_in_expr(ex->start);
                    substitute_in_expr(ex->end);
                    break;
                }
                case ast::ExprKind::SizeofPack:
                    break;
                case ast::ExprKind::PackExpansion:
                    substitute_in_expr(static_cast<ast::PackExpansionExpr*>(e)->operand);
                    break;
            }
        }

        void TypeSubstitutor::substitute_in_stmt(ast::Stmt* s)
        {
            if (!s)
                return;

            switch (s->kind)
            {
                case ast::StmtKind::Expr:
                    substitute_in_expr(static_cast<ast::ExprStmt*>(s)->expr);
                    break;
                case ast::StmtKind::DeclStmt:
                    substitute_in_decl(static_cast<ast::DeclStmt*>(s)->decl);
                    break;
                case ast::StmtKind::Return:
                    substitute_in_expr(static_cast<ast::ReturnStmt*>(s)->value);
                    break;
                case ast::StmtKind::Break:
                case ast::StmtKind::Continue:
                    break;
                case ast::StmtKind::While: {
                    auto* st = static_cast<ast::WhileStmt*>(s);
                    substitute_in_expr(st->condition);
                    substitute_in_block(st->body);
                    break;
                }
                case ast::StmtKind::DoWhile: {
                    auto* st = static_cast<ast::DoWhileStmt*>(s);
                    substitute_in_block(st->body);
                    substitute_in_expr(st->condition);
                    break;
                }
                case ast::StmtKind::For: {
                    auto* st = static_cast<ast::ForStmt*>(s);
                    substitute_in_stmt(st->init);
                    substitute_in_expr(st->cond);
                    substitute_in_expr(st->update);
                    substitute_in_block(st->body);
                    break;
                }
                case ast::StmtKind::ForIn: {
                    auto* st = static_cast<ast::ForInStmt*>(s);
                    substitute_in_type(st->item_type);
                    substitute_in_expr(st->iterable);
                    substitute_in_block(st->body);
                    break;
                }
                case ast::StmtKind::Defer:
                    substitute_in_stmt(static_cast<ast::DeferStmt*>(s)->body);
                    break;
                case ast::StmtKind::StaticIf: {
                    auto* st = static_cast<ast::StaticIfStmt*>(s);
                    substitute_in_expr(st->condition);
                    substitute_in_block(st->then_block);
                    substitute_in_stmt(st->else_branch);
                    break;
                }
                case ast::StmtKind::StaticMatch: {
                    auto* st = static_cast<ast::StaticMatchStmt*>(s);
                    substitute_in_expr(st->operand);
                    for (auto& arm : st->arms)
                    {
                        substitute_in_pattern(arm.pattern);
                        substitute_in_type(arm.type_pattern);
                        substitute_in_expr(arm.guard);
                        substitute_in_expr(arm.body);
                    }
                    break;
                }
                case ast::StmtKind::Ambiguous: {
                    auto* st = static_cast<ast::AmbiguousStmt*>(s);
                    substitute_in_decl(st->as_decl);
                    substitute_in_expr(st->as_expr);
                    break;
                }
                case ast::StmtKind::StaticFor: {
                    auto* st = static_cast<ast::StaticForStmt*>(s);
                    substitute_in_expr(st->pack_expr);
                    substitute_in_block(st->body);
                    break;
                }
            }
        }

        void TypeSubstitutor::substitute_in_block(ast::Block& b)
        {
            for (auto* stmt : b.stmts)
                substitute_in_stmt(stmt);
            if (b.tail)
                substitute_in_expr(b.tail);
        }

        void TypeSubstitutor::substitute_in_pattern(ast::Pattern* p)
        {
            if (!p)
                return;

            switch (p->kind)
            {
                case ast::PatternKind::Literal:
                    substitute_in_expr(static_cast<ast::LiteralPattern*>(p)->value);
                    break;
                case ast::PatternKind::Binding:
                case ast::PatternKind::Wildcard:
                    break;
                case ast::PatternKind::Ref:
                    substitute_in_pattern(static_cast<ast::RefPattern*>(p)->inner);
                    break;
                case ast::PatternKind::EnumDestructure: {
                    for (auto* sub : static_cast<ast::EnumDestructurePattern*>(p)->payload)
                        substitute_in_pattern(sub);
                    break;
                }
                case ast::PatternKind::StructDestructure: {
                    for (auto& f : static_cast<ast::StructDestructurePattern*>(p)->fields)
                        substitute_in_pattern(f.pattern);
                    break;
                }
                case ast::PatternKind::Range: {
                    auto* pp = static_cast<ast::RangePattern*>(p);
                    substitute_in_expr(pp->start);
                    substitute_in_expr(pp->end);
                    break;
                }
                case ast::PatternKind::Or: {
                    for (auto* alt : static_cast<ast::OrPattern*>(p)->alternatives)
                        substitute_in_pattern(alt);
                    break;
                }
            }
        }

        void TypeSubstitutor::substitute_in_decl(ast::Decl* d)
        {
            if (!d)
                return;

            switch (d->kind)
            {
                case ast::DeclKind::Var: {
                    auto* dd = static_cast<ast::VarDecl*>(d);
                    substitute_in_type(dd->type);
                    substitute_in_expr(dd->init);
                    break;
                }
                case ast::DeclKind::Func: {
                    auto* dd = static_cast<ast::FuncDecl*>(d);
                    substitute_in_type(dd->return_type);
                    for (auto& p : dd->params)
                    {
                        substitute_in_type(p.type);
                        if (p.type && p.type->sema.canonical)
                        {
                            auto canon = get_canonical(p.type->sema);
                            auto sub = m_bindings.substitute(canon);
                            if (sub != canon)
                                set_canonical(p.type->sema, sub);
                        }
                    }
                    substitute_in_expr(dd->constraint);
                    if (dd->body)
                        substitute_in_block(*dd->body);
                    break;
                }
                default:
                    break;
            }
        }

        class StaticFolder
        {
        public:
            StaticFolder(infer::TemplateBindings const& bindings, ast::FuncDecl const& template_fn, types::TypeContext& type_ctx, ast::AstContext& ast_ctx)
                : m_param_type_map(build_param_type_map(bindings, template_fn, type_ctx)), m_type_ctx(type_ctx), m_ast_ctx(ast_ctx)
            {
            }

            void fold_block(ast::Block& b);

            static si::InternedHashMap<types::TypePtr> build_param_type_map(infer::TemplateBindings const& bindings, ast::FuncDecl const& template_fn,
                                                                            types::TypeContext& type_ctx)
            {
                si::InternedHashMap<types::TypePtr> map;
                for (std::size_t i = 0; i < template_fn.template_params.size(); ++i)
                {
                    auto const& tp = template_fn.template_params[i];
                    auto param_ty = type_ctx.template_param_t(const_cast<ast::TemplateParam*>(&tp), tp.name, static_cast<std::uint32_t>(i));
                    auto substituted = bindings.substitute(param_ty);
                    if (substituted != param_ty)
                        map[tp.name] = substituted;
                }
                return map;
            }

        private:
            si::InternedHashMap<types::TypePtr> m_param_type_map;
            types::TypeContext& m_type_ctx;
            ast::AstContext& m_ast_ctx;

            std::pmr::vector<ast::StmtPtr> fold_static_if(ast::StaticIfStmt& si, std::pmr::polymorphic_allocator<> alloc);
            std::pmr::vector<ast::StmtPtr> fold_static_match(ast::StaticMatchStmt& sm, std::pmr::polymorphic_allocator<> alloc);
            void fold_in_stmt(ast::Stmt* s);
            void fold_in_expr(ast::Expr* e);

            [[nodiscard]] types::TypePtr resolve_primitive_type_name(std::string_view name) const
            {
                if (name == "void")
                    return m_type_ctx.m_voidt();
                if (name == "bool")
                    return m_type_ctx.m_boolt();
                if (name == "char")
                    return m_type_ctx.m_chart();
                if (name == "i8")
                    return m_type_ctx.int_t(8, true);
                if (name == "i16")
                    return m_type_ctx.int_t(16, true);
                if (name == "i32")
                    return m_type_ctx.int_t(32, true);
                if (name == "i64")
                    return m_type_ctx.int_t(64, true);
                if (name == "isize")
                    return m_type_ctx.isize_t();
                if (name == "u8")
                    return m_type_ctx.int_t(8, false);
                if (name == "u16")
                    return m_type_ctx.int_t(16, false);
                if (name == "u32")
                    return m_type_ctx.int_t(32, false);
                if (name == "u64")
                    return m_type_ctx.int_t(64, false);
                if (name == "usize")
                    return m_type_ctx.usize_t();
                if (name == "f32")
                    return m_type_ctx.float_t(32);
                if (name == "f64")
                    return m_type_ctx.float_t(64);
                return nullptr;
            }
        };

        std::pmr::vector<ast::StmtPtr> StaticFolder::fold_static_if(ast::StaticIfStmt& si, std::pmr::polymorphic_allocator<> alloc)
        {
            auto take_then = [&]() -> std::pmr::vector<ast::StmtPtr> {
                fold_block(si.then_block);
                return std::pmr::vector<ast::StmtPtr>(si.then_block.stmts.begin(), si.then_block.stmts.end(), alloc);
            };

            auto take_else = [&]() -> std::pmr::vector<ast::StmtPtr> {
                if (!si.else_branch)
                    return std::pmr::vector<ast::StmtPtr>(alloc);
                if (si.else_branch->kind == ast::StmtKind::StaticIf)
                    return fold_static_if(*static_cast<ast::StaticIfStmt*>(si.else_branch), alloc);
                fold_in_stmt(si.else_branch);
                std::pmr::vector<ast::StmtPtr> taken(alloc);
                taken.push_back(si.else_branch);
                return taken;
            };

            if (!si.is_type_if)
            {
                auto* bin = ast::node_cast<ast::BinaryExpr>(si.condition);
                auto* lhs_ident = bin ? ast::node_cast<ast::IdentExpr>(bin->lhs) : nullptr;
                if (lhs_ident && m_param_type_map.find(lhs_ident->name) != m_param_type_map.end())
                    si.is_type_if = true;
            }

            if (si.is_type_if)
            {
                auto* bin = ast::node_cast<ast::BinaryExpr>(si.condition);
                if (bin && bin->op == lex::TokenKind::EqEq)
                {
                    auto* ident = ast::node_cast<ast::IdentExpr>(bin->lhs);
                    if (ident)
                    {
                        auto it = m_param_type_map.find(ident->name);
                        if (it != m_param_type_map.end())
                        {
                            auto param_concrete = it->second;
                            types::TypePtr rhs_type = nullptr;

                            if (auto* rhs_ident = ast::node_cast<ast::IdentExpr>(bin->rhs))
                            {
                                if (rhs_ident->sema.resolved_type)
                                    rhs_type = get_resolved_type(rhs_ident->sema);
                                else
                                    rhs_type = resolve_primitive_type_name(rhs_ident->name);
                            }
                            else if (auto* type_ast = ast::node_cast<ast::TypeASTExpr>(bin->rhs))
                            {
                                if (type_ast->type_node && type_ast->type_node->sema.canonical)
                                    rhs_type = get_canonical(type_ast->type_node->sema);
                            }

                            if (rhs_type)
                            {
                                if (param_concrete == rhs_type)
                                    return take_then();
                                else
                                    return take_else();
                            }
                        }
                    }
                }
            }

            if (si.condition && si.condition->sema.const_value)
            {
                auto const* cv = si.condition->sema.const_value;
                if (cv->kind() == comptime::Value::Kind::Bool)
                {
                    if (cv->get_bool())
                        return take_then();
                    else
                        return take_else();
                }
            }

            std::pmr::vector<ast::StmtPtr> result(alloc);
            result.push_back(&si);
            return result;
        }

        std::pmr::vector<ast::StmtPtr> StaticFolder::fold_static_match(ast::StaticMatchStmt& sm, std::pmr::polymorphic_allocator<> alloc)
        {
            types::TypePtr scrutinee_type = nullptr;
            if (sm.is_type_match)
            {
                if (auto* ident = ast::node_cast<ast::IdentExpr>(sm.operand))
                {
                    auto it = m_param_type_map.find(ident->name);
                    if (it != m_param_type_map.end())
                        scrutinee_type = it->second;
                }
            }

            for (auto const& arm : sm.arms)
            {
                bool matches = false;

                if (arm.type_pattern)
                {
                    auto arm_type = get_canonical(arm.type_pattern->sema);
                    if (scrutinee_type && arm_type && scrutinee_type == arm_type)
                        matches = true;
                }
                else if (arm.pattern && arm.pattern->kind == ast::PatternKind::Wildcard)
                    matches = true;
                else if (arm.pattern && arm.pattern->kind == ast::PatternKind::Binding)
                    matches = true;

                if (matches)
                {
                    if (arm.body)
                    {
                        if (arm.body->kind == ast::ExprKind::Block)
                            fold_block(static_cast<ast::BlockExpr*>(arm.body)->body);
                        else
                            fold_in_expr(arm.body);
                    }

                    std::pmr::vector<ast::StmtPtr> result(alloc);

                    if (arm.body && arm.body->kind == ast::ExprKind::Block)
                    {
                        auto& blk = static_cast<ast::BlockExpr*>(arm.body)->body;
                        fold_block(blk);
                        for (auto* s : blk.stmts)
                            result.push_back(s);
                        if (blk.tail)
                        {
                            auto* tail_stmt = m_ast_ctx.make<ast::ExprStmt>(blk.tail->range, blk.tail);
                            result.push_back(tail_stmt);
                        }
                    }
                    else if (arm.body)
                    {
                        auto* stmt = m_ast_ctx.make<ast::ExprStmt>(arm.body->range, arm.body);
                        result.push_back(stmt);
                    }

                    return result;
                }
            }

            std::pmr::vector<ast::StmtPtr> result(alloc);
            result.push_back(&sm);
            return result;
        }

        void StaticFolder::fold_in_stmt(ast::Stmt* s)
        {
            if (!s)
                return;

            switch (s->kind)
            {
                case ast::StmtKind::While:
                    fold_block(static_cast<ast::WhileStmt*>(s)->body);
                    break;
                case ast::StmtKind::DoWhile:
                    fold_block(static_cast<ast::DoWhileStmt*>(s)->body);
                    break;
                case ast::StmtKind::For:
                    fold_block(static_cast<ast::ForStmt*>(s)->body);
                    break;
                case ast::StmtKind::ForIn:
                    fold_block(static_cast<ast::ForInStmt*>(s)->body);
                    break;
                case ast::StmtKind::Defer:
                    fold_in_stmt(static_cast<ast::DeferStmt*>(s)->body);
                    break;
                case ast::StmtKind::Expr:
                    fold_in_expr(static_cast<ast::ExprStmt*>(s)->expr);
                    break;
                default:
                    break;
            }
        }

        void StaticFolder::fold_in_expr(ast::Expr* e)
        {
            if (!e)
                return;

            switch (e->kind)
            {
                case ast::ExprKind::Block:
                    fold_block(static_cast<ast::BlockExpr*>(e)->body);
                    break;
                case ast::ExprKind::If: {
                    auto* ife = static_cast<ast::IfExpr*>(e);
                    fold_block(ife->then_block);
                    fold_in_expr(ife->else_branch);
                    break;
                }
                case ast::ExprKind::Match: {
                    for (auto& arm : static_cast<ast::MatchExpr*>(e)->arms)
                        fold_in_expr(arm.body);
                    break;
                }
                case ast::ExprKind::Unary:
                    fold_in_expr(static_cast<ast::UnaryExpr*>(e)->operand);
                    break;
                case ast::ExprKind::Binary: {
                    auto* bin = static_cast<ast::BinaryExpr*>(e);
                    fold_in_expr(bin->lhs);
                    fold_in_expr(bin->rhs);
                    break;
                }
                case ast::ExprKind::Call: {
                    auto* call = static_cast<ast::CallExpr*>(e);
                    fold_in_expr(call->callee);
                    for (auto* a : call->args)
                        fold_in_expr(a);
                    break;
                }
                case ast::ExprKind::Compiles:
                    fold_block(static_cast<ast::CompilesExpr*>(e)->body);
                    break;
                default:
                    break;
            }
        }

        void StaticFolder::fold_block(ast::Block& b)
        {
            std::pmr::vector<ast::StmtPtr> new_stmts(b.stmts.get_allocator());

            for (auto* stmt : b.stmts)
                if (stmt->kind == ast::StmtKind::StaticIf)
                {
                    auto folded = fold_static_if(*static_cast<ast::StaticIfStmt*>(stmt), b.stmts.get_allocator());
                    for (auto* f : folded)
                        new_stmts.push_back(f);
                }
                else if (stmt->kind == ast::StmtKind::StaticMatch)
                {
                    auto folded = fold_static_match(*static_cast<ast::StaticMatchStmt*>(stmt), b.stmts.get_allocator());
                    for (auto* f : folded)
                        new_stmts.push_back(f);
                }
                else
                {
                    fold_in_stmt(stmt);
                    new_stmts.push_back(stmt);
                }

            b.stmts = std::move(new_stmts);

            if (b.tail)
                fold_in_expr(b.tail);
        }

    } // anonymous namespace

} // namespace dcc::sema

export namespace dcc::sema
{

    struct InstantiatedFunc
    {
        ast::FuncDecl* decl;
        types::FuncPtrType* type;
    };

    [[nodiscard]] ast::TypeExpr* clone_type_from_canonical(types::TypePtr ty, ast::AstContext& ast_ctx, types::TypeContext& type_ctx)
    {
        if (!ty)
            return nullptr;

        auto const make_primitive = [&](lex::TokenKind tk) -> ast::PrimitiveType* {
            auto* r = ast_ctx.make<ast::PrimitiveType>(sm::SourceRange{}, tk);
            set_canonical(r->sema, ty);
            return r;
        };

        switch (ty->kind)
        {
            case types::TypeKind::Void:
                return make_primitive(lex::TokenKind::KwVoid);
            case types::TypeKind::Bool:
                return make_primitive(lex::TokenKind::KwBool);
            case types::TypeKind::Char:
                return make_primitive(lex::TokenKind::KwChar);
            case types::TypeKind::Int: {
                auto const* it = static_cast<types::IntType const*>(ty);
                if (it->is_pointer_sized)
                {
                    auto tk = it->is_signed ? lex::TokenKind::KwIsize : lex::TokenKind::KwUsize;
                    return make_primitive(tk);
                }
                auto tk = it->bits == 8    ? (it->is_signed ? lex::TokenKind::Kwi8 : lex::TokenKind::Kwu8)
                          : it->bits == 16 ? (it->is_signed ? lex::TokenKind::Kwi16 : lex::TokenKind::Kwu16)
                          : it->bits == 32 ? (it->is_signed ? lex::TokenKind::Kwi32 : lex::TokenKind::Kwu32)
                                           : (it->is_signed ? lex::TokenKind::Kwi64 : lex::TokenKind::Kwu64);
                return make_primitive(tk);
            }
            case types::TypeKind::Float: {
                auto const* ft = static_cast<types::FloatType const*>(ty);
                auto tk = ft->bits == 32 ? lex::TokenKind::Kwf32 : lex::TokenKind::Kwf64;
                return make_primitive(tk);
            }
            case types::TypeKind::Pointer: {
                auto const* pt = static_cast<types::PointerType const*>(ty);
                auto* pointee = clone_type_from_canonical(pt->pointee, ast_ctx, type_ctx);
                auto* r = ast_ctx.make<ast::PointerType>(sm::SourceRange{}, pointee);
                set_canonical(r->sema, ty);
                return r;
            }
            case types::TypeKind::Slice: {
                auto const* st = static_cast<types::SliceType const*>(ty);
                auto* elem = clone_type_from_canonical(st->element, ast_ctx, type_ctx);
                auto* r = ast_ctx.make<ast::SliceType>(sm::SourceRange{}, elem);
                set_canonical(r->sema, ty);
                return r;
            }
            case types::TypeKind::Array: {
                auto const* at = static_cast<types::ArrayType const*>(ty);
                auto* elem = clone_type_from_canonical(at->element, ast_ctx, type_ctx);
                auto* size = ast_ctx.make<ast::IntLiteralExpr>(sm::SourceRange{}, static_cast<std::int64_t>(at->count), std::to_string(at->count));
                auto* r = ast_ctx.make<ast::ArrayType>(sm::SourceRange{}, elem, size);
                set_canonical(r->sema, ty);
                return r;
            }
            case types::TypeKind::FuncPtr: {
                auto const* ft = static_cast<types::FuncPtrType const*>(ty);
                auto* ret = clone_type_from_canonical(ft->return_type, ast_ctx, type_ctx);
                auto* r = ast_ctx.make<ast::FuncPtrType>(sm::SourceRange{}, ret, ast_ctx.allocator());
                for (auto const* p : ft->params)
                    r->params.push_back(clone_type_from_canonical(p, ast_ctx, type_ctx));
                set_canonical(r->sema, ty);
                return r;
            }
            case types::TypeKind::Struct: {
                auto const* ut = static_cast<types::UserType const*>(ty);
                auto const* sd = reinterpret_cast<ast::StructDecl const*>(ut->decl);
                ast::Path path(sm::SourceRange{}, ast_ctx.allocator());
                path.segments.push_back({sd->name, sm::SourceRange{}});
                auto* nt = ast_ctx.make<ast::NamedType>(sm::SourceRange{}, std::move(path), ast_ctx.allocator());
                for (auto const& ta : ut->template_args)
                    nt->template_args.push_back(ast::TemplateArg{sm::SourceRange{}, clone_type_from_canonical(ta, ast_ctx, type_ctx), nullptr});
                set_canonical(nt->sema, ty);
                return nt;
            }
            case types::TypeKind::Union: {
                auto const* ut = static_cast<types::UserType const*>(ty);
                auto const* ud = reinterpret_cast<ast::UnionDecl const*>(ut->decl);
                ast::Path path(sm::SourceRange{}, ast_ctx.allocator());
                path.segments.push_back({ud->name, sm::SourceRange{}});
                auto* nt = ast_ctx.make<ast::NamedType>(sm::SourceRange{}, std::move(path), ast_ctx.allocator());
                for (auto const& ta : ut->template_args)
                    nt->template_args.push_back(ast::TemplateArg{sm::SourceRange{}, clone_type_from_canonical(ta, ast_ctx, type_ctx), nullptr});
                set_canonical(nt->sema, ty);
                return nt;
            }
            case types::TypeKind::Enum: {
                auto const* ut = static_cast<types::UserType const*>(ty);
                auto const* ed = reinterpret_cast<ast::EnumDecl const*>(ut->decl);
                ast::Path path(sm::SourceRange{}, ast_ctx.allocator());
                path.segments.push_back({ed->name, sm::SourceRange{}});
                auto* nt = ast_ctx.make<ast::NamedType>(sm::SourceRange{}, std::move(path), ast_ctx.allocator());
                for (auto const& ta : ut->template_args)
                    nt->template_args.push_back(ast::TemplateArg{sm::SourceRange{}, clone_type_from_canonical(ta, ast_ctx, type_ctx), nullptr});
                set_canonical(nt->sema, ty);
                return nt;
            }
            case types::TypeKind::Nominal: {
                auto const* nt = static_cast<types::NominalType const*>(ty);
                auto const* ud = reinterpret_cast<ast::UsingDecl const*>(nt->decl);
                ast::Path path(sm::SourceRange{}, ast_ctx.allocator());
                path.segments.push_back({ud->alias_path.simple_name(), sm::SourceRange{}});
                auto* named = ast_ctx.make<ast::NamedType>(sm::SourceRange{}, std::move(path), ast_ctx.allocator());
                set_canonical(named->sema, ty);
                return named;
            }
            default:
                return nullptr;
        }
    }

    [[nodiscard]] std::vector<ast::FuncParam> expand_func_params(ast::FuncDecl const& template_fn, infer::TemplateBindings const& bindings,
                                                                 ast::AstContext& ast_ctx, types::TypeContext& type_ctx,
                                                                 diag::DiagnosticEngine* diag = nullptr)
    {
        std::vector<ast::FuncParam> result;

        bool has_pack = false;
        std::size_t non_pack_count = template_fn.params.size();
        if (!template_fn.params.empty() && is_func_param_sema_pack(template_fn.params.back(), template_fn))
        {
            has_pack = true;
            non_pack_count = template_fn.params.size() - 1;
        }

        AstCloner cloner{ast_ctx};
        si::InternedHashMap<types::TypePtr> empty_name_map;
        for (std::size_t i = 0; i < non_pack_count; ++i)
        {
            auto const& p = template_fn.params[i];
            ast::FuncParam new_param = cloner.clone_func_param(p);
            {
                TypeSubstitutor sub{const_cast<infer::TemplateBindings&>(bindings), type_ctx, empty_name_map, template_fn.template_params, diag};
                sub.substitute_in_type(new_param.type);
            }
            result.push_back(std::move(new_param));
        }

        if (has_pack)
        {
            auto const& pack_param = template_fn.params.back();
            auto pack_ty = pack_param.type && pack_param.type->sema.canonical ? get_canonical(pack_param.type->sema) : nullptr;
            auto sub_pack_ty = pack_ty ? bindings.substitute(pack_ty) : nullptr;

            std::size_t pack_count = 0;
            types::TypePtr elem_type = nullptr;
            bool is_pack_binding = false;

            if (auto const* pack_type = types::type_cast<types::TypePackType>(sub_pack_ty))
            {
                elem_type = pack_type->element;
                if (auto const* tp = types::type_cast<types::TemplateParamType>(elem_type))
                {
                    auto* p = bindings.lookup_pack(tp);
                    if (p)
                    {
                        pack_count = p->size();
                        is_pack_binding = true;
                    }
                }
            }
            else if (auto const* tp = types::type_cast<types::TemplateParamType>(sub_pack_ty))
            {
                auto* p = bindings.lookup_pack(tp);
                if (p)
                {
                    pack_count = p->size();
                    elem_type = tp;
                    is_pack_binding = true;
                }
            }

            if (pack_count == 0 && sub_pack_ty && !is_pack_binding)
            {
                if (auto const* pack_type = types::type_cast<types::TypePackType>(sub_pack_ty))
                {
                    if (auto const* inner_tp = types::type_cast<types::TemplateParamType>(pack_type->element))
                    {
                        auto single = bindings.lookup(inner_tp);
                        if (single)
                        {
                            pack_count = 1;
                            elem_type = single;
                        }
                    }
                    else
                    {
                        pack_count = 1;
                        elem_type = pack_type->element;
                    }
                }
                else
                {
                    pack_count = 1;
                    elem_type = sub_pack_ty;
                }
            }

            if (pack_count == 0)
                return result;

            for (std::size_t i = 0; i < pack_count; ++i)
            {
                ast::FuncParam new_param;
                if (pack_count == 1)
                    new_param.name = pack_param.name;
                else
                {
                    auto name_str = std::format("{}_{}", pack_param.name, i);
                    auto* buf = static_cast<char*>(ast_ctx.resource()->allocate(name_str.size() + 1, alignof(char)));
                    std::memcpy(buf, name_str.data(), name_str.size() + 1);
                    new_param.name = std::string_view{buf, name_str.size()};
                }
                new_param.range = pack_param.range;
                new_param.sema = pack_param.sema;
                new_param.sema.param_index = static_cast<std::uint32_t>(non_pack_count + i);
                new_param.is_pack = false;
                new_param.type = nullptr;

                if (is_pack_binding)
                {
                    if (auto const* pack = bindings.lookup_pack(static_cast<types::TemplateParamType const*>(elem_type)))
                    {
                        if (i < pack->size())
                        {
                            auto elem = (*pack)[i];
                            new_param.type = clone_type_from_canonical(elem, ast_ctx, type_ctx);
                        }
                    }
                }
                else
                    new_param.type = clone_type_from_canonical(elem_type, ast_ctx, type_ctx);

                result.push_back(std::move(new_param));
            }
        }

        return result;
    }

    InstantiatedFunc instantiate_with_bindings(ast::FuncDecl const& template_fn, infer::TemplateBindings const& bindings, ast::AstContext& ast_ctx,
                                               types::TypeContext& type_ctx, diag::DiagnosticEngine* diag = nullptr)
    {
        std::pmr::vector<types::TypePtr> param_types(std::pmr::polymorphic_allocator<>{ast_ctx.resource()});

        bool has_pack = false;
        std::size_t non_pack_count = template_fn.params.size();
        if (!template_fn.params.empty() && is_func_param_sema_pack(template_fn.params.back(), template_fn))
        {
            has_pack = true;
            non_pack_count = template_fn.params.size() - 1;
        }

        for (std::size_t i = 0; i < non_pack_count; ++i)
        {
            auto const& p = template_fn.params[i];
            auto ty = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : type_ctx.m_errort();
            param_types.push_back(bindings.substitute(ty));
        }

        if (has_pack)
        {
            auto const& pack_param = template_fn.params.back();
            auto pack_ty = pack_param.type && pack_param.type->sema.canonical ? get_canonical(pack_param.type->sema) : nullptr;
            auto sub_pack_ty = pack_ty ? bindings.substitute(pack_ty) : nullptr;

            types::TypePtr elem_type = nullptr;
            if (auto const* pack_type = types::type_cast<types::TypePackType>(sub_pack_ty))
                elem_type = pack_type->element;
            else if (auto const* tp = types::type_cast<types::TemplateParamType>(sub_pack_ty))
                elem_type = tp;

            if (elem_type)
            {
                if (auto const* tp = types::type_cast<types::TemplateParamType>(elem_type))
                {
                    auto const* pack = bindings.lookup_pack(tp);
                    if (pack)
                    {
                        for (auto const& pt : *pack)
                            param_types.push_back(pt);
                    }
                    else
                    {
                        auto single = bindings.lookup(tp);
                        if (single)
                            param_types.push_back(single);
                        else
                            param_types.push_back(type_ctx.m_errort());
                    }
                }
                else
                    param_types.push_back(bindings.substitute(elem_type));
            }
            else
                param_types.push_back(sub_pack_ty ? sub_pack_ty : type_ctx.m_errort());
        }

        auto ret_ty = template_fn.return_type && template_fn.return_type->sema.canonical ? get_canonical(template_fn.return_type->sema) : type_ctx.m_voidt();
        ret_ty = bindings.substitute(ret_ty);

        auto fp = type_ctx.funcptr_t(ret_ty, param_types);
        auto* fp_type = const_cast<types::FuncPtrType*>(types::type_cast<types::FuncPtrType>(fp));

        auto param_type_map = StaticFolder::build_param_type_map(bindings, template_fn, type_ctx);

        si::InternedHashMap<comptime::Value> nttp_map;
        for (std::size_t i = 0; i < template_fn.template_params.size(); ++i)
        {
            auto const& tp = template_fn.template_params[i];
            if (!tp.value_type)
                continue;
            auto* param_ty = type_ctx.template_param_t(const_cast<ast::TemplateParam*>(&tp), tp.name, static_cast<std::uint32_t>(i));
            if (!param_ty)
                continue;
            auto const* bv = bindings.lookup_value(static_cast<types::TemplateParamType const*>(param_ty));
            if (bv)
                nttp_map[tp.name] = *bv;
        }

        class NttpReplacer
        {
        public:
            NttpReplacer(si::InternedHashMap<comptime::Value> const& map, ast::AstContext& ast_ctx) : m_map(map), m_ast_ctx(ast_ctx) {}

            void replace_in_block(ast::Block& b)
            {
                for (auto* stmt : b.stmts)
                    replace_in_stmt(stmt);
                if (b.tail)
                    replace_in_expr(b.tail);
            }

        private:
            si::InternedHashMap<comptime::Value> const& m_map;
            ast::AstContext& m_ast_ctx;

            void set_constant_sema(ast::Expr* e, comptime::Value const& v)
            {
                set_resolved_type(e->sema, v.type);
                e->sema.is_constant = true;
                e->sema.const_value = m_ast_ctx.own_value(v);
            }

            void replace_in_expr(ast::Expr* e)
            {
                if (!e)
                    return;

                if (e->kind == ast::ExprKind::Ident)
                {
                    auto* ident = static_cast<ast::IdentExpr*>(e);
                    auto it = m_map.find(ident->name);
                    if (it != m_map.end())
                    {
                        set_constant_sema(e, it->second);
                        return;
                    }
                }

                switch (e->kind)
                {
                    case ast::ExprKind::Unary:
                        replace_in_expr(static_cast<ast::UnaryExpr*>(e)->operand);
                        break;
                    case ast::ExprKind::Postfix:
                        replace_in_expr(static_cast<ast::PostfixExpr*>(e)->operand);
                        break;
                    case ast::ExprKind::Binary: {
                        auto* bin = static_cast<ast::BinaryExpr*>(e);
                        replace_in_expr(bin->lhs);
                        replace_in_expr(bin->rhs);
                        break;
                    }
                    case ast::ExprKind::Call: {
                        auto* call = static_cast<ast::CallExpr*>(e);
                        replace_in_expr(call->callee);
                        for (auto* a : call->args)
                            replace_in_expr(a);
                        break;
                    }
                    case ast::ExprKind::FieldAccess:
                        replace_in_expr(static_cast<ast::FieldAccessExpr*>(e)->object);
                        break;
                    case ast::ExprKind::PackAccess: {
                        auto* pa = static_cast<ast::PackAccessExpr*>(e);
                        replace_in_expr(pa->object);
                        replace_in_expr(pa->index);
                        break;
                    }
                    case ast::ExprKind::Index: {
                        auto* idx = static_cast<ast::IndexExpr*>(e);
                        replace_in_expr(idx->object);
                        replace_in_expr(idx->index);
                        break;
                    }
                    case ast::ExprKind::Cast: {
                        auto* cast = static_cast<ast::CastExpr*>(e);
                        replace_in_expr(cast->operand);
                        break;
                    }
                    case ast::ExprKind::Block:
                        replace_in_block(static_cast<ast::BlockExpr*>(e)->body);
                        break;
                    case ast::ExprKind::If: {
                        auto* ife = static_cast<ast::IfExpr*>(e);
                        replace_in_expr(ife->condition);
                        replace_in_block(ife->then_block);
                        replace_in_expr(ife->else_branch);
                        break;
                    }
                    case ast::ExprKind::Match: {
                        auto* me = static_cast<ast::MatchExpr*>(e);
                        replace_in_expr(me->operand);
                        for (auto& arm : me->arms)
                            replace_in_expr(arm.body);
                        break;
                    }
                    case ast::ExprKind::StructLiteral: {
                        for (auto& f : static_cast<ast::StructLiteralExpr*>(e)->fields)
                            replace_in_expr(f.value);
                        break;
                    }
                    case ast::ExprKind::Range: {
                        auto* r = static_cast<ast::RangeExpr*>(e);
                        replace_in_expr(r->start);
                        replace_in_expr(r->end);
                        break;
                    }
                    case ast::ExprKind::TemplateInst: {
                        auto* ti = static_cast<ast::TemplateInstExpr*>(e);
                        replace_in_expr(ti->callee);
                        for (auto& ta : ti->template_args)
                            if (ta.expr)
                                replace_in_expr(ta.expr);
                        break;
                    }
                    case ast::ExprKind::SizeofPack:
                        break;
                    case ast::ExprKind::PackExpansion:
                        replace_in_expr(static_cast<ast::PackExpansionExpr*>(e)->operand);
                        break;
                    default:
                        break;
                }
            }

            void replace_in_stmt(ast::Stmt* s)
            {
                if (!s)
                    return;

                switch (s->kind)
                {
                    case ast::StmtKind::Expr:
                        replace_in_expr(static_cast<ast::ExprStmt*>(s)->expr);
                        break;
                    case ast::StmtKind::Return:
                        replace_in_expr(static_cast<ast::ReturnStmt*>(s)->value);
                        break;
                    case ast::StmtKind::While: {
                        auto* ws = static_cast<ast::WhileStmt*>(s);
                        replace_in_expr(ws->condition);
                        replace_in_block(ws->body);
                        break;
                    }
                    case ast::StmtKind::DoWhile: {
                        auto* dw = static_cast<ast::DoWhileStmt*>(s);
                        replace_in_block(dw->body);
                        replace_in_expr(dw->condition);
                        break;
                    }
                    case ast::StmtKind::For: {
                        auto* fs = static_cast<ast::ForStmt*>(s);
                        replace_in_stmt(fs->init);
                        replace_in_expr(fs->cond);
                        replace_in_expr(fs->update);
                        replace_in_block(fs->body);
                        break;
                    }
                    case ast::StmtKind::ForIn: {
                        auto* fi = static_cast<ast::ForInStmt*>(s);
                        replace_in_expr(fi->iterable);
                        replace_in_block(fi->body);
                        break;
                    }
                    case ast::StmtKind::StaticIf: {
                        auto* si = static_cast<ast::StaticIfStmt*>(s);
                        replace_in_expr(si->condition);
                        replace_in_block(si->then_block);
                        replace_in_stmt(si->else_branch);
                        break;
                    }
                    case ast::StmtKind::StaticMatch: {
                        auto* sm = static_cast<ast::StaticMatchStmt*>(s);
                        replace_in_expr(sm->operand);
                        for (auto& arm : sm->arms)
                            replace_in_expr(arm.body);
                        break;
                    }
                    case ast::StmtKind::StaticFor: {
                        auto* sf = static_cast<ast::StaticForStmt*>(s);
                        replace_in_expr(sf->pack_expr);
                        replace_in_block(sf->body);
                        break;
                    }
                    default:
                        break;
                }
            }
        };

        std::optional<ast::Block> cloned_body;

        std::unordered_map<std::string, types::TypePtr> expanded_ptypes;
        std::unordered_map<std::string, ast::VarDecl*> expanded_decls;
        std::vector<ast::FuncParam> expanded_params;
        if (template_fn.body)
        {
            expanded_params = expand_func_params(template_fn, bindings, ast_ctx, type_ctx, diag);
            for (auto& p : expanded_params)
            {
                if (auto ty = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : nullptr)
                    expanded_ptypes[std::string{p.name}] = ty;

                auto* vd = ast_ctx.make<ast::VarDecl>(p.range, p.name, p.range);
                vd->type = p.type;
                vd->sema.storage = ast::StorageClass::Param;
                p.synthetic_decl = vd;
                expanded_decls[std::string{p.name}] = vd;
            }

            AstCloner cloner{ast_ctx};
            cloned_body = cloner.clone_block(*template_fn.body);

            TypeSubstitutor substitutor{bindings, type_ctx, param_type_map, template_fn.template_params, diag};
            substitutor.substitute_in_block(*cloned_body);

            if (!nttp_map.empty())
            {
                NttpReplacer replacer{nttp_map, ast_ctx};
                replacer.replace_in_block(*cloned_body);
            }

            struct PackInfo
            {
                std::vector<types::TypePtr> types;
                std::vector<comptime::Value> values;
                bool is_value_pack{false};
            };
            std::unordered_map<std::string_view, PackInfo> pack_lookup;

            for (auto const& tp : template_fn.template_params)
            {
                if (!tp.is_pack)
                    continue;

                auto* param_ty = type_ctx.template_param_t(const_cast<ast::TemplateParam*>(&tp), tp.name,
                                                           static_cast<std::uint32_t>(&tp - template_fn.template_params.data()));
                if (!param_ty)
                    continue;

                auto const* tp_ptr = static_cast<types::TemplateParamType const*>(param_ty);

                if (tp.value_type)
                {
                    auto* vp = const_cast<infer::TemplateBindings&>(bindings).lookup_value_pack(tp_ptr);
                    if (vp)
                    {
                        PackInfo pi;
                        pi.is_value_pack = true;
                        pi.values = *vp;
                        for (auto const& v : *vp)
                            pi.types.push_back(v.type);
                        pack_lookup[tp.name] = std::move(pi);
                    }
                }
                else
                {
                    auto* p = const_cast<infer::TemplateBindings&>(bindings).lookup_pack(tp_ptr);
                    if (p)
                    {
                        PackInfo pi;
                        pi.types = *p;
                        pack_lookup[tp.name] = std::move(pi);
                    }
                }
            }

            for (auto const& tp : template_fn.template_params)
            {
                if (tp.is_pack && !pack_lookup.count(tp.name))
                {
                    auto* param_ty = type_ctx.template_param_t(const_cast<ast::TemplateParam*>(&tp), tp.name,
                                                               static_cast<std::uint32_t>(&tp - template_fn.template_params.data()));
                    if (!param_ty)
                        continue;

                    auto const* tp_ptr = static_cast<types::TemplateParamType const*>(param_ty);
                    auto* p = const_cast<infer::TemplateBindings&>(bindings).lookup_pack(tp_ptr);
                    if (p)
                    {
                        PackInfo pi;
                        pi.types = *p;
                        pack_lookup[tp.name] = std::move(pi);
                    }
                    else
                    {
                        auto bound = bindings.lookup(tp_ptr);
                        if (bound)
                        {
                            PackInfo pi;
                            pi.types.push_back(bound);
                            pack_lookup[tp.name] = std::move(pi);
                        }
                    }
                }
            }

            for (auto const& p : template_fn.params)
            {
                if (p.type && p.type->kind == ast::TypeKind::PackIndex)
                    continue;

                if (!pack_lookup.count(p.name))
                {
                    auto canon = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : nullptr;
                    if (!canon)
                        continue;

                    bool param_is_pack = p.is_pack || types::type_cast<types::TypePackType>(canon) != nullptr;
                    if (!param_is_pack)
                    {
                        if (auto const* tpt = types::type_cast<types::TemplateParamType>(canon))
                        {
                            for (auto const& tp : template_fn.template_params)
                                if (tp.name == tpt->name && tp.is_pack)
                                {
                                    param_is_pack = true;
                                    break;
                                }
                        }
                    }
                    if (!param_is_pack)
                        continue;

                    types::TypePtr inner = canon;
                    if (auto const* pt = types::type_cast<types::TypePackType>(canon))
                        inner = pt->element;
                    if (auto const* tp = types::type_cast<types::TemplateParamType>(inner))
                    {
                        auto it = pack_lookup.find(tp->name);
                        if (it != pack_lookup.end())
                            pack_lookup[p.name] = it->second;
                    }
                }
            }

            struct PackExpander
            {
                ast::AstContext& ast_ctx;
                types::TypeContext& type_ctx;
                infer::TemplateBindings const& bindings;
                std::unordered_map<std::string_view, PackInfo> const& pack_info;
                si::InternedHashMap<types::TypePtr> const& type_map;
                si::InternedHashMap<types::TypePtr> const& pack_param_types;
                std::unordered_map<std::string, types::TypePtr> const& expanded_ptypes;
                std::unordered_map<std::string, ast::VarDecl*> const& expanded_decls;
                diag::DiagnosticEngine* m_diag{};

                PackExpander(ast::AstContext& ctx, types::TypeContext& tc, infer::TemplateBindings const& b,
                             std::unordered_map<std::string_view, PackInfo> const& pi, si::InternedHashMap<types::TypePtr> const& tm,
                             si::InternedHashMap<types::TypePtr> const& ppt, std::unordered_map<std::string, types::TypePtr> const& ept,
                             std::unordered_map<std::string, ast::VarDecl*> const& ed, diag::DiagnosticEngine* diag = nullptr)
                    : ast_ctx(ctx), type_ctx(tc), bindings(b), pack_info(pi), type_map(tm), pack_param_types(ppt), expanded_ptypes(ept), expanded_decls(ed),
                      m_diag(diag)
                {
                }

                void expand_block(ast::Block& blk)
                {
                    std::pmr::vector<ast::StmtPtr> new_stmts(blk.stmts.get_allocator());

                    for (auto* stmt : blk.stmts)
                    {
                        if (stmt->kind == ast::StmtKind::StaticFor)
                        {
                            auto expanded = expand_static_for(*static_cast<ast::StaticForStmt*>(stmt));
                            for (auto* s : expanded)
                            {
                                expand_in_stmt(s);
                                new_stmts.push_back(s);
                            }
                        }
                        else
                        {
                            expand_in_stmt(stmt);
                            new_stmts.push_back(stmt);
                        }
                    }

                    blk.stmts = std::move(new_stmts);

                    if (blk.tail)
                        expand_in_expr(blk.tail, true);
                }

                void expand_in_stmt(ast::Stmt* s)
                {
                    if (!s)
                        return;

                    switch (s->kind)
                    {
                        case ast::StmtKind::Expr:
                            if (auto* es = static_cast<ast::ExprStmt*>(s))
                                expand_in_expr(es->expr, false);
                            break;
                        case ast::StmtKind::DeclStmt: {
                            auto* ds = static_cast<ast::DeclStmt*>(s);
                            if (auto* vd = ast::node_cast<ast::VarDecl>(ds->decl))
                                expand_in_expr(vd->init, false);
                            break;
                        }
                        case ast::StmtKind::Return:
                            if (auto* rs = static_cast<ast::ReturnStmt*>(s))
                                expand_in_expr(rs->value, false);
                            break;
                        case ast::StmtKind::Break:
                            break;
                        case ast::StmtKind::Continue:
                            break;
                        case ast::StmtKind::While: {
                            auto* ws = static_cast<ast::WhileStmt*>(s);
                            expand_in_expr(ws->condition, false);
                            expand_block(ws->body);
                            break;
                        }
                        case ast::StmtKind::DoWhile: {
                            auto* dw = static_cast<ast::DoWhileStmt*>(s);
                            expand_block(dw->body);
                            expand_in_expr(dw->condition, false);
                            break;
                        }
                        case ast::StmtKind::For: {
                            auto* fs = static_cast<ast::ForStmt*>(s);
                            expand_in_stmt(fs->init);
                            expand_in_expr(fs->cond, false);
                            expand_in_expr(fs->update, false);
                            expand_block(fs->body);
                            break;
                        }
                        case ast::StmtKind::ForIn: {
                            auto* fi = static_cast<ast::ForInStmt*>(s);
                            expand_in_expr(fi->iterable, false);
                            expand_block(fi->body);
                            break;
                        }
                        case ast::StmtKind::Defer: {
                            auto* ds = static_cast<ast::DeferStmt*>(s);
                            expand_in_stmt(ds->body);
                            break;
                        }
                        case ast::StmtKind::StaticIf: {
                            auto* si = static_cast<ast::StaticIfStmt*>(s);
                            expand_in_expr(si->condition, false);
                            expand_block(si->then_block);
                            expand_in_stmt(si->else_branch);
                            break;
                        }
                        case ast::StmtKind::StaticMatch: {
                            auto* sm = static_cast<ast::StaticMatchStmt*>(s);
                            expand_in_expr(sm->operand, false);
                            for (auto& arm : sm->arms)
                                expand_in_expr(arm.body, false);
                            break;
                        }
                        case ast::StmtKind::StaticFor:
                            if (auto* sf = static_cast<ast::StaticForStmt*>(s))
                                expand_block(sf->body);
                            break;
                        case ast::StmtKind::Ambiguous: {
                            auto* amb = static_cast<ast::AmbiguousStmt*>(s);
                            if (amb->resolution == ast::AmbiguousStmt::Resolution::AsDecl && amb->as_decl)
                            {
                                if (auto* vd = ast::node_cast<ast::VarDecl>(amb->as_decl))
                                    expand_in_expr(vd->init, false);
                            }
                            else if (amb->resolution == ast::AmbiguousStmt::Resolution::AsExpr && amb->as_expr)
                            {
                                expand_in_expr(amb->as_expr, false);
                            }
                            break;
                        }
                        default:
                            if (m_diag)
                                m_diag->error(s->range, "internal error: unhandled statement kind {} in pack expansion",
                                              static_cast<int>(s->kind));
                            break;
                    }
                }

                ast::Expr* expand_sizeof_pack(std::string_view pack_name, sm::SourceRange range)
                {
                    std::size_t count = 0;
                    auto it = pack_info.find(pack_name);
                    if (it != pack_info.end())
                        count = it->second.types.size();

                    auto* lit = ast_ctx.make<ast::IntLiteralExpr>(range, static_cast<std::int64_t>(count), std::to_string(count));
                    auto u64_ty = type_ctx.int_t(64, false);
                    set_resolved_type(lit->sema, u64_ty);
                    lit->sema.is_constant = true;
                    auto val = comptime::Value::make_int(static_cast<std::int64_t>(count), u64_ty);
                    lit->sema.const_value = ast_ctx.own_value(std::move(val));
                    return lit;
                }

                [[nodiscard]] std::optional<std::int64_t> try_fold_index(ast::Expr* idx_expr) const
                {
                    if (!idx_expr)
                        return std::nullopt;

                    if (idx_expr->sema.const_value)
                    {
                        auto cv = idx_expr->sema.const_value;
                        if (cv->kind() == comptime::Value::Kind::Int)
                            return cv->get_int();
                        if (auto v = cv->const_to_int())
                            return v;
                        return std::nullopt;
                    }

                    if (auto* lit = ast::node_cast<ast::IntLiteralExpr>(idx_expr))
                        return lit->value;

                    if (auto* sp = ast::node_cast<ast::SizeofPackExpr>(idx_expr))
                    {
                        auto it = pack_info.find(sp->pack_name);
                        if (it != pack_info.end())
                            return static_cast<std::int64_t>(it->second.types.size());
                        return std::nullopt;
                    }

                    if (auto* un = ast::node_cast<ast::UnaryExpr>(idx_expr))
                    {
                        if (un->op == lex::TokenKind::Minus)
                        {
                            auto val = try_fold_index(un->operand);
                            if (val)
                                return -*val;
                        }
                        return std::nullopt;
                    }

                    if (auto* bin = ast::node_cast<ast::BinaryExpr>(idx_expr))
                    {
                        auto lhs = try_fold_index(bin->lhs);
                        auto rhs = try_fold_index(bin->rhs);
                        if (lhs && rhs)
                        {
                            switch (bin->op)
                            {
                                case lex::TokenKind::Plus:
                                    return *lhs + *rhs;
                                case lex::TokenKind::Minus:
                                    return *lhs - *rhs;
                                case lex::TokenKind::Star:
                                    return *lhs * *rhs;
                                case lex::TokenKind::Slash:
                                    if (*rhs != 0)
                                        return *lhs / *rhs;
                                    return std::nullopt;
                                default:
                                    return std::nullopt;
                            }
                        }
                        return std::nullopt;
                    }

                    return std::nullopt;
                }

                void expand_in_expr(ast::Expr*& e, bool in_call_args)
                {
                    if (!e)
                        return;

                    if (e->kind == ast::ExprKind::SizeofPack)
                    {
                        auto* sp = static_cast<ast::SizeofPackExpr*>(e);
                        auto it = pack_info.find(sp->pack_name);
                        if (it != pack_info.end())
                        {
                            e = expand_sizeof_pack(sp->pack_name, sp->range);
                            return;
                        }

                        e = expand_sizeof_pack(sp->pack_name, sp->range);
                        return;
                    }

                    if (e->kind == ast::ExprKind::PackExpansion && in_call_args)
                    {
                        auto* pe = static_cast<ast::PackExpansionExpr*>(e);

                        if (auto* idx = ast::node_cast<ast::IndexExpr>(pe->operand))
                        {
                            if (auto* obj_ident = ast::node_cast<ast::IdentExpr>(idx->object))
                            {
                                auto pit = pack_info.find(obj_ident->name);
                                if (pit != pack_info.end())
                                {
                                    if (m_diag)
                                        m_diag->error(pe->range, "pack-index expression cannot be expanded");
                                    return;
                                }
                            }
                        }

                        if (auto* pa = ast::node_cast<ast::PackAccessExpr>(pe->operand))
                        {
                            if (auto* obj_ident = ast::node_cast<ast::IdentExpr>(pa->object))
                            {
                                auto pit = pack_info.find(obj_ident->name);
                                if (pit != pack_info.end())
                                {
                                    if (m_diag)
                                        m_diag->error(pe->range, "pack-index expression cannot be expanded");
                                    return;
                                }
                            }
                        }

                        auto* ident = ast::node_cast<ast::IdentExpr>(pe->operand);
                        if (ident)
                        {
                            std::string_view pack_name = ident->name;
                            auto it = pack_info.find(pack_name);
                            if (it != pack_info.end())
                            {
                                if (!it->second.types.empty())
                                    ;
                            }
                        }

                        expand_in_expr(pe->operand, false);
                        return;
                    }

                    switch (e->kind)
                    {
                        case ast::ExprKind::Unary:
                            expand_in_expr(static_cast<ast::UnaryExpr*>(e)->operand, false);
                            break;
                        case ast::ExprKind::Postfix:
                            expand_in_expr(static_cast<ast::PostfixExpr*>(e)->operand, false);
                            break;
                        case ast::ExprKind::Binary: {
                            auto* bin = static_cast<ast::BinaryExpr*>(e);
                            expand_in_expr(bin->lhs, false);
                            expand_in_expr(bin->rhs, false);
                            break;
                        }
                        case ast::ExprKind::Call: {
                            auto* call = static_cast<ast::CallExpr*>(e);
                            expand_in_expr(call->callee, false);

                            std::pmr::vector<ast::ExprPtr> new_args(call->args.get_allocator());
                            for (auto* arg : call->args)
                            {
                                if (arg && arg->kind == ast::ExprKind::PackExpansion)
                                {
                                    auto* pe = static_cast<ast::PackExpansionExpr*>(arg);
                                    auto* ident = ast::node_cast<ast::IdentExpr>(pe->operand);
                                    if (ident)
                                    {
                                        auto it = pack_info.find(ident->name);
                                        if (it != pack_info.end() && !it->second.types.empty())
                                        {
                                            bool multi = it->second.types.size() > 1;
                                            for (std::size_t pi = 0; pi < it->second.types.size(); ++pi)
                                            {
                                                AstCloner cloner{ast_ctx};
                                                auto* cloned = cloner.clone_expr(pe->operand);
                                                auto elem_type = it->second.types[pi];
                                                set_resolved_type(cloned->sema, elem_type);
                                                auto* elem_ident = ast::node_cast<ast::IdentExpr>(cloned);
                                                if (elem_ident)
                                                {
                                                    auto name_key = multi ? std::format("{}_{}", ident->name, pi) : std::string{ident->name};
                                                    auto dit = expanded_decls.find(name_key);
                                                    if (dit != expanded_decls.end())
                                                    {
                                                        elem_ident->name = dit->second->name;
                                                        cloned->sema.resolved_decl = dit->second;
                                                    }
                                                    else if (multi)
                                                    {
                                                        auto* buf = static_cast<char*>(ast_ctx.resource()->allocate(name_key.size() + 1, alignof(char)));
                                                        std::memcpy(buf, name_key.data(), name_key.size() + 1);
                                                        elem_ident->name = std::string_view{buf, name_key.size()};
                                                    }

                                                    if (it->second.is_value_pack && pi < it->second.values.size())
                                                    {
                                                        auto const& v = it->second.values[pi];
                                                        cloned->sema.const_value = ast_ctx.own_value(v);
                                                        cloned->sema.is_constant = true;
                                                    }
                                                }
                                                new_args.push_back(cloned);
                                            }

                                            continue;
                                        }
                                    }
                                }
                                expand_in_expr(arg, true);
                                new_args.push_back(arg);
                            }
                            call->args = std::move(new_args);
                            break;
                        }
                        case ast::ExprKind::FieldAccess:
                            expand_in_expr(static_cast<ast::FieldAccessExpr*>(e)->object, false);
                            break;
                        case ast::ExprKind::Index: {
                            auto* idx = static_cast<ast::IndexExpr*>(e);
                            expand_in_expr(idx->object, false);
                            expand_in_expr(idx->index, false);

                            if (auto* obj_ident = ast::node_cast<ast::IdentExpr>(idx->object))
                            {
                                auto pit = pack_info.find(obj_ident->name);
                                if (pit != pack_info.end())
                                    if (m_diag)
                                        m_diag->error(idx->range, "use '.N' for pack indexing");
                            }
                            break;
                        }
                        case ast::ExprKind::PackAccess: {
                            auto* pa = static_cast<ast::PackAccessExpr*>(e);
                            expand_in_expr(pa->object, false);
                            expand_in_expr(pa->index, false);

                            auto replace_with_error = [&]() {
                                auto* err = ast_ctx.make<ast::IntLiteralExpr>(pa->range, 0, "0");
                                set_resolved_type(err->sema, type_ctx.m_errort());
                                e = err;
                            };

                            if (auto* obj_ident = ast::node_cast<ast::IdentExpr>(pa->object))
                            {
                                auto pit = pack_info.find(obj_ident->name);
                                if (pit != pack_info.end())
                                {
                                    auto const& pinfo = pit->second;
                                    auto const& pack_name = obj_ident->name;
                                    auto idx_val = try_fold_index(pa->index);
                                    if (idx_val)
                                    {
                                        std::int64_t index = *idx_val;
                                        if (index >= 0 && static_cast<std::size_t>(index) < pinfo.types.size())
                                        {
                                            bool single_elem = pinfo.types.size() == 1;
                                            std::string candidate_str;
                                            if (single_elem)
                                                candidate_str = std::string{pack_name};
                                            else
                                                candidate_str = std::format("{}_{}", pack_name, index);

                                            auto eit = expanded_ptypes.find(candidate_str);
                                            if (eit != expanded_ptypes.end())
                                            {
                                                auto dit = expanded_decls.find(candidate_str);
                                                std::string_view candidate;
                                                if (dit != expanded_decls.end())
                                                    candidate = dit->second->name;
                                                else
                                                {
                                                    auto* buf = static_cast<char*>(ast_ctx.resource()->allocate(candidate_str.size() + 1, alignof(char)));
                                                    std::memcpy(buf, candidate_str.data(), candidate_str.size() + 1);
                                                    candidate = std::string_view{buf, candidate_str.size()};
                                                }
                                                auto* new_ident = ast_ctx.make<ast::IdentExpr>(pa->range, candidate);
                                                set_resolved_type(new_ident->sema, eit->second);
                                                if (dit != expanded_decls.end())
                                                    new_ident->sema.resolved_decl = dit->second;
                                                if (pinfo.is_value_pack && static_cast<std::size_t>(index) < pinfo.values.size())
                                                {
                                                    auto const& v = pinfo.values[static_cast<std::size_t>(index)];
                                                    new_ident->sema.const_value = ast_ctx.own_value(v);
                                                    new_ident->sema.is_constant = true;
                                                }
                                                e = new_ident;
                                            }
                                            else
                                                replace_with_error();
                                        }
                                        else
                                        {
                                            if (m_diag)
                                                m_diag->error(pa->range, "pack index {} out of bounds for pack '{}' of length {}", index, pack_name,
                                                              pinfo.types.size());
                                            replace_with_error();
                                        }
                                    }
                                    else
                                    {
                                        if (m_diag)
                                            m_diag->error(pa->index->range, "pack index must be a compile-time integer");
                                        replace_with_error();
                                    }
                                }
                            }
                            break;
                        }
                        case ast::ExprKind::Ident: {
                            auto* ident = static_cast<ast::IdentExpr*>(e);
                            if (!e->sema.resolved_type)
                            {
                                auto it = pack_param_types.find(ident->name);
                                if (it != pack_param_types.end())
                                    set_resolved_type(e->sema, it->second);
                            }
                            break;
                        }
                        case ast::ExprKind::Cast:
                            expand_in_expr(static_cast<ast::CastExpr*>(e)->operand, false);
                            break;
                        case ast::ExprKind::Block:
                            expand_block(static_cast<ast::BlockExpr*>(e)->body);
                            break;
                        case ast::ExprKind::If: {
                            auto* ife = static_cast<ast::IfExpr*>(e);
                            expand_in_expr(ife->condition, false);
                            expand_block(ife->then_block);
                            expand_in_expr(ife->else_branch, false);
                            break;
                        }
                        case ast::ExprKind::Match: {
                            auto* me = static_cast<ast::MatchExpr*>(e);
                            expand_in_expr(me->operand, false);
                            for (auto& arm : me->arms)
                                expand_in_expr(arm.body, false);
                            break;
                        }
                        case ast::ExprKind::StructLiteral:
                            for (auto& f : static_cast<ast::StructLiteralExpr*>(e)->fields)
                                expand_in_expr(f.value, false);
                            break;
                        case ast::ExprKind::Range: {
                            auto* r = static_cast<ast::RangeExpr*>(e);
                            expand_in_expr(r->start, false);
                            expand_in_expr(r->end, false);
                            break;
                        }
                        case ast::ExprKind::TemplateInst: {
                            auto* ti = static_cast<ast::TemplateInstExpr*>(e);
                            expand_in_expr(ti->callee, false);
                            for (auto& ta : ti->template_args)
                                if (ta.expr)
                                    expand_in_expr(ta.expr, false);
                            break;
                        }
                        case ast::ExprKind::Compiles: {
                            auto* ce = static_cast<ast::CompilesExpr*>(e);
                            expand_block(ce->body);
                            break;
                        }
                        default:
                            break;
                    }

                }

                std::pmr::vector<ast::StmtPtr> expand_static_for(ast::StaticForStmt& sf)
                {
                    std::pmr::vector<ast::StmtPtr> result(sf.body.stmts.get_allocator());

                    if (auto* range = ast::node_cast<ast::RangeExpr>(sf.pack_expr))
                    {
                        AstCloner cloner{ast_ctx};
                        auto* cloned_start = range->start ? cloner.clone_expr(range->start) : nullptr;
                        auto* cloned_end = range->end ? cloner.clone_expr(range->end) : nullptr;

                        expand_in_expr(cloned_start, false);
                        expand_in_expr(cloned_end, false);

                        std::int64_t start_val = 0;
                        if (cloned_start)
                        {
                            auto folded = try_fold_index(cloned_start);
                            if (!folded)
                            {
                                if (m_diag)
                                    m_diag->error(range->start->range, "static for range start must be a compile-time constant integer");
                                result.push_back(&sf);
                                return result;
                            }
                            start_val = *folded;
                        }

                        if (!cloned_end)
                        {
                            if (m_diag)
                                m_diag->error(sf.range, "static for range requires an end bound");
                            result.push_back(&sf);
                            return result;
                        }

                        std::int64_t end_val = 0;
                        {
                            auto folded = try_fold_index(cloned_end);
                            if (!folded)
                            {
                                if (m_diag)
                                    m_diag->error(range->end->range, "static for range end must be a compile-time constant integer");
                                result.push_back(&sf);
                                return result;
                            }
                            end_val = *folded;
                        }

                        std::int64_t last_val = range->inclusive ? end_val : end_val - 1;

                        for (std::int64_t i = start_val; i <= last_val; ++i)
                        {
                            AstCloner cloner2{ast_ctx};
                            auto cloned_block = cloner2.clone_block(sf.body);

                            struct IntLoopVarReplacer
                            {
                                ast::AstContext& ctx;
                                types::TypeContext& type_ctx;
                                std::string_view var_name;
                                std::int64_t idx_value;
                                std::unordered_map<std::string_view, PackInfo> const& m_pack_info;

                                void replace_in_block(ast::Block& blk)
                                {
                                    for (auto* stmt : blk.stmts)
                                        replace_in_stmt(stmt);
                                    if (blk.tail)
                                        replace_in_expr(blk.tail);
                                }

                                void replace_in_stmt(ast::Stmt* s)
                                {
                                    if (!s)
                                        return;
                                    switch (s->kind)
                                    {
                                        case ast::StmtKind::Expr:
                                            if (auto* es = static_cast<ast::ExprStmt*>(s))
                                                replace_in_expr(es->expr);
                                            break;
                                        case ast::StmtKind::DeclStmt: {
                                            if (auto* ds = static_cast<ast::DeclStmt*>(s))
                                                if (auto* vd = ast::node_cast<ast::VarDecl>(ds->decl))
                                                    replace_in_expr(vd->init);
                                            break;
                                        }
                                        case ast::StmtKind::Return:
                                            if (auto* rs = static_cast<ast::ReturnStmt*>(s))
                                                replace_in_expr(rs->value);
                                            break;
                                        case ast::StmtKind::While: {
                                            auto* ws = static_cast<ast::WhileStmt*>(s);
                                            replace_in_expr(ws->condition);
                                            replace_in_block(ws->body);
                                            break;
                                        }
                                        case ast::StmtKind::DoWhile: {
                                            auto* dw = static_cast<ast::DoWhileStmt*>(s);
                                            replace_in_block(dw->body);
                                            replace_in_expr(dw->condition);
                                            break;
                                        }
                                        case ast::StmtKind::For: {
                                            auto* fs = static_cast<ast::ForStmt*>(s);
                                            replace_in_stmt(fs->init);
                                            replace_in_expr(fs->cond);
                                            replace_in_expr(fs->update);
                                            replace_in_block(fs->body);
                                            break;
                                        }
                                        case ast::StmtKind::ForIn: {
                                            auto* fi = static_cast<ast::ForInStmt*>(s);
                                            replace_in_expr(fi->iterable);
                                            replace_in_block(fi->body);
                                            break;
                                        }
                                        case ast::StmtKind::StaticIf: {
                                            auto* si = static_cast<ast::StaticIfStmt*>(s);
                                            replace_in_expr(si->condition);
                                            replace_in_block(si->then_block);
                                            replace_in_stmt(si->else_branch);
                                            break;
                                        }
                                        case ast::StmtKind::StaticMatch: {
                                            auto* sm = static_cast<ast::StaticMatchStmt*>(s);
                                            replace_in_expr(sm->operand);
                                            for (auto& arm : sm->arms)
                                                replace_in_expr(arm.body);
                                            break;
                                        }
                                        case ast::StmtKind::Defer: {
                                            auto* ds = static_cast<ast::DeferStmt*>(s);
                                            replace_in_stmt(ds->body);
                                            break;
                                        }
                                        default:
                                            break;
                                    }
                                }

                                    void replace_in_expr(ast::Expr*& e)
                                    {
                                        if (!e)
                                            return;
                                        if (e->kind == ast::ExprKind::Ident)
                                        {
                                            auto* ident = static_cast<ast::IdentExpr*>(e);
                                            if (ident->name == var_name)
                                            {
                                                auto* lit = ctx.make<ast::IntLiteralExpr>(e->range, idx_value, std::to_string(idx_value));
                                                auto u64_ty = type_ctx.int_t(64, false);
                                                set_resolved_type(lit->sema, u64_ty);
                                                lit->sema.is_constant = true;
                                                auto val = comptime::Value::make_int(idx_value, u64_ty);
                                                lit->sema.const_value = ctx.own_value(std::move(val));
                                                e = lit;
                                                return;
                                            }
                                        }

                                        if (e->kind == ast::ExprKind::FieldAccess)
                                        {
                                            auto* fa = static_cast<ast::FieldAccessExpr*>(e);
                                            if (fa->field == var_name)
                                            {
                                                if (auto* obj_ident = ast::node_cast<ast::IdentExpr>(fa->object))
                                                {
                                                    if (m_pack_info.count(obj_ident->name) != 0)
                                                    {
                                                        replace_in_expr(fa->object);

                                                        auto* lit = ctx.make<ast::IntLiteralExpr>(fa->field_range, idx_value, std::to_string(idx_value));
                                                        auto u64_ty = type_ctx.int_t(64, false);
                                                        set_resolved_type(lit->sema, u64_ty);
                                                        lit->sema.is_constant = true;
                                                        auto val = comptime::Value::make_int(idx_value, u64_ty);
                                                        lit->sema.const_value = ctx.own_value(std::move(val));

                                                        auto* pa = ctx.make<ast::PackAccessExpr>(e->range, fa->object, lit);
                                                        e = pa;
                                                        return;
                                                    }
                                                }
                                            }
                                        }

                                        switch (e->kind)
                                        {
                                            case ast::ExprKind::Unary:
                                                replace_in_expr(static_cast<ast::UnaryExpr*>(e)->operand);
                                                break;
                                            case ast::ExprKind::Postfix:
                                                replace_in_expr(static_cast<ast::PostfixExpr*>(e)->operand);
                                                break;
                                            case ast::ExprKind::Binary: {
                                                auto* bin = static_cast<ast::BinaryExpr*>(e);
                                                replace_in_expr(bin->lhs);
                                                replace_in_expr(bin->rhs);
                                                break;
                                            }
                                            case ast::ExprKind::Call: {
                                                auto* call = static_cast<ast::CallExpr*>(e);
                                                replace_in_expr(call->callee);
                                                for (auto*& a : call->args)
                                                    replace_in_expr(a);
                                                break;
                                            }
                                            case ast::ExprKind::FieldAccess:
                                                replace_in_expr(static_cast<ast::FieldAccessExpr*>(e)->object);
                                                break;
                                            case ast::ExprKind::Index: {
                                                auto* idx = static_cast<ast::IndexExpr*>(e);
                                                replace_in_expr(idx->object);
                                                replace_in_expr(idx->index);
                                                break;
                                            }
                                            case ast::ExprKind::PackAccess: {
                                                auto* pa = static_cast<ast::PackAccessExpr*>(e);
                                                replace_in_expr(pa->object);
                                                replace_in_expr(pa->index);
                                                break;
                                            }
                                            case ast::ExprKind::Cast:
                                                replace_in_expr(static_cast<ast::CastExpr*>(e)->operand);
                                                break;
                                            case ast::ExprKind::Block:
                                                replace_in_block(static_cast<ast::BlockExpr*>(e)->body);
                                                break;
                                            case ast::ExprKind::If: {
                                                auto* ife = static_cast<ast::IfExpr*>(e);
                                                replace_in_expr(ife->condition);
                                                replace_in_block(ife->then_block);
                                                replace_in_expr(ife->else_branch);
                                                break;
                                            }
                                            case ast::ExprKind::Match: {
                                                auto* me = static_cast<ast::MatchExpr*>(e);
                                                replace_in_expr(me->operand);
                                                for (auto& arm : me->arms)
                                                    replace_in_expr(arm.body);
                                                break;
                                            }
                                            case ast::ExprKind::StructLiteral: {
                                                for (auto& f : static_cast<ast::StructLiteralExpr*>(e)->fields)
                                                    replace_in_expr(f.value);
                                                break;
                                            }
                                            case ast::ExprKind::Range: {
                                                auto* r = static_cast<ast::RangeExpr*>(e);
                                                replace_in_expr(r->start);
                                                replace_in_expr(r->end);
                                                break;
                                            }
                                            case ast::ExprKind::TemplateInst: {
                                                auto* ti = static_cast<ast::TemplateInstExpr*>(e);
                                                replace_in_expr(ti->callee);
                                                for (auto& ta : ti->template_args)
                                                    if (ta.expr)
                                                        replace_in_expr(ta.expr);
                                                break;
                                            }
                                            case ast::ExprKind::SizeofPack:
                                                break;
                                            case ast::ExprKind::PackExpansion:
                                                replace_in_expr(static_cast<ast::PackExpansionExpr*>(e)->operand);
                                                break;
                                            case ast::ExprKind::Compiles: {
                                                auto* ce = static_cast<ast::CompilesExpr*>(e);
                                                replace_in_block(ce->body);
                                                break;
                                            }
                                            default:
                                                break;
                                        }
                                    };

                                };

                            IntLoopVarReplacer replacer{ast_ctx, type_ctx, sf.item_name, i, pack_info};
                            replacer.replace_in_block(cloned_block);

                            expand_block(cloned_block);

                            for (auto* s : cloned_block.stmts)
                                result.push_back(s);

                            if (cloned_block.tail)
                            {
                                auto* tail_stmt = ast_ctx.make<ast::ExprStmt>(cloned_block.tail->range, cloned_block.tail);
                                result.push_back(tail_stmt);
                            }
                        }

                        return result;
                    }

                    std::string_view pack_name;
                    if (auto* ident = ast::node_cast<ast::IdentExpr>(sf.pack_expr))
                        pack_name = ident->name;
                    else
                    {
                        result.push_back(&sf);
                        return result;
                    }

                    auto it = pack_info.find(pack_name);
                    if (it == pack_info.end() || it->second.types.empty())
                        return result;

                    auto const& pack = it->second;

                    for (std::size_t elem_idx = 0; elem_idx < pack.types.size(); ++elem_idx)
                    {
                        AstCloner cloner{ast_ctx};
                        auto cloned_block = cloner.clone_block(sf.body);

                        struct LoopVarReplacer
                        {
                            ast::AstContext& ctx;
                            types::TypeContext& type_ctx;
                            std::string_view var_name;
                            types::TypePtr elem_type;
                            comptime::Value const* elem_value;
                            bool is_value;
                            std::unordered_map<std::string, ast::VarDecl*> const& expanded_decls;
                            std::string_view pack_param_name;
                            std::size_t elem_index;
                            bool multi;

                            void replace_in_block(ast::Block& blk)
                            {
                                for (auto* stmt : blk.stmts)
                                    replace_in_stmt(stmt);
                                if (blk.tail)
                                    replace_in_expr(blk.tail);
                            }

                                void replace_in_stmt(ast::Stmt* s)
                                {
                                    if (!s)
                                        return;

                                    switch (s->kind)
                                    {
                                        case ast::StmtKind::Expr:
                                            if (auto* es = static_cast<ast::ExprStmt*>(s))
                                                replace_in_expr(es->expr);
                                            break;
                                        case ast::StmtKind::DeclStmt: {
                                            if (auto* ds = static_cast<ast::DeclStmt*>(s))
                                                if (auto* vd = ast::node_cast<ast::VarDecl>(ds->decl))
                                                    replace_in_expr(vd->init);
                                            break;
                                        }
                                        case ast::StmtKind::Return:
                                        if (auto* rs = static_cast<ast::ReturnStmt*>(s))
                                            replace_in_expr(rs->value);
                                        break;
                                    case ast::StmtKind::While: {
                                        auto* ws = static_cast<ast::WhileStmt*>(s);
                                        replace_in_expr(ws->condition);
                                        replace_in_block(ws->body);
                                        break;
                                    }
                                    case ast::StmtKind::DoWhile: {
                                        auto* dw = static_cast<ast::DoWhileStmt*>(s);
                                        replace_in_block(dw->body);
                                        replace_in_expr(dw->condition);
                                        break;
                                    }
                                    case ast::StmtKind::For: {
                                        auto* fs = static_cast<ast::ForStmt*>(s);
                                        replace_in_stmt(fs->init);
                                        replace_in_expr(fs->cond);
                                        replace_in_expr(fs->update);
                                        replace_in_block(fs->body);
                                        break;
                                    }
                                    case ast::StmtKind::ForIn: {
                                        auto* fi = static_cast<ast::ForInStmt*>(s);
                                        replace_in_expr(fi->iterable);
                                        replace_in_block(fi->body);
                                        break;
                                    }
                                    case ast::StmtKind::StaticIf: {
                                        auto* si = static_cast<ast::StaticIfStmt*>(s);
                                        replace_in_expr(si->condition);
                                        replace_in_block(si->then_block);
                                        replace_in_stmt(si->else_branch);
                                        break;
                                    }
                                    case ast::StmtKind::StaticMatch: {
                                        auto* sm = static_cast<ast::StaticMatchStmt*>(s);
                                        replace_in_expr(sm->operand);
                                        for (auto& arm : sm->arms)
                                            replace_in_expr(arm.body);
                                        break;
                                    }
                                    case ast::StmtKind::Defer: {
                                        auto* ds = static_cast<ast::DeferStmt*>(s);
                                        replace_in_stmt(ds->body);
                                        break;
                                    }
                                    default:
                                        break;
                                }
                            }

                            void replace_in_expr(ast::Expr*& e)
                            {
                                if (!e)
                                    return;

                                if (e->kind == ast::ExprKind::Ident)
                                {
                                    auto* ident = static_cast<ast::IdentExpr*>(e);
                                    if (ident->name == var_name)
                                    {
                                        if (is_value && elem_value)
                                        {
                                            set_resolved_type(e->sema, elem_value->type);
                                            e->sema.is_constant = true;
                                            e->sema.const_value = ctx.own_value(*elem_value);
                                        }
                                        else
                                            set_resolved_type(e->sema, elem_type);

                                        std::string name_key = multi ? std::format("{}_{}", pack_param_name, elem_index) : std::string{pack_param_name};
                                        auto dit = expanded_decls.find(name_key);
                                        if (dit != expanded_decls.end())
                                        {
                                            ident->name = dit->second->name;
                                            e->sema.resolved_decl = dit->second;
                                        }
                                        return;
                                    }
                                }

                                switch (e->kind)
                                {
                                    case ast::ExprKind::Unary:
                                        replace_in_expr(static_cast<ast::UnaryExpr*>(e)->operand);
                                        break;
                                    case ast::ExprKind::Postfix:
                                        replace_in_expr(static_cast<ast::PostfixExpr*>(e)->operand);
                                        break;
                                    case ast::ExprKind::Binary: {
                                        auto* bin = static_cast<ast::BinaryExpr*>(e);
                                        replace_in_expr(bin->lhs);
                                        replace_in_expr(bin->rhs);
                                        break;
                                    }
                                    case ast::ExprKind::Call: {
                                        auto* call = static_cast<ast::CallExpr*>(e);
                                        replace_in_expr(call->callee);
                                        for (auto*& a : call->args)
                                            replace_in_expr(a);
                                        break;
                                    }
                                    case ast::ExprKind::FieldAccess:
                                        replace_in_expr(static_cast<ast::FieldAccessExpr*>(e)->object);
                                        break;
                                    case ast::ExprKind::Index: {
                                        auto* idx = static_cast<ast::IndexExpr*>(e);
                                        replace_in_expr(idx->object);
                                        replace_in_expr(idx->index);
                                        break;
                                    }
                                    case ast::ExprKind::PackAccess: {
                                        auto* pa = static_cast<ast::PackAccessExpr*>(e);
                                        replace_in_expr(pa->object);
                                        replace_in_expr(pa->index);
                                        break;
                                    }
                                    case ast::ExprKind::Cast:
                                        replace_in_expr(static_cast<ast::CastExpr*>(e)->operand);
                                        break;
                                    case ast::ExprKind::Block:
                                        replace_in_block(static_cast<ast::BlockExpr*>(e)->body);
                                        break;
                                    case ast::ExprKind::If: {
                                        auto* ife = static_cast<ast::IfExpr*>(e);
                                        replace_in_expr(ife->condition);
                                        replace_in_block(ife->then_block);
                                        replace_in_expr(ife->else_branch);
                                        break;
                                    }
                                    case ast::ExprKind::Match: {
                                        auto* me = static_cast<ast::MatchExpr*>(e);
                                        replace_in_expr(me->operand);
                                        for (auto& arm : me->arms)
                                            replace_in_expr(arm.body);
                                        break;
                                    }
                                    case ast::ExprKind::StructLiteral: {
                                        for (auto& f : static_cast<ast::StructLiteralExpr*>(e)->fields)
                                            replace_in_expr(f.value);
                                        break;
                                    }
                                    case ast::ExprKind::Range: {
                                        auto* r = static_cast<ast::RangeExpr*>(e);
                                        replace_in_expr(r->start);
                                        replace_in_expr(r->end);
                                        break;
                                    }
                                    case ast::ExprKind::TemplateInst: {
                                        auto* ti = static_cast<ast::TemplateInstExpr*>(e);
                                        replace_in_expr(ti->callee);
                                        for (auto& ta : ti->template_args)
                                            if (ta.expr)
                                                replace_in_expr(ta.expr);
                                        break;
                                    }
                                    case ast::ExprKind::SizeofPack:
                                        break;
                                    case ast::ExprKind::PackExpansion:
                                        replace_in_expr(static_cast<ast::PackExpansionExpr*>(e)->operand);
                                        break;
                                    case ast::ExprKind::Compiles: {
                                        auto* ce = static_cast<ast::CompilesExpr*>(e);
                                        replace_in_block(ce->body);
                                        break;
                                    }
                                    default:
                                        break;
                                }
                            }
                        };

                        comptime::Value const* elem_val = nullptr;
                        if (it->second.is_value_pack && elem_idx < it->second.values.size())
                            elem_val = &it->second.values[elem_idx];

                        bool multi_pack = pack.types.size() > 1;
                        LoopVarReplacer replacer{ast_ctx,   type_ctx, sf.item_name, pack.types[elem_idx], elem_val, it->second.is_value_pack, expanded_decls,
                                                 pack_name, elem_idx, multi_pack};
                        replacer.replace_in_block(cloned_block);

                        for (auto* s : cloned_block.stmts)
                            result.push_back(s);

                        if (cloned_block.tail)
                        {
                            auto* tail_stmt = ast_ctx.make<ast::ExprStmt>(cloned_block.tail->range, cloned_block.tail);
                            result.push_back(tail_stmt);
                        }
                    }

                    return result;
                }
            };

            si::InternedHashMap<types::TypePtr> pack_param_types;
            for (auto const& tp : template_fn.template_params)
                if (tp.is_pack)
                    if (auto* pt = type_ctx.template_param_t(const_cast<ast::TemplateParam*>(&tp), tp.name,
                                                             static_cast<std::uint32_t>(&tp - template_fn.template_params.data())))
                        pack_param_types[tp.name] = static_cast<types::TypePtr>(pt);

            for (auto const& p : template_fn.params)
                if (!pack_param_types.count(p.name))
                    if (auto canon = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : nullptr)
                        if (auto const* tpt = types::type_cast<types::TemplateParamType>(canon))
                            for (auto const& tp : template_fn.template_params)
                                if (tp.name == tpt->name && tp.is_pack)
                                {
                                    pack_param_types[p.name] = types::TypePtr(tpt);
                                    break;
                                }

            PackExpander expander{ast_ctx, type_ctx, bindings, pack_lookup, param_type_map, pack_param_types, expanded_ptypes, expanded_decls, diag};
            expander.expand_block(*cloned_body);

            StaticFolder folder{bindings, template_fn, type_ctx, ast_ctx};
            folder.fold_block(*cloned_body);
        }

        auto* syn_decl = ast_ctx.make<ast::FuncDecl>(template_fn.range, template_fn.name, template_fn.name_range);

        syn_decl->return_type = template_fn.return_type ? [&]() -> ast::TypeExpr* {
            AstCloner cloner{ast_ctx};
            auto* ty = cloner.clone_type(template_fn.return_type);
            TypeSubstitutor sub{bindings, type_ctx, param_type_map, template_fn.template_params, diag};
            sub.substitute_in_type(ty);

            if (ty->sema.canonical)
            {
                auto canon = get_canonical(ty->sema);
                if (canon != fp_type->return_type)
                {
                    auto new_ft = type_ctx.funcptr_t(canon, fp_type->params);
                    if (auto* new_fp = const_cast<types::FuncPtrType*>(types::type_cast<types::FuncPtrType>(new_ft)))
                        fp_type = new_fp;
                }
            }

            return ty;
        }()
            : nullptr;

        syn_decl->template_params = {};

        for (auto& p : expanded_params)
            syn_decl->params.push_back(std::move(p));

        syn_decl->body = std::move(cloned_body);

        syn_decl->sema.is_intrinsic = template_fn.sema.is_intrinsic;

        if (diag)
        {
            struct Verifier
            {
                diag::DiagnosticEngine* d;
                ast::FuncDecl const* fn;

                void check_canon(sm::SourceRange loc, types::TypePtr canon, char const* ctx) const
                {
                    if (!canon)
                        return;
                    if (types::type_cast<types::TypePackType>(canon))
                        d->error(loc, "internal error: TypePackType survived in {} into lowerable code", ctx);
                    if (types::type_cast<types::TemplateParamType>(canon))
                        d->error(loc, "internal error: TemplateParamType survived in {} into lowerable code", ctx);
                }

                void check_type(ast::TypeExpr const* t, char const* ctx) const
                {
                    if (!t)
                        return;
                    auto canon = get_canonical(t->sema);
                    bool resolved = canon && !types::type_cast<types::TypePackType>(canon)
                                    && !types::type_cast<types::TemplateParamType>(canon);
                    if (t->kind == ast::TypeKind::PackIndex && !resolved)
                        d->error(t->range, "internal error: PackIndexType survived in {} into lowerable code", ctx);
                    check_canon(t->range, canon, ctx);
                }

                void walk_expr(ast::Expr const* e) const
                {
                    if (!e)
                        return;
                    if (e->kind == ast::ExprKind::PackAccess)
                        d->error(e->range, "internal error: PackAccessExpr survived into lowerable code");
                    check_canon(e->range, get_resolved_type(e->sema), "expr type");
                    switch (e->kind)
                    {
                        case ast::ExprKind::Unary:
                            walk_expr(static_cast<ast::UnaryExpr const*>(e)->operand);
                            break;
                        case ast::ExprKind::Postfix:
                            walk_expr(static_cast<ast::PostfixExpr const*>(e)->operand);
                            break;
                        case ast::ExprKind::Binary: {
                            auto* bin = static_cast<ast::BinaryExpr const*>(e);
                            walk_expr(bin->lhs);
                            walk_expr(bin->rhs);
                            break;
                        }
                        case ast::ExprKind::Call: {
                            auto* call = static_cast<ast::CallExpr const*>(e);
                            walk_expr(call->callee);
                            for (auto* a : call->args)
                                walk_expr(a);
                            break;
                        }
                        case ast::ExprKind::FieldAccess:
                            walk_expr(static_cast<ast::FieldAccessExpr const*>(e)->object);
                            break;
                        case ast::ExprKind::Index: {
                            auto* idx = static_cast<ast::IndexExpr const*>(e);
                            walk_expr(idx->object);
                            walk_expr(idx->index);
                            break;
                        }
                        case ast::ExprKind::Cast: {
                            auto* c = static_cast<ast::CastExpr const*>(e);
                            walk_expr(c->operand);
                            check_type(c->target, "cast target");
                            break;
                        }
                        case ast::ExprKind::Block:
                            walk_block(static_cast<ast::BlockExpr const*>(e)->body);
                            break;
                        case ast::ExprKind::If:
                            walk_expr(static_cast<ast::IfExpr const*>(e)->condition);
                            walk_block(static_cast<ast::IfExpr const*>(e)->then_block);
                            walk_expr(static_cast<ast::IfExpr const*>(e)->else_branch);
                            break;
                        case ast::ExprKind::Match: {
                            auto* me = static_cast<ast::MatchExpr const*>(e);
                            walk_expr(me->operand);
                            for (auto const& arm : me->arms)
                                walk_expr(arm.body);
                            break;
                        }
                        case ast::ExprKind::StructLiteral: {
                            auto* sl = static_cast<ast::StructLiteralExpr const*>(e);
                            check_type(sl->type, "struct literal type");
                            for (auto const& f : sl->fields)
                                walk_expr(f.value);
                            break;
                        }
                        case ast::ExprKind::Sizeof:
                            check_type(static_cast<ast::SizeofExpr const*>(e)->target, "sizeof");
                            break;
                        case ast::ExprKind::Alignof:
                            check_type(static_cast<ast::AlignofExpr const*>(e)->target, "alignof");
                            break;
                        case ast::ExprKind::Offsetof:
                            check_type(static_cast<ast::OffsetofExpr const*>(e)->target, "offsetof");
                            break;
                        case ast::ExprKind::Compiles:
                            walk_block(static_cast<ast::CompilesExpr const*>(e)->body);
                            break;
                        case ast::ExprKind::TypeAST:
                            check_type(static_cast<ast::TypeASTExpr const*>(e)->type_node, "type-as-expr");
                            break;
                        case ast::ExprKind::TemplateInst: {
                            auto* ti = static_cast<ast::TemplateInstExpr const*>(e);
                            walk_expr(ti->callee);
                            for (auto const& ta : ti->template_args)
                            {
                                check_type(ta.type, "template arg");
                                walk_expr(ta.expr);
                            }
                            break;
                        }
                        case ast::ExprKind::Range: {
                            auto* r = static_cast<ast::RangeExpr const*>(e);
                            walk_expr(r->start);
                            walk_expr(r->end);
                            break;
                        }
                        case ast::ExprKind::PackExpansion:
                            walk_expr(static_cast<ast::PackExpansionExpr const*>(e)->operand);
                            break;
                        default:
                            break;
                    }
                }

                void walk_stmt(ast::Stmt const* s) const
                {
                    if (!s)
                        return;
                    switch (s->kind)
                    {
                        case ast::StmtKind::Expr:
                            walk_expr(static_cast<ast::ExprStmt const*>(s)->expr);
                            break;
                        case ast::StmtKind::DeclStmt: {
                            auto* ds = static_cast<ast::DeclStmt const*>(s);
                            if (auto* vd = ast::node_cast<ast::VarDecl>(ds->decl))
                            {
                                check_type(vd->type, "local decl type");
                                walk_expr(vd->init);
                            }
                            break;
                        }
                        case ast::StmtKind::Return:
                            walk_expr(static_cast<ast::ReturnStmt const*>(s)->value);
                            break;
                        case ast::StmtKind::While:
                            walk_expr(static_cast<ast::WhileStmt const*>(s)->condition);
                            walk_block(static_cast<ast::WhileStmt const*>(s)->body);
                            break;
                        case ast::StmtKind::DoWhile:
                            walk_block(static_cast<ast::DoWhileStmt const*>(s)->body);
                            walk_expr(static_cast<ast::DoWhileStmt const*>(s)->condition);
                            break;
                        case ast::StmtKind::For: {
                            auto* fs = static_cast<ast::ForStmt const*>(s);
                            walk_stmt(fs->init);
                            walk_expr(fs->cond);
                            walk_expr(fs->update);
                            walk_block(fs->body);
                            break;
                        }
                        case ast::StmtKind::ForIn:
                            check_type(static_cast<ast::ForInStmt const*>(s)->item_type, "for-in item type");
                            walk_expr(static_cast<ast::ForInStmt const*>(s)->iterable);
                            walk_block(static_cast<ast::ForInStmt const*>(s)->body);
                            break;
                        case ast::StmtKind::Defer:
                            walk_stmt(static_cast<ast::DeferStmt const*>(s)->body);
                            break;
                        case ast::StmtKind::StaticIf:
                            walk_expr(static_cast<ast::StaticIfStmt const*>(s)->condition);
                            walk_block(static_cast<ast::StaticIfStmt const*>(s)->then_block);
                            walk_stmt(static_cast<ast::StaticIfStmt const*>(s)->else_branch);
                            break;
                        case ast::StmtKind::StaticMatch:
                            walk_expr(static_cast<ast::StaticMatchStmt const*>(s)->operand);
                            for (auto const& arm : static_cast<ast::StaticMatchStmt const*>(s)->arms)
                                walk_expr(arm.body);
                            break;
                        case ast::StmtKind::StaticFor:
                            walk_expr(static_cast<ast::StaticForStmt const*>(s)->pack_expr);
                            walk_block(static_cast<ast::StaticForStmt const*>(s)->body);
                            break;
                        default:
                            break;
                    }
                }

                void walk_block(ast::Block const& blk) const
                {
                    for (auto* stmt : blk.stmts)
                        walk_stmt(stmt);
                    walk_expr(blk.tail);
                }

                void run()
                {
                    check_type(fn->return_type, "return type");

                    for (auto const& p : fn->params)
                        check_type(p.type, "parameter type");

                    if (fn->body)
                        walk_block(*fn->body);
                }
            };

            Verifier{diag, syn_decl}.run();
        }

        return InstantiatedFunc{syn_decl, fp_type};
    }

    enum class SpecState : std::uint8_t
    {
        Pending,
        Analyzing,
        Analyzed,
        Failed,
    };

    struct SpecializationEntry
    {
        ast::FuncDecl* decl{};
        types::FuncPtrType* type{};
        SpecState state{SpecState::Pending};
        sm::SourceRange first_site{};
    };

    struct SpecResult
    {
        ast::FuncDecl* decl{};
        types::FuncPtrType* type{};
        SpecState state{SpecState::Pending};
        bool is_new{false};
    };

    struct CanonicalArg
    {
        enum class Tag : std::uint8_t
        {
            Type,
            Value,
        };

        Tag tag{Tag::Type};

        types::TypePtr type_ptr{};

        std::size_t value_hash{};
        std::shared_ptr<comptime::Value> value_data{};

        CanonicalArg() = default;

        static CanonicalArg make_type(types::TypePtr t)
        {
            CanonicalArg a;
            a.tag = Tag::Type;
            a.type_ptr = t;
            return a;
        }

        static CanonicalArg make_value(comptime::Value v)
        {
            CanonicalArg a;
            a.tag = Tag::Value;
            a.type_ptr = v.type;
            a.value_hash = v.hash();
            a.value_data = std::make_shared<comptime::Value>(std::move(v));
            return a;
        }

        [[nodiscard]] bool operator==(CanonicalArg const& other) const noexcept
        {
            if (tag != other.tag)
                return false;
            if (type_ptr != other.type_ptr)
                return false;
            if (tag == Tag::Type)
                return true;
            if (!value_data && !other.value_data)
                return true;
            if (!value_data || !other.value_data)
                return false;

            if (value_data->kind() == comptime::Value::Kind::Float && other.value_data->kind() == comptime::Value::Kind::Float)
                return std::bit_cast<std::uint64_t>(value_data->get_float()) == std::bit_cast<std::uint64_t>(other.value_data->get_float());

            return *value_data == *other.value_data;
        }
    };

    struct CanonicalArgHash
    {
        std::size_t operator()(CanonicalArg const& a) const noexcept
        {
            auto h = std::hash<int>{}(static_cast<int>(a.tag));
            h = h * 1099511628211ull ^ std::hash<types::TypePtr>{}(a.type_ptr);
            if (a.tag == CanonicalArg::Tag::Value)
                h = h * 1099511628211ull ^ a.value_hash;
            return h;
        }
    };

    struct SpecializationView
    {
        ast::FuncDecl const* template_decl{};
        std::vector<CanonicalArg> canonical_args;
        ast::FuncDecl* specialization_decl{};
    };

    class SpecializationRegistry
    {
    public:
        SpecializationRegistry() = default;

        [[nodiscard]] std::vector<SpecializationView> entries() const
        {
            std::vector<SpecializationView> result;
            result.reserve(m_entries.size());
            for (auto const& [key, entry] : m_entries)
            {
                SpecializationView view;
                view.template_decl = key.decl;
                view.canonical_args = key.args;
                view.specialization_decl = entry.decl;
                result.push_back(std::move(view));
            }
            return result;
        }

        [[nodiscard]] SpecResult get_or_instantiate(ast::FuncDecl const& template_fn, infer::TemplateBindings const& bindings,
                                                    sm::SourceRange instantiation_site, ast::AstContext& ast_ctx, types::TypeContext& type_ctx,
                                                    diag::DiagnosticEngine* diag = nullptr)
        {
            auto key = make_key(template_fn, bindings, type_ctx);

            if (auto it = m_entries.find(key); it != m_entries.end())
            {
                auto& entry = it->second;
                return SpecResult{entry.decl, entry.type, entry.state, false};
            }

            auto result = instantiate_with_bindings(template_fn, bindings, ast_ctx, type_ctx, diag);

            SpecializationEntry entry;
            entry.decl = result.decl;
            entry.type = result.type;
            entry.state = SpecState::Pending;
            entry.first_site = instantiation_site;

            auto [it, inserted] = m_entries.emplace(std::move(key), entry);
            m_decl_to_key.emplace(result.decl, it->first);

            m_generic_to_specs[&template_fn].push_back(result.decl);

            return SpecResult{result.decl, result.type, SpecState::Pending, true};
        }

        void mark_analyzing(ast::FuncDecl const* decl)
        {
            auto* entry = find_by_decl(decl);
            if (entry && entry->state == SpecState::Pending)
                entry->state = SpecState::Analyzing;
        }

        void mark_analyzed(ast::FuncDecl const* decl)
        {
            auto* entry = find_by_decl(decl);
            if (entry)
                entry->state = SpecState::Analyzed;
        }

        void mark_failed(ast::FuncDecl const* decl)
        {
            auto* entry = find_by_decl(decl);
            if (entry)
                entry->state = SpecState::Failed;
        }

        [[nodiscard]] std::span<ast::FuncDecl const* const> specializations_of(ast::FuncDecl const* generic_fn) const noexcept
        {
            auto it = m_generic_to_specs.find(generic_fn);
            if (it == m_generic_to_specs.end())
                return {};
            return it->second;
        }

        [[nodiscard]] std::size_t entry_count() const noexcept { return m_entries.size(); }

        [[nodiscard]] std::string dump() const
        {
            struct EntryView
            {
                std::string fn_name;
                std::string args_str;
                int state;
            };
            std::vector<EntryView> sorted;
            sorted.reserve(m_entries.size());

            for (auto const& [key, entry] : m_entries)
            {
                EntryView ev;
                auto const* fn = key.decl;
                ev.fn_name = fn ? std::string{fn->name} : std::string{"<null>"};

                std::string args;
                for (std::size_t i = 0; i < key.args.size(); ++i)
                {
                    if (i > 0)
                        args += ", ";

                    auto const& arg = key.args[i];
                    if (arg.tag == CanonicalArg::Tag::Type)
                    {
                        if (arg.type_ptr)
                        {
                            switch (arg.type_ptr->kind)
                            {
                                case types::TypeKind::Int: {
                                    auto const* it = static_cast<types::IntType const*>(arg.type_ptr);
                                    args += std::format("{}{}", it->is_signed ? 'i' : 'u', unsigned(it->bits));
                                    break;
                                }
                                case types::TypeKind::Float: {
                                    auto const* ft = static_cast<types::FloatType const*>(arg.type_ptr);
                                    args += std::format("f{}", unsigned(ft->bits));
                                    break;
                                }
                                case types::TypeKind::Bool:
                                    args += "bool";
                                    break;
                                case types::TypeKind::Char:
                                    args += "char";
                                    break;
                                case types::TypeKind::Void:
                                    args += "void";
                                    break;
                                case types::TypeKind::Struct: {
                                    auto const* ut = static_cast<types::UserType const*>(arg.type_ptr);
                                    auto const* sd = reinterpret_cast<ast::StructDecl const*>(ut->decl);
                                    args += sd ? std::string{sd->name} : "struct";
                                    break;
                                }
                                case types::TypeKind::Union: {
                                    auto const* ut = static_cast<types::UserType const*>(arg.type_ptr);
                                    auto const* ud = reinterpret_cast<ast::UnionDecl const*>(ut->decl);
                                    args += ud ? std::string{ud->name} : "union";
                                    break;
                                }
                                case types::TypeKind::Enum: {
                                    auto const* ut = static_cast<types::UserType const*>(arg.type_ptr);
                                    auto const* ed = reinterpret_cast<ast::EnumDecl const*>(ut->decl);
                                    args += ed ? std::string{ed->name} : "enum";
                                    break;
                                }
                                default:
                                    args += "<type>";
                                    break;
                            }
                        }
                        else
                            args += "<null>";
                    }
                    else
                    {
                        if (!arg.value_data)
                        {
                            args += "<none>";
                            continue;
                        }
                        auto const& v = *arg.value_data;
                        switch (v.kind())
                        {
                            case comptime::Value::Kind::Int:
                                args += std::to_string(v.get_int());
                                break;
                            case comptime::Value::Kind::Bool:
                                args += v.get_bool() ? "true" : "false";
                                break;
                            case comptime::Value::Kind::Char:
                                args += std::to_string(v.get_char());
                                break;
                            case comptime::Value::Kind::Float: {
                                double fv = v.get_float();
                                if (fv == 0.0 && std::signbit(fv))
                                    args += "-0";
                                else
                                    args += std::format("{:g}", fv);
                                break;
                            }
                            case comptime::Value::Kind::Null:
                                args += "null";
                                break;
                            case comptime::Value::Kind::String:
                                args += std::format("\"{}\"", v.get_string());
                                break;
                            default:
                                args += "<value>";
                                break;
                        }
                    }
                }
                ev.args_str = std::move(args);
                ev.state = static_cast<int>(entry.state);
                sorted.push_back(std::move(ev));
            }

            std::ranges::sort(sorted, [](EntryView const& a, EntryView const& b) {
                if (a.fn_name != b.fn_name)
                    return a.fn_name < b.fn_name;

                return a.args_str < b.args_str;
            });

            std::string out;
            for (auto const& ev : sorted)
                out += std::format("fn={} args=[{}] state={}\n", ev.fn_name, ev.args_str, ev.state);
            return out;
        }

        [[nodiscard]] static std::vector<CanonicalArg> canonical_args_from_bindings(ast::FuncDecl const& template_fn, infer::TemplateBindings const& bindings,
                                                                                    types::TypeContext& type_ctx)
        {
            std::vector<CanonicalArg> args;
            args.reserve(template_fn.template_params.size());
            for (std::size_t i = 0; i < template_fn.template_params.size(); ++i)
            {
                auto const& tp = template_fn.template_params[i];
                auto* param_ty = type_ctx.template_param_t(const_cast<ast::TemplateParam*>(&tp), tp.name, static_cast<std::uint32_t>(i));

                if (tp.is_pack)
                {
                    if (tp.value_type)
                    {
                        auto const* pack = bindings.lookup_value_pack(static_cast<types::TemplateParamType const*>(param_ty));
                        if (pack)
                            for (auto const& v : *pack)
                                args.push_back(CanonicalArg::make_value(v));
                        else
                        {
                            auto const* type_pack = bindings.lookup_pack(static_cast<types::TemplateParamType const*>(param_ty));
                            if (type_pack)
                            {
                                for (auto const& pt : *type_pack)
                                    args.push_back(CanonicalArg::make_type(pt));
                            }
                            else
                            {
                                auto single = bindings.lookup(static_cast<types::TemplateParamType const*>(param_ty));
                                if (single)
                                    args.push_back(CanonicalArg::make_type(single));
                            }
                        }
                    }
                    else
                    {
                        auto const* pack = bindings.lookup_pack(static_cast<types::TemplateParamType const*>(param_ty));
                        if (pack)
                        {
                            for (auto const& pt : *pack)
                                args.push_back(CanonicalArg::make_type(pt));
                        }
                        else
                        {
                            auto single = bindings.lookup(static_cast<types::TemplateParamType const*>(param_ty));
                            if (single)
                                args.push_back(CanonicalArg::make_type(single));
                        }
                    }
                }
                else if (tp.value_type)
                {
                    auto const* bv = bindings.lookup_value(static_cast<types::TemplateParamType const*>(param_ty));
                    if (bv)
                        args.push_back(CanonicalArg::make_value(*bv));
                    else
                        args.push_back(CanonicalArg::make_value(comptime::Value::make_null(nullptr)));
                }
                else
                {
                    if (param_ty)
                    {
                        auto resolved = bindings.substitute(param_ty);
                        args.push_back(CanonicalArg::make_type(resolved ? resolved : param_ty));
                    }
                    else
                        args.push_back(CanonicalArg::make_type(nullptr));
                }
            }
            return args;
        }

    private:
        struct Key
        {
            ast::FuncDecl const* decl{};
            std::vector<CanonicalArg> args;

            bool operator==(Key const& other) const noexcept
            {
                if (decl != other.decl)
                    return false;
                if (args.size() != other.args.size())
                    return false;
                for (std::size_t i = 0; i < args.size(); ++i)
                    if (!(args[i] == other.args[i]))
                        return false;
                return true;
            }
        };

        struct KeyHash
        {
            std::size_t operator()(Key const& k) const noexcept
            {
                std::size_t h = std::hash<ast::FuncDecl const*>{}(k.decl);
                for (auto const& arg : k.args)
                    h = h * 1099511628211ull ^ CanonicalArgHash {}(arg);
                return h;
            }
        };

        std::unordered_map<Key, SpecializationEntry, KeyHash> m_entries;
        std::unordered_map<ast::FuncDecl const*, Key> m_decl_to_key;
        std::unordered_map<ast::FuncDecl const*, std::vector<ast::FuncDecl const*>> m_generic_to_specs;

        [[nodiscard]] Key make_key(ast::FuncDecl const& template_fn, infer::TemplateBindings const& bindings, types::TypeContext& type_ctx) const
        {
            Key key;
            key.decl = &template_fn;
            key.args = canonical_args_from_bindings(template_fn, bindings, type_ctx);
            return key;
        }

        [[nodiscard]] SpecializationEntry* find_by_decl(ast::FuncDecl const* decl)
        {
            auto kit = m_decl_to_key.find(decl);
            if (kit == m_decl_to_key.end())
                return nullptr;
            auto it = m_entries.find(kit->second);
            return it != m_entries.end() ? &it->second : nullptr;
        }
    };

} // namespace dcc::sema
