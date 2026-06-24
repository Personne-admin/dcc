export module dcc.ast.serializer;

import std;
import dcc.ast;
import dcc.ast.visitor;
import dcc.lex.tokens;

export namespace dcc::ast
{
    class AstSerializer : RecursiveAstVisitor
    {
    public:
        [[nodiscard]] static std::string dump(const TranslationUnit* tu)
        {
            AstSerializer s;
            s.print_translation_unit(tu);
            return s.m_out;
        }

        [[nodiscard]] static std::string dump(const Decl* decl)
        {
            AstSerializer s;
            s.visitDecl(decl);
            return s.m_out;
        }
        [[nodiscard]] static std::string dump(const Expr* expr)
        {
            AstSerializer s;
            s.visitExpr(expr);
            return s.m_out;
        }
        [[nodiscard]] static std::string dump(const Stmt* stmt)
        {
            AstSerializer s;
            s.visitStmt(stmt);
            return s.m_out;
        }
        [[nodiscard]] static std::string dump(const TypeExpr* type)
        {
            AstSerializer s;
            s.visitTypeExpr(type);
            return s.m_out;
        }
        [[nodiscard]] static std::string dump(const Pattern* pat)
        {
            AstSerializer s;
            s.visitPattern(pat);
            return s.m_out;
        }

        void visitDecl(Decl const* decl) override
        {
            if (!decl)
            {
                line("<null-decl>");
                return;
            }
            RecursiveAstVisitor::visitDecl(decl);
        }
        void visitStmt(Stmt const* stmt) override
        {
            if (!stmt)
            {
                line("<null-stmt>");
                return;
            }
            RecursiveAstVisitor::visitStmt(stmt);
        }
        void visitExpr(Expr const* expr) override
        {
            if (!expr)
            {
                line("<null-expr>");
                return;
            }
            RecursiveAstVisitor::visitExpr(expr);
        }
        void visitTypeExpr(TypeExpr const* type) override
        {
            if (!type)
            {
                line("<null-type>");
                return;
            }
            RecursiveAstVisitor::visitTypeExpr(type);
        }
        void visitPattern(Pattern const* pat) override
        {
            if (!pat)
            {
                line("<null-pattern>");
                return;
            }
            RecursiveAstVisitor::visitPattern(pat);
        }

        void visitPrimitiveType(PrimitiveType const* t) override { line_fmt("Primitive {}", token_str(t->which)); }

        void visitNamedType(NamedType const* t) override
        {
            line_fmt("Named {}", path_str(t->path));
            if (!t->template_args.empty())
            {
                IndentScope is(m_indent_level);
                print_template_args(t->template_args);
            }
        }
        void visitPointerType(PointerType const* t) override
        {
            line("Pointer");
            IndentScope is(m_indent_level);
            visitTypeExpr(t->pointee);
        }
        void visitArrayType(ArrayType const* t) override
        {
            line("Array");
            IndentScope is(m_indent_level);
            line("Element");
            {
                IndentScope is2(m_indent_level);
                visitTypeExpr(t->element);
            }
            if (t->size)
            {
                line("Size");
                IndentScope is2(m_indent_level);
                visitExpr(t->size);
            }
        }
        void visitSliceType(SliceType const* t) override
        {
            line("Slice");
            IndentScope is(m_indent_level);
            visitTypeExpr(t->element);
        }
        void visitFamType(FamType const* t) override
        {
            line("Fam");
            IndentScope is(m_indent_level);
            visitTypeExpr(t->element);
        }
        void visitFuncPtrType(FuncPtrType const* t) override
        {
            line("FuncPtr");
            IndentScope is(m_indent_level);
            if (t->return_type)
            {
                line("Return");
                IndentScope is2(m_indent_level);
                visitTypeExpr(t->return_type);
            }
            if (!t->params.empty())
            {
                line("Params");
                IndentScope is2(m_indent_level);
                for (auto* p : t->params)
                    visitTypeExpr(p);
            }
        }
        void visitQualifiedType(QualifiedType const* t) override
        {
            line_fmt("Qualified {}", qual_str(t->quals));
            IndentScope is(m_indent_level);
            visitTypeExpr(t->inner);
        }

