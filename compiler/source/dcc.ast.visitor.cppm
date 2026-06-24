export module dcc.ast.visitor;

import std;
import dcc.ast;

export namespace dcc::ast
{
    struct Visitor
    {
        Visitor() = default;
        virtual ~Visitor() = default;
        Visitor(Visitor const&) = default;
        Visitor& operator=(Visitor const&) = default;
        Visitor(Visitor&&) = default;
        Visitor& operator=(Visitor&&) = default;

        virtual void visitTranslationUnit(TranslationUnit const*) {}
        virtual void visitDecl(Decl const*) {}
        virtual void visitStmt(Stmt const*) {}
        virtual void visitExpr(Expr const*) {}
        virtual void visitTypeExpr(TypeExpr const*) {}
        virtual void visitPattern(Pattern const*) {}
        virtual void visitBlock(Block const&) {}
        virtual void visitMatchArm(MatchArm const&) {}
        virtual void visitTemplateArg(TemplateArg const&) {}
        virtual void visitTemplateArgs(std::pmr::vector<TemplateArg> const&) {}
        virtual void visitTemplateParams(std::pmr::vector<TemplateParam> const&) {}
        virtual void visitAttrs(std::pmr::vector<Attribute> const&) {}

        virtual void visitPrimitiveType(PrimitiveType const*) {}
        virtual void visitNamedType(NamedType const*) {}
        virtual void visitPointerType(PointerType const*) {}
        virtual void visitArrayType(ArrayType const*) {}
        virtual void visitSliceType(SliceType const*) {}
        virtual void visitFamType(FamType const*) {}
        virtual void visitFuncPtrType(FuncPtrType const*) {}
        virtual void visitQualifiedType(QualifiedType const*) {}
        virtual void visitPackIndexType(PackIndexType const*) {}

        virtual void visitIntLiteralExpr(IntLiteralExpr const*) {}
        virtual void visitFloatLiteralExpr(FloatLiteralExpr const*) {}
        virtual void visitStringLiteralExpr(StringLiteralExpr const*) {}
        virtual void visitU16StringLiteralExpr(U16StringLiteralExpr const*) {}
        virtual void visitCharLiteralExpr(CharLiteralExpr const*) {}
        virtual void visitU16CharLiteralExpr(U16CharLiteralExpr const*) {}
        virtual void visitBoolLiteralExpr(BoolLiteralExpr const*) {}
        virtual void visitNullLiteralExpr(NullLiteralExpr const*) {}
        virtual void visitIdentExpr(IdentExpr const*) {}
        virtual void visitPathExpr(PathExpr const*) {}
        virtual void visitUnaryExpr(UnaryExpr const*) {}
        virtual void visitPostfixExpr(PostfixExpr const*) {}
        virtual void visitBinaryExpr(BinaryExpr const*) {}
        virtual void visitCallExpr(CallExpr const*) {}
        virtual void visitFieldAccessExpr(FieldAccessExpr const*) {}
        virtual void visitIndexExpr(IndexExpr const*) {}
        virtual void visitPackAccessExpr(PackAccessExpr const*) {}
        virtual void visitCastExpr(CastExpr const*) {}
        virtual void visitBlockExpr(BlockExpr const*) {}
        virtual void visitIfExpr(IfExpr const*) {}
        virtual void visitMatchExpr(MatchExpr const*) {}
        virtual void visitStructLiteralExpr(StructLiteralExpr const*) {}
        virtual void visitSizeofExpr(SizeofExpr const*) {}
        virtual void visitSizeofPackExpr(SizeofPackExpr const*) {}
        virtual void visitPackExpansionExpr(PackExpansionExpr const*) {}
        virtual void visitAlignofExpr(AlignofExpr const*) {}
        virtual void visitOffsetofExpr(OffsetofExpr const*) {}
        virtual void visitCompilesExpr(CompilesExpr const*) {}
        virtual void visitRangeExpr(RangeExpr const*) {}
        virtual void visitTypeASTExpr(TypeASTExpr const*) {}
        virtual void visitTemplateInstExpr(TemplateInstExpr const*) {}

        virtual void visitExprStmt(ExprStmt const*) {}
        virtual void visitDeclStmt(DeclStmt const*) {}
        virtual void visitReturnStmt(ReturnStmt const*) {}
        virtual void visitBreakStmt(BreakStmt const*) {}
        virtual void visitContinueStmt(ContinueStmt const*) {}
        virtual void visitWhileStmt(WhileStmt const*) {}
        virtual void visitDoWhileStmt(DoWhileStmt const*) {}
        virtual void visitForStmt(ForStmt const*) {}
        virtual void visitForInStmt(ForInStmt const*) {}
        virtual void visitDeferStmt(DeferStmt const*) {}
        virtual void visitStaticIfStmt(StaticIfStmt const*) {}
        virtual void visitStaticMatchStmt(StaticMatchStmt const*) {}
        virtual void visitStaticForStmt(StaticForStmt const*) {}
        virtual void visitAmbiguousStmt(AmbiguousStmt const*) {}

        virtual void visitModuleDecl(ModuleDecl const*) {}
        virtual void visitImportDecl(ImportDecl const*) {}
        virtual void visitUsingDecl(UsingDecl const*) {}
        virtual void visitStructDecl(StructDecl const*) {}
        virtual void visitUnionDecl(UnionDecl const*) {}
        virtual void visitEnumDecl(EnumDecl const*) {}
        virtual void visitFuncDecl(FuncDecl const*) {}
        virtual void visitVarDecl(VarDecl const*) {}
        virtual void visitStaticIfGroup(StaticIfGroup const*) {}

        virtual void visitLiteralPattern(LiteralPattern const*) {}
        virtual void visitBindingPattern(BindingPattern const*) {}
        virtual void visitRefPattern(RefPattern const*) {}
        virtual void visitWildcardPattern(WildcardPattern const*) {}
        virtual void visitEnumDestructurePattern(EnumDestructurePattern const*) {}
        virtual void visitStructDestructurePattern(StructDestructurePattern const*) {}
        virtual void visitRangePattern(RangePattern const*) {}
        virtual void visitOrPattern(OrPattern const*) {}
    };

    struct RecursiveAstVisitor : Visitor
    {
        void visitTranslationUnit(TranslationUnit const* tu) override;
        void visitDecl(Decl const* decl) override;
        void visitStmt(Stmt const* stmt) override;
        void visitExpr(Expr const* expr) override;
        void visitTypeExpr(TypeExpr const* type_expr) override;
        void visitPattern(Pattern const* pat) override;
        void visitBlock(Block const& block) override;
        void visitMatchArm(MatchArm const& arm) override;
        void visitTemplateArg(TemplateArg const& arg) override;
        void visitTemplateArgs(std::pmr::vector<TemplateArg> const& args) override;
        void visitTemplateParams(std::pmr::vector<TemplateParam> const& params) override;
        void visitAttrs(std::pmr::vector<Attribute> const& attrs) override;

        void visitPrimitiveType(PrimitiveType const*) override {}
        void visitNamedType(NamedType const*) override;
        void visitPointerType(PointerType const*) override;
        void visitArrayType(ArrayType const*) override;
        void visitSliceType(SliceType const*) override;
        void visitFamType(FamType const*) override;
        void visitFuncPtrType(FuncPtrType const*) override;
        void visitQualifiedType(QualifiedType const*) override;
        void visitPackIndexType(PackIndexType const*) override;

        void visitIntLiteralExpr(IntLiteralExpr const*) override {}
        void visitFloatLiteralExpr(FloatLiteralExpr const*) override {}
        void visitStringLiteralExpr(StringLiteralExpr const*) override {}
        void visitU16StringLiteralExpr(U16StringLiteralExpr const*) override {}
        void visitCharLiteralExpr(CharLiteralExpr const*) override {}
        void visitU16CharLiteralExpr(U16CharLiteralExpr const*) override {}
        void visitBoolLiteralExpr(BoolLiteralExpr const*) override {}
        void visitNullLiteralExpr(NullLiteralExpr const*) override {}
        void visitIdentExpr(IdentExpr const*) override {}
        void visitPathExpr(PathExpr const*) override;
        void visitUnaryExpr(UnaryExpr const*) override;
        void visitPostfixExpr(PostfixExpr const*) override;
        void visitBinaryExpr(BinaryExpr const*) override;
        void visitCallExpr(CallExpr const*) override;
        void visitFieldAccessExpr(FieldAccessExpr const*) override;
        void visitIndexExpr(IndexExpr const*) override;
        void visitPackAccessExpr(PackAccessExpr const*) override;
        void visitCastExpr(CastExpr const*) override;
        void visitBlockExpr(BlockExpr const*) override;
        void visitIfExpr(IfExpr const*) override;
        void visitMatchExpr(MatchExpr const*) override;
        void visitStructLiteralExpr(StructLiteralExpr const*) override;
        void visitSizeofExpr(SizeofExpr const*) override;
        void visitSizeofPackExpr(SizeofPackExpr const*) override;
        void visitPackExpansionExpr(PackExpansionExpr const*) override;
        void visitAlignofExpr(AlignofExpr const*) override;
        void visitOffsetofExpr(OffsetofExpr const*) override;
        void visitCompilesExpr(CompilesExpr const*) override;
        void visitRangeExpr(RangeExpr const*) override;
        void visitTypeASTExpr(TypeASTExpr const*) override;
        void visitTemplateInstExpr(TemplateInstExpr const*) override;