        void visitPackIndexType(PackIndexType const* t) override
        {
            line("PackIndex");
            IndentScope is(m_indent_level);
            line("Base");
            {
                IndentScope is2(m_indent_level);
                visitTypeExpr(t->base);
            }
            line("Index");
            {
                IndentScope is2(m_indent_level);
                visitExpr(t->index);
            }
        }

        void visitIntLiteralExpr(IntLiteralExpr const* e) override { line_fmt("IntLiteral {}", e->value); }
        void visitFloatLiteralExpr(FloatLiteralExpr const* e) override { line_fmt("FloatLiteral {}", e->value); }
        void visitStringLiteralExpr(StringLiteralExpr const* e) override { line_fmt("StringLiteral \"{}\"", std::string_view{e->value}); }
        void visitU16StringLiteralExpr(U16StringLiteralExpr const* e) override
        {
            std::string utf8_repr;
            utf8_repr.reserve(e->value.size() * 3);
            for (auto cu : e->value)
            {
                if (cu < 0x80)
                    utf8_repr.push_back(static_cast<char>(cu));
                else
                    std::format_to(std::back_inserter(utf8_repr), "\\u{:04x}", static_cast<unsigned>(cu));
            }

            line_fmt("U16StringLiteral \"{}\"", utf8_repr);
        }
        void visitCharLiteralExpr(CharLiteralExpr const* e) override { line_fmt("CharLiteral {}", e->codepoint); }
        void visitU16CharLiteralExpr(U16CharLiteralExpr const* e) override { line_fmt("U16CharLiteral {}", e->value); }
        void visitBoolLiteralExpr(BoolLiteralExpr const* e) override { line_fmt("BoolLiteral {}", e->value); }
        void visitNullLiteralExpr(NullLiteralExpr const*) override { line("NullLiteral"); }
        void visitIdentExpr(IdentExpr const* e) override { line_fmt("Ident {}", e->name); }
        void visitPathExpr(PathExpr const* e) override
        {
            line_fmt("Path {}", path_str(e->path));
            if (!e->explicit_enum_args.empty())
            {
                IndentScope is(m_indent_level);
                print_template_args(e->explicit_enum_args);
            }
        }
        void visitUnaryExpr(UnaryExpr const* e) override
        {
            line_fmt("Unary op={}", token_str(e->op));
            IndentScope is(m_indent_level);
            visitExpr(e->operand);
        }
        void visitPostfixExpr(PostfixExpr const* e) override
        {
            line_fmt("Postfix op={}", token_str(e->op));
            IndentScope is(m_indent_level);
            visitExpr(e->operand);
        }
        void visitBinaryExpr(BinaryExpr const* e) override
        {
            line_fmt("Binary op={}", token_str(e->op));
            IndentScope is(m_indent_level);
            visitExpr(e->lhs);
            visitExpr(e->rhs);
        }
        void visitCallExpr(CallExpr const* e) override
        {
            line("Call");
            IndentScope is(m_indent_level);
            line("Callee");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->callee);
            }
            if (!e->args.empty())
            {
                line("Args");
                IndentScope is2(m_indent_level);
                for (auto* a : e->args)
                    visitExpr(a);
            }
        }
        void visitFieldAccessExpr(FieldAccessExpr const* e) override
        {
            line_fmt("FieldAccess field={}", e->field);
            IndentScope is(m_indent_level);
            visitExpr(e->object);
        }
        void visitIndexExpr(IndexExpr const* e) override
        {
            line("Index");
            IndentScope is(m_indent_level);
            line("Object");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->object);
            }
            line("Subscript");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->index);
            }
        }
        void visitPackAccessExpr(PackAccessExpr const* e) override
        {
            line("PackAccess");
            IndentScope is(m_indent_level);
            line("Object");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->object);
            }
            line("Index");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->index);
            }
        }
        void visitCastExpr(CastExpr const* e) override
        {
            line("Cast");
            IndentScope is(m_indent_level);
            line("Target");
            {
                IndentScope is2(m_indent_level);
                visitTypeExpr(e->target);
            }
            line("Operand");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->operand);
            }
        }
        void visitBlockExpr(BlockExpr const* e) override { print_block("Block", e->body); }
        void visitIfExpr(IfExpr const* e) override
        {
            line("If");
            IndentScope is(m_indent_level);
            line("Cond");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->condition);
            }
            print_block("Then", e->then_block);
            if (e->else_branch)
            {
                line("Else");
                IndentScope is2(m_indent_level);
                visitExpr(e->else_branch);
            }
        }
        void visitMatchExpr(MatchExpr const* e) override
        {
            line("Match");
            IndentScope is(m_indent_level);
            line("Operand");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->operand);
            }
            for (const auto& a : e->arms)
                print_match_arm(a);
        }
        void visitStructLiteralExpr(StructLiteralExpr const* e) override
        {
            line("StructLiteral");
            IndentScope is(m_indent_level);
            if (e->type)
            {
                line("Type");
                IndentScope is2(m_indent_level);
                visitTypeExpr(e->type);
            }
            for (const auto& f : e->fields)
            {
                line_fmt("Field name={}", f.name);
                IndentScope is2(m_indent_level);
                visitExpr(f.value);
            }
        }
        void visitSizeofExpr(SizeofExpr const* e) override
        {
            line("Sizeof");
            IndentScope is(m_indent_level);
            visitTypeExpr(e->target);
        }
        void visitSizeofPackExpr(SizeofPackExpr const* e) override { line_fmt("SizeofPack name={}", e->pack_name); }
        void visitPackExpansionExpr(PackExpansionExpr const* e) override
        {
            line("PackExpansion");
            IndentScope is(m_indent_level);
            visitExpr(e->operand);
        }
        void visitAlignofExpr(AlignofExpr const* e) override
        {
            line("Alignof");
            IndentScope is(m_indent_level);
            visitTypeExpr(e->target);
        }
        void visitOffsetofExpr(OffsetofExpr const* e) override
        {
            line_fmt("Offsetof field={}", e->field);
            IndentScope is(m_indent_level);
            visitTypeExpr(e->target);
        }
        void visitCompilesExpr(CompilesExpr const* e) override
        {
            line("Compiles");
            IndentScope is(m_indent_level);
            if (!e->params.empty())
            {
                line("Params");
                IndentScope is2(m_indent_level);
                for (const auto& p : e->params)
                {
                    if (p.is_pack)
                        line_fmt("Param name={} pack", p.name);
                    else
                        line_fmt("Param name={}", p.name);
                    if (p.type)
                    {
                        IndentScope is3(m_indent_level);
                        visitTypeExpr(p.type);
                    }
                }
            }
            print_block("Body", e->body);
        }
        void visitRangeExpr(RangeExpr const* e) override
        {
            line_fmt("Range inclusive={}", e->inclusive);
            IndentScope is(m_indent_level);
            if (e->start)
            {
                line("Start");
                IndentScope is2(m_indent_level);
                visitExpr(e->start);
            }
            if (e->end)
            {
                line("End");
                IndentScope is2(m_indent_level);
                visitExpr(e->end);
            }
        }
        void visitTypeASTExpr(TypeASTExpr const* e) override
        {
            line("TypeAST");
            IndentScope is(m_indent_level);
            visitTypeExpr(e->type_node);
        }
        void visitTemplateInstExpr(TemplateInstExpr const* e) override
        {
            line("TemplateInst");
            IndentScope is(m_indent_level);
            line("Callee");
            {
                IndentScope is2(m_indent_level);
                visitExpr(e->callee);
            }
            print_template_args(e->template_args);
        }

        void visitLiteralPattern(LiteralPattern const* p) override
        {
            line("LiteralPattern");
            IndentScope is(m_indent_level);
            visitExpr(p->value);
        }
        void visitBindingPattern(BindingPattern const* p) override
        {
            if (p->by_reference)
                line_fmt("BindingPattern name={} by_ref=true", p->name);
            else
                line_fmt("BindingPattern name={}", p->name);
        }
        void visitRefPattern(RefPattern const* p) override
        {
            line("RefPattern");
            IndentScope is(m_indent_level);
            visitPattern(p->inner);
        }
        void visitWildcardPattern(WildcardPattern const*) override { line("WildcardPattern"); }
        void visitEnumDestructurePattern(EnumDestructurePattern const* p) override
        {
            line_fmt("EnumDestructurePattern variant={}", path_str(p->variant_path));
            if (!p->payload.empty())
            {
                IndentScope is(m_indent_level);
                for (auto* sub : p->payload)
                    visitPattern(sub);
            }
        }
        void visitStructDestructurePattern(StructDestructurePattern const* p) override
        {
            line_fmt("StructDestructurePattern type={}{}", path_str(p->type_path), p->has_rest ? " has_rest" : "");
            IndentScope is(m_indent_level);
            for (const auto& f : p->fields)
            {
                line_fmt("Field name={}", f.field_name);
                IndentScope is2(m_indent_level);
                visitPattern(f.pattern);
            }
        }
        void visitRangePattern(RangePattern const* p) override
        {
            line_fmt("RangePattern inclusive={}", p->inclusive);
            IndentScope is(m_indent_level);
            if (p->start)
            {
                line("Start");
                IndentScope is2(m_indent_level);
                visitExpr(p->start);
            }
            if (p->end)
            {
                line("End");
                IndentScope is2(m_indent_level);
                visitExpr(p->end);
            }
        }
        void visitOrPattern(OrPattern const* p) override
        {
            line("OrPattern");
            IndentScope is(m_indent_level);
            for (auto* alt : p->alternatives)
                visitPattern(alt);
        }

        void visitExprStmt(ExprStmt const* s) override
        {
            line("ExprStmt");
            IndentScope is(m_indent_level);
            visitExpr(s->expr);
        }
        void visitDeclStmt(DeclStmt const* s) override { visitDecl(s->decl); }
        void visitReturnStmt(ReturnStmt const* s) override
        {
            line("Return");
            if (s->value)
            {
                IndentScope is(m_indent_level);
                visitExpr(s->value);
            }
        }
        void visitBreakStmt(BreakStmt const*) override { line("Break"); }
        void visitContinueStmt(ContinueStmt const*) override { line("Continue"); }
        void visitWhileStmt(WhileStmt const* s) override
        {
            line("While");
            IndentScope is(m_indent_level);
            line("Cond");
            {
                IndentScope is2(m_indent_level);
                visitExpr(s->condition);
            }
            print_block("Body", s->body);
        }
        void visitDoWhileStmt(DoWhileStmt const* s) override
        {
            line("DoWhile");
            IndentScope is(m_indent_level);
            print_block("Body", s->body);
            line("Cond");
            {
                IndentScope is2(m_indent_level);
                visitExpr(s->condition);
            }
        }
        void visitForStmt(ForStmt const* s) override
        {
            line("For");
            IndentScope is(m_indent_level);
            if (s->init)
            {
                line("Init");
                IndentScope is2(m_indent_level);
                visitStmt(s->init);
            }
            if (s->cond)
            {
                line("Cond");
                IndentScope is2(m_indent_level);
                visitExpr(s->cond);
            }
            if (s->update)
            {
                line("Update");
                IndentScope is2(m_indent_level);
                visitExpr(s->update);
            }
            print_block("Body", s->body);
        }
        void visitForInStmt(ForInStmt const* s) override
        {
            if (s->by_reference)
                line_fmt("ForIn name={} by_ref=true", s->item_name);
            else
                line_fmt("ForIn name={}", s->item_name);
            IndentScope is(m_indent_level);
            if (s->item_type)
            {
                line("ItemType");
                IndentScope is2(m_indent_level);
                visitTypeExpr(s->item_type);
            }
            line("Iterable");
            {
                IndentScope is2(m_indent_level);
                visitExpr(s->iterable);
            }
            print_block("Body", s->body);
        }
        void visitDeferStmt(DeferStmt const* s) override
        {
            line("Defer");
            IndentScope is(m_indent_level);
            visitStmt(s->body);
        }
        void visitStaticIfStmt(StaticIfStmt const* s) override
        {
            line("StaticIf");
            IndentScope is(m_indent_level);
            line("Cond");
            {
                IndentScope is2(m_indent_level);
                visitExpr(s->condition);
            }
            print_block("Then", s->then_block);
            if (s->else_branch)
            {
                line("Else");
                IndentScope is2(m_indent_level);
                visitStmt(s->else_branch);
            }
        }
        void visitStaticMatchStmt(StaticMatchStmt const* s) override
        {
            line("StaticMatch");
            IndentScope is(m_indent_level);
            line("Operand");
            {
                IndentScope is2(m_indent_level);
                visitExpr(s->operand);
            }
            for (const auto& a : s->arms)
                print_match_arm(a);
        }
        void visitStaticForStmt(StaticForStmt const* s) override
        {
            line_fmt("StaticFor name={} type_for={}", s->item_name, s->is_type_for);
            IndentScope is(m_indent_level);
            line("Pack");
            {
                IndentScope is2(m_indent_level);
                visitExpr(s->pack_expr);
            }
            print_block("Body", s->body);
        }
        void visitAmbiguousStmt(AmbiguousStmt const* s) override
        {
            std::string_view res = "unresolved";
            switch (s->resolution)
            {
                case AmbiguousStmt::Resolution::Unresolved:
                    res = "unresolved";
                    break;
                case AmbiguousStmt::Resolution::AsDecl:
                    res = "as_decl";
                    break;
                case AmbiguousStmt::Resolution::AsExpr:
                    res = "as_expr";
                    break;
            }
            line_fmt("AmbiguousStmt resolution={}", res);
            IndentScope is(m_indent_level);
            if (s->as_decl)
            {
                line("AsDecl");
                IndentScope is2(m_indent_level);
                visitDecl(s->as_decl);
            }
            if (s->as_expr)
            {
                line("AsExpr");
                IndentScope is2(m_indent_level);
                visitExpr(s->as_expr);
            }
        }

        void visitModuleDecl(ModuleDecl const* d) override
        {
            line_fmt("Module {}", path_str(d->module_path));
            if (!d->attrs.empty())
            {
                IndentScope is(m_indent_level);
                print_attrs(d->attrs);
            }
        }
        void visitImportDecl(ImportDecl const* d) override
        {
            line_fmt("Import {}", path_str(d->module_path));
            IndentScope is(m_indent_level);
            print_attrs(d->attrs);
            print_decl_modifiers(static_cast<Decl const*>(d));
        }
        void visitUsingDecl(UsingDecl const* d) override
        {
            std::string_view k = "?";
            switch (d->using_kind)
            {
                case UsingKind::Alias:
                    k = "alias";
                    break;
                case UsingKind::BareImport:
                    k = "bare";
                    break;
                case UsingKind::Wildcard:
                    k = "wildcard";
                    break;
                case UsingKind::List:
                    k = "list";
                    break;
                case UsingKind::Concept:
                    k = "concept";
                    break;
            }
            line_fmt("Using kind={}{}", k, d->is_spill ? " spill" : "");
            IndentScope is(m_indent_level);
            print_attrs(d->attrs);
            print_decl_modifiers(static_cast<Decl const*>(d));
            if (!d->alias_path.is_empty())
                line_fmt("alias={}", path_str(d->alias_path));
            print_template_params(d->template_params);
            if (d->target_type)
            {
                line("TargetType");
                IndentScope is2(m_indent_level);
                visitTypeExpr(d->target_type);
            }
            if (!d->target_path.is_empty())
                line_fmt("target_path={}", path_str(d->target_path));
            if (!d->target_items.empty())
            {
                line("Targets");
                IndentScope is2(m_indent_level);
                bool has_nested = false;
                for (auto const* item : d->target_items)
                    if (!item->children.empty())
                    {
                        has_nested = true;
                        break;
                    }
                if (has_nested)
                {
                    for (auto const* item : d->target_items)
                        print_using_item(item);
                }
                else
                {
                    for (auto const* item : d->target_items)
                        line(path_str(item->path));
                }
            }
            else if (!d->target_list.empty())
            {
                line("Targets");
                IndentScope is2(m_indent_level);
                for (const auto& tp : d->target_list)
                    line(path_str(tp));
            }
            if (d->target_expr)
            {
                line("TargetExpr");
                IndentScope is2(m_indent_level);
                visitExpr(d->target_expr);
            }
        }
        void visitStructDecl(StructDecl const* d) override
        {
            line_fmt("Struct name={}", d->name);
            IndentScope is(m_indent_level);
            print_attrs(d->attrs);
            print_decl_modifiers(static_cast<Decl const*>(d));
            print_template_params(d->template_params);
            if (d->constraint)
            {
                line("Constraint");
                IndentScope is2(m_indent_level);
                visitExpr(d->constraint);
            }
            for (const auto& f : d->fields)
                print_field_decl(f);
        }
        void visitUnionDecl(UnionDecl const* d) override
        {
            line_fmt("Union name={}", d->name);
            IndentScope is(m_indent_level);
            print_attrs(d->attrs);
            print_decl_modifiers(static_cast<Decl const*>(d));
            for (const auto& f : d->fields)
                print_field_decl(f);
        }
        void visitEnumDecl(EnumDecl const* d) override
        {
            line_fmt("Enum name={}{}", d->name, d->is_tagged ? " tagged" : "");
            IndentScope is(m_indent_level);
            print_attrs(d->attrs);
            print_decl_modifiers(static_cast<Decl const*>(d));
            print_template_params(d->template_params);
            if (d->backing_type)
            {
                line("Backing");
                IndentScope is2(m_indent_level);
                visitTypeExpr(d->backing_type);
            }
            if (d->constraint)
            {
                line("Constraint");
                IndentScope is2(m_indent_level);
                visitExpr(d->constraint);
            }
            for (const auto& v : d->variants)
                print_enum_variant(v);
        }
        void visitFuncDecl(FuncDecl const* d) override
        {
            line_fmt("Func name={}", d->name);
            IndentScope is(m_indent_level);
            print_attrs(d->attrs);
            print_decl_modifiers(static_cast<Decl const*>(d));
            print_template_params(d->template_params);
            if (d->return_type)
            {
                line("Return");
                IndentScope is2(m_indent_level);
                visitTypeExpr(d->return_type);
            }
            if (!d->params.empty())
            {
                line("Params");
                IndentScope is2(m_indent_level);
                for (const auto& p : d->params)
                    print_func_param(p);
            }
            if (d->constraint)
            {
                line("Constraint");
                IndentScope is2(m_indent_level);
                visitExpr(d->constraint);
            }
            if (d->body)
                print_block("Body", *d->body);
        }
        void visitVarDecl(VarDecl const* d) override
        {
            line_fmt("Var name={}", d->name);
            IndentScope is(m_indent_level);
            print_attrs(d->attrs);
            print_decl_modifiers(static_cast<Decl const*>(d));
            if (d->type)
            {
                line("Type");
                IndentScope is2(m_indent_level);
                visitTypeExpr(d->type);
            }
            if (d->init)
            {
                line("Init");
                IndentScope is2(m_indent_level);
                visitExpr(d->init);
            }
        }
        void visitStaticIfGroup(StaticIfGroup const* g) override
        {
            line("StaticIfGroup");
            IndentScope is(m_indent_level);
            if (g->condition)
            {
                line("Cond");
                IndentScope is2(m_indent_level);
                visitExpr(g->condition);
            }
            line("Then");
            {
                IndentScope is2(m_indent_level);
                for (auto* d : g->then_decls)
                    visitDecl(d);
            }
            if (g->else_group)
            {
                line("Else");
                IndentScope is2(m_indent_level);
                visitStaticIfGroup(g->else_group);
            }
        }

    private:
        std::string m_out;
        std::size_t m_indent_level = 0;

        struct IndentScope
        {
            std::size_t& level;
            IndentScope(std::size_t& l) : level(l) { ++level; }
            ~IndentScope() { --level; }
            IndentScope(const IndentScope&) = delete;
            IndentScope& operator=(const IndentScope&) = delete;
        };

        void pad() { m_out.append(m_indent_level * 2, ' '); }

        void line(std::string_view str)
        {
            pad();
            m_out += str;
            m_out += '\n';
        }

        template <typename... Args> void line_fmt(std::format_string<Args...> fmt, Args&&... args)
        {
            pad();
            std::format_to(std::back_inserter(m_out), fmt, std::forward<Args>(args)...);
            m_out += '\n';
        }

        static std::string path_str(const Path& p)
        {
            std::string s;
            for (std::size_t i = 0; i < p.segments.size(); ++i)
            {
                if (i > 0)
                    s += "::";
                s += p.segments[i].name;
            }
            return s;
        }

        static std::string token_str(lex::TokenKind k) { return std::string{lex::to_string(k)}; }

        static std::string qual_str(Qual q)
        {
            std::string s;
            if (has_qual(q, Qual::Const))
                s += "const ";
            if (has_qual(q, Qual::Volatile))
                s += "volatile ";
            if (has_qual(q, Qual::Restrict))
                s += "restrict ";
            if (!s.empty())
                s.pop_back();
            return s;
        }

        void print_translation_unit(const TranslationUnit* tu)
        {
            if (!tu)
            {
                line("<null-tu>");
                return;
            }
            line("TranslationUnit");
            IndentScope is(m_indent_level);
            if (tu->module_decl)
                visitDecl(tu->module_decl);
            for (auto* d : tu->imports)
                visitDecl(d);
            for (auto* d : tu->decls)
                visitDecl(d);
        }

        void print_attrs(const std::pmr::vector<Attribute>& attrs)
        {
            for (const auto& a : attrs)
                if (a.args.empty())
                    line_fmt("@{}", a.name);
                else
                {
                    line_fmt("@{}", a.name);
                    IndentScope is(m_indent_level);
                    for (auto* e : a.args)
                        visitExpr(e);
                }
        }

        void print_decl_modifiers(const Decl* d)
        {
            if (d->is_public)
                line("public");
            if (d->is_extern)
                line("extern");
        }

        void print_template_params(const std::pmr::vector<TemplateParam>& params)
        {
            if (params.empty())
                return;
            line("TemplateParams");
            IndentScope is(m_indent_level);
            for (const auto& tp : params)
            {
                if (tp.is_pack)
                    line_fmt("Param name={} pack", tp.name);
                else
                    line_fmt("Param name={}", tp.name);
                if (tp.value_type)
                {
                    IndentScope is2(m_indent_level);
                    line("ValueType");
                    IndentScope is3(m_indent_level);
                    visitTypeExpr(tp.value_type);
                }
            }
        }

        void print_template_args(const std::pmr::vector<TemplateArg>& args)
        {
            if (args.empty())
                return;
            line("TemplateArgs");
            IndentScope is(m_indent_level);
            for (const auto& a : args)
            {
                line("Arg");
                IndentScope is2(m_indent_level);
                if (a.type)
                    visitTypeExpr(a.type);
                if (a.expr)
                    visitExpr(a.expr);
            }
        }

        void print_block(std::string_view label, const Block& b)
        {
            line(label);
            IndentScope is(m_indent_level);
            for (auto* s : b.stmts)
                visitStmt(s);
            if (b.tail)
            {
                line("Tail");
                IndentScope is2(m_indent_level);
                visitExpr(b.tail);
            }
        }

        void print_match_arm(const MatchArm& arm)
        {
            line("Arm");
            IndentScope is(m_indent_level);
            if (arm.pattern)
                visitPattern(arm.pattern);
            if (arm.guard)
            {
                line("Guard");
                IndentScope is2(m_indent_level);
                visitExpr(arm.guard);
            }
            if (arm.body)
            {
                line("Body");
                IndentScope is2(m_indent_level);
                visitExpr(arm.body);
            }
        }

        void print_field_decl(const FieldDecl& f)
        {
            line_fmt("Field name={}", f.name);
            IndentScope is(m_indent_level);
            if (f.type)
                visitTypeExpr(f.type);
        }

        void print_enum_variant(const EnumVariant& v)
        {
            line_fmt("Variant name={}", v.name);
            IndentScope is(m_indent_level);
            print_attrs(v.attrs);
            if (!v.payload.empty())
            {
                line("Payload");
                IndentScope is2(m_indent_level);
                for (auto* t : v.payload)
                    visitTypeExpr(t);
            }
            if (v.explicit_value)
            {
                line("Value");
                IndentScope is2(m_indent_level);
                visitExpr(v.explicit_value);
            }
        }

        void print_func_param(const FuncParam& p)
        {
            if (p.is_pack)
                line_fmt("Param name={} pack", p.name);
            else
                line_fmt("Param name={}", p.name);
            if (p.type)
            {
                IndentScope is(m_indent_level);
                visitTypeExpr(p.type);
            }
        }

        void print_using_item(UsingItem const* item)
        {
            if (item->children.empty())
            {
                line_fmt("Item {}", path_str(item->path));
            }
            else
            {
                line_fmt("Group {}", path_str(item->path));
                IndentScope is(m_indent_level);
                for (auto const* child : item->children)
                    print_using_item(child);
            }
        }
    };

} // namespace dcc::ast