        void visitExprStmt(ExprStmt const*) override;
        void visitDeclStmt(DeclStmt const*) override;
        void visitReturnStmt(ReturnStmt const*) override;
        void visitBreakStmt(BreakStmt const*) override {}
        void visitContinueStmt(ContinueStmt const*) override {}
        void visitWhileStmt(WhileStmt const*) override;
        void visitDoWhileStmt(DoWhileStmt const*) override;
        void visitForStmt(ForStmt const*) override;
        void visitForInStmt(ForInStmt const*) override;
        void visitDeferStmt(DeferStmt const*) override;
        void visitStaticIfStmt(StaticIfStmt const*) override;
        void visitStaticMatchStmt(StaticMatchStmt const*) override;
        void visitStaticForStmt(StaticForStmt const*) override;
        void visitAmbiguousStmt(AmbiguousStmt const*) override;

        void visitModuleDecl(ModuleDecl const*) override;
        void visitImportDecl(ImportDecl const*) override;
        void visitUsingDecl(UsingDecl const*) override;
        void visitStructDecl(StructDecl const*) override;
        void visitUnionDecl(UnionDecl const*) override;
        void visitEnumDecl(EnumDecl const*) override;
        void visitFuncDecl(FuncDecl const*) override;
        void visitVarDecl(VarDecl const*) override;
        void visitStaticIfGroup(StaticIfGroup const*) override;

        void visitLiteralPattern(LiteralPattern const*) override;
        void visitBindingPattern(BindingPattern const*) override {}
        void visitRefPattern(RefPattern const*) override;
        void visitWildcardPattern(WildcardPattern const*) override {}
        void visitEnumDestructurePattern(EnumDestructurePattern const*) override;
        void visitStructDestructurePattern(StructDestructurePattern const*) override;
        void visitRangePattern(RangePattern const*) override;
        void visitOrPattern(OrPattern const*) override;
    };

} // namespace dcc::ast

module :private;

namespace dcc::ast
{
    void RecursiveAstVisitor::visitBlock(Block const& block)
    {
        for (const auto* s : block.stmts)
            if (s)
                visitStmt(s);

        if (block.tail)
            visitExpr(block.tail);
    }

    void RecursiveAstVisitor::visitMatchArm(MatchArm const& arm)
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

    void RecursiveAstVisitor::visitTemplateArg(TemplateArg const& arg)
    {
        if (arg.type)
            visitTypeExpr(arg.type);
        if (arg.expr)
            visitExpr(arg.expr);
    }

    void RecursiveAstVisitor::visitTemplateArgs(std::pmr::vector<TemplateArg> const& args)
    {
        for (auto const& a : args)
            visitTemplateArg(a);
    }

    void RecursiveAstVisitor::visitTemplateParams(std::pmr::vector<TemplateParam> const& params)
    {
        for (auto const& tp : params)
            if (tp.value_type)
                visitTypeExpr(tp.value_type);
    }

    void RecursiveAstVisitor::visitAttrs(std::pmr::vector<Attribute> const& attrs)
    {
        for (auto const& a : attrs)
            for (auto* arg : a.args)
                if (arg)
                    visitExpr(arg);
    }

    void RecursiveAstVisitor::visitTranslationUnit(TranslationUnit const* tu)
    {
        if (!tu)
            return;

        if (tu->module_decl)
            visitDecl(tu->module_decl);

        for (const auto* d : tu->imports)
            if (d)
                visitDecl(d);

        for (const auto* d : tu->decls)
            if (d)
                visitDecl(d);
    }

    void RecursiveAstVisitor::visitPattern(Pattern const* pat)
    {
        if (!pat)
            return;

        switch (pat->kind)
        {
            case PatternKind::Literal:
                visitLiteralPattern(node_cast<LiteralPattern>(pat));
                break;
            case PatternKind::Binding:
                visitBindingPattern(node_cast<BindingPattern>(pat));
                break;
            case PatternKind::Ref:
                visitRefPattern(node_cast<RefPattern>(pat));
                break;
            case PatternKind::Wildcard:
                visitWildcardPattern(node_cast<WildcardPattern>(pat));
                break;
            case PatternKind::EnumDestructure:
                visitEnumDestructurePattern(node_cast<EnumDestructurePattern>(pat));
                break;
            case PatternKind::StructDestructure:
                visitStructDestructurePattern(node_cast<StructDestructurePattern>(pat));
                break;
            case PatternKind::Range:
                visitRangePattern(node_cast<RangePattern>(pat));
                break;
            case PatternKind::Or:
                visitOrPattern(node_cast<OrPattern>(pat));
                break;
        }
    }

    void RecursiveAstVisitor::visitLiteralPattern(LiteralPattern const* p)
    {
        if (p->value)
            visitExpr(p->value);
    }

    void RecursiveAstVisitor::visitRefPattern(RefPattern const* p)
    {
        if (p->inner)
            visitPattern(p->inner);
    }

    void RecursiveAstVisitor::visitEnumDestructurePattern(EnumDestructurePattern const* p)
    {
        for (const auto* sub : p->payload)
            if (sub)
                visitPattern(sub);
    }

    void RecursiveAstVisitor::visitStructDestructurePattern(StructDestructurePattern const* p)
    {
        for (auto const& f : p->fields)
            if (f.pattern)
                visitPattern(f.pattern);
    }

    void RecursiveAstVisitor::visitRangePattern(RangePattern const* p)
    {
        if (p->start)
            visitExpr(p->start);
        if (p->end)
            visitExpr(p->end);
    }

    void RecursiveAstVisitor::visitOrPattern(OrPattern const* p)
    {
        for (auto* alt : p->alternatives)
            if (alt)
                visitPattern(alt);
    }

    void RecursiveAstVisitor::visitTypeExpr(TypeExpr const* type_expr)
    {
        if (!type_expr)
            return;

        switch (type_expr->kind)
        {
            case TypeKind::Primitive:
                visitPrimitiveType(node_cast<PrimitiveType>(type_expr));
                break;
            case TypeKind::Named:
                visitNamedType(node_cast<NamedType>(type_expr));
                break;
            case TypeKind::Pointer:
                visitPointerType(node_cast<PointerType>(type_expr));
                break;
            case TypeKind::Array:
                visitArrayType(node_cast<ArrayType>(type_expr));
                break;
            case TypeKind::Slice:
                visitSliceType(node_cast<SliceType>(type_expr));
                break;
            case TypeKind::Fam:
                visitFamType(node_cast<FamType>(type_expr));
                break;
            case TypeKind::FuncPtr:
                visitFuncPtrType(node_cast<FuncPtrType>(type_expr));
                break;
            case TypeKind::Qualified:
                visitQualifiedType(node_cast<QualifiedType>(type_expr));
                break;
            case TypeKind::PackIndex:
                visitPackIndexType(node_cast<PackIndexType>(type_expr));
                break;
        }
    }

    void RecursiveAstVisitor::visitNamedType(NamedType const* t)
    {
        visitTemplateArgs(t->template_args);
    }

    void RecursiveAstVisitor::visitPointerType(PointerType const* t)
    {
        if (t->pointee)
            visitTypeExpr(t->pointee);
    }

    void RecursiveAstVisitor::visitArrayType(ArrayType const* t)
    {
        if (t->element)
            visitTypeExpr(t->element);
        if (t->size)
            visitExpr(t->size);
    }

    void RecursiveAstVisitor::visitSliceType(SliceType const* t)
    {
        if (t->element)
            visitTypeExpr(t->element);
    }

    void RecursiveAstVisitor::visitFamType(FamType const* t)
    {
        if (t->element)
            visitTypeExpr(t->element);
    }

    void RecursiveAstVisitor::visitFuncPtrType(FuncPtrType const* t)
    {
        if (t->return_type)
            visitTypeExpr(t->return_type);
        for (const auto* p : t->params)
            if (p)
                visitTypeExpr(p);
    }

    void RecursiveAstVisitor::visitQualifiedType(QualifiedType const* t)
    {
        if (t->inner)
            visitTypeExpr(t->inner);
    }

    void RecursiveAstVisitor::visitPackIndexType(PackIndexType const* t)
    {
        if (t->base)
            visitTypeExpr(t->base);
        if (t->index)
            visitExpr(t->index);
    }

    void RecursiveAstVisitor::visitExpr(Expr const* expr)
    {
        if (!expr)
            return;

        switch (expr->kind)
        {
            case ExprKind::IntLiteral:
                visitIntLiteralExpr(node_cast<IntLiteralExpr>(expr));
                break;
            case ExprKind::FloatLiteral:
                visitFloatLiteralExpr(node_cast<FloatLiteralExpr>(expr));
                break;
            case ExprKind::StringLiteral:
                visitStringLiteralExpr(node_cast<StringLiteralExpr>(expr));
                break;
            case ExprKind::U16StringLiteral:
                visitU16StringLiteralExpr(node_cast<U16StringLiteralExpr>(expr));
                break;
            case ExprKind::CharLiteral:
                visitCharLiteralExpr(node_cast<CharLiteralExpr>(expr));
                break;
            case ExprKind::U16CharLiteral:
                visitU16CharLiteralExpr(node_cast<U16CharLiteralExpr>(expr));
                break;
            case ExprKind::BoolLiteral:
                visitBoolLiteralExpr(node_cast<BoolLiteralExpr>(expr));
                break;
            case ExprKind::NullLiteral:
                visitNullLiteralExpr(node_cast<NullLiteralExpr>(expr));
                break;
            case ExprKind::Ident:
                visitIdentExpr(node_cast<IdentExpr>(expr));
                break;
            case ExprKind::PathExpr:
                visitPathExpr(node_cast<PathExpr>(expr));
                break;
            case ExprKind::Unary:
                visitUnaryExpr(node_cast<UnaryExpr>(expr));
                break;
            case ExprKind::Postfix:
                visitPostfixExpr(node_cast<PostfixExpr>(expr));
                break;
            case ExprKind::Binary:
                visitBinaryExpr(node_cast<BinaryExpr>(expr));
                break;
            case ExprKind::Call:
                visitCallExpr(node_cast<CallExpr>(expr));
                break;
            case ExprKind::FieldAccess:
                visitFieldAccessExpr(node_cast<FieldAccessExpr>(expr));
                break;
            case ExprKind::Index:
                visitIndexExpr(node_cast<IndexExpr>(expr));
                break;
            case ExprKind::PackAccess:
                visitPackAccessExpr(node_cast<PackAccessExpr>(expr));
                break;
            case ExprKind::Cast:
                visitCastExpr(node_cast<CastExpr>(expr));
                break;
            case ExprKind::Block:
                visitBlockExpr(node_cast<BlockExpr>(expr));
                break;
            case ExprKind::If:
                visitIfExpr(node_cast<IfExpr>(expr));
                break;
            case ExprKind::Match:
                visitMatchExpr(node_cast<MatchExpr>(expr));
                break;
            case ExprKind::StructLiteral:
                visitStructLiteralExpr(node_cast<StructLiteralExpr>(expr));
                break;
            case ExprKind::Sizeof:
                visitSizeofExpr(node_cast<SizeofExpr>(expr));
                break;
            case ExprKind::SizeofPack:
                visitSizeofPackExpr(node_cast<SizeofPackExpr>(expr));
                break;
            case ExprKind::PackExpansion:
                visitPackExpansionExpr(node_cast<PackExpansionExpr>(expr));
                break;
            case ExprKind::Alignof:
                visitAlignofExpr(node_cast<AlignofExpr>(expr));
                break;
            case ExprKind::Offsetof:
                visitOffsetofExpr(node_cast<OffsetofExpr>(expr));
                break;
            case ExprKind::Compiles:
                visitCompilesExpr(node_cast<CompilesExpr>(expr));
                break;
            case ExprKind::Range:
                visitRangeExpr(node_cast<RangeExpr>(expr));
                break;
            case ExprKind::TypeAST:
                visitTypeASTExpr(node_cast<TypeASTExpr>(expr));
                break;
            case ExprKind::TemplateInst:
                visitTemplateInstExpr(node_cast<TemplateInstExpr>(expr));
                break;
        }
    }

    void RecursiveAstVisitor::visitPathExpr(PathExpr const* e)
    {
        visitTemplateArgs(e->explicit_enum_args);
    }

    void RecursiveAstVisitor::visitUnaryExpr(UnaryExpr const* e)
    {
        if (e->operand)
            visitExpr(e->operand);
    }

    void RecursiveAstVisitor::visitPostfixExpr(PostfixExpr const* e)
    {
        if (e->operand)
            visitExpr(e->operand);
    }

    void RecursiveAstVisitor::visitBinaryExpr(BinaryExpr const* e)
    {
        if (e->lhs)
            visitExpr(e->lhs);
        if (e->rhs)
            visitExpr(e->rhs);
    }

    void RecursiveAstVisitor::visitCallExpr(CallExpr const* e)
    {
        if (e->callee)
            visitExpr(e->callee);
        for (auto* a : e->args)
            if (a)
                visitExpr(a);
    }

    void RecursiveAstVisitor::visitFieldAccessExpr(FieldAccessExpr const* e)
    {
        if (e->object)
            visitExpr(e->object);
    }

    void RecursiveAstVisitor::visitIndexExpr(IndexExpr const* e)
    {
        if (e->object)
            visitExpr(e->object);
        if (e->index)
            visitExpr(e->index);
    }

    void RecursiveAstVisitor::visitPackAccessExpr(PackAccessExpr const* e)
    {
        if (e->object)
            visitExpr(e->object);
        if (e->index)
            visitExpr(e->index);
    }

    void RecursiveAstVisitor::visitCastExpr(CastExpr const* e)
    {
        if (e->operand)
            visitExpr(e->operand);
        if (e->target)
            visitTypeExpr(e->target);
    }

    void RecursiveAstVisitor::visitBlockExpr(BlockExpr const* e)
    {
        visitBlock(e->body);
    }

    void RecursiveAstVisitor::visitIfExpr(IfExpr const* e)
    {
        if (e->condition)
            visitExpr(e->condition);
        visitBlock(e->then_block);
        if (e->else_branch)
            visitExpr(e->else_branch);
    }

    void RecursiveAstVisitor::visitMatchExpr(MatchExpr const* e)
    {
        if (e->operand)
            visitExpr(e->operand);
        for (auto const& arm : e->arms)
            visitMatchArm(arm);
    }

    void RecursiveAstVisitor::visitStructLiteralExpr(StructLiteralExpr const* e)
    {
        if (e->type)
            visitTypeExpr(e->type);
        for (auto const& f : e->fields)
            if (f.value)
                visitExpr(f.value);
    }

    void RecursiveAstVisitor::visitSizeofExpr(SizeofExpr const* e)
    {
        if (e->target)
            visitTypeExpr(e->target);
    }

    void RecursiveAstVisitor::visitSizeofPackExpr(SizeofPackExpr const*) {}

    void RecursiveAstVisitor::visitPackExpansionExpr(PackExpansionExpr const* e)
    {
        if (e->operand)
            visitExpr(e->operand);
    }

    void RecursiveAstVisitor::visitAlignofExpr(AlignofExpr const* e)
    {
        if (e->target)
            visitTypeExpr(e->target);
    }

    void RecursiveAstVisitor::visitOffsetofExpr(OffsetofExpr const* e)
    {
        if (e->target)
            visitTypeExpr(e->target);
    }

    void RecursiveAstVisitor::visitCompilesExpr(CompilesExpr const* e)
    {
        for (auto const& p : e->params)
            if (p.type)
                visitTypeExpr(p.type);
        visitBlock(e->body);
    }

    void RecursiveAstVisitor::visitRangeExpr(RangeExpr const* e)
    {
        if (e->start)
            visitExpr(e->start);
        if (e->end)
            visitExpr(e->end);
    }

    void RecursiveAstVisitor::visitTypeASTExpr(TypeASTExpr const* e)
    {
        if (e->type_node)
            visitTypeExpr(e->type_node);
    }

    void RecursiveAstVisitor::visitTemplateInstExpr(TemplateInstExpr const* e)
    {
        if (e->callee)
            visitExpr(e->callee);
        visitTemplateArgs(e->template_args);
    }

    void RecursiveAstVisitor::visitStmt(Stmt const* stmt)
    {
        if (!stmt)
            return;

        switch (stmt->kind)
        {
            case StmtKind::Expr:
                visitExprStmt(node_cast<ExprStmt>(stmt));
                break;
            case StmtKind::DeclStmt:
                visitDeclStmt(node_cast<DeclStmt>(stmt));
                break;
            case StmtKind::Return:
                visitReturnStmt(node_cast<ReturnStmt>(stmt));
                break;
            case StmtKind::Break:
                visitBreakStmt(node_cast<BreakStmt>(stmt));
                break;
            case StmtKind::Continue:
                visitContinueStmt(node_cast<ContinueStmt>(stmt));
                break;
            case StmtKind::While:
                visitWhileStmt(node_cast<WhileStmt>(stmt));
                break;
            case StmtKind::DoWhile:
                visitDoWhileStmt(node_cast<DoWhileStmt>(stmt));
                break;
            case StmtKind::For:
                visitForStmt(node_cast<ForStmt>(stmt));
                break;
            case StmtKind::ForIn:
                visitForInStmt(node_cast<ForInStmt>(stmt));
                break;
            case StmtKind::Defer:
                visitDeferStmt(node_cast<DeferStmt>(stmt));
                break;
            case StmtKind::StaticIf:
                visitStaticIfStmt(node_cast<StaticIfStmt>(stmt));
                break;
            case StmtKind::StaticMatch:
                visitStaticMatchStmt(node_cast<StaticMatchStmt>(stmt));
                break;
            case StmtKind::StaticFor:
                visitStaticForStmt(node_cast<StaticForStmt>(stmt));
                break;
            case StmtKind::Ambiguous:
                visitAmbiguousStmt(node_cast<AmbiguousStmt>(stmt));
                break;
        }
    }

    void RecursiveAstVisitor::visitExprStmt(ExprStmt const* s)
    {
        if (s->expr)
            visitExpr(s->expr);
    }

    void RecursiveAstVisitor::visitDeclStmt(DeclStmt const* s)
    {
        if (s->decl)
            visitDecl(s->decl);
    }

    void RecursiveAstVisitor::visitReturnStmt(ReturnStmt const* s)
    {
        if (s->value)
            visitExpr(s->value);
    }

    void RecursiveAstVisitor::visitWhileStmt(WhileStmt const* s)
    {
        if (s->condition)
            visitExpr(s->condition);
        visitBlock(s->body);
    }

    void RecursiveAstVisitor::visitDoWhileStmt(DoWhileStmt const* s)
    {
        visitBlock(s->body);
        if (s->condition)
            visitExpr(s->condition);
    }

    void RecursiveAstVisitor::visitForStmt(ForStmt const* s)
    {
        if (s->init)
            visitStmt(s->init);
        if (s->cond)
            visitExpr(s->cond);
        if (s->update)
            visitExpr(s->update);
        visitBlock(s->body);
    }

    void RecursiveAstVisitor::visitForInStmt(ForInStmt const* s)
    {
        if (s->item_type)
            visitTypeExpr(s->item_type);
        if (s->iterable)
            visitExpr(s->iterable);
        visitBlock(s->body);
    }

    void RecursiveAstVisitor::visitDeferStmt(DeferStmt const* s)
    {
        if (s->body)
            visitStmt(s->body);
    }

    void RecursiveAstVisitor::visitStaticIfStmt(StaticIfStmt const* s)
    {
        if (s->condition)
            visitExpr(s->condition);
        visitBlock(s->then_block);
        if (s->else_branch)
            visitStmt(s->else_branch);
    }

    void RecursiveAstVisitor::visitStaticMatchStmt(StaticMatchStmt const* s)
    {
        if (s->operand)
            visitExpr(s->operand);
        for (auto const& arm : s->arms)
            visitMatchArm(arm);
    }

    void RecursiveAstVisitor::visitStaticForStmt(StaticForStmt const* s)
    {
        if (s->pack_expr)
            visitExpr(s->pack_expr);
        visitBlock(s->body);
    }

    void RecursiveAstVisitor::visitAmbiguousStmt(AmbiguousStmt const* s)
    {
        if (s->as_decl)
            visitDecl(s->as_decl);
        if (s->as_expr)
            visitExpr(s->as_expr);
    }

    void RecursiveAstVisitor::visitDecl(Decl const* decl)
    {
        if (!decl)
            return;

        switch (decl->kind)
        {
            case DeclKind::Module:
                visitModuleDecl(node_cast<ModuleDecl>(decl));
                break;
            case DeclKind::Import:
                visitImportDecl(node_cast<ImportDecl>(decl));
                break;
            case DeclKind::Using:
                visitUsingDecl(node_cast<UsingDecl>(decl));
                break;
            case DeclKind::Struct:
                visitStructDecl(node_cast<StructDecl>(decl));
                break;
            case DeclKind::Union:
                visitUnionDecl(node_cast<UnionDecl>(decl));
                break;
            case DeclKind::Enum:
                visitEnumDecl(node_cast<EnumDecl>(decl));
                break;
            case DeclKind::Func:
                visitFuncDecl(node_cast<FuncDecl>(decl));
                break;
            case DeclKind::Var:
                visitVarDecl(node_cast<VarDecl>(decl));
                break;
            case DeclKind::StaticIfGroup:
                visitStaticIfGroup(node_cast<StaticIfGroup>(decl));
                break;
        }
    }

    void RecursiveAstVisitor::visitModuleDecl(ModuleDecl const* d)
    {
        visitAttrs(d->attrs);
    }

    void RecursiveAstVisitor::visitImportDecl(ImportDecl const* d)
    {
        visitAttrs(d->attrs);
    }

    void RecursiveAstVisitor::visitUsingDecl(UsingDecl const* d)
    {
        visitAttrs(d->attrs);
        visitTemplateParams(d->template_params);
        if (d->target_type)
            visitTypeExpr(d->target_type);
        if (d->target_expr)
            visitExpr(d->target_expr);
    }

    void RecursiveAstVisitor::visitStructDecl(StructDecl const* d)
    {
        visitAttrs(d->attrs);
        visitTemplateParams(d->template_params);
        if (d->constraint)
            visitExpr(d->constraint);
        for (auto const& f : d->fields)
            if (f.type)
                visitTypeExpr(f.type);
    }

    void RecursiveAstVisitor::visitUnionDecl(UnionDecl const* d)
    {
        visitAttrs(d->attrs);
        for (auto const& f : d->fields)
            if (f.type)
                visitTypeExpr(f.type);
    }

    void RecursiveAstVisitor::visitEnumDecl(EnumDecl const* d)
    {
        visitAttrs(d->attrs);
        visitTemplateParams(d->template_params);
        if (d->backing_type)
            visitTypeExpr(d->backing_type);
        if (d->constraint)
            visitExpr(d->constraint);
        for (auto const& v : d->variants)
        {
            for (const auto* t : v.payload)
                if (t)
                    visitTypeExpr(t);
            if (v.explicit_value)
                visitExpr(v.explicit_value);
        }
    }

    void RecursiveAstVisitor::visitFuncDecl(FuncDecl const* d)
    {
        visitAttrs(d->attrs);
        if (d->return_type)
            visitTypeExpr(d->return_type);
        visitTemplateParams(d->template_params);
        for (auto const& p : d->params)
            if (p.type)
                visitTypeExpr(p.type);
        if (d->constraint)
            visitExpr(d->constraint);
        if (d->body.has_value())
            visitBlock(*d->body);
    }

    void RecursiveAstVisitor::visitVarDecl(VarDecl const* d)
    {
        visitAttrs(d->attrs);
        if (d->type)
            visitTypeExpr(d->type);
        if (d->init)
            visitExpr(d->init);
    }

    void RecursiveAstVisitor::visitStaticIfGroup(StaticIfGroup const* g)
    {
        if (g->condition)
            visitExpr(g->condition);
        for (auto* d : g->then_decls)
            visitDecl(d);
        if (g->else_group)
            visitStaticIfGroup(g->else_group);
    }

} // namespace dcc::ast
