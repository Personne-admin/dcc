module;

#include <algorithm>

export module dcc.sema.body_analyzer;

import std;
import dcc.ast;
import dcc.comptime;
import dcc.const_eval;
import dcc.diag;
import dcc.si;
import dcc.sm;
import dcc.lex.tokens;
import dcc.types;
import dcc.sema.infer;
import dcc.sema.scope;
import dcc.sema.instantiator;
import dcc.sema.type_helpers;

export namespace dcc::sema
{
    void analyze_bodies(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, ast::AstContext& ast_ctx,
                        types::TypeContext& type_ctx, std::pmr::polymorphic_allocator<> alloc, SpecializationRegistry& reg);

    [[nodiscard]] std::optional<infer::TemplateBindings> make_bindings(types::TypeContext& types, types::TypePtr ty);
    [[nodiscard]] types::TypePtr substitute_in_nominal_context(types::TypeContext& types, types::TypePtr ty, types::TypePtr context);
    void ensure_tagged_enum_complete(types::TypeContext& types, std::pmr::polymorphic_allocator<> alloc, types::TypePtr ty);
    void complete_all_templated_tagged_enums(types::TypeContext& types, std::pmr::polymorphic_allocator<> alloc);

    class BodyDumper
    {
    public:
        [[nodiscard]] static std::string dump(ModuleInfo const& mod)
        {
            BodyDumper d;
            d.line_fmt("Module {}", mod.canonical_path.str());
            d.m_indent++;
            if (mod.tu)
                d.print_tu(*mod.tu);

            return d.m_out;
        }

        [[nodiscard]] static std::string dump_decl(ast::FuncDecl const& f)
        {
            BodyDumper d;
            d.print_decl(f);
            return d.m_out;
        }

    private:
        std::string m_out;
        std::size_t m_indent{};

        void pad() { m_out.append(m_indent * 2, ' '); }

        template <typename... A> void line_fmt(std::format_string<A...> fmt, A&&... args)
        {
            pad();
            std::format_to(std::back_inserter(m_out), fmt, std::forward<A>(args)...);
            m_out += '\n';
        }

        void line(std::string_view s)
        {
            pad();
            m_out += s;
            m_out += '\n';
        }

        template <typename P> static std::string decl_name(P const* d)
        {
            auto const* dd = reinterpret_cast<ast::Decl const*>(d);
            if (!dd)
                return "<null>";

            switch (dd->kind)
            {
                case ast::DeclKind::Struct:
                    return std::string{static_cast<ast::StructDecl const*>(dd)->name};
                case ast::DeclKind::Union:
                    return std::string{static_cast<ast::UnionDecl const*>(dd)->name};
                case ast::DeclKind::Enum:
                    return std::string{static_cast<ast::EnumDecl const*>(dd)->name};
                case ast::DeclKind::Func:
                    return std::string{static_cast<ast::FuncDecl const*>(dd)->name};
                case ast::DeclKind::Var:
                    return std::string{static_cast<ast::VarDecl const*>(dd)->name};
                case ast::DeclKind::Using:
                    return static_cast<ast::UsingDecl const*>(dd)->alias_path.is_empty()
                               ? std::string{"<using>"}
                               : std::string{static_cast<ast::UsingDecl const*>(dd)->alias_path.segments.back().name};
                case ast::DeclKind::Module:
                    return "module";
                case ast::DeclKind::Import:
                    return "import";
                case ast::DeclKind::StaticIfGroup:
                    return "static if";
            }
            return "<decl>";
        }

        static std::string type_str(std::nullptr_t) { return "<unresolved>"; }

        static std::string raw_type_str(ast::TypeExpr const* t)
        {
            if (!t)
                return "<unresolved>";

            switch (t->kind)
            {
                case ast::TypeKind::Primitive:
                    switch (static_cast<ast::PrimitiveType const*>(t)->which)
                    {
                        case lex::TokenKind::KwVoid:
                            return "void";
                        case lex::TokenKind::KwBool:
                            return "bool";
                        case lex::TokenKind::KwChar:
                            return "char";
                        case lex::TokenKind::Kwi32:
                            return "i32";
                        case lex::TokenKind::Kwu32:
                            return "u32";
                        case lex::TokenKind::Kwi64:
                            return "i64";
                        case lex::TokenKind::Kwu64:
                            return "u64";
                        case lex::TokenKind::Kwf32:
                            return "f32";
                        case lex::TokenKind::Kwf64:
                            return "f64";
                        default:
                            return "<type>";
                    }
                case ast::TypeKind::Named:
                    return static_cast<ast::NamedType const*>(t)->path.is_simple() ? std::string{static_cast<ast::NamedType const*>(t)->path.simple_name()}
                                                                                   : std::string{"<type>"};
                default:
                    return "<type>";
            }
        }

        template <typename P> static std::string type_str(P const* ty) { return format_dcc_type(reinterpret_cast<types::TypePtr>(ty)); }

        static std::string const_str(std::nullptr_t) { return "<none>"; }

        template <typename P> static std::string const_str(P const* c)
        {
            auto const* cc = reinterpret_cast<comptime::Value const*>(c);
            if (!cc)
                return "<none>";

            switch (cc->kind())
            {
                case comptime::Value::Kind::Int:
                    return std::to_string(cc->get_int());
                case comptime::Value::Kind::Bool:
                    return cc->get_bool() ? "true" : "false";
                case comptime::Value::Kind::Char:
                    return std::to_string(cc->get_char());
                case comptime::Value::Kind::Float:
                    return std::format("{:g}", cc->get_float());
                case comptime::Value::Kind::Null:
                    return "null";
                case comptime::Value::Kind::String:
                    return std::format("\"{}\"", cc->get_string());
                case comptime::Value::Kind::Aggregate: {
                    std::string r = "agg(";
                    for (std::size_t i = 0; i < cc->size(); ++i)
                    {
                        if (i)
                            r += ", ";
                        r += const_str(&cc->at(i));
                    }
                    r += ")";
                    return r;
                }
                case comptime::Value::Kind::Slice: {
                    std::string r = "slice(";
                    for (std::size_t i = 0; i < cc->size(); ++i)
                    {
                        if (i)
                            r += ", ";
                        r += const_str(&cc->at(i));
                    }
                    r += ")";
                    return r;
                }
                default:
                    return "<const>";
            }
        }

        static std::string expr_suffix(ast::Expr const& e)
        {
            std::string s = std::format("[type={} const={}", type_str(e.sema.resolved_type), const_str(e.sema.const_value));
            if (e.sema.is_lvalue)
                s += " lvalue";
            if (e.sema.is_constant)
                s += " constant";
            if (e.sema.is_diverging)
                s += " diverging";
            if (e.sema.implicit_addr_of)
                s += " addr_of";
            if (e.sema.implicit_deref)
                s += " deref";
            if (e.sema.resolved_decl)
                s += std::format(" decl={}", decl_name(e.sema.resolved_decl));
            if (e.sema.resolved_specialization)
                s += std::format(" spec={}", decl_name(e.sema.resolved_specialization));
            if (e.sema.ufcs_callee)
                s += std::format(" ufcs={}", decl_name(e.sema.ufcs_callee));
            s += "]";
            return s;
        }

        void print_template_args(std::pmr::vector<ast::TemplateArg> const& args)
        {
            if (args.empty())
                return;

            line("TemplateArgs");
            m_indent++;
            for (auto const& a : args)
            {
                line("Arg");
                m_indent++;
                if (a.type)
                {
                    if (auto const* pt = ast::node_cast<ast::PrimitiveType>(a.type))
                    {
                        switch (pt->which)
                        {
                            case lex::TokenKind::KwVoid:
                                line("Primitive void");
                                break;
                            case lex::TokenKind::KwBool:
                                line("Primitive bool");
                                break;
                            case lex::TokenKind::KwChar:
                                line("Primitive char");
                                break;
                            case lex::TokenKind::Kwi32:
                                line("Primitive i32");
                                break;
                            case lex::TokenKind::Kwu32:
                                line("Primitive u32");
                                break;
                            case lex::TokenKind::Kwi64:
                                line("Primitive i64");
                                break;
                            case lex::TokenKind::Kwu64:
                                line("Primitive u64");
                                break;
                            case lex::TokenKind::Kwf32:
                                line("Primitive f32");
                                break;
                            case lex::TokenKind::Kwf64:
                                line("Primitive f64");
                                break;
                            default:
                                line("Primitive <type>");
                                break;
                        }
                    }
                    else
                        line("ArgType");
                }
                if (a.expr)
                    print_expr(*a.expr);
                m_indent--;
            }
            m_indent--;
        }

        void print_tu(ast::TranslationUnit const& tu)
        {
            for (auto* d : tu.decls)
                print_decl(*d);
        }

        void print_decl(ast::Decl const& d)
        {
            if (auto const* f = ast::node_cast<ast::FuncDecl>(&d))
            {
                line_fmt("Func {} diverging={}", f->name, f->sema.is_diverging ? "true" : "false");
                m_indent++;
                for (auto const& p : f->params)
                    line_fmt("Param {} type={} storage={} offset={} index={}", p.name,
                             p.type && p.type->sema.canonical ? type_str(get_canonical(p.type->sema)) : raw_type_str(p.type),
                             p.sema.storage == ast::StorageClass::Param ? "param" : "?", p.sema.frame_offset, p.sema.param_index);

                if (f->body)
                    print_block(*f->body);

                m_indent--;
            }
            else if (auto const* v = ast::node_cast<ast::VarDecl>(&d))
            {
                line_fmt("Var {} type={} storage={} offset={}", v->name,
                         v->type && v->type->sema.canonical ? type_str(get_canonical(v->type->sema)) : raw_type_str(v->type), storage_str(v->sema.storage),
                         v->sema.frame_offset);

                if (v->init)
                {
                    m_indent++;
                    print_expr(*v->init);
                    m_indent--;
                }
            }
        }

        static std::string storage_str(ast::StorageClass sc)
        {
            switch (sc)
            {
                case ast::StorageClass::Unresolved:
                    return "unresolved";
                case ast::StorageClass::Local:
                    return "local";
                case ast::StorageClass::ModuleGlobal:
                    return "module";
                case ast::StorageClass::Static:
                    return "static";
                case ast::StorageClass::Extern:
                    return "extern";
                case ast::StorageClass::Param:
                    return "param";
            }
            return "?";
        }

        void print_block(ast::Block const& b)
        {
            line("Body");
            m_indent++;
            for (auto* s : b.stmts)
                print_stmt(*s);
            if (b.tail)
                print_expr(*b.tail);
            print_exit_defers(b.exit_defers);
            m_indent--;
        }

        static bool block_empty(ast::Block const& b) noexcept { return b.stmts.empty() && !b.tail; }

        void print_exit_defers(std::span<sm::SourceRange const> defers)
        {
            for (auto const& defer_range : defers)
            {
                std::ignore = defer_range;
                line("RunsDefer");
            }
        }

        void print_stmt(ast::Stmt const& s)
        {
            switch (s.kind)
            {
                case ast::StmtKind::Expr:
                    if (static_cast<ast::ExprStmt const&>(s).expr)
                        print_expr(*static_cast<ast::ExprStmt const&>(s).expr);
                    else
                        line("<null expr>");
                    break;
                case ast::StmtKind::DeclStmt:
                    print_decl(*static_cast<ast::DeclStmt const&>(s).decl);
                    break;
                case ast::StmtKind::Return:
                    line("Return");
                    m_indent++;
                    if (static_cast<ast::ReturnStmt const&>(s).value)
                        print_expr(*static_cast<ast::ReturnStmt const&>(s).value);
                    print_exit_defers(static_cast<ast::ReturnStmt const&>(s).exit_defers);
                    m_indent--;
                    break;
                case ast::StmtKind::Break:
                    line("Break");
                    print_exit_defers(static_cast<ast::BreakStmt const&>(s).exit_defers);
                    break;
                case ast::StmtKind::Continue:
                    line("Continue");
                    print_exit_defers(static_cast<ast::ContinueStmt const&>(s).exit_defers);
                    break;
                case ast::StmtKind::While:
                    line("While");
                    m_indent++;
                    if (static_cast<ast::WhileStmt const&>(s).condition)
                        print_expr(*static_cast<ast::WhileStmt const&>(s).condition);
                    else
                        line("<null condition>");
                    print_block(static_cast<ast::WhileStmt const&>(s).body);
                    m_indent--;
                    break;
                case ast::StmtKind::DoWhile:
                    line("DoWhile");
                    m_indent++;
                    print_block(static_cast<ast::DoWhileStmt const&>(s).body);
                    if (static_cast<ast::DoWhileStmt const&>(s).condition)
                        print_expr(*static_cast<ast::DoWhileStmt const&>(s).condition);
                    else
                        line("<null condition>");
                    m_indent--;
                    break;
                case ast::StmtKind::For:
                    line("For");
                    m_indent++;
                    if (static_cast<ast::ForStmt const&>(s).init)
                        print_stmt(*static_cast<ast::ForStmt const&>(s).init);
                    if (static_cast<ast::ForStmt const&>(s).cond)
                        print_expr(*static_cast<ast::ForStmt const&>(s).cond);
                    if (static_cast<ast::ForStmt const&>(s).update)
                        print_expr(*static_cast<ast::ForStmt const&>(s).update);
                    print_block(static_cast<ast::ForStmt const&>(s).body);
                    m_indent--;
                    break;
                case ast::StmtKind::ForIn: {
                    auto const& fi = static_cast<ast::ForInStmt const&>(s);
                    if (fi.by_reference)
                        line_fmt("ForIn {} by_ref=true", fi.item_name);
                    else
                        line_fmt("ForIn {}", fi.item_name);
                    m_indent++;
                    if (fi.iterable)
                        print_expr(*fi.iterable);
                    print_block(fi.body);
                    m_indent--;
                    break;
                }
                case ast::StmtKind::Defer:
                    line("Defer");
                    m_indent++;
                    if (static_cast<ast::DeferStmt const&>(s).body)
                        print_stmt(*static_cast<ast::DeferStmt const&>(s).body);
                    m_indent--;
                    break;
                case ast::StmtKind::StaticIf:
                    line_fmt("StaticIf taken={}", static_cast<ast::StaticIfStmt const&>(s).taken_branch);
                    m_indent++;
                    if (static_cast<ast::StaticIfStmt const&>(s).condition)
                        print_expr(*static_cast<ast::StaticIfStmt const&>(s).condition);
                    else
                        line("<null condition>");
                    if (!block_empty(static_cast<ast::StaticIfStmt const&>(s).then_block))
                        print_block(static_cast<ast::StaticIfStmt const&>(s).then_block);
                    if (static_cast<ast::StaticIfStmt const&>(s).else_branch)
                        print_stmt(*static_cast<ast::StaticIfStmt const&>(s).else_branch);
                    m_indent--;
                    break;
                case ast::StmtKind::StaticMatch:
                    line_fmt("StaticMatch taken={}", static_cast<ast::StaticMatchStmt const&>(s).taken_arm);
                    m_indent++;
                    if (static_cast<ast::StaticMatchStmt const&>(s).operand)
                        print_expr(*static_cast<ast::StaticMatchStmt const&>(s).operand);
                    else
                        line("<null operand>");
                    for (auto const& a : static_cast<ast::StaticMatchStmt const&>(s).arms)
                    {
                        line("Arm");
                        m_indent++;
                        if (a.pattern)
                            print_pattern(*a.pattern);
                        if (a.body)
                            print_expr(*a.body);
                        m_indent--;
                    }
                    m_indent--;
                    break;
                case ast::StmtKind::StaticFor: {
                    auto& sf = static_cast<ast::StaticForStmt const&>(s);
                    if (sf.is_range_for)
                        line_fmt("StaticFor name={} range_for=true", sf.item_name);
                    else
                        line_fmt("StaticFor name={}", sf.item_name);
                    break;
                }
                case ast::StmtKind::Ambiguous:
                    line_fmt("Ambiguous {}", static_cast<int>(static_cast<ast::AmbiguousStmt const&>(s).resolution));
                    break;
            }
        }

        void print_pattern(ast::Pattern const& p)
        {
            switch (p.kind)
            {
                case ast::PatternKind::Literal:
                    line("LiteralPattern");
                    break;
                case ast::PatternKind::Binding: {
                    auto const& bp = static_cast<ast::BindingPattern const&>(p);
                    if (bp.by_reference)
                        line_fmt("BindingPattern {} by_ref=true", bp.name);
                    else
                        line_fmt("BindingPattern {}", bp.name);
                    break;
                }
                case ast::PatternKind::Ref:
                    line("RefPattern");
                    break;
                case ast::PatternKind::Wildcard:
                    line("WildcardPattern");
                    break;
                case ast::PatternKind::EnumDestructure:
                    line_fmt("EnumDestructurePattern {}", static_cast<ast::EnumDestructurePattern const&>(p).variant_path.simple_name());
                    break;
                case ast::PatternKind::StructDestructure:
                    line_fmt("StructDestructurePattern {}", static_cast<ast::StructDestructurePattern const&>(p).type_path.simple_name());
                    break;
                case ast::PatternKind::Range:
                    line("RangePattern");
                    break;
                case ast::PatternKind::Or:
                    line("OrPattern");
                    break;
            }
        }

        static std::string path_str(ast::Path const& p)
        {
            std::string s;
            for (std::size_t i = 0; i < p.segments.size(); ++i)
            {
                if (i)
                    s += "::";
                s += p.segments[i].name;
            }
            return s;
        }

        void print_expr(ast::Expr const& e)
        {
            switch (e.kind)
            {
                case ast::ExprKind::IntLiteral:
                    line_fmt("IntLiteral {} {}", static_cast<ast::IntLiteralExpr const&>(e).value, expr_suffix(e));
                    break;
                case ast::ExprKind::FloatLiteral:
                    line_fmt("FloatLiteral {} {}", static_cast<ast::FloatLiteralExpr const&>(e).value, expr_suffix(e));
                    break;
                case ast::ExprKind::StringLiteral:
                    line_fmt("StringLiteral {} {}", static_cast<ast::StringLiteralExpr const&>(e).value, expr_suffix(e));
                    break;
                case ast::ExprKind::U16StringLiteral:
                    line_fmt("U16StringLiteral [{}cu] {}", static_cast<ast::U16StringLiteralExpr const&>(e).value.size(), expr_suffix(e));
                    break;
                case ast::ExprKind::CharLiteral:
                    line_fmt("CharLiteral {} {}", static_cast<ast::CharLiteralExpr const&>(e).codepoint, expr_suffix(e));
                    break;
                case ast::ExprKind::U16CharLiteral:
                    line_fmt("U16CharLiteral {} {}", static_cast<ast::U16CharLiteralExpr const&>(e).value, expr_suffix(e));
                    break;
                case ast::ExprKind::BoolLiteral:
                    line_fmt("BoolLiteral {} {}", static_cast<ast::BoolLiteralExpr const&>(e).value, expr_suffix(e));
                    break;
                case ast::ExprKind::NullLiteral:
                    line_fmt("NullLiteral {}", expr_suffix(e));
                    break;
                case ast::ExprKind::Ident:
                    line_fmt("Ident {} {}", static_cast<ast::IdentExpr const&>(e).name, expr_suffix(e));
                    break;
                case ast::ExprKind::PathExpr:
                    line_fmt("Path {} {}", path_str(static_cast<ast::PathExpr const&>(e).path), expr_suffix(e));
                    if (!static_cast<ast::PathExpr const&>(e).explicit_enum_args.empty())
                    {
                        m_indent++;
                        print_template_args(static_cast<ast::PathExpr const&>(e).explicit_enum_args);
                        m_indent--;
                    }
                    break;
                case ast::ExprKind::Unary:
                    line_fmt("Unary {} {}", lex::to_string(static_cast<ast::UnaryExpr const&>(e).op), expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::UnaryExpr const&>(e).operand)
                        print_expr(*static_cast<ast::UnaryExpr const&>(e).operand);
                    else
                        line("<null>");
                    m_indent--;
                    break;
                case ast::ExprKind::Postfix:
                    line_fmt("Postfix {} {}", lex::to_string(static_cast<ast::PostfixExpr const&>(e).op), expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::PostfixExpr const&>(e).operand)
                        print_expr(*static_cast<ast::PostfixExpr const&>(e).operand);
                    else
                        line("<null>");
                    m_indent--;
                    break;
                case ast::ExprKind::Binary:
                    line_fmt("Binary {} {}", lex::to_string(static_cast<ast::BinaryExpr const&>(e).op), expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::BinaryExpr const&>(e).lhs)
                        print_expr(*static_cast<ast::BinaryExpr const&>(e).lhs);
                    else
                        line("<null lhs>");
                    if (static_cast<ast::BinaryExpr const&>(e).rhs)
                        print_expr(*static_cast<ast::BinaryExpr const&>(e).rhs);
                    else
                        line("<null rhs>");
                    m_indent--;
                    break;
                case ast::ExprKind::Call:
                    line_fmt("Call {}", expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::CallExpr const&>(e).callee)
                        print_expr(*static_cast<ast::CallExpr const&>(e).callee);
                    else
                        line("<null callee>");
                    for (auto* a : static_cast<ast::CallExpr const&>(e).args)
                        if (a)
                            print_expr(*a);
                    m_indent--;
                    break;
                case ast::ExprKind::FieldAccess:
                    line_fmt("FieldAccess {} {}", static_cast<ast::FieldAccessExpr const&>(e).field, expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::FieldAccessExpr const&>(e).object)
                        print_expr(*static_cast<ast::FieldAccessExpr const&>(e).object);
                    else
                        line("<null>");
                    m_indent--;
                    break;
                case ast::ExprKind::Index:
                    line_fmt("Index {}", expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::IndexExpr const&>(e).object)
                        print_expr(*static_cast<ast::IndexExpr const&>(e).object);
                    else
                        line("<null>");
                    if (static_cast<ast::IndexExpr const&>(e).index)
                        print_expr(*static_cast<ast::IndexExpr const&>(e).index);
                    else
                        line("<null>");
                    m_indent--;
                    break;
                case ast::ExprKind::PackAccess:
                    line_fmt("PackAccess {}", expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::PackAccessExpr const&>(e).object)
                        print_expr(*static_cast<ast::PackAccessExpr const&>(e).object);
                    else
                        line("<null>");
                    if (static_cast<ast::PackAccessExpr const&>(e).index)
                        print_expr(*static_cast<ast::PackAccessExpr const&>(e).index);
                    else
                        line("<null>");
                    m_indent--;
                    break;
                case ast::ExprKind::Cast:
                    line_fmt("Cast {}", expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::CastExpr const&>(e).operand)
                        print_expr(*static_cast<ast::CastExpr const&>(e).operand);
                    else
                        line("<null>");
                    m_indent--;
                    break;
                case ast::ExprKind::Block: {
                    auto const& be = static_cast<ast::BlockExpr const&>(e);
                    if (be.sema.construction_kind != ast::ExprSema::ConstructionKind::None && be.body.stmts.empty())
                    {
                        line_fmt("StructLiteral {}", expr_suffix(e));
                        m_indent++;
                        if (be.body.tail)
                            print_expr(*be.body.tail);
                        m_indent--;
                    }
                    else
                    {
                        line_fmt("Block {}", expr_suffix(e));
                        m_indent++;
                        print_block(be.body);
                        m_indent--;
                    }
                    break;
                }
                case ast::ExprKind::If:
                    line_fmt("If {}", expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::IfExpr const&>(e).condition)
                        print_expr(*static_cast<ast::IfExpr const&>(e).condition);
                    else
                        line("<null condition>");
                    print_block(static_cast<ast::IfExpr const&>(e).then_block);
                    if (static_cast<ast::IfExpr const&>(e).else_branch)
                        print_expr(*static_cast<ast::IfExpr const&>(e).else_branch);
                    m_indent--;
                    break;
                case ast::ExprKind::Match:
                    line_fmt("Match {}", expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::MatchExpr const&>(e).operand)
                        print_expr(*static_cast<ast::MatchExpr const&>(e).operand);
                    else
                        line("<null operand>");
                    for (auto const& a : static_cast<ast::MatchExpr const&>(e).arms)
                    {
                        line("Arm");
                        m_indent++;
                        if (a.pattern)
                            print_pattern(*a.pattern);
                        if (a.body)
                            print_expr(*a.body);
                        m_indent--;
                    }
                    m_indent--;
                    break;
                case ast::ExprKind::StructLiteral:
                    line_fmt("StructLiteral {}", expr_suffix(e));
                    m_indent++;
                    for (auto const& f : static_cast<ast::StructLiteralExpr const&>(e).fields)
                        if (f.value)
                            print_expr(*f.value);
                    m_indent--;
                    break;
                case ast::ExprKind::Sizeof:
                    line_fmt("Sizeof {}", expr_suffix(e));
                    break;
                case ast::ExprKind::Alignof:
                    line_fmt("Alignof {}", expr_suffix(e));
                    break;
                case ast::ExprKind::Offsetof:
                    line_fmt("Offsetof {}", expr_suffix(e));
                    break;
                case ast::ExprKind::Compiles:
                    line_fmt("Compiles {}", expr_suffix(e));
                    print_block(static_cast<ast::CompilesExpr const&>(e).body);
                    break;
                case ast::ExprKind::Range:
                    line_fmt("Range {}", expr_suffix(e));
                    m_indent++;
                    if (auto const& r = static_cast<ast::RangeExpr const&>(e); r.start)
                        print_expr(*r.start);
                    if (auto const& r = static_cast<ast::RangeExpr const&>(e); r.end)
                        print_expr(*r.end);
                    m_indent--;
                    break;
                case ast::ExprKind::TypeAST:
                    line_fmt("TypeAST {}", expr_suffix(e));
                    break;
                case ast::ExprKind::TemplateInst:
                    line_fmt("TemplateInst {}", expr_suffix(e));
                    m_indent++;
                    if (static_cast<ast::TemplateInstExpr const&>(e).callee)
                        print_expr(*static_cast<ast::TemplateInstExpr const&>(e).callee);
                    else
                        line("<null>");
                    m_indent--;
                    break;
                case ast::ExprKind::SizeofPack:
                    line_fmt("SizeofPack name={} {}", static_cast<ast::SizeofPackExpr const&>(e).pack_name, expr_suffix(e));
                    break;
                case ast::ExprKind::PackExpansion:
                    line_fmt("PackExpansion {}", expr_suffix(e));
                    break;
            }
        }
    };

    namespace detail
    {
        using ConstructionKind = ast::ExprSema::ConstructionKind;

        struct CommittedSpecialization
        {
            ast::FuncDecl const* decl{};
            types::FuncPtrType const* type{};

            explicit operator bool() const noexcept { return decl != nullptr; }
        };

        struct ExprResult
        {
            types::TypePtr type{};
            comptime::Value const* constant{};
            ast::Decl const* resolved_decl{};
            CommittedSpecialization spec_commit{};
            ast::Decl const* ufcs_callee{};
            ConstructionKind construction_kind{ConstructionKind::None};
            ast::EnumVariant const* constructed_variant{};
            bool is_lvalue{};
            bool is_constant{};
            bool is_diverging{};
            bool is_type_instantiation{};
        };

        struct StmtResult
        {
            bool falls_through{true};
            bool diverges{false};
            bool foldable{true};
        };

    } // namespace detail

    [[nodiscard]] std::string format_type_str(types::TypePtr ty)
    {
        return format_dcc_type(ty);
    }

    enum class RequirementMode : std::uint8_t
    {
        Bool,
        Diagnostic,
    };

    struct RequirementFailure
    {
        sm::SourceRange requirement_range;
        std::string source_text;
        std::vector<diag::Diagnostic> inner_diagnostics;
    };

    struct ConceptFrame
    {
        std::string_view concept_name;
        std::pmr::vector<std::pair<std::string_view, std::pmr::string>> param_bindings;
        sm::SourceRange call_location;
        ConceptFrame const* parent{};
        bool is_required_by{};

        ConceptFrame(std::string_view n, std::pmr::vector<std::pair<std::string_view, std::pmr::string>> b, sm::SourceRange l, ConceptFrame const* p,
                     bool required_by, std::pmr::polymorphic_allocator<> a)
            : concept_name(n), param_bindings(std::move(b), a), call_location(l), parent(p), is_required_by(required_by)
        {
        }
    };

    class BodyAnalyzer
    {
    public:
        BodyAnalyzer(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, ast::AstContext& ast_ctx, types::TypeContext& type_ctx,
                     std::pmr::polymorphic_allocator<> alloc, SpecializationRegistry& spec_registry)
            : m_modules{modules}, m_diag{diag}, m_ast_ctx{ast_ctx}, m_types{type_ctx}, m_alloc{alloc}, m_spec_registry{spec_registry}
        {
        }

        void run()
        {
            for (auto const& m : m_modules)
            {
                if (!m->tu)
                    continue;

                analyze_module(*m);
                m->state = std::max(m->state, ModuleState::BodiesAnalyzed);
            }
        }

        void analyze_single_function(ModuleInfo& mod, ast::FuncDecl& fn) { analyze_function(mod, fn); }

    private:
        std::span<std::unique_ptr<ModuleInfo> const> m_modules;
        diag::DiagnosticEngine& m_diag;
        ast::AstContext& m_ast_ctx;
        types::TypeContext& m_types;
        std::pmr::polymorphic_allocator<> m_alloc;
        SpecializationRegistry& m_spec_registry;
        bool m_suppress_errors{};
        std::uint32_t m_suppressed_error_count{};
        bool m_allow_implicit_enum{true};
        bool m_disallow_nested_implicit_enum{};
        bool m_analyzing_call_callee{};
        bool m_allow_range_expr{};
        std::pmr::unordered_map<ast::Decl const*, types::TypePtr> m_inferred_local_types{};
        std::pmr::unordered_map<ast::Decl const*, std::uint32_t> m_decl_reads{};
        std::pmr::unordered_map<ast::Decl const*, std::uint32_t> m_decl_writes{};
        std::vector<sm::SourceRange> m_active_defers{};
        std::uint32_t m_defer_depth{};

        struct CanonicalGuard
        {
            ast::TypeSema* sema{};
            types::TypePtr saved{};
            ~CanonicalGuard()
            {
                if (sema)
                    set_canonical(*sema, saved);
            }

            CanonicalGuard() = default;
            CanonicalGuard(CanonicalGuard&& o) noexcept : sema(o.sema), saved(o.saved) { o.sema = nullptr; }
            CanonicalGuard& operator=(CanonicalGuard&&) = delete;
            CanonicalGuard(CanonicalGuard const&) = delete;
            CanonicalGuard& operator=(CanonicalGuard const&) = delete;
        };

        infer::TemplateBindings const* m_concept_bindings{};
        std::pmr::vector<CanonicalGuard>* m_concept_canon_guards{};

        std::unordered_set<ast::UsingDecl const*> m_concept_evaluation_stack{};
        std::unordered_set<ast::Decl const*> m_constraint_checking_set{};
        std::unordered_set<void const*> m_type_constraint_guard_set{};
        std::unordered_set<void const*> m_type_walk_guard_set{};
        ConceptFrame const* m_concept_frame_stack{};
        std::vector<std::string> m_concept_notes;
        std::vector<diag::Diagnostic>* m_captured_diagnostics{};
        ModuleInfo* m_specialization_defining_module{};

        class ErrorSuppressionGuard
        {
        public:
            explicit ErrorSuppressionGuard(bool& suppress, std::uint32_t& suppressed_count) noexcept
                : m_suppress(suppress), m_saved_suppress(suppress), m_suppressed_count(suppressed_count), m_saved_count(suppressed_count)
            {
                m_suppress = true;
            }
            ~ErrorSuppressionGuard() noexcept { m_suppress = m_saved_suppress; }

            [[nodiscard]] bool had_suppressed_errors() const noexcept { return m_suppressed_count != m_saved_count; }

            ErrorSuppressionGuard(ErrorSuppressionGuard const&) = delete;
            ErrorSuppressionGuard& operator=(ErrorSuppressionGuard const&) = delete;

        private:
            bool& m_suppress;
            bool m_saved_suppress{};
            std::uint32_t& m_suppressed_count;
            std::uint32_t m_saved_count{};
        };

        class NestedImplicitEnumGuard
        {
        public:
            explicit NestedImplicitEnumGuard(bool& disallow_nested_implicit_enum) noexcept
                : m_disallow_nested_implicit_enum(disallow_nested_implicit_enum), m_saved(disallow_nested_implicit_enum)
            {
                m_disallow_nested_implicit_enum = true;
            }

            ~NestedImplicitEnumGuard() noexcept { m_disallow_nested_implicit_enum = m_saved; }

        private:
            bool& m_disallow_nested_implicit_enum;
            bool m_saved{};
        };

        struct ConstEnv
        {
            ConstEnv const* parent{};
            si::InternedPmrHashMap<comptime::Value const*> values;

            ConstEnv(ConstEnv const* p, std::pmr::polymorphic_allocator<> a) : parent(p), values(a) {}
        };

        template <typename... A> void error(sm::SourceRange range, std::format_string<A...> fmt, A&&... args)
        {
            if (!m_suppress_errors)
            {
                m_diag.error(range, fmt, std::forward<A>(args)...);
                if (m_captured_diagnostics)
                {
                    auto idx = m_diag.diagnostic_count();
                    if (idx > 0)
                    {
                        auto last_since = m_diag.diagnostics_since(idx - 1);
                        if (!last_since.empty())
                            m_captured_diagnostics->push_back(last_since[0]);
                    }
                }
            }
            else
                ++m_suppressed_error_count;
        }

        template <typename... A> void warning(sm::SourceRange range, std::format_string<A...> fmt, A&&... args)
        {
            if (!m_suppress_errors)
                m_diag.warning(range, fmt, std::forward<A>(args)...);
        }

        [[nodiscard]] static bool has_error(types::TypePtr ty) noexcept { return !ty || ty->kind == types::TypeKind::Error; }

        void check_type_valid_for_value(sm::SourceRange range, types::TypePtr ty, std::string_view context)
        {
            if (!ty || has_error(ty))
                return;

            if (is_concrete_void_type(ty))
            {
                if (context.find("return") == std::string_view::npos)
                {
                    error(range, "void type is not allowed in {}", context);
                    return;
                }
            }

            if (types::is_fam_type(ty))
            {
                error(range, "flexible array member type `{}` is not allowed in {}", format_type_str(ty), context);
                return;
            }

            if (auto const* st = types::type_cast<types::StructType>(ty))
            {
                if (st->has_fam)
                    error(range, "struct with flexible array member is not allowed in {}", context);
            }

            if (auto const* at = types::type_cast<types::ArrayType>(ty))
            {
                if (types::is_fam_type(at->element) || types::type_has_fam_struct(at->element))
                    error(range, "array of flexible-array-member type is not allowed");
            }

            if (types::type_cast<types::RuntimeArrayType>(ty))
            {
                if (context.find("variable") == std::string_view::npos)
                    error(range, "runtime-sized array type is not allowed in {}", context);
            }
        }

        void track_decl_read(ast::Decl const* decl)
        {
            if (auto const* v = ast::node_cast<ast::VarDecl>(decl);
                v && (v->sema.storage == ast::StorageClass::Local || v->sema.storage == ast::StorageClass::Param))
                ++m_decl_reads[decl];
        }

        void track_decl_write(ast::Decl const* decl)
        {
            if (auto const* v = ast::node_cast<ast::VarDecl>(decl);
                v && (v->sema.storage == ast::StorageClass::Local || v->sema.storage == ast::StorageClass::Param))
                ++m_decl_writes[decl];
        }

        ConstEnv* make_const_env(ConstEnv const* parent)
        {
            auto* p = m_alloc.allocate_object<ConstEnv>();
            return std::construct_at(p, parent, m_alloc);
        }

        [[nodiscard]] comptime::Value const* lookup_constant(ConstEnv const* env, std::string_view name) const
        {
            for (auto const* e = env; e; e = e->parent)
            {
                auto it = e->values.find(name);
                if (it != e->values.end())
                    return it->second;
            }

            return nullptr;
        }

        void define_constant(ConstEnv& env, std::string_view name, comptime::Value const* value) { env.values.insert_or_assign(name, value); }

        std::pmr::vector<sm::SourceRange> snapshot_exit_defers() const
        {
            std::pmr::vector<sm::SourceRange> out{m_alloc};
            out.assign(m_active_defers.rbegin(), m_active_defers.rend());
            return out;
        }

        void register_inferred_local_type(ast::Decl const* decl, types::TypePtr type)
        {
            if (decl)
                m_inferred_local_types.insert_or_assign(decl, type);
        }

        [[nodiscard]] types::TypePtr inferred_local_type(ast::Decl const* decl) const
        {
            if (!decl)
                return nullptr;

            if (auto it = m_inferred_local_types.find(decl); it != m_inferred_local_types.end())
                return it->second;

            return nullptr;
        }

        static std::uint64_t align_up(std::uint64_t n, std::uint32_t align) noexcept
        {
            if (align <= 1)
                return n;
            auto rem = n % align;
            return rem ? (n + (align - rem)) : n;
        }

        void complete_tagged_enum(types::TypePtr ty) { ensure_tagged_enum_complete(m_types, m_alloc, ty); }

        [[nodiscard]] std::uint32_t allocate_frame_slot(std::uint32_t& next_off, types::TypePtr type)
        {
            if (has_error(type))
                return next_off;

            auto layout = layout_of(type);
            if (!layout)
                return next_off;

            auto const aligned = static_cast<std::uint32_t>(align_up(next_off, std::max<std::uint32_t>(1, layout->align)));
            next_off = layout->size > 0 ? static_cast<std::uint32_t>(aligned + layout->size) : aligned;
            return aligned;
        }

        struct Layout
        {
            std::uint64_t size{};
            std::uint32_t align{1};
        };

        [[nodiscard]] bool fits_int_type(std::int64_t value, types::IntType const& ty) const noexcept
        {
            auto const bits = ty.bits;
            if (ty.is_signed)
            {
                if (bits >= 64)
                    return true;
                auto const min = -(std::int64_t(1) << (bits - 1));
                auto const max = (std::int64_t(1) << (bits - 1)) - 1;
                return value >= min && value <= max;
            }

            if (value < 0)
                return false;
            if (bits >= 64)
                return true;
            auto const max = (std::uint64_t(1) << bits) - 1;
            return static_cast<std::uint64_t>(value) <= max;
        }

        [[nodiscard]] types::TypePtr default_int_type(std::int64_t value, types::TypePtr expected) const noexcept
        {
            if (auto const* it = types::type_cast<types::IntType>(expected); it && fits_int_type(value, *it))
                return expected;
            return m_types.int_t(32, true);
        }

        [[nodiscard]] static bool contains_template_param(types::TypePtr ty) noexcept
        {
            if (!ty)
                return false;

            switch (ty->kind)
            {
                case types::TypeKind::TemplateParam:
                    return true;
                case types::TypeKind::Pointer:
                    return contains_template_param(static_cast<types::PointerType const*>(ty)->pointee);
                case types::TypeKind::Array:
                    return contains_template_param(static_cast<types::ArrayType const*>(ty)->element);
                case types::TypeKind::RuntimeArray:
                    return contains_template_param(static_cast<types::RuntimeArrayType const*>(ty)->element);
                case types::TypeKind::Slice:
                    return contains_template_param(static_cast<types::SliceType const*>(ty)->element);
                case types::TypeKind::Fam:
                    return contains_template_param(static_cast<types::FamType const*>(ty)->element);
                case types::TypeKind::FuncPtr: {
                    auto const* fp = static_cast<types::FuncPtrType const*>(ty);
                    if (contains_template_param(fp->return_type))
                        return true;
                    for (auto const* p : fp->params)
                        if (contains_template_param(p))
                            return true;
                    return false;
                }
                case types::TypeKind::Struct:
                case types::TypeKind::Union:
                case types::TypeKind::Enum: {
                    auto const* u = static_cast<types::UserType const*>(ty);
                    for (auto const* a : u->template_args)
                        if (contains_template_param(a))
                            return true;
                    if (ty->kind == types::TypeKind::Enum)
                        return contains_template_param(static_cast<types::EnumType const*>(ty)->backing);
                    return false;
                }
                default:
                    return false;
            }
        }

        [[nodiscard]] static bool is_contextual_literal(ast::Expr const& expr) noexcept
        {
            switch (expr.kind)
            {
                case ast::ExprKind::IntLiteral:
                case ast::ExprKind::FloatLiteral:
                case ast::ExprKind::NullLiteral:
                    return true;
                case ast::ExprKind::StructLiteral:
                    return !static_cast<ast::StructLiteralExpr const&>(expr).type;
                default:
                    return false;
            }
        }

        using ConstructionKind = ast::ExprSema::ConstructionKind;

        [[nodiscard]] static bool variant_has_attr(ast::EnumVariant const& v, std::string_view name) noexcept
        {
            return std::any_of(v.attrs.begin(), v.attrs.end(), [&](auto const& a) { return a.name == name; });
        }

        [[nodiscard]] static ConstructionKind construction_kind_for_type(types::TypePtr ty) noexcept
        {
            if (!ty)
                return ConstructionKind::None;

            switch (ty->kind)
            {
                case types::TypeKind::Struct:
                case types::TypeKind::Union:
                    return ConstructionKind::Struct;
                case types::TypeKind::Array:
                    return ConstructionKind::Array;
                case types::TypeKind::Slice:
                    return ConstructionKind::Slice;
                case types::TypeKind::Enum:
                    return ConstructionKind::Enum;
                case types::TypeKind::Nominal:
                    return construction_kind_for_type(static_cast<types::NominalType const*>(ty)->underlying);
                default:
                    return ConstructionKind::None;
            }
        }

        [[nodiscard]] Scope* make_probe_scope(Scope const& scope)
        {
            auto* probe = make_scope(scope.kind(), scope.parent());
            for (auto const& entry : scope.bindings())
            {
                auto const& binding = entry.second;
                if (binding.has_type)
                    probe->define_type(binding.type_sym);

                if (binding.value_syms.empty())
                    continue;

                bool all_functions = true;
                for (auto const& sym : binding.value_syms)
                    all_functions &= sym.kind == SymbolKind::Function;

                if (all_functions)
                {
                    for (auto const& sym : binding.value_syms)
                        probe->add_function_overload(sym);
                }
                else
                {
                    for (auto const& sym : binding.value_syms)
                        probe->define_variable(sym);
                }
            }

            return probe;
        }

        enum class CallRank : std::uint8_t
        {
            ReceiverExact = 0,
            ReceiverAutoRef = 1,
            ReceiverAutoRefConst = 2,
            ReceiverAutoRefQualMismatch = 3,
            ReceiverAutoDeref = 4,
            ConcreteExact = 5,
            StringLiteralCharSliceConst = 6,
            StringLiteralCharSliceMutable = 7,
            StringLiteralU8SliceConst = 8,
            StringLiteralU8SliceMutable = 9,
            StringLiteralCharPointerConst = 10,
            StringLiteralCharPointerMutable = 11,
            StringLiteralU8PointerConst = 12,
            StringLiteralU8PointerMutable = 13,
            U16StringLiteralSliceConst = 14,
            U16StringLiteralSliceMutable = 15,
            U16StringLiteralPointerConst = 16,
            U16StringLiteralPointerMutable = 17,
            StructContextualExact = 18,
            ArrayContextualExact = 19,
            SliceContextualExact = 20,
            EnumContextualExact = 21,
            LiteralContextualExact = 22,
            TemplateExact = 23,
        };

        enum class UfcsReceiverMatch : std::uint8_t
        {
            None,
            Exact,
            AutoRef,
            AutoRefConst,
            AutoRefQualMismatch,
            AutoDeref,
        };

        struct RankedCandidate
        {
            Symbol const* sym{};
            std::vector<CallRank> ranks;
            types::FuncPtrType const* explicit_fp{};
            UfcsReceiverMatch receiver_match{UfcsReceiverMatch::None};
        };

        struct CandidateInfo
        {
            Symbol const* sym{};
            types::FuncPtrType const* explicit_fp{};
            std::string signature;
            sm::SourceRange def_range;
            std::string reason;
        };

        static void collect_candidate_rejection(std::vector<CandidateInfo>& out, Symbol const& sym, std::string reason,
                                                types::FuncPtrType const* explicit_fp = nullptr)
        {
            auto& info = out.emplace_back();
            info.sym = &sym;
            info.explicit_fp = explicit_fp;
            info.reason = std::move(reason);

            if (explicit_fp)
            {
                auto const& f = *static_cast<ast::FuncDecl const*>(sym.decl);
                info.def_range = f.range;
                std::string sig = std::string{f.name};
                sig += '(';
                for (std::size_t i = 0; i < explicit_fp->params.size(); ++i)
                {
                    if (i)
                        sig += ", ";
                    sig += format_type_str(explicit_fp->params[i]);
                }

                sig += ')';
                info.signature = std::move(sig);
            }
            else if (sym.decl && sym.decl->kind == ast::DeclKind::Func)
            {
                auto const& f = *static_cast<ast::FuncDecl const*>(sym.decl);
                info.def_range = f.range;
                std::string sig = std::string{f.name};
                sig += '(';
                for (std::size_t i = 0; i < f.params.size(); ++i)
                {
                    if (i)
                        sig += ", ";
                    if (f.params[i].type)
                        sig += format_type_str(get_canonical(f.params[i].type->sema));
                    else
                        sig += "<unknown>";
                }

                sig += ')';
                info.signature = std::move(sig);
            }
            else
                info.signature = "<unknown>";
        }

        void emit_overload_error(sm::SourceRange range, std::string primary_msg, std::vector<CandidateInfo> const& candidates)
        {
            if (m_suppress_errors)
            {
                ++m_suppressed_error_count;
                return;
            }

            auto diag_obj = diag::Diagnostic{diag::Severity::Error, std::move(primary_msg)}.primary(range);
            for (auto const& cand : candidates)
            {
                auto loc = format_source_location(cand.def_range);
                std::move(diag_obj).note(std::format("candidate `{}`\n  --> {}", cand.signature, loc));
                std::move(diag_obj).note(std::format("reason: {}", cand.reason));
            }

            m_diag.emit(std::move(diag_obj));
        }

        [[nodiscard]] std::string trace_template_deduction(ast::FuncDecl const* func, std::span<types::TypePtr const> params,
                                                           std::span<types::TypePtr const> actuals)
        {
            if (!func || func->template_params.empty())
                return {};

            infer::TemplateBindings b{m_types};
            std::string result;

            std::map<std::string_view, std::vector<std::pair<std::size_t, std::string>>> deductions;

            for (std::size_t i = 0; i < params.size() && i < actuals.size(); ++i)
            {
                auto* tpt = types::type_cast<types::TemplateParamType>(params[i]);
                if (!tpt)
                    continue;

                auto r = b.deduce(params[i], actuals[i]);
                std::string type_str;
                if (r)
                {
                    auto bound = b.lookup(tpt);
                    type_str = bound ? format_type_str(bound) : format_type_str(actuals[i]);
                }
                else
                {
                    auto existing = b.lookup(tpt);
                    if (existing)
                        type_str = format_type_str(actuals[i]);
                    else
                        type_str = std::string{r.detail};
                }

                deductions[tpt->name].push_back({i + 1, std::move(type_str)});
            }

            for (auto& [name, entries] : deductions)
            {
                if (entries.empty())
                    continue;

                for (auto& [arg_idx, ty] : entries)
                {
                    if (!result.empty())
                        result += "; ";
                    result += std::format("parameter `{}` deduced as `{}` from argument {}", name, ty, arg_idx);
                }

                if (entries.size() > 1)
                {
                    auto const& first = entries[0].second;
                    bool has_conflict = false;
                    for (std::size_t j = 1; j < entries.size(); ++j)
                    {
                        if (entries[j].second != first)
                        {
                            has_conflict = true;
                            break;
                        }
                    }

                    if (has_conflict)
                        result += " - conflicts";
                }
            }

            for (auto const& tp : func->template_params)
            {
                if (tp.value_type)
                    continue;

                bool found = false;
                for (auto& [name, _] : deductions)
                {
                    if (name == tp.name)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    if (!result.empty())
                        result += "; ";
                    result += std::format("parameter `{}` could not be deduced from call arguments", tp.name);
                }
            }

            return result;
        }

        enum class ExplicitInstFailure : std::uint8_t
        {
            None,
            NotTemplate,
            CountMismatch,
            ValueArg,
            Constraint,
        };

        struct ExplicitInstCandidate
        {
            Symbol const* sym{};
            types::FuncPtrType const* fp{};
            ast::FuncDecl const* template_fn{};
            infer::TemplateBindings bindings;

            ExplicitInstCandidate(Symbol const* s, types::FuncPtrType const* f, ast::FuncDecl const* tf, infer::TemplateBindings&& b)
                : sym(s), fp(f), template_fn(tf), bindings(std::move(b))
            {
            }
        };

        struct ExplicitInstScan
        {
            std::vector<ExplicitInstCandidate> viable;
            bool saw_function{};
            bool saw_non_template{};
            bool saw_template{};
            bool saw_count_mismatch{};
            bool saw_value_arg{};
            bool saw_constraint_failure{};
        };

        [[nodiscard]] types::TypePtr resolve_template_param_actual(std::span<ast::TemplateParam const> params, infer::TemplateBindings const& bindings,
                                                                   std::string_view name)
        {
            for (std::size_t i = 0; i < params.size(); ++i)
            {
                auto const& tp = params[i];
                if (tp.name != name)
                    continue;

                auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                if (!param_ty)
                    return nullptr;

                auto actual = bindings.substitute(param_ty);
                return actual && actual->kind != types::TypeKind::TemplateParam ? actual : nullptr;
            }

            return nullptr;
        }

        [[nodiscard]] types::TypePtr resolve_template_param_actual(ast::FuncDecl const& f, infer::TemplateBindings const& bindings, std::string_view name)
        {
            return resolve_template_param_actual(std::span<ast::TemplateParam const>{f.template_params.data(), f.template_params.size()}, bindings, name);
        }

        [[nodiscard]] static std::vector<types::TypePtr> expand_pack_param_type(types::TypePtr ty, infer::TemplateBindings const& bindings)
        {
            std::vector<types::TypePtr> result;

            auto expand = [&](types::TypePtr inner) -> std::vector<types::TypePtr> {
                if (auto const* tpp = types::type_cast<types::TemplateParamType>(inner))
                {
                    if (auto* pack = bindings.lookup_pack(tpp))
                    {
                        std::vector<types::TypePtr> out;
                        out.reserve(pack->size());
                        for (auto const& pt : *pack)
                            out.push_back(pt);

                        return out;
                    }
                }

                return {bindings.substitute(inner)};
            };

            if (auto const* pack_ty = types::type_cast<types::TypePackType>(ty))
                return expand(pack_ty->element);

            if (auto const* tp = types::type_cast<types::TemplateParamType>(ty))
                return expand(tp);

            return {bindings.substitute(ty)};
        }

        struct PackArityInfo
        {
            bool has_pack = false;
            std::size_t non_pack_count = 0;
            std::size_t min_required = 0;
        };

        [[nodiscard]] static PackArityInfo compute_pack_arity(ast::FuncDecl const* func, std::size_t param_count, bool nttps_already_resolved = false)
        {
            PackArityInfo info;
            info.non_pack_count = param_count;
            info.min_required = param_count;

            if (!func || nttps_already_resolved)
                return info;

            if (func->params.empty())
                return info;

            if (!is_func_param_sema_pack(func->params.back(), *func))
                return info;

            info.has_pack = true;
            info.non_pack_count = param_count - 1;

            info.min_required = info.non_pack_count;
            return info;
        }

        [[nodiscard]] types::TypePtr resolve_constraint_arg_type(ModuleInfo& mod, Scope const& scope, std::span<ast::TemplateParam const> params,
                                                                 infer::TemplateBindings const& bindings, ast::Expr const& arg)
        {
            if (auto const* id = ast::node_cast<ast::IdentExpr>(&arg))
                return resolve_template_param_actual(params, bindings, id->name);

            if (auto const* path = ast::node_cast<ast::PathExpr>(&arg); path && path->path.is_simple())
                return resolve_template_param_actual(params, bindings, path->path.simple_name());

            if (auto const* type_expr = ast::node_cast<ast::TypeASTExpr>(&arg))
            {
                auto* ty = type_expr->type_node ? get_canonical(type_expr->type_node->sema) : nullptr;
                if (!ty && type_expr->type_node)
                    ty = resolve_type_node(mod, scope, type_expr->type_node);
                return ty ? bindings.substitute(ty) : nullptr;
            }

            auto build_nominal = [&](ast::Decl const* d, std::vector<types::TypePtr> const& args) -> types::TypePtr {
                if (auto const* sd = ast::node_cast<ast::StructDecl>(d))
                    return m_types.nominal_t(types::TypeKind::Struct, sd, args);
                if (auto const* ud = ast::node_cast<ast::UnionDecl>(d))
                    return m_types.nominal_t(types::TypeKind::Union, ud, args);
                if (auto const* ed = ast::node_cast<ast::EnumDecl>(d))
                    return m_types.nominal_t(types::TypeKind::Enum, ed, args);
                return nullptr;
            };

            auto lookup_type_decl = [&](std::string_view n) -> ast::Decl const* {
                auto const* sym = scope.lookup_type(n);
                if (!sym && mod.own_scope)
                    sym = mod.own_scope->lookup_type(n);
                if (!sym)
                    return nullptr;
                auto const* d = sym->decl;
                if (auto const* use = ast::node_cast<ast::UsingDecl>(d))
                {
                    if (use->using_kind == ast::UsingKind::Alias && use->target_type && use->target_type->sema.canonical)
                        return nullptr;
                    return nullptr;
                }
                if (!ast::node_cast<ast::StructDecl>(d) && !ast::node_cast<ast::UnionDecl>(d) && !ast::node_cast<ast::EnumDecl>(d))
                    return nullptr;
                return d;
            };

            if (auto const* ti = ast::node_cast<ast::TemplateInstExpr>(&arg))
            {
                auto const* id = ast::node_cast<ast::IdentExpr>(ti->callee);
                if (!id)
                    return nullptr;
                auto const* d = lookup_type_decl(id->name);
                if (!d)
                    return nullptr;
                std::vector<types::TypePtr> resolved_args;
                for (auto const& ta : ti->template_args)
                {
                    types::TypePtr arg_type = nullptr;
                    if (ta.type)
                    {
                        if (auto const* nt = ast::node_cast<ast::NamedType>(ta.type); nt && nt->path.is_simple() && nt->template_args.empty())
                            arg_type = resolve_template_param_actual(params, bindings, nt->path.simple_name());
                        if (!arg_type)
                        {
                            arg_type = resolve_type_node(mod, scope, ta.type);
                            if (arg_type)
                                arg_type = bindings.substitute(arg_type);
                        }
                    }
                    if (!arg_type)
                        return nullptr;
                    resolved_args.push_back(arg_type);
                }
                return build_nominal(d, resolved_args);
            }

            if (auto const* call = ast::node_cast<ast::CallExpr>(&arg))
            {
                auto const* id = ast::node_cast<ast::IdentExpr>(call->callee);
                if (!id)
                    return nullptr;

                auto const* sym = scope.lookup_type(id->name);
                if (!sym && mod.own_scope)
                    sym = mod.own_scope->lookup_type(id->name);
                if (!sym)
                    return nullptr;
                auto const* d = sym->decl;
                if (auto const* use = ast::node_cast<ast::UsingDecl>(d))
                {
                    if (use->using_kind == ast::UsingKind::Alias && use->target_type && use->target_type->sema.canonical)
                        return get_canonical(use->target_type->sema);
                    return nullptr;
                }
                if (!ast::node_cast<ast::StructDecl>(d) && !ast::node_cast<ast::UnionDecl>(d) && !ast::node_cast<ast::EnumDecl>(d))
                    return nullptr;
                std::vector<types::TypePtr> resolved_args;
                for (auto const* arg_expr : call->args)
                {
                    auto arg_type = resolve_constraint_arg_type(mod, scope, params, bindings, *arg_expr);
                    if (!arg_type)
                        return nullptr;
                    resolved_args.push_back(arg_type);
                }
                return build_nominal(d, resolved_args);
            }

            return nullptr;
        }

        [[nodiscard]] types::TypePtr resolve_constraint_arg_type(ModuleInfo& mod, Scope const& scope, ast::FuncDecl const& f,
                                                                 infer::TemplateBindings const& bindings, ast::Expr const& arg)
        {
            return resolve_constraint_arg_type(mod, scope, std::span<ast::TemplateParam const>{f.template_params.data(), f.template_params.size()}, bindings,
                                               arg);
        }

        [[nodiscard]] ast::UsingDecl const* resolve_concept_decl(ModuleInfo& mod, Scope const& scope, ast::Expr const& callee,
                                                                 ModuleInfo const* defining_mod = nullptr)
        {
            auto try_lookup = [](Scope const* s, std::string_view name) -> ast::UsingDecl const* {
                if (!s)
                    return nullptr;

                auto const* sym = s->lookup_type(name);
                if (!sym)
                    return nullptr;

                auto const* use = ast::node_cast<ast::UsingDecl>(sym->decl);
                return use && use->using_kind == ast::UsingKind::Concept ? use : nullptr;
            };

            if (!defining_mod)
                defining_mod = m_specialization_defining_module;

            if (auto const* id = ast::node_cast<ast::IdentExpr>(&callee))
            {
                auto search_imports = [&](ModuleInfo const& m) -> ast::UsingDecl const* {
                    for (auto const& imp : m.imports)
                    {
                        if (imp.target)
                        {
                            if (imp.target->export_scope)
                                if (auto const* use = try_lookup(imp.target->export_scope, id->name))
                                    return use;

                            if (imp.target->own_scope)
                                if (auto const* use = try_lookup(imp.target->own_scope, id->name))
                                    return use;
                        }
                    }
                    return nullptr;
                };

                if (defining_mod && defining_mod != &mod)
                {
                    if (auto const* use = try_lookup(defining_mod->own_scope, id->name))
                        return use;
                    if (auto const* use = search_imports(*defining_mod))
                        return use;
                    if (auto const* use = try_lookup(&scope, id->name))
                        return use;
                }
                else
                {
                    if (auto const* use = try_lookup(&scope, id->name))
                        return use;
                    if (auto const* use = try_lookup(mod.own_scope, id->name))
                        return use;
                }

                if (auto const* use = search_imports(mod))
                    return use;

                return nullptr;
            }

            if (auto const* path = ast::node_cast<ast::PathExpr>(&callee))
            {
                auto resolve_path = [&](ast::Path const& p) -> ast::UsingDecl const* {
                    if (p.is_simple())
                    {
                        if (defining_mod && defining_mod != &mod)
                        {
                            if (auto const* use = try_lookup(defining_mod->own_scope, p.simple_name()))
                                return use;

                            for (auto const& imp : defining_mod->imports)
                            {
                                if (imp.target)
                                {
                                    if (imp.target->export_scope)
                                        if (auto const* use = try_lookup(imp.target->export_scope, p.simple_name()))
                                            return use;
                                    if (imp.target->own_scope)
                                        if (auto const* use = try_lookup(imp.target->own_scope, p.simple_name()))
                                            return use;
                                }
                            }
                            return try_lookup(&scope, p.simple_name());
                        }
                        return try_lookup(&scope, p.simple_name());
                    }
                    if (mod.own_scope)
                    {
                        if (defining_mod && defining_mod != &mod)
                        {
                            if (auto const* use = try_lookup(defining_mod->own_scope, p.simple_name()))
                                return use;

                            for (auto const& imp : defining_mod->imports)
                            {
                                if (imp.target)
                                {
                                    if (imp.target->export_scope)
                                        if (auto const* use = try_lookup(imp.target->export_scope, p.simple_name()))
                                            return use;
                                    if (imp.target->own_scope)
                                        if (auto const* use = try_lookup(imp.target->own_scope, p.simple_name()))
                                            return use;
                                }
                            }
                        }
                        return try_lookup(mod.own_scope, p.simple_name());
                    }
                    return nullptr;
                };

                return resolve_path(path->path);
            }

            return nullptr;
        }

        [[nodiscard]] std::string extract_source_text(sm::SourceRange range) const
        {
            auto const& sm = m_diag.source_manager();
            auto const* file = sm.get(range.begin.fileId);
            if (!file)
                return {};

            auto text = file->text();
            auto start = range.begin.offset;
            auto end = range.end.offset;
            if (start < text.size() && end <= text.size() && start <= end)
            {
                auto s = text.substr(start, end - start);
                if (s.size() > 120)
                    return std::string{s.substr(0, 117)} + "...";

                return std::string{s};
            }

            return {};
        }

        [[nodiscard]] std::string format_source_location(sm::SourceRange const& range) const
        {
            auto const& sm = m_diag.source_manager();
            auto const* file = sm.get(range.begin.fileId);
            if (!file)
                return "<unknown>";

            auto lc = file->line_col(range.begin.offset);
            if (!lc)
                return std::format("{}:?:?", file->path().string());

            return std::format("{}:{}:{}", file->path().string(), lc->line, lc->column);
        }

        void push_concept_frame(std::string_view concept_name, std::pmr::vector<std::pair<std::string_view, std::pmr::string>> param_bindings,
                                sm::SourceRange call_location, bool is_required_by)
        {
            auto* frame =
                m_alloc.new_object<ConceptFrame>(concept_name, std::move(param_bindings), call_location, m_concept_frame_stack, is_required_by, m_alloc);
            m_concept_frame_stack = frame;
        }

        void pop_concept_frame()
        {
            if (m_concept_frame_stack)
                m_concept_frame_stack = m_concept_frame_stack->parent;
        }

        struct ConceptFrameGuard
        {
            BodyAnalyzer& analyzer;
            explicit ConceptFrameGuard(BodyAnalyzer& a) noexcept : analyzer(a) {}
            ~ConceptFrameGuard() { analyzer.pop_concept_frame(); }
        };

        [[nodiscard]] std::pair<std::optional<bool>, std::vector<RequirementFailure>>
        check_requirements(ModuleInfo& mod, ast::FuncDecl const* fn, Scope& scope, ast::CompilesExpr const& compiles, infer::TemplateBindings const& bindings,
                           RequirementMode mode, std::span<ast::TemplateParam const> env_params = {})
        {
            std::vector<RequirementFailure> failures;

            auto* inner = make_scope(ScopeKind::Block, &scope);
            std::uint32_t tmp_off{};

            struct ConceptBindingsGuard
            {
                infer::TemplateBindings const*& slot;
                infer::TemplateBindings const* saved;
                ~ConceptBindingsGuard() { slot = saved; }
                ConceptBindingsGuard(infer::TemplateBindings const*& s, infer::TemplateBindings const* v) : slot(s), saved(s) { s = v; }
                ConceptBindingsGuard(ConceptBindingsGuard const&) = delete;
                ConceptBindingsGuard& operator=(ConceptBindingsGuard const&) = delete;
            } bindings_guard{m_concept_bindings, &bindings};

            for (std::size_t i = 0; i < env_params.size(); ++i)
            {
                auto const& tp = env_params[i];
                if (tp.value_type)
                    continue;

                auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                if (!param_ty)
                    continue;

                auto* scope_canon = param_ty;
                if (auto* concrete = bindings.lookup(static_cast<types::TemplateParamType const*>(param_ty)))
                    scope_canon = concrete;

                auto* dummy_type = m_ast_ctx.make<ast::PrimitiveType>(tp.range, lex::TokenKind::KwVoid);
                sema::set_canonical(dummy_type->sema, scope_canon);
                auto* v = m_ast_ctx.make<ast::VarDecl>(tp.range, tp.name, tp.range);
                v->type = dummy_type;
                v->sema.storage = ast::StorageClass::Local;
                Symbol sym{};
                sym.name = tp.name;
                sym.kind = SymbolKind::TypeAlias;
                sym.decl = v;
                sym.definition_range = tp.range;
                inner->define_type(sym);
            }

            std::pmr::vector<CanonicalGuard> canon_guards{m_alloc};
            canon_guards.reserve(compiles.params.size());

            struct CanonGuardsPtrGuard
            {
                std::pmr::vector<CanonicalGuard>*& slot;
                std::pmr::vector<CanonicalGuard>* saved;
                ~CanonGuardsPtrGuard() { slot = saved; }
                CanonGuardsPtrGuard(std::pmr::vector<CanonicalGuard>*& s, std::pmr::vector<CanonicalGuard>* v) : slot(s), saved(s) { s = v; }
                CanonGuardsPtrGuard(CanonGuardsPtrGuard const&) = delete;
                CanonGuardsPtrGuard& operator=(CanonGuardsPtrGuard const&) = delete;
            } canon_guards_ptr_guard{m_concept_canon_guards, &canon_guards};

            for (auto const& p : compiles.params)
            {
                auto type = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : nullptr;
                auto substituted = type ? bindings.substitute(type) : nullptr;
                auto* v = make_local_decl(p.name, p.range, p.type, ast::StorageClass::Local, allocate_frame_slot(tmp_off, substituted));
                if (substituted && p.type)
                {
                    auto& g = canon_guards.emplace_back();
                    g.sema = std::addressof(p.type->sema);
                    g.saved = get_canonical(p.type->sema);
                    set_canonical(p.type->sema, substituted);
                }

                define_local(*inner, v);
            }

            {
                std::unordered_set<ast::TypeSema*> guarded;
                auto do_guard = [&](ast::TypeExpr* t) {
                    if (!t || !t->sema.canonical)
                        return;
                    if (!guarded.insert(std::addressof(t->sema)).second)
                        return;
                    auto canon = get_canonical(t->sema);
                    auto sub = bindings.substitute(canon);
                    if (sub && sub != canon)
                    {
                        auto& g = canon_guards.emplace_back();
                        g.sema = std::addressof(t->sema);
                        g.saved = canon;
                        set_canonical(t->sema, sub);
                    }
                };

                std::function<void(ast::TypeExpr*)> visit_type;
                std::function<void(ast::Expr*)> visit_expr;
                std::function<void(ast::Stmt*)> visit_stmt;
                std::function<void(ast::Decl*)> visit_decl;
                std::function<void(ast::Block const&)> visit_block;

                visit_type = [&](ast::TypeExpr* t) {
                    if (!t)
                        return;
                    do_guard(t);
                    switch (t->kind)
                    {
                        case ast::TypeKind::Pointer:
                            visit_type(static_cast<ast::PointerType*>(t)->pointee);
                            break;
                        case ast::TypeKind::Array:
                            visit_type(static_cast<ast::ArrayType*>(t)->element);
                            break;
                        case ast::TypeKind::Slice:
                            visit_type(static_cast<ast::SliceType*>(t)->element);
                            break;
                        case ast::TypeKind::Fam:
                            visit_type(static_cast<ast::FamType*>(t)->element);
                            break;
                        case ast::TypeKind::FuncPtr: {
                            auto* fp = static_cast<ast::FuncPtrType*>(t);
                            visit_type(fp->return_type);
                            for (auto* p : fp->params)
                                visit_type(p);
                            break;
                        }
                        case ast::TypeKind::Qualified:
                            visit_type(static_cast<ast::QualifiedType*>(t)->inner);
                            break;
                        case ast::TypeKind::Named: {
                            auto* nt = static_cast<ast::NamedType*>(t);
                            for (auto& ta : nt->template_args)
                                visit_type(ta.type);
                            break;
                        }
                        case ast::TypeKind::PackIndex:
                            break;
                        case ast::TypeKind::Primitive:
                            break;
                    }
                };

                visit_expr = [&](ast::Expr* e) {
                    if (!e)
                        return;

                    switch (e->kind)
                    {
                        case ast::ExprKind::Cast: {
                            auto* cast = static_cast<ast::CastExpr*>(e);
                            visit_type(cast->target);
                            visit_expr(cast->operand);
                            return;
                        }
                        case ast::ExprKind::StructLiteral: {
                            auto* sl = static_cast<ast::StructLiteralExpr*>(e);
                            visit_type(sl->type);
                            for (auto& f : sl->fields)
                                visit_expr(f.value);
                            return;
                        }
                        case ast::ExprKind::Sizeof:
                            visit_type(static_cast<ast::SizeofExpr*>(e)->target);
                            return;
                        case ast::ExprKind::Alignof:
                            visit_type(static_cast<ast::AlignofExpr*>(e)->target);
                            return;
                        case ast::ExprKind::Offsetof:
                            visit_type(static_cast<ast::OffsetofExpr*>(e)->target);
                            return;
                        case ast::ExprKind::TypeAST:
                            visit_type(static_cast<ast::TypeASTExpr*>(e)->type_node);
                            return;
                        case ast::ExprKind::Unary:
                            visit_expr(static_cast<ast::UnaryExpr*>(e)->operand);
                            return;
                        case ast::ExprKind::Postfix:
                            visit_expr(static_cast<ast::PostfixExpr*>(e)->operand);
                            return;
                        case ast::ExprKind::Binary: {
                            auto* b = static_cast<ast::BinaryExpr*>(e);
                            visit_expr(b->lhs);
                            visit_expr(b->rhs);
                            return;
                        }
                        case ast::ExprKind::Call: {
                            auto* c = static_cast<ast::CallExpr*>(e);
                            visit_expr(c->callee);
                            for (auto* arg : c->args)
                                visit_expr(arg);
                            return;
                        }
                        case ast::ExprKind::FieldAccess:
                            visit_expr(static_cast<ast::FieldAccessExpr*>(e)->object);
                            return;
                        case ast::ExprKind::Index:
                            visit_expr(static_cast<ast::IndexExpr*>(e)->object);
                            visit_expr(static_cast<ast::IndexExpr*>(e)->index);
                            return;
                        case ast::ExprKind::PackAccess:
                            visit_expr(static_cast<ast::PackAccessExpr*>(e)->object);
                            visit_expr(static_cast<ast::PackAccessExpr*>(e)->index);
                            return;
                        case ast::ExprKind::Block: {
                            auto* blk = static_cast<ast::BlockExpr*>(e);
                            visit_block(blk->body);
                            return;
                        }
                        case ast::ExprKind::If: {
                            auto* ife = static_cast<ast::IfExpr*>(e);
                            visit_expr(ife->condition);
                            visit_block(ife->then_block);
                            visit_expr(ife->else_branch);
                            return;
                        }
                        case ast::ExprKind::Match: {
                            auto* m = static_cast<ast::MatchExpr*>(e);
                            visit_expr(m->operand);
                            for (auto& arm : m->arms)
                            {
                                visit_expr(arm.guard);
                                visit_expr(arm.body);
                            }
                            return;
                        }
                        case ast::ExprKind::TemplateInst: {
                            auto* ti = static_cast<ast::TemplateInstExpr*>(e);
                            visit_expr(ti->callee);
                            for (auto& ta : ti->template_args)
                            {
                                visit_type(ta.type);
                                visit_expr(ta.expr);
                            }
                            return;
                        }
                        case ast::ExprKind::Range: {
                            auto* r = static_cast<ast::RangeExpr*>(e);
                            visit_expr(r->start);
                            visit_expr(r->end);
                            return;
                        }
                        default:
                            return;
                    }
                };

                visit_stmt = [&](ast::Stmt* s) {
                    if (!s)
                        return;
                    switch (s->kind)
                    {
                        case ast::StmtKind::Expr:
                            visit_expr(static_cast<ast::ExprStmt*>(s)->expr);
                            break;
                        case ast::StmtKind::DeclStmt:
                            visit_decl(static_cast<ast::DeclStmt*>(s)->decl);
                            break;
                        case ast::StmtKind::Return:
                            visit_expr(static_cast<ast::ReturnStmt*>(s)->value);
                            break;
                        case ast::StmtKind::While:
                            visit_expr(static_cast<ast::WhileStmt*>(s)->condition);
                            visit_block(static_cast<ast::WhileStmt*>(s)->body);
                            break;
                        case ast::StmtKind::DoWhile:
                            visit_block(static_cast<ast::DoWhileStmt*>(s)->body);
                            visit_expr(static_cast<ast::DoWhileStmt*>(s)->condition);
                            break;
                        case ast::StmtKind::For: {
                            auto* f = static_cast<ast::ForStmt*>(s);
                            visit_stmt(f->init);
                            visit_expr(f->cond);
                            visit_expr(f->update);
                            visit_block(f->body);
                            break;
                        }
                        case ast::StmtKind::ForIn: {
                            auto* fi = static_cast<ast::ForInStmt*>(s);
                            visit_type(fi->item_type);
                            visit_expr(fi->iterable);
                            visit_block(fi->body);
                            break;
                        }
                        case ast::StmtKind::Defer:
                            visit_stmt(static_cast<ast::DeferStmt*>(s)->body);
                            break;
                        case ast::StmtKind::StaticIf: {
                            auto* si = static_cast<ast::StaticIfStmt*>(s);
                            visit_expr(si->condition);
                            visit_block(si->then_block);
                            visit_stmt(si->else_branch);
                            break;
                        }
                        case ast::StmtKind::StaticMatch: {
                            auto* sm = static_cast<ast::StaticMatchStmt*>(s);
                            visit_expr(sm->operand);
                            for (auto& arm : sm->arms)
                            {
                                visit_expr(arm.guard);
                                visit_expr(arm.body);
                            }
                            break;
                        }
                        case ast::StmtKind::StaticFor: {
                            auto* sf = static_cast<ast::StaticForStmt*>(s);
                            visit_expr(sf->pack_expr);
                            visit_block(sf->body);
                            break;
                        }
                        case ast::StmtKind::Ambiguous: {
                            auto* amb = static_cast<ast::AmbiguousStmt*>(s);
                            visit_decl(amb->as_decl);
                            visit_expr(amb->as_expr);
                            break;
                        }
                        case ast::StmtKind::Break:
                        case ast::StmtKind::Continue:
                            break;
                    }
                };

                visit_decl = [&](ast::Decl* d) {
                    if (!d)
                        return;
                    switch (d->kind)
                    {
                        case ast::DeclKind::Var: {
                            auto* v = static_cast<ast::VarDecl*>(d);
                            visit_type(v->type);
                            visit_expr(v->init);
                            break;
                        }
                        case ast::DeclKind::Func: {
                            auto* f = static_cast<ast::FuncDecl*>(d);
                            visit_type(f->return_type);
                            for (auto& p : f->params)
                                visit_type(p.type);

                            break;
                        }
                        case ast::DeclKind::Using: {
                            auto* u = static_cast<ast::UsingDecl*>(d);
                            visit_type(u->target_type);
                            break;
                        }
                        case ast::DeclKind::Struct: {
                            auto* sd = static_cast<ast::StructDecl*>(d);
                            for (auto& f : sd->fields)
                                visit_type(f.type);
                            break;
                        }
                        case ast::DeclKind::Union: {
                            auto* ud = static_cast<ast::UnionDecl*>(d);
                            for (auto& f : ud->fields)
                                visit_type(f.type);
                            break;
                        }
                        case ast::DeclKind::Enum: {
                            auto* ed = static_cast<ast::EnumDecl*>(d);
                            visit_type(ed->backing_type);
                            for (auto& v : ed->variants)
                                for (auto& p : v.payload)
                                    visit_type(p);
                            break;
                        }
                        case ast::DeclKind::StaticIfGroup: {
                            auto* sig = static_cast<ast::StaticIfGroup*>(d);
                            visit_expr(sig->condition);
                            for (auto* td : sig->then_decls)
                                visit_decl(td);
                            if (sig->else_group)
                                visit_decl(sig->else_group);
                            break;
                        }
                        case ast::DeclKind::Module:
                        case ast::DeclKind::Import:
                            break;
                    }
                };

                visit_block = [&](ast::Block const& blk) {
                    for (auto* stmt : blk.stmts)
                        visit_stmt(stmt);
                    visit_expr(blk.tail);
                };

                visit_block(compiles.body);
            }

            if (mode == RequirementMode::Bool)
            {
                ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
                std::ignore = analyze_block(mod, const_cast<ast::FuncDecl*>(fn), *inner, const_cast<ast::Block&>(compiles.body), 0, tmp_off, nullptr, nullptr);
                return {!suppress.had_suppressed_errors(), {}};
            }

            auto saved_silent = m_diag.silent();
            m_diag.set_silent(true);

            auto const& block = compiles.body;
            for (auto* stmt : block.stmts)
            {
                auto* probe_scope = make_probe_scope(*inner);
                std::uint32_t probe_off = tmp_off;
                auto start_idx = m_diag.diagnostic_count();

                std::ignore = analyze_stmt(mod, const_cast<ast::FuncDecl*>(fn), *probe_scope, *stmt, 0, probe_off, nullptr);

                auto new_diags = m_diag.diagnostics_since(start_idx);
                if (!new_diags.empty())
                {
                    RequirementFailure f;
                    f.requirement_range = stmt->range;
                    f.source_text = extract_source_text(stmt->range);
                    f.inner_diagnostics.assign(new_diags.begin(), new_diags.end());
                    failures.push_back(std::move(f));
                }
            }

            if (block.tail)
            {
                auto* probe_scope = make_probe_scope(*inner);
                std::uint32_t probe_off = tmp_off;
                auto start_idx = m_diag.diagnostic_count();

                std::ignore = analyze_expr(mod, const_cast<ast::FuncDecl*>(fn), *probe_scope, *block.tail, 0, probe_off, nullptr, nullptr);

                auto new_diags = m_diag.diagnostics_since(start_idx);
                if (!new_diags.empty())
                {
                    RequirementFailure f;
                    f.requirement_range = block.tail->range;
                    f.source_text = extract_source_text(block.tail->range);
                    f.inner_diagnostics.assign(new_diags.begin(), new_diags.end());
                    failures.push_back(std::move(f));
                }
            }

            m_diag.set_silent(saved_silent);

            bool const satisfied = failures.empty();
            return {satisfied ? std::optional<bool>(true) : std::optional<bool>(false), std::move(failures)};
        }

        void append_concept_frame_note(ConceptFrame const& frame)
        {
            auto loc = format_source_location(frame.call_location);
            if (frame.is_required_by)
            {
                std::string bindings_str;
                for (std::size_t i = 0; i < frame.param_bindings.size(); ++i)
                {
                    if (i)
                        bindings_str += ", ";
                    bindings_str += std::format("{} = {}", frame.param_bindings[i].first, std::string_view{frame.param_bindings[i].second});
                }
                if (bindings_str.empty())
                    m_concept_notes.push_back(std::format("required by concept `{}`\n  --> {}", frame.concept_name, loc));
                else
                    m_concept_notes.push_back(std::format("required by concept `{}` with {}\n  --> {}", frame.concept_name, bindings_str, loc));
            }
            else
                m_concept_notes.push_back(std::format("in definition of concept `{}`\n  --> {}", frame.concept_name, loc));
        }

        void emit_constraint_error(sm::SourceRange range, std::string message)
        {
            if (!m_suppress_errors)
            {
                auto diag_obj = diag::Diagnostic{diag::Severity::Error, std::move(message)}.primary(range);
                for (auto& n : m_concept_notes)
                    std::move(diag_obj).note(std::move(n));
                m_concept_notes.clear();
                m_diag.emit(std::move(diag_obj));
            }
            else
            {
                ++m_suppressed_error_count;
                m_concept_notes.clear();
            }
        }

        void diagnose_explicit_constraint_failure(ModuleInfo& mod, Scope& scope, ast::FuncDecl const& f, std::span<ast::TemplateArg const> template_args)
        {
            if (f.template_params.empty() || template_args.size() != f.template_params.size())
                return;

            infer::TemplateBindings bindings{m_types};
            bool ok = true;
            for (std::size_t i = 0; i < template_args.size(); ++i)
            {
                auto const& tp = f.template_params[i];
                auto const& arg = template_args[i];

                if (tp.value_type)
                    continue;

                if (arg.expr || !arg.type)
                {
                    ok = false;
                    break;
                }
                auto param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                if (!param_ty)
                {
                    ok = false;
                    break;
                }
                auto actual = resolve_type_node(mod, scope, arg.type);
                if (!actual)
                {
                    ok = false;
                    break;
                }
                if (auto r = bindings.deduce(param_ty, actual); !r)
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
                std::ignore = check_template_constraint(mod, scope, f, bindings, true, find_defining_module(f));
        }

        void diagnose_implicit_constraint_failure(ModuleInfo& mod, Scope& scope, std::span<Symbol const> syms, std::span<ast::Expr* const> arg_exprs,
                                                  std::uint32_t next_off, int loop_depth, ConstEnv const* const_env, types::TypePtr expected_type)
        {
            for (auto const& sym : syms)
            {
                if (!sym.decl || sym.decl->kind != ast::DeclKind::Func)
                    continue;

                auto const& f = *static_cast<ast::FuncDecl const*>(sym.decl);
                if (!f.constraint)
                    continue;

                std::vector<types::TypePtr> params;
                params.reserve(f.params.size());
                for (auto const& p : f.params)
                    params.push_back(p.type ? get_canonical(p.type->sema) : m_types.m_errort());

                if (params.size() != arg_exprs.size())
                    continue;

                auto* probe_scope = make_probe_scope(scope);
                std::uint32_t probe_off = next_off;

                infer::TemplateBindings b{m_types};

                if (expected_type && !f.template_params.empty() && f.return_type)
                {
                    auto return_ty = get_canonical(f.return_type->sema);
                    if (return_ty && contains_template_param(return_ty) && !contains_template_param(expected_type))
                        std::ignore = b.deduce(return_ty, expected_type);
                }

                std::vector<types::TypePtr> actuals;
                actuals.reserve(arg_exprs.size());
                bool ok = true;
                for (std::size_t i = 0; i < arg_exprs.size(); ++i)
                {
                    auto param_ty = b.substitute(params[i]);
                    auto r = analyze_expr(mod, nullptr, *probe_scope, *arg_exprs[i], loop_depth, probe_off, param_ty, const_env);
                    if (has_error(r.type))
                    {
                        ok = false;
                        break;
                    }
                    actuals.push_back(r.type);
                }

                if (!ok)
                    continue;

                if (!b.deduce_function(params, actuals))
                    continue;

                std::ignore = check_template_constraint(mod, scope, f, b, true);

                break;
            }
        }

        [[nodiscard]] std::optional<bool> evaluate_concept_expr(ModuleInfo& mod, Scope& scope, ast::FuncDecl const* f,
                                                                std::span<ast::TemplateParam const> env_params, infer::TemplateBindings const& bindings,
                                                                ast::Expr const& expr, RequirementMode mode = RequirementMode::Bool)
        {
            if (ast::node_cast<ast::IdentExpr>(&expr))
            {
                if (auto const* concept_decl = resolve_concept_decl(mod, scope, expr))
                    if (concept_decl->template_params.empty())
                        return evaluate_concept_decl(mod, scope, f, *concept_decl, bindings, mode);

                return std::nullopt;
            }

            if (ast::node_cast<ast::PathExpr>(&expr))
            {
                if (auto const* concept_decl = resolve_concept_decl(mod, scope, expr))
                    if (concept_decl->template_params.empty())
                        return evaluate_concept_decl(mod, scope, f, *concept_decl, bindings, mode);

                return std::nullopt;
            }

            if (auto const* c = ast::node_cast<ast::CompilesExpr>(&expr))
            {
                if (mode == RequirementMode::Diagnostic)
                {
                    auto [result, failures] = check_requirements(mod, f, scope, *c, bindings, RequirementMode::Diagnostic, env_params);
                    for (auto const& rf : failures)
                    {
                        auto req_loc = format_source_location(rf.requirement_range);
                        std::string req_text = rf.source_text.empty() ? "<source>" : rf.source_text;

                        while (!req_text.empty() && (req_text.back() == ';' || req_text.back() == ' ' || req_text.back() == '\t'))
                            req_text.pop_back();

                        bool first_reason = true;
                        for (auto const& inner : rf.inner_diagnostics)
                        {
                            for (auto const& lab : inner.labels())
                            {
                                auto lab_loc = format_source_location(lab.range);
                                std::string msg = inner.message();
                                if (!lab.message.empty())
                                    msg += std::format(": {}", lab.message);

                                if (first_reason)
                                {
                                    first_reason = false;
                                    m_concept_notes.push_back(std::format("requirement failed: {}: {}\n  --> {}", req_text, msg, req_loc));
                                }
                                else
                                    m_concept_notes.push_back(std::format("  reason: {}\n  --> {}", msg, lab_loc));
                            }
                            for (auto const& n : inner.notes())
                                m_concept_notes.push_back(std::string{n});
                        }

                        if (first_reason)
                            m_concept_notes.push_back(std::format("requirement failed: {}\n  --> {}", req_text, req_loc));
                    }
                    return result;
                }
                return evaluate_compiles_expr(mod, scope, f, bindings, *c, env_params);
            }

            if (auto const* u = ast::node_cast<ast::UnaryExpr>(&expr))
            {
                if (u->op != lex::TokenKind::Bang || !u->operand)
                    return std::nullopt;

                if (mode == RequirementMode::Diagnostic)
                {
                    auto saved_notes = m_concept_notes.size();
                    auto operand = evaluate_concept_expr(mod, scope, f, env_params, bindings, *u->operand, mode);
                    if (!operand.has_value())
                        return std::nullopt;

                    if (*operand)
                    {
                        m_concept_notes.resize(saved_notes);
                        auto neg_source = extract_source_text(u->range);
                        auto operand_source = extract_source_text(u->operand->range);
                        m_concept_notes.push_back(std::format("constraint `{}` failed because `{}` was satisfied", neg_source, operand_source));
                        return false;
                    }

                    m_concept_notes.resize(saved_notes);
                    return true;
                }

                auto operand = evaluate_concept_expr(mod, scope, f, env_params, bindings, *u->operand, mode);
                if (!operand.has_value())
                    return std::nullopt;

                return !*operand;
            }

            if (auto const* b = ast::node_cast<ast::BinaryExpr>(&expr))
            {
                auto is_and = b->op == lex::TokenKind::Amp || b->op == lex::TokenKind::AmpAmp;
                auto is_or = b->op == lex::TokenKind::Pipe || b->op == lex::TokenKind::PipePipe;
                if ((!is_and && !is_or) || !b->lhs || !b->rhs)
                    return std::nullopt;

                if (mode == RequirementMode::Diagnostic)
                {
                    if (is_and)
                    {
                        auto lhs = evaluate_concept_expr(mod, scope, f, env_params, bindings, *b->lhs, mode);
                        if (lhs.has_value() && !*lhs)
                            return false;

                        if (!lhs.has_value())
                            return std::nullopt;

                        auto rhs = evaluate_concept_expr(mod, scope, f, env_params, bindings, *b->rhs, mode);
                        if (!rhs.has_value())
                            return std::nullopt;

                        return *rhs;
                    }
                    else
                    {
                        auto saved_notes = m_concept_notes.size();
                        auto lhs = evaluate_concept_expr(mod, scope, f, env_params, bindings, *b->lhs, mode);
                        if (lhs.has_value() && *lhs)
                        {
                            m_concept_notes.resize(saved_notes);
                            return true;
                        }

                        if (!lhs.has_value())
                            return std::nullopt;

                        auto saved_lhs = m_concept_notes.size();
                        auto rhs = evaluate_concept_expr(mod, scope, f, env_params, bindings, *b->rhs, mode);
                        if (rhs.has_value() && *rhs)
                        {
                            m_concept_notes.resize(saved_notes);
                            return true;
                        }

                        if (!rhs.has_value())
                            return std::nullopt;

                        m_concept_notes.insert(m_concept_notes.begin() + static_cast<std::ptrdiff_t>(saved_lhs), "all alternatives of disjunction failed:");
                        return false;
                    }
                }

                auto lhs = evaluate_concept_expr(mod, scope, f, env_params, bindings, *b->lhs);
                if (is_and)
                {
                    if (lhs.has_value() && !*lhs)
                        return false;
                }
                else if (lhs.has_value() && *lhs)
                    return true;

                auto rhs = evaluate_concept_expr(mod, scope, f, env_params, bindings, *b->rhs);
                if (!lhs.has_value() || !rhs.has_value())
                    return std::nullopt;

                return is_and ? (*lhs && *rhs) : (*lhs || *rhs);
            }

            if (auto const* call = ast::node_cast<ast::CallExpr>(&expr))
            {
                if (!call->callee)
                    return std::nullopt;
                auto const* concept_decl = resolve_concept_decl(mod, scope, *call->callee);
                if (!concept_decl || !concept_decl->target_expr || call->args.size() != concept_decl->template_params.size())
                    return std::nullopt;

                infer::TemplateBindings concept_bindings{m_types};
                for (std::size_t i = 0; i < call->args.size(); ++i)
                {
                    auto actual = resolve_constraint_arg_type(mod, scope, env_params, bindings, *call->args[i]);
                    if (!actual)
                        return std::nullopt;

                    auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(concept_decl->template_params[i])),
                                                              concept_decl->template_params[i].name, static_cast<std::uint32_t>(i));
                    if (!param_ty || !concept_bindings.deduce(param_ty, actual))
                        return std::nullopt;
                }

                return evaluate_concept_decl(mod, scope, f, *concept_decl, concept_bindings, mode);
            }

            return std::nullopt;
        }

        [[nodiscard]] std::optional<bool> evaluate_compiles_expr(ModuleInfo& mod, Scope& scope, ast::FuncDecl const* f, infer::TemplateBindings const& bindings,
                                                                 ast::CompilesExpr const& compiles, std::span<ast::TemplateParam const> env_params = {})
        {
            auto [result, failures] = check_requirements(mod, f, scope, compiles, bindings, RequirementMode::Bool, env_params);
            std::ignore = failures;
            return result;
        }

        [[nodiscard]] std::optional<bool> evaluate_concept_decl(ModuleInfo& mod, Scope& scope, ast::FuncDecl const* f, ast::UsingDecl const& concept_decl,
                                                                infer::TemplateBindings const& bindings, RequirementMode mode = RequirementMode::Bool)
        {
            if (!concept_decl.target_expr)
                return std::nullopt;

            if (!m_concept_evaluation_stack.insert(&concept_decl).second)
                return std::nullopt;

            struct StackGuard
            {
                std::unordered_set<ast::UsingDecl const*>& stack;
                ast::UsingDecl const* value;
                ~StackGuard() { stack.erase(value); }
            } guard{m_concept_evaluation_stack, &concept_decl};

            auto* concept_defining_mod = find_defining_module(concept_decl);
            auto* saved_concept_mod = m_specialization_defining_module;
            if (concept_defining_mod && concept_defining_mod != m_specialization_defining_module)
                m_specialization_defining_module = concept_defining_mod;

            struct ConceptDefiningModGuard
            {
                ModuleInfo*& slot;
                ModuleInfo* saved;
                ~ConceptDefiningModGuard() { slot = saved; }
            } cdmg{m_specialization_defining_module, saved_concept_mod};
            std::ignore = cdmg;

            if (mode == RequirementMode::Diagnostic)
            {
                auto concept_name = concept_decl.alias_path.is_empty() ? std::string_view{} : concept_decl.alias_path.segments.back().name;
                std::pmr::vector<std::pair<std::string_view, std::pmr::string>> param_bindings{m_alloc};
                for (std::size_t i = 0; i < concept_decl.template_params.size(); ++i)
                {
                    auto const& tp = concept_decl.template_params[i];
                    auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                    if (param_ty)
                    {
                        auto concrete = bindings.lookup(static_cast<types::TemplateParamType const*>(param_ty));
                        auto type_str = concrete ? format_type_str(concrete) : format_type_str(param_ty);
                        param_bindings.emplace_back(tp.name, std::pmr::string{type_str, m_alloc});
                    }
                }
                push_concept_frame(concept_name, std::move(param_bindings), concept_decl.range, false);
                append_concept_frame_note(*m_concept_frame_stack);
                ConceptFrameGuard frame_guard{*this};

                auto params = std::span<ast::TemplateParam const>{concept_decl.template_params.data(), concept_decl.template_params.size()};
                return evaluate_concept_expr(mod, scope, f, params, bindings, *concept_decl.target_expr, mode);
            }

            auto params = std::span<ast::TemplateParam const>{concept_decl.template_params.data(), concept_decl.template_params.size()};
            return evaluate_concept_expr(mod, scope, f, params, bindings, *concept_decl.target_expr);
        }

        [[nodiscard]] bool check_template_constraint(ModuleInfo& mod, Scope& scope, ast::FuncDecl const& f, infer::TemplateBindings const& bindings,
                                                     bool diagnostic = false, ModuleInfo const* defining_mod = nullptr)
        {
            if (!f.constraint)
                return true;

            auto const* call = ast::node_cast<ast::CallExpr>(f.constraint);
            if (!call)
                return true;

            auto const* concept_decl = resolve_concept_decl(mod, scope, *call->callee, defining_mod);
            if (!concept_decl)
                return false;

            auto* saved_defining_mod = m_specialization_defining_module;
            if (defining_mod && defining_mod != m_specialization_defining_module)
                m_specialization_defining_module = const_cast<ModuleInfo*>(defining_mod);

            struct DefiningModGuard
            {
                ModuleInfo*& slot;
                ModuleInfo* saved;
                ~DefiningModGuard() { slot = saved; }
            } dm_guard{m_specialization_defining_module, saved_defining_mod};
            std::ignore = dm_guard;

            {
                [[maybe_unused]] ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
                auto evaluated = evaluate_concept_expr(mod, scope, &f, std::span<ast::TemplateParam const>{f.template_params.data(), f.template_params.size()},
                                                       bindings, *f.constraint);
                if (evaluated.value_or(false) && !suppress.had_suppressed_errors())
                    return true;
            }

            if (diagnostic)
            {
                std::pmr::vector<std::pair<std::string_view, std::pmr::string>> param_bindings{m_alloc};
                for (std::size_t i = 0; i < f.template_params.size(); ++i)
                {
                    auto const& tp = f.template_params[i];
                    auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                    if (param_ty)
                    {
                        auto concrete = bindings.lookup(static_cast<types::TemplateParamType const*>(param_ty));
                        auto type_str = concrete ? format_type_str(concrete) : format_type_str(param_ty);
                        param_bindings.emplace_back(tp.name, std::pmr::string{type_str, m_alloc});
                    }
                }
                auto concept_name = concept_decl->alias_path.is_empty() ? std::string_view{} : concept_decl->alias_path.segments.back().name;
                push_concept_frame(concept_name, std::move(param_bindings), f.constraint->range, true);
                append_concept_frame_note(*m_concept_frame_stack);
                ConceptFrameGuard frame_guard{*this};

                std::ignore = evaluate_concept_expr(mod, scope, &f, std::span<ast::TemplateParam const>{f.template_params.data(), f.template_params.size()},
                                                    bindings, *f.constraint, RequirementMode::Diagnostic);
            }

            return false;
        }

        [[nodiscard]] bool check_template_constraint(ModuleInfo& mod, Scope& scope, std::span<ast::TemplateParam const> template_params,
                                                     ast::Expr const* constraint, infer::TemplateBindings const& bindings, bool diagnostic = false,
                                                     ModuleInfo const* defining_mod = nullptr)
        {
            if (!constraint)
                return true;

            auto const* call = ast::node_cast<ast::CallExpr>(constraint);
            if (!call)
                return true;

            auto const* concept_decl = resolve_concept_decl(mod, scope, *call->callee, defining_mod);
            if (!concept_decl)
                return false;

            auto* saved_defining_mod = m_specialization_defining_module;
            if (defining_mod && defining_mod != m_specialization_defining_module)
                m_specialization_defining_module = const_cast<ModuleInfo*>(defining_mod);

            struct DefiningModGuard
            {
                ModuleInfo*& slot;
                ModuleInfo* saved;
                ~DefiningModGuard() { slot = saved; }
            } dm_guard{m_specialization_defining_module, saved_defining_mod};
            std::ignore = dm_guard;

            {
                [[maybe_unused]] ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
                auto evaluated = evaluate_concept_expr(mod, scope, nullptr, template_params, bindings, *constraint);
                if (evaluated.value_or(false) && !suppress.had_suppressed_errors())
                    return true;
            }

            if (diagnostic)
            {
                std::pmr::vector<std::pair<std::string_view, std::pmr::string>> param_bindings{m_alloc};
                for (std::size_t i = 0; i < template_params.size(); ++i)
                {
                    auto const& tp = template_params[i];
                    auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                    if (param_ty)
                    {
                        auto concrete = bindings.lookup(static_cast<types::TemplateParamType const*>(param_ty));
                        auto type_str = concrete ? format_type_str(concrete) : format_type_str(param_ty);
                        param_bindings.emplace_back(tp.name, std::pmr::string{type_str, m_alloc});
                    }
                }
                auto concept_name = concept_decl->alias_path.is_empty() ? std::string_view{} : concept_decl->alias_path.segments.back().name;
                push_concept_frame(concept_name, std::move(param_bindings), constraint->range, true);
                append_concept_frame_note(*m_concept_frame_stack);
                ConceptFrameGuard frame_guard{*this};

                std::ignore = evaluate_concept_expr(mod, scope, nullptr, template_params, bindings, *constraint, RequirementMode::Diagnostic);
            }

            return false;
        }

        [[nodiscard]] bool check_type_constraint(ModuleInfo& mod, Scope const& scope, ast::Decl const* decl, std::span<types::TypePtr const> type_args,
                                                 sm::SourceRange use_range)
        {
            ast::Expr const* constraint = nullptr;
            std::span<ast::TemplateParam const> template_params;
            std::string_view decl_name;

            if (auto const* sd = ast::node_cast<ast::StructDecl>(decl))
            {
                constraint = sd->constraint;
                template_params = {sd->template_params.data(), sd->template_params.size()};
                decl_name = sd->name;
            }
            else if (auto const* ed = ast::node_cast<ast::EnumDecl>(decl))
            {
                constraint = ed->constraint;
                template_params = {ed->template_params.data(), ed->template_params.size()};
                decl_name = ed->name;
            }
            else
                return true;

            if (!constraint)
                return true;

            if (!m_constraint_checking_set.insert(decl).second)
            {
                error(use_range, "recursive type constraint on `{}`", decl_name);
                return false;
            }
            struct RecursionGuard
            {
                std::unordered_set<ast::Decl const*>& set;
                ast::Decl const* d;
                ~RecursionGuard() { set.erase(d); }
            } guard{m_constraint_checking_set, decl};
            std::ignore = guard;

            if (template_params.empty())
                return true;

            infer::TemplateBindings bindings{m_types};
            for (std::size_t i = 0; i < template_params.size(); ++i)
            {
                auto const& tp = template_params[i];
                if (i >= type_args.size())
                    break;

                auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                if (!param_ty || !bindings.deduce(param_ty, type_args[i]))
                    return false;
            }

            auto& mutable_scope = const_cast<Scope&>(scope);
            auto* defining_mod = find_defining_module(*decl);
            auto result = check_template_constraint(mod, mutable_scope, template_params, constraint, bindings, true, defining_mod);

            if (!result)
                error(use_range, "template constraint not satisfied for `{}`", decl_name);

            return result;
        }

        void check_nominal_type_constraint(ModuleInfo& mod, Scope const& scope, types::TypePtr ty, sm::SourceRange use_range)
        {
            if (!ty)
                return;

            ast::Expr const* constraint = nullptr;
            std::span<ast::TemplateParam const> template_params;
            std::string_view decl_name;
            ast::Decl const* decl = nullptr;

            if (auto const* st = types::type_cast<types::StructType>(ty))
            {
                auto const* sd = reinterpret_cast<ast::StructDecl const*>(st->decl);
                constraint = sd->constraint;
                template_params = {sd->template_params.data(), sd->template_params.size()};
                decl_name = sd->name;
                decl = static_cast<ast::Decl const*>(sd);
            }
            else if (auto const* et = types::type_cast<types::EnumType>(ty))
            {
                auto const* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                constraint = ed->constraint;
                template_params = {ed->template_params.data(), ed->template_params.size()};
                decl_name = ed->name;
                decl = static_cast<ast::Decl const*>(ed);
            }

            if (!constraint)
                return;

            if (template_params.empty())
                return;

            auto opt_bindings = make_bindings(ty);
            if (!opt_bindings)
                return;

            auto* guard_key = reinterpret_cast<void const*>(ty);
            if (!m_type_constraint_guard_set.insert(guard_key).second)
            {
                error(use_range, "recursive type constraint on `{}`", decl_name);
                return;
            }

            struct RecursionGuard
            {
                std::unordered_set<void const*>& set;
                void const* d;
                ~RecursionGuard() { set.erase(d); }
            } guard{m_type_constraint_guard_set, guard_key};
            std::ignore = guard;

            auto& mutable_scope = const_cast<Scope&>(scope);
            auto* defining_mod = decl ? find_defining_module(*decl) : nullptr;
            if (!check_template_constraint(mod, mutable_scope, template_params, constraint, *opt_bindings, true, defining_mod))
                error(use_range, "template constraint not satisfied for `{}`", decl_name);
        }

        void check_type_constraints_in_type(ModuleInfo& mod, Scope const& scope, types::TypePtr ty, sm::SourceRange use_range)
        {
            if (!ty)
                return;

            if (!m_type_walk_guard_set.insert(reinterpret_cast<void const*>(ty)).second)
                return;

            struct RecursionGuard
            {
                std::unordered_set<void const*>& set;
                void const* d;
                ~RecursionGuard() { set.erase(d); }
            } guard{m_type_walk_guard_set, reinterpret_cast<void const*>(ty)};
            std::ignore = guard;

            switch (ty->kind)
            {
                case types::TypeKind::Struct:
                case types::TypeKind::Enum:
                    check_nominal_type_constraint(mod, scope, ty, use_range);
                    {
                        auto const* ut = static_cast<types::UserType const*>(ty);
                        for (auto const* arg : ut->template_args)
                            check_type_constraints_in_type(mod, scope, arg, use_range);
                    }
                    break;
                case types::TypeKind::Pointer:
                    check_type_constraints_in_type(mod, scope, static_cast<types::PointerType const*>(ty)->pointee, use_range);
                    break;
                case types::TypeKind::Array:
                    check_type_constraints_in_type(mod, scope, static_cast<types::ArrayType const*>(ty)->element, use_range);
                    break;
                case types::TypeKind::RuntimeArray:
                    check_type_constraints_in_type(mod, scope, static_cast<types::RuntimeArrayType const*>(ty)->element, use_range);
                    break;
                case types::TypeKind::Slice:
                    check_type_constraints_in_type(mod, scope, static_cast<types::SliceType const*>(ty)->element, use_range);
                    break;
                case types::TypeKind::Fam:
                    check_type_constraints_in_type(mod, scope, static_cast<types::FamType const*>(ty)->element, use_range);
                    break;
                case types::TypeKind::Nominal:
                    check_type_constraints_in_type(mod, scope, static_cast<types::NominalType const*>(ty)->underlying, use_range);
                    break;
                case types::TypeKind::FuncPtr: {
                    auto const* fp = static_cast<types::FuncPtrType const*>(ty);
                    check_type_constraints_in_type(mod, scope, fp->return_type, use_range);
                    for (auto const* p : fp->params)
                        check_type_constraints_in_type(mod, scope, p, use_range);
                    break;
                }
                default:
                    break;
            }
        }

        static void record_resolved_specialization(ast::ExprSema& sema, detail::CommittedSpecialization const* spec) noexcept
        {
            sema.resolved_specialization = spec ? spec->decl : nullptr;
        }

        [[nodiscard]] ModuleInfo* find_defining_module(ast::Decl const& decl) noexcept
        {
            auto* range = &decl.range;
            if (auto const* fd = ast::node_cast<ast::FuncDecl>(&decl); fd && fd->name_range.valid())
                range = &fd->name_range;

            if (range->begin.fileId == sm::FileId::Invalid)
                return nullptr;

            for (auto const& m : m_modules)
                if (m && m->tu && m->file_id == range->begin.fileId)
                    return m.get();

            return nullptr;
        }

        [[nodiscard]] ModuleInfo* find_template_defining_module(ast::FuncDecl const& f) noexcept { return find_defining_module(f); }

        [[nodiscard]] Symbol const* resolve_value_path_with_fallback(Scope& primary, ast::Path const& path) const
        {
            if (auto const* sym = resolve_value_path(primary, path))
                return sym;

            if (m_specialization_defining_module && m_specialization_defining_module->own_scope)
                return resolve_value_path(*m_specialization_defining_module->own_scope, path);

            return nullptr;
        }

        [[nodiscard]] Symbol const* resolve_type_path_with_fallback(Scope& primary, ast::Path const& path) const
        {
            if (auto const* sym = resolve_type_path(primary, path))
                return sym;

            if (m_specialization_defining_module && m_specialization_defining_module->own_scope)
                return resolve_type_path(*m_specialization_defining_module->own_scope, path);

            return nullptr;
        }

        [[nodiscard]] detail::CommittedSpecialization commit_specialization(ModuleInfo& mod, ast::FuncDecl const& f, infer::TemplateBindings const& bindings,
                                                                            sm::SourceRange range)
        {
            if (f.template_params.empty())
                return {};

            auto spec = m_spec_registry.get_or_instantiate(f, bindings, range, m_ast_ctx, m_types, &m_diag);
            if (spec.is_new)
            {
                m_spec_registry.mark_analyzing(spec.decl);
                if (spec.decl && spec.decl->body)
                {
                    auto* saved_defining_mod = m_specialization_defining_module;
                    m_specialization_defining_module = find_template_defining_module(f);

                    {
                        auto* temp_scope = make_scope(ScopeKind::Block, nullptr);
                        if (spec.decl->body)
                        {
                            for (auto* stmt : spec.decl->body->stmts)
                                reevaluate_type_static_if_branches(*stmt, f, bindings, mod, *temp_scope);
                        }
                    }

                    auto pre_count = m_diag.diagnostic_count();
                    analyze_single_function(mod, *spec.decl);
                    auto post_count = m_diag.diagnostic_count();

                    m_specialization_defining_module = saved_defining_mod;

                    if (post_count > pre_count)
                        m_spec_registry.mark_failed(spec.decl);
                    else
                        m_spec_registry.mark_analyzed(spec.decl);
                }
                else
                    m_spec_registry.mark_analyzed(spec.decl);
            }
            return detail::CommittedSpecialization{spec.decl, spec.type};
        }

        [[nodiscard]] std::optional<types::FuncPtrType const*> resolve_explicit_probe(ModuleInfo& mod, Scope& scope, ast::FuncDecl const& f,
                                                                                      std::span<ast::TemplateArg const> template_args,
                                                                                      std::optional<infer::TemplateBindings>& out_bindings,
                                                                                      ExplicitInstFailure* failure = nullptr)
        {
            if (failure)
                *failure = ExplicitInstFailure::None;

            if (template_args.empty())
            {
                bool all_pack = !f.template_params.empty();
                for (auto const& tp : f.template_params)
                {
                    if (!tp.is_pack)
                    {
                        all_pack = false;
                        break;
                    }
                }
                if (all_pack)
                    ;
                else if (!f.template_params.empty() && f.template_params.back().is_pack)
                    ;
                else
                {
                    if (failure)
                        *failure = f.template_params.empty() ? ExplicitInstFailure::NotTemplate : ExplicitInstFailure::CountMismatch;
                    return std::nullopt;
                }
            }

            if (f.template_params.empty())
            {
                if (failure)
                    *failure = ExplicitInstFailure::NotTemplate;
                return std::nullopt;
            }

            bool has_pack = !f.template_params.empty() && f.template_params.back().is_pack;
            std::size_t non_pack_count = has_pack ? f.template_params.size() - 1 : f.template_params.size();

            if (!has_pack)
            {
                if (f.template_params.size() != template_args.size())
                {
                    if (failure)
                        *failure = ExplicitInstFailure::CountMismatch;
                    return std::nullopt;
                }
            }
            else
            {
                if (template_args.size() < non_pack_count)
                {
                    if (template_args.size() < f.template_params.size())
                    {
                        auto const& missing = f.template_params[template_args.size()];
                        if (missing.value_type && !missing.is_pack)
                            error(missing.range, "missing required value template argument for '{}'", missing.name);
                    }

                    if (failure)
                        *failure = ExplicitInstFailure::CountMismatch;
                    return std::nullopt;
                }
            }

            infer::TemplateBindings bindings{m_types};
            std::size_t i = 0;

            for (; i < non_pack_count; ++i)
            {
                auto const& tp = f.template_params[i];
                auto const& arg = template_args[i];
                auto param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));

                if (tp.value_type)
                {
                    auto vt_type = tp.value_type->sema.canonical ? get_canonical(tp.value_type->sema) : nullptr;
                    if (!vt_type)
                    {
                        if (failure)
                            *failure = ExplicitInstFailure::ValueArg;
                        return std::nullopt;
                    }

                    if (!arg.expr)
                    {
                        if (failure)
                            *failure = ExplicitInstFailure::CountMismatch;
                        return std::nullopt;
                    }

                    std::uint32_t probe_off = 0;
                    auto analyzed = analyze_expr(mod, nullptr, scope, *arg.expr, 0, probe_off, vt_type, nullptr);
                    if (has_error(analyzed.type) || !analyzed.constant)
                    {
                        error(arg.range, "non-type template argument must be a constant expression");
                        auto type_str = format_type_str(vt_type);
                        m_diag.note(tp.range, "in non-type template parameter `{}` of type `{}`", tp.name, type_str);
                        if (failure)
                            *failure = ExplicitInstFailure::ValueArg;
                        return std::nullopt;
                    }

                    bindings.bind_value(static_cast<types::TemplateParamType const*>(param_ty), *analyzed.constant);
                    continue;
                }

                if (arg.expr)
                {
                    if (failure)
                        *failure = ExplicitInstFailure::CountMismatch;
                    return std::nullopt;
                }

                if (!param_ty || !arg.type)
                {
                    if (failure)
                        *failure = ExplicitInstFailure::CountMismatch;
                    return std::nullopt;
                }

                auto actual = resolve_type_node(mod, scope, arg.type);
                if (!actual)
                {
                    if (failure)
                        *failure = ExplicitInstFailure::CountMismatch;
                    return std::nullopt;
                }

                if (auto r = bindings.deduce(param_ty, actual); !r)
                {
                    if (failure)
                        *failure = ExplicitInstFailure::CountMismatch;
                    return std::nullopt;
                }
            }

            if (has_pack)
            {
                auto const& tp = f.template_params.back();
                if (tp.value_type)
                {
                    std::vector<comptime::Value> pack_values;
                    for (; i < template_args.size(); ++i)
                    {
                        auto const& arg = template_args[i];
                        auto vt_type = tp.value_type->sema.canonical ? get_canonical(tp.value_type->sema) : nullptr;
                        if (!vt_type)
                        {
                            if (failure)
                                *failure = ExplicitInstFailure::ValueArg;
                            return std::nullopt;
                        }
                        if (!arg.expr)
                        {
                            error(arg.range, "non-constant value supplied for value pack '{}'", tp.name);
                            if (failure)
                                *failure = ExplicitInstFailure::ValueArg;
                            return std::nullopt;
                        }
                        std::uint32_t probe_off = 0;
                        auto analyzed = analyze_expr(mod, nullptr, scope, *arg.expr, 0, probe_off, vt_type, nullptr);
                        if (has_error(analyzed.type) || !analyzed.constant)
                        {
                            error(arg.range, "non-constant value supplied for value pack '{}'", tp.name);
                            if (failure)
                                *failure = ExplicitInstFailure::ValueArg;
                            return std::nullopt;
                        }

                        if (analyzed.type != vt_type)
                        {
                            error(arg.range, "value pack element type mismatch: expected '{}', got '{}'", format_type_str(vt_type),
                                  format_type_str(analyzed.type));
                            if (failure)
                                *failure = ExplicitInstFailure::ValueArg;
                            return std::nullopt;
                        }

                        pack_values.push_back(*analyzed.constant);
                    }

                    auto pack_param_ty =
                        m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(non_pack_count));

                    std::ignore = bindings.bind_value_pack(static_cast<types::TemplateParamType const*>(pack_param_ty), std::move(pack_values));
                }
                else
                {
                    std::vector<types::TypePtr> pack_types;
                    for (; i < template_args.size(); ++i)
                    {
                        auto const& arg = template_args[i];
                        if (arg.expr)
                        {
                            if (failure)
                                *failure = ExplicitInstFailure::CountMismatch;
                            return std::nullopt;
                        }
                        auto actual = resolve_type_node(mod, scope, arg.type);
                        if (!actual)
                        {
                            if (failure)
                                *failure = ExplicitInstFailure::CountMismatch;
                            return std::nullopt;
                        }
                        pack_types.push_back(actual);
                    }

                    auto pack_param_ty =
                        m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(non_pack_count));

                    std::ignore = bindings.bind_pack(static_cast<types::TemplateParamType const*>(pack_param_ty), pack_types);
                }
            }

            if (!check_template_constraint(mod, scope, f, bindings, false, find_defining_module(f)))
            {
                if (failure)
                    *failure = ExplicitInstFailure::Constraint;
                return std::nullopt;
            }

            std::pmr::vector<types::TypePtr> param_types(m_ast_ctx.resource());
            for (auto const& p : f.params)
            {
                auto ty = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : m_types.m_errort();
                if (is_func_param_sema_pack(p, f))
                {
                    auto expanded = expand_pack_param_type(ty, bindings);
                    for (auto const& et : expanded)
                        param_types.push_back(et);
                }
                else
                {
                    auto sub = bindings.substitute(ty);
                    param_types.push_back(sub);
                }
            }

            auto ret_ty = f.return_type && f.return_type->sema.canonical ? get_canonical(f.return_type->sema) : m_types.m_voidt();
            ret_ty = bindings.substitute(ret_ty);

            auto fp = m_types.funcptr_t(ret_ty, {param_types.data(), param_types.size()});
            auto* fp_type = types::type_cast<types::FuncPtrType>(fp);

            out_bindings.emplace(std::move(bindings));
            return fp_type;
        }

        [[nodiscard]] detail::CommittedSpecialization commit_candidate(ModuleInfo& mod, ExplicitInstCandidate const& cand,
                                                                       std::span<ast::TemplateArg const> template_args = {})
        {
            std::ignore = template_args;
            return commit_specialization(mod, *cand.template_fn, cand.bindings, cand.template_fn->range);
        }

        [[nodiscard]] std::optional<types::FuncPtrType const*> instantiate_explicit_function(ModuleInfo& mod, Scope& scope, ast::FuncDecl const& f,
                                                                                             std::span<ast::TemplateArg const> template_args,
                                                                                             ExplicitInstFailure* failure = nullptr,
                                                                                             ast::FuncDecl const** out_specialization_decl = nullptr)
        {
            std::optional<infer::TemplateBindings> opt_bindings;
            if (auto fp = resolve_explicit_probe(mod, scope, f, template_args, opt_bindings, failure))
            {
                auto spec = commit_specialization(mod, f, *opt_bindings, f.range);
                if (out_specialization_decl)
                    *out_specialization_decl = spec.decl;
                if (f.name.starts_with("atomic_"))
                    validate_atomic_type_param(f, *opt_bindings, template_args);
                return spec.type;
            }
            return std::nullopt;
        }

        enum class AtomicOp
        {
            Load,
            Store,
            Exchange,
            FetchAdd,
            FetchSub,
            FetchAnd,
            FetchOr,
            FetchXor,
            Fence,
            Unknown
        };

        static AtomicOp classify_atomic_op(std::string_view name)
        {
            if (name == "atomic_load")
                return AtomicOp::Load;
            if (name == "atomic_store")
                return AtomicOp::Store;
            if (name == "atomic_exchange")
                return AtomicOp::Exchange;
            if (name == "atomic_fetch_add")
                return AtomicOp::FetchAdd;
            if (name == "atomic_fetch_sub")
                return AtomicOp::FetchSub;
            if (name == "atomic_fetch_and")
                return AtomicOp::FetchAnd;
            if (name == "atomic_fetch_or")
                return AtomicOp::FetchOr;
            if (name == "atomic_fetch_xor")
                return AtomicOp::FetchXor;
            if (name == "atomic_fence")
                return AtomicOp::Fence;
            return AtomicOp::Unknown;
        }

        void validate_atomic_order(ast::FuncDecl const& f, detail::ExprResult const& order_arg, ast::Expr const* order_expr, AtomicOp op)
        {
            if (!order_arg.constant)
            {
                error(order_expr->range, "MemoryOrder argument to `{}` must be a compile-time constant", f.name);
                return;
            }

            if (order_arg.constant->kind() != comptime::Value::Kind::Int)
            {
                error(order_expr->range, "MemoryOrder argument to `{}` must be an integer value", f.name);
                return;
            }

            auto raw_order = order_arg.constant->get_int();

            bool order_ok = true;
            switch (op)
            {
                case AtomicOp::Load:
                    if (raw_order != 0 && raw_order != 1 && raw_order != 4)
                    {
                        error(order_expr->range, "atomic_load allows MemoryOrder Relaxed(0), Acquire(1), or SeqCst(4); got {}", raw_order);
                        order_ok = false;
                    }
                    break;
                case AtomicOp::Store:
                    if (raw_order != 0 && raw_order != 2 && raw_order != 4)
                    {
                        error(order_expr->range, "atomic_store allows MemoryOrder Relaxed(0), Release(2), or SeqCst(4); got {}", raw_order);
                        order_ok = false;
                    }
                    break;
                case AtomicOp::Fence:
                    if (raw_order != 1 && raw_order != 2 && raw_order != 3 && raw_order != 4)
                    {
                        error(order_expr->range, "atomic_fence allows MemoryOrder Acquire(1), Release(2), AcqRel(3), or SeqCst(4); got {}", raw_order);
                        order_ok = false;
                    }
                    break;
                default:
                    if (raw_order < 0 || raw_order > 4)
                    {
                        error(order_expr->range, "invalid MemoryOrder value: {}", raw_order);
                        order_ok = false;
                    }
                    break;
            }
            std::ignore = order_ok;
        }

        void validate_atomic_type_param(ast::FuncDecl const& f, infer::TemplateBindings const& bindings, std::span<ast::TemplateArg const> template_args,
                                        sm::SourceRange fallback_range = {})
        {
            types::TemplateParamType const* type_param_key = nullptr;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(f.template_params.size()); ++i)
            {
                auto const& tp = f.template_params[i];
                if (!tp.value_type)
                {
                    type_param_key =
                        static_cast<types::TemplateParamType const*>(m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, i));
                    break;
                }
            }

            auto op = classify_atomic_op(f.name);
            if (op == AtomicOp::Unknown || op == AtomicOp::Fence)
                return;

            if (type_param_key)
            {
                auto* concrete_t = bindings.lookup(type_param_key);

                auto type_range = [&]() -> sm::SourceRange {
                    for (std::size_t ti = 0; ti < f.template_params.size() && ti < template_args.size(); ++ti)
                        if (!f.template_params[ti].value_type)
                            return template_args[ti].range;

                    if (fallback_range.valid())
                        return fallback_range;

                    return f.range;
                }();

                if (concrete_t && concrete_t->kind == types::TypeKind::Float)
                    error(type_range, "atomic intrinsic does not support float type `{}`", format_type_str(concrete_t));
                else if (concrete_t && (concrete_t->kind == types::TypeKind::Struct || concrete_t->kind == types::TypeKind::Union ||
                                        concrete_t->kind == types::TypeKind::Enum))
                    error(type_range, "atomic intrinsic does not support aggregate type `{}`", format_type_str(concrete_t));
                else if (concrete_t && concrete_t->kind == types::TypeKind::Pointer &&
                         (op == AtomicOp::FetchAdd || op == AtomicOp::FetchSub || op == AtomicOp::FetchAnd || op == AtomicOp::FetchOr ||
                          op == AtomicOp::FetchXor))
                    error(type_range, "atomic RMW operations on pointer type `{}` are not supported", format_type_str(concrete_t));
            }
        }

        void reevaluate_type_static_if_branches(ast::Stmt& s, ast::FuncDecl const& fn, infer::TemplateBindings const& bindings, ModuleInfo& mod, Scope& scope)
        {
            auto* si = ast::node_cast<ast::StaticIfStmt>(&s);
            if (si && si->is_type_if && si->condition)
            {
                auto const* bin = ast::node_cast<ast::BinaryExpr>(si->condition);
                if (bin && bin->op == lex::TokenKind::EqEq)
                {
                    auto const* lhs_ident = ast::node_cast<ast::IdentExpr>(bin->lhs);
                    if (lhs_ident)
                    {
                        types::TemplateParamType const* param_type = nullptr;
                        std::uint32_t idx = 0;
                        for (auto const& tp : fn.template_params)
                        {
                            if (tp.name == lhs_ident->name)
                            {
                                param_type = static_cast<types::TemplateParamType const*>(
                                    m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, idx));
                                break;
                            }
                            ++idx;
                        }

                        if (param_type)
                        {
                            auto concrete = bindings.lookup(param_type);
                            if (concrete)
                            {
                                types::TypePtr target_type = nullptr;

                                if (auto const* rhs_ident = ast::node_cast<ast::IdentExpr>(bin->rhs))
                                {
                                    ast::Path path(m_ast_ctx.allocator());
                                    path.segments.push_back({rhs_ident->name, rhs_ident->range});
                                    path.range = rhs_ident->range;
                                    auto* type_expr = m_ast_ctx.make<ast::NamedType>(rhs_ident->range, std::move(path));
                                    target_type = resolve_type_node(mod, scope, type_expr);
                                }
                                else if (auto const* rhs_type_ast = ast::node_cast<ast::TypeASTExpr>(bin->rhs))
                                {
                                    if (rhs_type_ast->type_node)
                                        target_type = resolve_type_node(mod, scope, rhs_type_ast->type_node);
                                }

                                bool types_match = target_type && (concrete == target_type || concrete->kind == target_type->kind);
                                si->taken_branch = types_match ? 0 : 1;
                            }
                        }
                    }
                }
            }

            if (si)
            {
                for (auto* stmt : si->then_block.stmts)
                    reevaluate_type_static_if_branches(*stmt, fn, bindings, mod, scope);
                if (si->else_branch)
                    reevaluate_type_static_if_branches(*si->else_branch, fn, bindings, mod, scope);
            }
            else if (auto* while_s = ast::node_cast<ast::WhileStmt>(&s))
            {
                for (auto* stmt : while_s->body.stmts)
                    reevaluate_type_static_if_branches(*stmt, fn, bindings, mod, scope);
            }
            else if (auto* dw = ast::node_cast<ast::DoWhileStmt>(&s))
            {
                for (auto* stmt : dw->body.stmts)
                    reevaluate_type_static_if_branches(*stmt, fn, bindings, mod, scope);
            }
            else if (auto* for_s = ast::node_cast<ast::ForStmt>(&s))
            {
                for (auto* stmt : for_s->body.stmts)
                    reevaluate_type_static_if_branches(*stmt, fn, bindings, mod, scope);
            }
            else if (auto* forin = ast::node_cast<ast::ForInStmt>(&s))
            {
                for (auto* stmt : forin->body.stmts)
                    reevaluate_type_static_if_branches(*stmt, fn, bindings, mod, scope);
            }
            else if (auto* defer = ast::node_cast<ast::DeferStmt>(&s))
            {
                if (defer->body)
                    reevaluate_type_static_if_branches(*defer->body, fn, bindings, mod, scope);
            }
        }

        void reevaluate_static_if_branches_in_fn(ast::FuncDecl const& fn, infer::TemplateBindings const& bindings, ModuleInfo& mod, Scope& scope)
        {
            if (fn.body)
            {
                for (auto* stmt : fn.body->stmts)
                    reevaluate_type_static_if_branches(*stmt, fn, bindings, mod, scope);
            }
        }

        [[nodiscard]] static bool decl_is_top_level_const(ast::Decl const& d) noexcept
        {
            auto const* ty = [&]() -> ast::TypeExpr const* {
                switch (d.kind)
                {
                    case ast::DeclKind::Var:
                        return static_cast<ast::VarDecl const&>(d).type;
                    case ast::DeclKind::Func:
                        return nullptr;
                    default:
                        return nullptr;
                }
            }();

            auto const* q = ast::node_cast<ast::QualifiedType>(ty);
            return q && ast::has_qual(q->quals, ast::Qual::Const);
        }

        [[nodiscard]] static CallRank rank_for_exact_arg(ast::Expr const& arg, detail::ExprResult const& analyzed, types::TypePtr param)
        {
            if (contains_template_param(param))
                return CallRank::TemplateExact;

            auto actual = analyzed.type;

            if (arg.kind == ast::ExprKind::StringLiteral)
            {
                if (auto const* st = types::type_cast<types::SliceType>(param))
                {
                    auto const* elem = st->element;
                    bool is_char = elem->kind == types::TypeKind::Char;
                    bool is_u8 = elem->kind == types::TypeKind::Int && static_cast<types::IntType const*>(elem)->bits == 8 &&
                                 !static_cast<types::IntType const*>(elem)->is_signed;

                    bool is_const = types::has_qual(st->element_quals, types::Qual::Const);

                    if (is_char)
                        return is_const ? CallRank::StringLiteralCharSliceConst : CallRank::StringLiteralCharSliceMutable;
                    if (is_u8)
                        return is_const ? CallRank::StringLiteralU8SliceConst : CallRank::StringLiteralU8SliceMutable;
                }
                if (auto const* pt = types::type_cast<types::PointerType>(param))
                {
                    auto const* pointee = pt->pointee;
                    bool is_char = pointee->kind == types::TypeKind::Char;
                    bool is_u8 = pointee->kind == types::TypeKind::Int && static_cast<types::IntType const*>(pointee)->bits == 8 &&
                                 !static_cast<types::IntType const*>(pointee)->is_signed;
                    bool is_const = types::has_qual(pt->pointee_quals, types::Qual::Const);

                    if (is_char)
                        return is_const ? CallRank::StringLiteralCharPointerConst : CallRank::StringLiteralCharPointerMutable;
                    if (is_u8)
                        return is_const ? CallRank::StringLiteralU8PointerConst : CallRank::StringLiteralU8PointerMutable;
                }
            }
            else if (arg.kind == ast::ExprKind::U16StringLiteral)
            {
                if (auto const* st = types::type_cast<types::SliceType>(param))
                {
                    auto const* elem = st->element;
                    bool is_u16 = elem->kind == types::TypeKind::Int && static_cast<types::IntType const*>(elem)->bits == 16 &&
                                  !static_cast<types::IntType const*>(elem)->is_signed;
                    bool is_const = types::has_qual(st->element_quals, types::Qual::Const);

                    if (is_u16)
                        return is_const ? CallRank::U16StringLiteralSliceConst : CallRank::U16StringLiteralSliceMutable;
                }
                if (auto const* pt = types::type_cast<types::PointerType>(param))
                {
                    auto const* pointee = pt->pointee;
                    bool is_u16 = pointee->kind == types::TypeKind::Int && static_cast<types::IntType const*>(pointee)->bits == 16 &&
                                  !static_cast<types::IntType const*>(pointee)->is_signed;
                    bool is_const = types::has_qual(pt->pointee_quals, types::Qual::Const);

                    if (is_u16)
                        return is_const ? CallRank::U16StringLiteralPointerConst : CallRank::U16StringLiteralPointerMutable;
                }
            }

            if (actual == param)
            {
                switch (analyzed.construction_kind)
                {
                    case ConstructionKind::Struct:
                        return CallRank::StructContextualExact;
                    case ConstructionKind::Array:
                        return CallRank::ArrayContextualExact;
                    case ConstructionKind::Slice:
                        return CallRank::SliceContextualExact;
                    case ConstructionKind::Enum:
                        return CallRank::EnumContextualExact;
                    case ConstructionKind::None:
                        break;
                }

                return is_contextual_literal(arg) ? CallRank::LiteralContextualExact : CallRank::ConcreteExact;
            }

            if (actual && param && actual->kind == types::TypeKind::Array && param->kind == types::TypeKind::Slice)
                return CallRank::SliceContextualExact;

            return CallRank::ConcreteExact;
        }

        [[nodiscard]] std::optional<std::pair<UfcsReceiverMatch, types::TypePtr>> match_ufcs_receiver(detail::ExprResult const& analyzed, types::TypePtr param)
        {
            if (!analyzed.type || !param)
                return std::nullopt;

            if (analyzed.type == param)
                return std::pair{UfcsReceiverMatch::Exact, analyzed.type};

            auto const* param_ptr = types::type_cast<types::PointerType>(param);
            if (param_ptr)
            {
                if (analyzed.is_lvalue && (analyzed.type == param_ptr->pointee || contains_template_param(param_ptr->pointee)))
                {
                    bool receiver_has_const = analyzed.resolved_decl && decl_is_top_level_const(*analyzed.resolved_decl);
                    bool param_wants_const = types::has_qual(param_ptr->pointee_quals, types::Qual::Const);

                    if (param_wants_const)
                    {
                        types::Qual out_quals = types::Qual::Const;
                        if (types::has_qual(param_ptr->pointee_quals, types::Qual::Volatile))
                            out_quals = out_quals | types::Qual::Volatile;
                        if (types::has_qual(param_ptr->pointee_quals, types::Qual::Restrict))
                            out_quals = out_quals | types::Qual::Restrict;

                        if (receiver_has_const)
                            return std::pair{UfcsReceiverMatch::AutoRefConst, m_types.pointer_to(analyzed.type, out_quals)};
                        else
                            return std::pair{UfcsReceiverMatch::AutoRefQualMismatch, m_types.pointer_to(analyzed.type, out_quals)};
                    }

                    if (!receiver_has_const)
                    {
                        types::Qual out_quals = types::Qual::None;
                        if (types::has_qual(param_ptr->pointee_quals, types::Qual::Volatile))
                            out_quals = out_quals | types::Qual::Volatile;
                        if (types::has_qual(param_ptr->pointee_quals, types::Qual::Restrict))
                            out_quals = out_quals | types::Qual::Restrict;
                        return std::pair{UfcsReceiverMatch::AutoRef, m_types.pointer_to(analyzed.type, out_quals)};
                    }
                }
                return std::nullopt;
            }

            auto const* recv_ptr = types::type_cast<types::PointerType>(analyzed.type);
            if (recv_ptr && recv_ptr->pointee == param)
                return std::pair{UfcsReceiverMatch::AutoDeref, param};

            if (contains_template_param(param))
                return std::pair{UfcsReceiverMatch::Exact, analyzed.type};

            return std::nullopt;
        }

        [[nodiscard]] ExplicitInstScan scan_explicit_instantiations(ModuleInfo& mod, Scope& scope, std::span<Symbol const> syms,
                                                                    std::span<ast::TemplateArg const> template_args)
        {
            ExplicitInstScan scan{};
            for (auto const& sym : syms)
            {
                if (!sym.decl || sym.decl->kind != ast::DeclKind::Func)
                    continue;

                scan.saw_function = true;
                auto const& f = *static_cast<ast::FuncDecl const*>(sym.decl);
                ExplicitInstFailure failure{};
                std::optional<infer::TemplateBindings> opt_bindings;
                if (auto fp = resolve_explicit_probe(mod, scope, f, template_args, opt_bindings, &failure))
                {
                    scan.saw_template = scan.saw_template || !f.template_params.empty();
                    scan.viable.emplace_back(&sym, *fp, &f, std::move(*opt_bindings));
                    continue;
                }

                if (failure == ExplicitInstFailure::NotTemplate)
                    scan.saw_non_template = true;
                else if (failure == ExplicitInstFailure::CountMismatch)
                    scan.saw_count_mismatch = true;
                else if (failure == ExplicitInstFailure::ValueArg)
                    scan.saw_value_arg = true;
                else if (failure == ExplicitInstFailure::Constraint)
                    scan.saw_constraint_failure = true;
            }
            return scan;
        }

        [[nodiscard]] std::optional<RankedCandidate>
        probe_candidate_from_params(ModuleInfo& mod, Scope& scope, Symbol const& sym, std::span<types::TypePtr const> params,
                                    std::span<ast::Expr* const> arg_exprs, std::uint32_t next_off, int loop_depth, ConstEnv const* const_env,
                                    bool* had_suppressed_errors = nullptr, bool* had_constraint_failure = nullptr, bool* had_non_constraint_failure = nullptr,
                                    types::TypePtr expected_type = nullptr, bool nttps_already_resolved = false, std::string* rejection_reason = nullptr,
                                    infer::TemplateBindings const* pre_resolved_bindings = nullptr)
        {
            ast::FuncDecl const* func = nullptr;
            if (sym.decl && sym.decl->kind == ast::DeclKind::Func)
                func = static_cast<ast::FuncDecl const*>(sym.decl);

            std::size_t num_value_tparams = 0;
            if (!nttps_already_resolved && func)
            {
                for (auto const& tp : func->template_params)
                    if (tp.value_type && !tp.is_pack)
                        ++num_value_tparams;
            }

            auto pack_arity = compute_pack_arity(func, params.size(), nttps_already_resolved);
            bool has_func_pack = pack_arity.has_pack;
            std::size_t non_pack_func_params = pack_arity.non_pack_count;
            std::size_t min_required = pack_arity.min_required;

            if (!has_func_pack)
            {
                if (params.size() + num_value_tparams != arg_exprs.size())
                {
                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = std::format("argument count mismatch: expected {}, got {}", params.size() + num_value_tparams, arg_exprs.size());

                    return std::nullopt;
                }
            }
            else
            {
                if (arg_exprs.size() < min_required + num_value_tparams)
                {
                    if (had_suppressed_errors)
                        *had_suppressed_errors = true;

                    if (rejection_reason)
                        *rejection_reason = std::format("argument count mismatch: expected at least {} (with pack), got {}", min_required + num_value_tparams,
                                                        arg_exprs.size());

                    return std::nullopt;
                }
            }

            if (std::ranges::any_of(arg_exprs, [](auto* e) { return e == nullptr; }))
            {
                if (had_non_constraint_failure)
                    *had_non_constraint_failure = true;

                if (rejection_reason)
                    *rejection_reason = "incomplete call argument";

                return std::nullopt;
            }

            auto* probe_scope = make_probe_scope(scope);
            std::uint32_t probe_off = next_off;
            [[maybe_unused]] ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
            [[maybe_unused]] NestedImplicitEnumGuard nested_enum_guard{m_disallow_nested_implicit_enum};

            infer::TemplateBindings b{m_types};
            if (expected_type && func && !func->template_params.empty() && func->return_type)
            {
                auto return_ty = get_canonical(func->return_type->sema);
                if (return_ty && contains_template_param(return_ty) && !contains_template_param(expected_type))
                    std::ignore = b.deduce(return_ty, expected_type);
            }

            for (std::size_t vi = 0; vi < num_value_tparams; ++vi)
            {
                ast::TemplateParam const* vtparam = nullptr;
                {
                    std::size_t idx = 0;
                    for (auto const& tp : func->template_params)
                        if (tp.value_type && !tp.is_pack)
                        {
                            if (idx == vi)
                            {
                                vtparam = &tp;
                                break;
                            }
                            ++idx;
                        }
                }

                if (!vtparam || !vtparam->value_type || !vtparam->value_type->sema.canonical)
                {
                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = "value template argument type is not resolved";

                    return std::nullopt;
                }

                auto vt_type = get_canonical(vtparam->value_type->sema);
                auto r = analyze_expr(mod, nullptr, *probe_scope, *arg_exprs[vi], loop_depth, probe_off, vt_type, const_env);
                if (has_error(r.type))
                {
                    if (had_suppressed_errors)
                        *had_suppressed_errors = suppress.had_suppressed_errors();

                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = std::format("cannot convert value template argument {} to required type", vi);

                    return std::nullopt;
                }

                if (!r.constant)
                {
                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = std::format("value template argument {} is not a constant expression", vi);

                    return std::nullopt;
                }

                {
                    auto* pty = m_types.template_param_t(const_cast<ast::TemplateParam*>(vtparam), vtparam->name, static_cast<std::uint32_t>(vi));
                    if (pty)
                        b.bind_value(static_cast<types::TemplateParamType const*>(pty), *r.constant);
                }
            }

            std::size_t func_arg_start = num_value_tparams;
            std::size_t func_arg_count = arg_exprs.size() - num_value_tparams;

            std::vector<detail::ExprResult> args;
            args.reserve(func_arg_count);
            std::vector<types::TypePtr> actual_param_types;
            actual_param_types.reserve(func_arg_count);

            for (std::size_t i = 0; i < non_pack_func_params; ++i)
            {
                auto param_ty = b.substitute(params[i]);
                actual_param_types.push_back(param_ty);
                auto r = analyze_expr(mod, nullptr, *probe_scope, *arg_exprs[func_arg_start + i], loop_depth, probe_off, param_ty, const_env);

                if (has_error(r.type))
                {
                    if (had_suppressed_errors)
                        *had_suppressed_errors = suppress.had_suppressed_errors();

                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = std::format("cannot convert argument {}: expected `{}`", func_arg_start + i + 1, format_type_str(param_ty));

                    return std::nullopt;
                }
                args.push_back(r);
            }

            std::vector<types::TypePtr> pack_arg_types;
            std::vector<comptime::Value> pack_arg_values;
            if (has_func_pack)
            {
                auto pack_param_ty = b.substitute(params.back());
                for (std::size_t i = non_pack_func_params; i < func_arg_count; ++i)
                {
                    types::TypePtr expected_ty = nullptr;
                    if (auto const* pt = types::type_cast<types::TypePackType>(pack_param_ty))
                        expected_ty = b.substitute(pt->element);
                    else if (auto const* tp = types::type_cast<types::TemplateParamType>(pack_param_ty))
                        expected_ty = tp;

                    auto r = analyze_expr(mod, nullptr, *probe_scope, *arg_exprs[func_arg_start + i], loop_depth, probe_off, expected_ty, const_env);
                    if (has_error(r.type))
                    {
                        if (had_suppressed_errors)
                            *had_suppressed_errors = suppress.had_suppressed_errors();

                        if (had_non_constraint_failure)
                            *had_non_constraint_failure = true;

                        if (rejection_reason)
                            *rejection_reason = std::format("cannot convert argument {} (pack element)", func_arg_start + i + 1);

                        return std::nullopt;
                    }
                    args.push_back(r);
                    pack_arg_types.push_back(r.type);
                    if (r.constant && r.constant->kind() == comptime::Value::Kind::String)
                        pack_arg_values.push_back(*r.constant);
                    else
                        pack_arg_values.push_back(comptime::Value{});
                }
            }

            std::vector<types::TypePtr> actuals;
            actuals.reserve(args.size());
            for (auto const& a : args)
                actuals.push_back(a.type);

            std::vector<types::TypePtr> deduce_params;
            deduce_params.reserve(non_pack_func_params + (has_func_pack ? 1 : 0));
            for (std::size_t i = 0; i < non_pack_func_params; ++i)
                deduce_params.push_back(params[i]);

            if (has_func_pack)
            {
                auto pack_elem = params.back();
                if (auto const* pt = types::type_cast<types::TypePackType>(params.back()))
                    pack_elem = pt->element;
                deduce_params.push_back(m_types.type_pack_t(pack_elem));
            }

            auto deduce_result = b.deduce_function(deduce_params, actuals);
            if (!deduce_result)
            {
                if (had_suppressed_errors)
                    *had_suppressed_errors = suppress.had_suppressed_errors();

                if (had_non_constraint_failure)
                    *had_non_constraint_failure = true;

                if (rejection_reason)
                {
                    bool arg_mismatch_found = false;
                    for (std::size_t i = 0; i < actuals.size() && i < deduce_params.size(); ++i)
                    {
                        auto subbed_param = b.substitute(deduce_params[i]);
                        if (actuals[i] != subbed_param && !has_error(actuals[i]) && !has_error(subbed_param) &&
                            !types::type_cast<types::TemplateParamType>(subbed_param))
                        {
                            *rejection_reason = std::format("cannot convert argument {}: expected `{}`, found `{}`", func_arg_start + i + 1,
                                                            format_type_str(subbed_param), format_type_str(actuals[i]));

                            arg_mismatch_found = true;
                            break;
                        }
                    }
                    if (!arg_mismatch_found)
                    {
                        auto trace = trace_template_deduction(func, params, actuals);
                        if (!trace.empty())
                            *rejection_reason = std::format("template argument deduction failed: {}", trace);
                        else
                            *rejection_reason = std::format("template argument deduction failed: {}", deduce_result.detail);
                    }
                }

                return std::nullopt;
            }

            if (has_func_pack && !pack_arg_types.empty())
            {
                if (!func->template_params.empty() && func->template_params.back().is_pack)
                {
                    auto const& tp = func->template_params.back();
                    auto* pack_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name,
                                                             static_cast<std::uint32_t>(func->template_params.size() - 1));
                    if (pack_ty)
                    {
                        bool has_any_value = std::ranges::any_of(pack_arg_values, [](comptime::Value const& v) { return v.kind() != comptime::Value::Kind::Null; });
                        if (has_any_value)
                            std::ignore = b.bind_pack(static_cast<types::TemplateParamType const*>(pack_ty), pack_arg_types, pack_arg_values);
                        else
                            std::ignore = b.bind_pack(static_cast<types::TemplateParamType const*>(pack_ty), pack_arg_types);
                    }
                }
            }

            if (!check_template_constraint(mod, scope, *func, pre_resolved_bindings ? *pre_resolved_bindings : b, false, sym.module))
            {
                if (had_constraint_failure)
                    *had_constraint_failure = true;

                if (rejection_reason)
                    *rejection_reason = "template constraint not satisfied";

                return std::nullopt;
            }

            RankedCandidate out{};
            out.sym = &sym;
            out.ranks.reserve(arg_exprs.size());

            for (std::size_t vi = 0; vi < num_value_tparams; ++vi)
                out.ranks.push_back(CallRank::ConcreteExact);

            for (std::size_t i = 0; i < non_pack_func_params; ++i)
            {
                auto const* param = params[i];
                if (!param || !args[i].type || args[i].type->kind == types::TypeKind::Error)
                {
                    if (had_suppressed_errors)
                        *had_suppressed_errors = suppress.had_suppressed_errors();

                    if (rejection_reason)
                        *rejection_reason = std::format("cannot determine conversion rank for argument {}", func_arg_start + i);

                    return std::nullopt;
                }

                out.ranks.push_back(rank_for_exact_arg(*arg_exprs[func_arg_start + i], args[i], param));
            }

            for (std::size_t i = non_pack_func_params; i < args.size(); ++i)
                out.ranks.push_back(CallRank::TemplateExact);

            if (had_suppressed_errors)
                *had_suppressed_errors = suppress.had_suppressed_errors();

            return out;
        }

        [[nodiscard]] std::optional<RankedCandidate> probe_ufcs_candidate(ModuleInfo& mod, Scope& scope, Symbol const& sym, ast::Expr& object,
                                                                          std::span<ast::Expr* const> arg_exprs, std::uint32_t next_off, int loop_depth,
                                                                          ConstEnv const* const_env, bool* had_suppressed_errors = nullptr,
                                                                          bool* had_constraint_failure = nullptr, bool* had_non_constraint_failure = nullptr,
                                                                          types::TypePtr expected_type = nullptr, std::string* rejection_reason = nullptr)
        {
            if (!sym.decl || sym.decl->kind != ast::DeclKind::Func)
                return std::nullopt;

            auto const& f = *static_cast<ast::FuncDecl const*>(sym.decl);
            std::vector<types::TypePtr> params;
            params.reserve(f.params.size());
            for (auto const& p : f.params)
                params.push_back(p.type ? get_canonical(p.type->sema) : m_types.m_errort());

            std::size_t num_value_tparams = 0;
            for (auto const& tp : f.template_params)
                if (tp.value_type && !tp.is_pack)
                    ++num_value_tparams;

            auto pack_arity = compute_pack_arity(&f, params.size());
            bool has_func_pack = pack_arity.has_pack;
            std::size_t non_pack_func_params = pack_arity.non_pack_count;
            std::size_t min_required = pack_arity.min_required;

            std::size_t actual_count = arg_exprs.size() + 1;
            if (!has_func_pack)
            {
                if (params.size() + num_value_tparams != actual_count)
                {
                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = std::format("argument count mismatch: expected {}, got {}", params.size() + num_value_tparams, actual_count);

                    return std::nullopt;
                }
            }
            else
            {
                if (actual_count < min_required + num_value_tparams)
                {
                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason =
                            std::format("argument count mismatch: expected at least {}, got {}", min_required + num_value_tparams, actual_count);

                    return std::nullopt;
                }
            }

            if (std::ranges::any_of(arg_exprs, [](auto* e) { return e == nullptr; }))
            {
                if (had_non_constraint_failure)
                    *had_non_constraint_failure = true;

                if (rejection_reason)
                    *rejection_reason = "incomplete call argument";

                return std::nullopt;
            }

            auto* probe_scope = make_probe_scope(scope);
            std::uint32_t probe_off = next_off;
            ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
            [[maybe_unused]] NestedImplicitEnumGuard nested_enum_guard{m_disallow_nested_implicit_enum};

            infer::TemplateBindings b{m_types};

            auto param0 = b.substitute(params[0]);
            auto receiver = analyze_expr(mod, nullptr, *probe_scope, object, loop_depth, probe_off, param0, const_env);
            if (has_error(receiver.type))
            {
                if (had_suppressed_errors)
                    *had_suppressed_errors = suppress.had_suppressed_errors();

                if (had_non_constraint_failure)
                    *had_non_constraint_failure = true;

                if (rejection_reason)
                    *rejection_reason = std::format("receiver type mismatch: expected `{}`", format_type_str(param0));

                return std::nullopt;
            }

            auto match = match_ufcs_receiver(receiver, param0);
            if (!match)
            {
                if (had_suppressed_errors)
                    *had_suppressed_errors = suppress.had_suppressed_errors();

                if (had_non_constraint_failure)
                    *had_non_constraint_failure = true;

                if (rejection_reason)
                    *rejection_reason =
                        std::format("receiver type mismatch: expected `{}`, found `{}`", format_type_str(param0), format_type_str(receiver.type));

                return std::nullopt;
            }

            for (std::size_t vi = 0; vi < num_value_tparams; ++vi)
            {
                ast::TemplateParam const* vtparam = nullptr;
                {
                    std::size_t idx = 0;
                    for (auto const& tp : f.template_params)
                        if (tp.value_type && !tp.is_pack)
                        {
                            if (idx == vi)
                            {
                                vtparam = &tp;
                                break;
                            }
                            ++idx;
                        }
                }

                if (!vtparam || !vtparam->value_type || !vtparam->value_type->sema.canonical)
                {
                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = "value template argument type is not resolved";

                    return std::nullopt;
                }

                auto vt_type = get_canonical(vtparam->value_type->sema);
                auto r = analyze_expr(mod, nullptr, *probe_scope, *arg_exprs[vi], loop_depth, probe_off, vt_type, const_env);
                if (has_error(r.type))
                {
                    if (had_suppressed_errors)
                        *had_suppressed_errors = suppress.had_suppressed_errors();

                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = std::format("value template argument {} could not be evaluated", vi);

                    return std::nullopt;
                }

                if (r.constant)
                {
                    auto* pty = m_types.template_param_t(const_cast<ast::TemplateParam*>(vtparam), vtparam->name, static_cast<std::uint32_t>(vi));
                    if (pty)
                        b.bind_value(static_cast<types::TemplateParamType const*>(pty), *r.constant);
                }
            }

            std::size_t func_arg_start = num_value_tparams;
            std::size_t func_arg_count = arg_exprs.size() - num_value_tparams;

            std::vector<detail::ExprResult> args;
            args.reserve(func_arg_count);
            std::vector<types::TypePtr> pack_arg_types;
            std::vector<comptime::Value> pack_arg_values;

            std::size_t non_pack_after_receiver = non_pack_func_params > 0 ? non_pack_func_params - 1 : 0;
            for (std::size_t i = 0; i < non_pack_after_receiver; ++i)
            {
                auto param_ty = b.substitute(params[i + 1]);
                auto r = analyze_expr(mod, nullptr, *probe_scope, *arg_exprs[func_arg_start + i], loop_depth, probe_off, param_ty, const_env);
                if (has_error(r.type))
                {
                    if (had_suppressed_errors)
                        *had_suppressed_errors = suppress.had_suppressed_errors();

                    if (had_non_constraint_failure)
                        *had_non_constraint_failure = true;

                    if (rejection_reason)
                        *rejection_reason = std::format("cannot convert argument {}: expected `{}`", i + 1, format_type_str(param_ty));

                    return std::nullopt;
                }
                args.push_back(r);
            }

            if (has_func_pack)
            {
                auto pack_param_ty = b.substitute(params.back());
                for (std::size_t i = non_pack_after_receiver; i < func_arg_count; ++i)
                {
                    types::TypePtr expected_ty = nullptr;
                    if (auto const* pt = types::type_cast<types::TypePackType>(pack_param_ty))
                        expected_ty = b.substitute(pt->element);
                    else if (auto const* tp = types::type_cast<types::TemplateParamType>(pack_param_ty))
                        expected_ty = tp;

                    auto r = analyze_expr(mod, nullptr, *probe_scope, *arg_exprs[func_arg_start + i], loop_depth, probe_off, expected_ty, const_env);
                    if (has_error(r.type))
                    {
                        if (had_suppressed_errors)
                            *had_suppressed_errors = suppress.had_suppressed_errors();

                        if (had_non_constraint_failure)
                            *had_non_constraint_failure = true;

                        if (rejection_reason)
                            *rejection_reason = std::format("cannot convert argument {} (pack element)", i + 1);

                        return std::nullopt;
                    }
                    args.push_back(r);
                    pack_arg_types.push_back(r.type);
                    if (r.constant && r.constant->kind() == comptime::Value::Kind::String)
                        pack_arg_values.push_back(*r.constant);
                    else
                        pack_arg_values.push_back(comptime::Value{});
                }
            }

            std::vector<types::TypePtr> actuals;
            actuals.reserve(args.size() + 1);
            actuals.push_back(match->second);
            for (auto const& a : args)
                actuals.push_back(a.type);

            std::vector<types::TypePtr> deduce_params;
            deduce_params.reserve(non_pack_func_params + (has_func_pack ? 1 : 0));
            for (std::size_t i = 0; i < non_pack_func_params; ++i)
                deduce_params.push_back(params[i]);

            if (has_func_pack)
            {
                auto pack_elem = params.back();
                if (auto const* pt = types::type_cast<types::TypePackType>(params.back()))
                    pack_elem = pt->element;
                deduce_params.push_back(m_types.type_pack_t(pack_elem));
            }

            if (!b.deduce_function(deduce_params, actuals))
            {
                if (had_suppressed_errors)
                    *had_suppressed_errors = suppress.had_suppressed_errors();

                if (had_non_constraint_failure)
                    *had_non_constraint_failure = true;

                if (rejection_reason)
                {
                    bool arg_mismatch_found = false;
                    for (std::size_t i = 0; i < actuals.size() && i < deduce_params.size(); ++i)
                    {
                        auto subbed_param = b.substitute(deduce_params[i]);
                        if (actuals[i] != subbed_param && !has_error(actuals[i]) && !has_error(subbed_param) &&
                            !types::type_cast<types::TemplateParamType>(subbed_param))
                        {
                            if (i == 0)
                                *rejection_reason = std::format("receiver type mismatch: expected `{}`, found `{}`", format_type_str(subbed_param),
                                                                format_type_str(actuals[i]));
                            else
                                *rejection_reason = std::format("cannot convert argument {}: expected `{}`, found `{}`", i, format_type_str(subbed_param),
                                                                format_type_str(actuals[i]));
                            arg_mismatch_found = true;
                            break;
                        }
                    }
                    if (!arg_mismatch_found)
                    {
                        auto trace = trace_template_deduction(&f, params, actuals);
                        if (!trace.empty())
                            *rejection_reason = std::format("template argument deduction failed: {}", trace);
                        else
                            *rejection_reason = "template argument deduction failed";
                    }
                }

                return std::nullopt;
            }

            if (has_func_pack && !pack_arg_types.empty())
            {
                if (!f.template_params.empty() && f.template_params.back().is_pack)
                {
                    auto const& tp = f.template_params.back();
                    auto* pack_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name,
                                                             static_cast<std::uint32_t>(f.template_params.size() - 1));
                    if (pack_ty)
                    {
                        bool has_any_value = std::ranges::any_of(pack_arg_values, [](comptime::Value const& v) { return v.kind() != comptime::Value::Kind::Null; });
                        if (has_any_value)
                            std::ignore = b.bind_pack(static_cast<types::TemplateParamType const*>(pack_ty), pack_arg_types, pack_arg_values);
                        else
                            std::ignore = b.bind_pack(static_cast<types::TemplateParamType const*>(pack_ty), pack_arg_types);
                    }
                }
            }

            if (expected_type && !f.template_params.empty() && f.return_type)
            {
                auto return_ty = get_canonical(f.return_type->sema);
                if (return_ty && contains_template_param(return_ty))
                    std::ignore = b.deduce(return_ty, expected_type);
            }

            if (!check_template_constraint(mod, scope, f, b, false, sym.module))
            {
                if (had_constraint_failure)
                    *had_constraint_failure = true;

                if (rejection_reason)
                    *rejection_reason = "template constraint not satisfied";

                return std::nullopt;
            }

            RankedCandidate out{};
            out.sym = &sym;
            out.receiver_match = match->first;
            out.ranks.reserve(1 + 1 + num_value_tparams + args.size());
            switch (match->first)
            {
                case UfcsReceiverMatch::Exact:
                    out.ranks.push_back(CallRank::ReceiverExact);
                    break;
                case UfcsReceiverMatch::AutoRef:
                    out.ranks.push_back(CallRank::ReceiverAutoRef);
                    break;
                case UfcsReceiverMatch::AutoRefConst:
                    out.ranks.push_back(CallRank::ReceiverAutoRefConst);
                    break;
                case UfcsReceiverMatch::AutoRefQualMismatch:
                    out.ranks.push_back(CallRank::ReceiverAutoRefQualMismatch);
                    break;
                case UfcsReceiverMatch::AutoDeref:
                    out.ranks.push_back(CallRank::ReceiverAutoDeref);
                    break;
                case UfcsReceiverMatch::None:
                    if (had_suppressed_errors)
                        *had_suppressed_errors = suppress.had_suppressed_errors();

                    return std::nullopt;
            }

            out.ranks.push_back(rank_for_exact_arg(object, receiver, params[0]));

            for (std::size_t vi = 0; vi < num_value_tparams; ++vi)
                out.ranks.push_back(CallRank::ConcreteExact);

            for (std::size_t i = 0; i < non_pack_after_receiver; ++i)
                out.ranks.push_back(rank_for_exact_arg(*arg_exprs[func_arg_start + i], args[i], params[i + 1]));

            for (std::size_t i = non_pack_after_receiver; i < args.size(); ++i)
                out.ranks.push_back(CallRank::TemplateExact);

            if (had_suppressed_errors)
                *had_suppressed_errors = suppress.had_suppressed_errors();

            return out;
        }

        [[nodiscard]] std::optional<RankedCandidate> probe_candidate(ModuleInfo& mod, Scope& scope, Symbol const& sym, std::span<ast::Expr* const> arg_exprs,
                                                                     std::uint32_t next_off, int loop_depth, ConstEnv const* const_env,
                                                                     bool* had_suppressed_errors = nullptr, bool* had_constraint_failure = nullptr,
                                                                     bool* had_non_constraint_failure = nullptr, types::TypePtr expected_type = nullptr,
                                                                     std::string* rejection_reason = nullptr)
        {
            if (!sym.decl || sym.decl->kind != ast::DeclKind::Func)
                return std::nullopt;

            auto const& f = *static_cast<ast::FuncDecl const*>(sym.decl);
            std::vector<types::TypePtr> params;
            params.reserve(f.params.size());
            for (auto const& p : f.params)
                params.push_back(p.type ? get_canonical(p.type->sema) : m_types.m_errort());

            return probe_candidate_from_params(mod, scope, sym, params, arg_exprs, next_off, loop_depth, const_env, had_suppressed_errors,
                                               had_constraint_failure, had_non_constraint_failure, expected_type, false, rejection_reason);
        }

        [[nodiscard]] std::optional<detail::ExprResult> invoke_ufcs_candidate(ModuleInfo& mod, Scope& scope, Symbol const& sym, ast::Expr& object,
                                                                              std::span<ast::Expr* const> arg_exprs, sm::SourceRange range, int loop_depth,
                                                                              std::uint32_t& next_off, ConstEnv const* const_env,
                                                                              UfcsReceiverMatch expected_match, types::TypePtr expected_type = nullptr)
        {
            if (!sym.decl || sym.decl->kind != ast::DeclKind::Func)
                return std::nullopt;

            auto const& f = *static_cast<ast::FuncDecl const*>(sym.decl);
            std::vector<types::TypePtr> params;
            params.reserve(f.params.size());
            for (auto const& p : f.params)
                params.push_back(p.type ? get_canonical(p.type->sema) : m_types.m_errort());

            std::size_t num_value_tparams = 0;
            for (auto const& tp : f.template_params)
                if (tp.value_type)
                    ++num_value_tparams;

            auto pack_arity = compute_pack_arity(&f, params.size());
            bool has_func_pack = pack_arity.has_pack;
            std::size_t non_pack_func_params = pack_arity.non_pack_count;
            std::size_t min_required = pack_arity.min_required;

            std::size_t actual_count = arg_exprs.size() + 1;
            if (!has_func_pack)
            {
                if (params.size() + num_value_tparams != actual_count)
                {
                    auto loc = format_source_location(f.range);
                    error(range, "argument count mismatch for UFCS call to `{}`: expected {}, got {}", f.name, params.size() + num_value_tparams, actual_count);
                    m_diag.note(f.range, "declared at {}", loc);
                    return std::nullopt;
                }
            }
            else
            {
                if (actual_count < min_required + num_value_tparams)
                {
                    auto loc = format_source_location(f.range);
                    error(range, "argument count mismatch for UFCS call to `{}`: expected at least {}, got {}", f.name, min_required + num_value_tparams,
                          actual_count);
                    m_diag.note(f.range, "declared at {}", loc);
                    return std::nullopt;
                }
            }

            infer::TemplateBindings b{m_types};

            auto param0 = b.substitute(params[0]);
            auto receiver = analyze_expr(mod, nullptr, scope, object, loop_depth, next_off, param0, const_env);
            if (has_error(receiver.type))
            {
                error(range, "receiver type mismatch for UFCS call to `{}`: expected `{}`", f.name, format_type_str(param0));
                return std::nullopt;
            }

            auto match = match_ufcs_receiver(receiver, param0);
            if (!match || match->first != expected_match)
                return std::nullopt;

            for (std::size_t vi = 0; vi < num_value_tparams; ++vi)
            {
                ast::TemplateParam const* vtparam = nullptr;
                {
                    std::size_t idx = 0;
                    for (auto const& tp : f.template_params)
                        if (tp.value_type)
                        {
                            if (idx == vi)
                            {
                                vtparam = &tp;
                                break;
                            }
                            ++idx;
                        }
                }

                if (!vtparam || !vtparam->value_type || !vtparam->value_type->sema.canonical)
                    return std::nullopt;

                auto vt_type = get_canonical(vtparam->value_type->sema);
                auto r = analyze_expr(mod, nullptr, scope, *arg_exprs[vi], loop_depth, next_off, vt_type, const_env);
                if (has_error(r.type))
                    return std::nullopt;

                if (r.constant)
                {
                    auto* pty = m_types.template_param_t(const_cast<ast::TemplateParam*>(vtparam), vtparam->name, static_cast<std::uint32_t>(vi));
                    if (pty)
                        b.bind_value(static_cast<types::TemplateParamType const*>(pty), *r.constant);
                }
            }

            std::size_t func_arg_start = num_value_tparams;
            std::size_t func_arg_count = arg_exprs.size() - num_value_tparams;

            std::vector<detail::ExprResult> args;
            args.reserve(func_arg_count);
            std::vector<types::TypePtr> pack_arg_types;
            std::vector<comptime::Value> pack_arg_values;

            std::size_t non_pack_after_receiver = non_pack_func_params > 0 ? non_pack_func_params - 1 : 0;
            for (std::size_t i = 0; i < non_pack_after_receiver; ++i)
            {
                auto param_ty = b.substitute(params[i + 1]);
                auto r = analyze_expr(mod, nullptr, scope, *arg_exprs[func_arg_start + i], loop_depth, next_off, param_ty, const_env);
                if (has_error(r.type))
                    return std::nullopt;

                args.push_back(r);
            }

            if (has_func_pack)
            {
                auto pack_param_ty = b.substitute(params.back());
                for (std::size_t i = non_pack_after_receiver; i < func_arg_count; ++i)
                {
                    types::TypePtr expected_ty = nullptr;
                    if (auto const* pt = types::type_cast<types::TypePackType>(pack_param_ty))
                        expected_ty = b.substitute(pt->element);
                    else if (auto const* tp = types::type_cast<types::TemplateParamType>(pack_param_ty))
                        expected_ty = tp;

                    auto r = analyze_expr(mod, nullptr, scope, *arg_exprs[func_arg_start + i], loop_depth, next_off, expected_ty, const_env);
                    if (has_error(r.type))
                        return std::nullopt;

                    args.push_back(r);
                    pack_arg_types.push_back(r.type);
                    if (r.constant && r.constant->kind() == comptime::Value::Kind::String)
                        pack_arg_values.push_back(*r.constant);
                    else
                        pack_arg_values.push_back(comptime::Value{});
                }
            }

            std::vector<types::TypePtr> actuals;
            actuals.reserve(args.size() + 1);
            actuals.push_back(match->second);
            for (auto const& a : args)
                actuals.push_back(a.type);

            std::vector<types::TypePtr> deduce_params;
            deduce_params.reserve(non_pack_func_params + (has_func_pack ? 1 : 0));
            for (std::size_t i = 0; i < non_pack_func_params; ++i)
                deduce_params.push_back(params[i]);

            if (has_func_pack)
            {
                auto pack_elem = params.back();
                if (auto const* pt = types::type_cast<types::TypePackType>(params.back()))
                    pack_elem = pt->element;
                deduce_params.push_back(m_types.type_pack_t(pack_elem));
            }

            auto deduce_result = b.deduce_function(deduce_params, actuals);
            if (!deduce_result)
            {
                bool reported = false;
                for (std::size_t i = 0; i < actuals.size() && i < deduce_params.size(); ++i)
                {
                    auto param_ty = b.substitute(deduce_params[i]);
                    if (actuals[i] != param_ty && !has_error(actuals[i]) && !has_error(param_ty))
                    {
                        if (i == 0)
                            error(range, "receiver type mismatch for UFCS call to `{}`: expected `{}`, found `{}`", f.name, format_type_str(param_ty),
                                  format_type_str(actuals[i]));
                        else
                            error(range, "argument {} of UFCS call to `{}` has wrong type: expected `{}`, found `{}`", i, f.name, format_type_str(param_ty),
                                  format_type_str(actuals[i]));

                        reported = true;
                        break;
                    }
                }
                if (!reported)
                {
                    auto loc = format_source_location(f.range);
                    error(range, "call argument mismatch for UFCS call to `{}`: {}", f.name,
                          deduce_result.detail.empty() ? "template argument deduction failed" : deduce_result.detail);
                    m_diag.note(f.range, "declared at {}", loc);
                }
                else
                {
                    auto loc = format_source_location(f.range);
                    m_diag.note(f.range, "declared at {}", loc);
                }

                return std::nullopt;
            }

            if (expected_type && !f.template_params.empty() && f.return_type)
            {
                auto return_ty = get_canonical(f.return_type->sema);
                if (return_ty && contains_template_param(return_ty))
                    std::ignore = b.deduce(return_ty, expected_type);
            }

            object.sema.implicit_addr_of = false;
            object.sema.implicit_deref = false;
            switch (match->first)
            {
                case UfcsReceiverMatch::Exact:
                    break;
                case UfcsReceiverMatch::AutoRef:
                case UfcsReceiverMatch::AutoRefConst:
                case UfcsReceiverMatch::AutoRefQualMismatch:
                    object.sema.implicit_addr_of = true;
                    break;
                case UfcsReceiverMatch::AutoDeref:
                    object.sema.implicit_deref = true;
                    break;
                case UfcsReceiverMatch::None:
                    return std::nullopt;
            }

            if (has_func_pack && !pack_arg_types.empty())
            {
                if (!f.template_params.empty() && f.template_params.back().is_pack)
                {
                    auto const& tp = f.template_params.back();
                    auto* pack_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name,
                                                             static_cast<std::uint32_t>(f.template_params.size() - 1));
                    if (pack_ty)
                    {
                        bool has_any_value = std::ranges::any_of(pack_arg_values, [](comptime::Value const& v) { return v.kind() != comptime::Value::Kind::Null; });
                        if (has_any_value)
                            std::ignore = b.bind_pack(static_cast<types::TemplateParamType const*>(pack_ty), pack_arg_types, pack_arg_values);
                        else
                            std::ignore = b.bind_pack(static_cast<types::TemplateParamType const*>(pack_ty), pack_arg_types);
                    }
                }
            }

            detail::CommittedSpecialization committed_spec{};
            if (!f.template_params.empty())
                committed_spec = commit_specialization(mod, f, b, f.range);

            detail::ExprResult out{};
            out.type = b.substitute(f.return_type ? get_canonical(f.return_type->sema) : m_types.m_voidt());
            out.resolved_decl = &f;
            out.spec_commit = committed_spec;
            return out;
        }

        [[nodiscard]] static bool dominates(RankedCandidate const& a, RankedCandidate const& b) noexcept
        {
            bool better = false;
            for (std::size_t i = 0; i < a.ranks.size(); ++i)
            {
                if (std::to_underlying(a.ranks[i]) > std::to_underlying(b.ranks[i]))
                    return false;

                better |= std::to_underlying(a.ranks[i]) < std::to_underlying(b.ranks[i]);
            }
            return better;
        }

        [[nodiscard]] std::optional<std::size_t> choose_best_candidate(std::span<RankedCandidate const> candidates) const
        {
            if (candidates.empty())
                return std::nullopt;

            std::vector<std::size_t> best;
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                bool dominated = false;
                for (std::size_t j = 0; j < candidates.size(); ++j)
                {
                    if (i == j)
                        continue;

                    if (dominates(candidates[j], candidates[i]))
                    {
                        dominated = true;
                        break;
                    }
                }
                if (!dominated)
                    best.push_back(i);
            }

            return best.size() == 1 ? std::optional<std::size_t>{best.front()} : std::nullopt;
        }

        [[nodiscard]] std::optional<detail::ExprResult> invoke_ranked_candidate(ModuleInfo& mod, Scope& scope, Symbol const& sym,
                                                                                std::span<ast::Expr* const> arg_exprs, sm::SourceRange range, int loop_depth,
                                                                                std::uint32_t& next_off, ConstEnv const* const_env,
                                                                                types::TypePtr expected_type = nullptr)
        {
            if (!sym.decl || sym.decl->kind != ast::DeclKind::Func)
                return std::nullopt;

            auto& f = *static_cast<ast::FuncDecl const*>(sym.decl);
            auto r = invoke_function(mod, scope, f, arg_exprs, range, loop_depth, next_off, const_env, false, expected_type);
            if (!r.type || r.type->kind == types::TypeKind::Error)
                return std::nullopt;

            r.resolved_decl = sym.decl;
            return r;
        }

        [[nodiscard]] std::optional<detail::ExprResult> invoke_explicit_ranked_candidate(ModuleInfo& mod, Scope& scope, Symbol const* sym,
                                                                                         types::FuncPtrType const* fp,
                                                                                         detail::CommittedSpecialization const& spec,
                                                                                         std::span<ast::Expr* const> arg_exprs, sm::SourceRange range,
                                                                                         int loop_depth, std::uint32_t& next_off, ConstEnv const* const_env)
        {
            auto r = invoke_funcptr(mod, scope, fp, arg_exprs, range, loop_depth, next_off, const_env);
            if (!r.type || r.type->kind == types::TypeKind::Error)
                return std::nullopt;

            r.resolved_decl = sym->decl;
            r.spec_commit = spec;
            return r;
        }

        [[nodiscard]] std::optional<infer::TemplateBindings> make_bindings(types::TypePtr ty)
        {
            if (!ty)
                return std::nullopt;

            std::vector<types::TypePtr> args;
            std::vector<ast::TemplateParam const*> template_params;

            if (ty->kind == types::TypeKind::Nominal)
                ty = static_cast<types::NominalType const*>(ty)->underlying;

            switch (ty->kind)
            {
                case types::TypeKind::Struct: {
                    auto const* st = static_cast<types::StructType const*>(ty);
                    auto const* decl = reinterpret_cast<ast::StructDecl const*>(st->decl);
                    template_params.reserve(decl->template_params.size());
                    for (auto const& tp : decl->template_params)
                        template_params.push_back(&tp);

                    args.assign(st->template_args.begin(), st->template_args.end());
                    break;
                }
                case types::TypeKind::Enum: {
                    auto const* et = static_cast<types::EnumType const*>(ty);
                    auto const* decl = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                    template_params.reserve(decl->template_params.size());
                    for (auto const& tp : decl->template_params)
                        template_params.push_back(&tp);

                    args.assign(et->template_args.begin(), et->template_args.end());
                    break;
                }
                default:
                    return std::nullopt;
            }

            if (template_params.empty())
                return std::nullopt;
            if (template_params.size() != args.size())
                return std::nullopt;

            std::optional<infer::TemplateBindings> bindings;
            bindings.emplace(m_types);
            for (std::size_t i = 0; i < template_params.size(); ++i)
            {
                auto const& tp = *template_params[i];
                auto param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                if (auto r = bindings->deduce(param_ty, args[i]); !r)
                    return std::nullopt;
            }

            return bindings;
        }

        [[nodiscard]] std::optional<Layout> layout_of(types::TypePtr ty)
        {
            std::unordered_set<types::TypePtr> seen;
            return layout_of_impl(ty, seen);
        }

        [[nodiscard]] std::optional<Layout> layout_of_impl(types::TypePtr ty, std::unordered_set<types::TypePtr>& seen)
        {
            if (!ty)
                return std::nullopt;

            if (!seen.insert(ty).second)
                return std::nullopt;

            struct SeenGuard
            {
                std::unordered_set<types::TypePtr>& seen;
                types::TypePtr ty;
                ~SeenGuard() { seen.erase(ty); }
            } guard{seen, ty};
            std::ignore = guard;

            switch (ty->kind)
            {
                case types::TypeKind::Void:
                    return Layout{0, 1};
                case types::TypeKind::Bool:
                case types::TypeKind::Int:
                case types::TypeKind::Float:
                case types::TypeKind::Char:
                case types::TypeKind::NullT:
                case types::TypeKind::Pointer:
                case types::TypeKind::Slice:
                case types::TypeKind::Range:
                case types::TypeKind::RangeInclusive:
                case types::TypeKind::FuncPtr:
                    return Layout{ty->byte_size, std::max<std::uint32_t>(1, ty->byte_align)};
                case types::TypeKind::Array: {
                    auto const* a = static_cast<types::ArrayType const*>(ty);
                    auto elem = layout_of_impl(a->element, seen);
                    if (!elem)
                        return std::nullopt;
                    return Layout{elem->size * a->count, elem->align};
                }
                case types::TypeKind::Fam:
                case types::TypeKind::RuntimeArray:
                case types::TypeKind::TemplateParam:
                case types::TypeKind::TypePack:
                case types::TypeKind::Error:
                    return std::nullopt;
                case types::TypeKind::Struct:
                case types::TypeKind::Union:
                case types::TypeKind::Enum:
                    return layout_of_nominal(ty, seen);
                case types::TypeKind::Nominal:
                    return layout_of_impl(static_cast<types::NominalType const*>(ty)->underlying, seen);
            }

            return std::nullopt;
        }

        [[nodiscard]] std::optional<Layout> layout_of_nominal(types::TypePtr ty, std::unordered_set<types::TypePtr>& seen)
        {
            if (!ty)
                return std::nullopt;

            std::optional<infer::TemplateBindings> bindings;
            if (auto b = make_bindings(ty))
                bindings.emplace(std::move(*b));

            if (ty->kind == types::TypeKind::Enum)
            {
                auto* et_mut = const_cast<types::EnumType*>(static_cast<types::EnumType const*>(ty));
                ensure_tagged_enum_complete(m_types, m_alloc, et_mut);

                auto const* et = static_cast<types::EnumType const*>(ty);
                auto const* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                auto backing = ed->backing_type ? get_canonical(ed->backing_type->sema) : nullptr;
                if (bindings)
                    backing = bindings->substitute(backing);

                if (backing)
                {
                    auto lb = layout_of_impl(backing, seen);
                    if (lb)
                        return lb;
                }

                return Layout{ty->byte_size, std::max<std::uint32_t>(1, ty->byte_align)};
            }

            if (!ty->layout_is_default)
                return Layout{ty->byte_size, std::max<std::uint32_t>(1, ty->byte_align)};

            auto fields = record_fields(ty, bindings ? &*bindings : nullptr);
            if (fields.empty() && ty->kind == types::TypeKind::Struct)
                return Layout{0, 1};

            if (ty->kind == types::TypeKind::Union)
            {
                std::uint64_t size{};
                std::uint32_t align{1};
                for (auto const& field : fields)
                {
                    auto l = layout_of_impl(field.type, seen);
                    if (!l)
                        return std::nullopt;
                    align = std::max(align, l->align);
                    size = std::max(size, l->size);
                }
                return Layout{align_up(size, align), align};
            }

            std::uint64_t size{};
            std::uint32_t align{1};
            bool has_fam = false;
            for (auto const& field : fields)
            {
                if (types::is_fam_type(field.type))
                {
                    has_fam = true;
                    auto* elem = types::fam_element(field.type);
                    if (elem)
                        align = std::max(align, elem->byte_align);

                    size = align_up(size, align);
                    continue;
                }

                auto l = layout_of_impl(field.type, seen);
                if (!l)
                    return std::nullopt;
                align = std::max(align, l->align);
                size = align_up(size, l->align);
                size += l->size;
            }

            if (has_fam)
                return Layout{size, align};

            return Layout{align_up(size, align), align};
        }

        [[nodiscard]] std::vector<infer::RecordField> record_fields(types::TypePtr ty, infer::TemplateBindings const* bindings)
        {
            std::vector<infer::RecordField> fields;
            if (!ty)
                return fields;

            auto append = [&](auto const& record) {
                fields.reserve(record.fields.size());
                for (auto const& f : record.fields)
                {
                    auto fty = f.type ? get_canonical(f.type->sema) : nullptr;
                    if (bindings)
                        fty = bindings->substitute(fty);
                    fields.push_back({f.name, fty});
                }
            };

            switch (ty->kind)
            {
                case types::TypeKind::Struct:
                    append(*reinterpret_cast<ast::StructDecl const*>(static_cast<types::StructType const*>(ty)->decl));
                    break;
                case types::TypeKind::Union:
                    append(*reinterpret_cast<ast::UnionDecl const*>(static_cast<types::UnionType const*>(ty)->decl));
                    break;
                case types::TypeKind::Nominal:
                    return record_fields(static_cast<types::NominalType const*>(ty)->underlying, bindings);
                default:
                    break;
            }

            return fields;
        }

        [[nodiscard]] types::TypePtr substitute_in_nominal_context(types::TypePtr ty, types::TypePtr context)
        {
            if (!context)
                return ty;

            auto bindings = make_bindings(context);
            if (!bindings)
                return ty;

            return bindings->substitute(ty);
        }

        [[nodiscard]] static std::optional<comptime::BinaryOp> token_to_arith_binop(lex::TokenKind op) noexcept { return const_eval::token_to_arith_binop(op); }

        [[nodiscard]] static std::optional<comptime::BinaryOp> token_to_cmp_binop(lex::TokenKind op) noexcept { return const_eval::token_to_cmp_binop(op); }

        comptime::Value const* fold_int_binary(lex::TokenKind op, std::int64_t lhs, std::int64_t rhs, types::TypePtr out_type, sm::SourceRange range)
        {
            auto result = const_eval::fold_int_binary(op, lhs, rhs, out_type);
            if (result)
                return make_value(std::move(*result));

            if (op == lex::TokenKind::Plus || op == lex::TokenKind::Minus || op == lex::TokenKind::Star)
                error(range, "integer overflow in constant expression");
            else if (op == lex::TokenKind::LtLt || op == lex::TokenKind::GtGt)
                error(range, "shift amount out of range in constant expression");

            return nullptr;
        }

        comptime::Value const* fold_int_cmp(lex::TokenKind op, std::int64_t lhs, std::int64_t rhs, types::TypePtr out_type)
        {
            auto result = const_eval::fold_int_cmp(op, lhs, rhs, out_type);
            if (result)
                return make_value(std::move(*result));

            return nullptr;
        }

        [[nodiscard]] static bool can_assign_return(types::TypePtr expected, types::TypePtr got) noexcept
        {
            if (!expected || !got)
                return true;
            if (expected == got)
                return true;

            if (expected->kind == types::TypeKind::Float && got->kind == types::TypeKind::Float)
                return true;

            if (expected->kind == types::TypeKind::Int && got->kind == types::TypeKind::Int)
            {
                auto const* e = static_cast<types::IntType const*>(expected);
                auto const* g = static_cast<types::IntType const*>(got);
                if (e->is_pointer_sized || g->is_pointer_sized)
                    return false;

                return e->bits >= g->bits;
            }

            if (expected->kind == types::TypeKind::Float && got->kind == types::TypeKind::Int)
                return true;

            if (expected->kind == types::TypeKind::Pointer && got->kind == types::TypeKind::NullT)
                return true;

            if (expected->kind == types::TypeKind::Pointer && got->kind == types::TypeKind::Pointer)
            {
                auto const* ep = static_cast<types::PointerType const*>(expected);
                auto const* gp = static_cast<types::PointerType const*>(got);
                return ep->pointee == gp->pointee && ep->pointee_quals == gp->pointee_quals;
            }

            if (expected->kind == types::TypeKind::Slice && got->kind == types::TypeKind::Slice)
            {
                auto const* es = static_cast<types::SliceType const*>(expected);
                auto const* gs = static_cast<types::SliceType const*>(got);
                return es->element == gs->element && es->element_quals == gs->element_quals;
            }

            if (expected->kind == types::TypeKind::Slice && got->kind == types::TypeKind::Array)
            {
                auto const* es = static_cast<types::SliceType const*>(expected);
                auto const* ga = static_cast<types::ArrayType const*>(got);
                return es->element == ga->element;
            }

            if (expected->kind == types::TypeKind::Pointer)
            {
                auto const* ep = static_cast<types::PointerType const*>(expected);
                if (got->kind == types::TypeKind::Array)
                {
                    auto const* ga = static_cast<types::ArrayType const*>(got);
                    if (ep->pointee == ga->element)
                        return true;
                }
                if (got->kind == types::TypeKind::RuntimeArray)
                {
                    auto const* gra = static_cast<types::RuntimeArrayType const*>(got);
                    if (ep->pointee == gra->element)
                        return true;
                }
            }

            return false;
        }

        void analyze_module(ModuleInfo& mod)
        {
            for (auto* d : mod.tu->decls)
                if (auto* f = ast::node_cast<ast::FuncDecl>(d))
                    analyze_function(mod, *f);
                else if (auto* v = ast::node_cast<ast::VarDecl>(d))
                    analyze_var(mod, *v);
                else if (auto* sd = ast::node_cast<ast::StructDecl>(d))
                    analyze_struct_fields(mod, *sd);
                else if (auto* ud = ast::node_cast<ast::UnionDecl>(d))
                    analyze_union_fields(mod, *ud);
                else if (auto* ed = ast::node_cast<ast::EnumDecl>(d))
                    analyze_enum_fields(mod, *ed);
        }

        [[nodiscard]] static bool is_template_specialization_body(ast::FuncDecl const& fn)
        {
            if (!fn.body)
                return false;
            auto body_has_inst_expr = [](ast::Expr const* e) -> bool {
                if (!e)
                    return false;
                if (e->sema.is_from_instantiation)
                    return true;
                return false;
            };

            for (auto* s : fn.body->stmts)
            {
                if (!s)
                    continue;
                if (s->kind == ast::StmtKind::Expr)
                {
                    if (body_has_inst_expr(static_cast<ast::ExprStmt const*>(s)->expr))
                        return true;
                }
                else if (s->kind == ast::StmtKind::Return)
                {
                    if (body_has_inst_expr(static_cast<ast::ReturnStmt const*>(s)->value))
                        return true;
                }
            }
            if (body_has_inst_expr(fn.body->tail))
                return true;
            return false;
        }

        void analyze_function(ModuleInfo& mod, ast::FuncDecl& fn)
        {
            if (fn.sema.storage != ast::StorageClass::Unresolved)
                return;

            auto* root = make_scope(ScopeKind::Function, nullptr);
            auto* root_consts = make_const_env(nullptr);
            std::uint32_t frame_off = 0;
            bool is_spec_body = is_template_specialization_body(fn);
            for (std::size_t i = 0; i < fn.params.size(); ++i)
            {
                auto& p = fn.params[i];
                p.sema.storage = ast::StorageClass::Param;
                p.sema.param_index = static_cast<std::uint32_t>(i);

                if (p.type && p.type->kind == ast::TypeKind::Qualified)
                {
                    auto const* qt = static_cast<ast::QualifiedType const*>(p.type);
                    if (ast::has_qual(qt->quals, ast::Qual::Restrict))
                    {
                        auto* resolved = get_canonical(p.type->sema);
                        if (resolved && !types::type_cast<types::PointerType>(resolved) && !types::type_cast<types::SliceType>(resolved))
                            error(p.range, "restrict qualifier is only allowed on pointer or slice types");
                    }
                }

                auto type = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : nullptr;

                if (!is_spec_body || !type || type->kind != types::TypeKind::Void)
                    check_type_valid_for_value(p.range, type, "parameter type");
                if (type && mod.own_scope)
                    check_type_constraints_in_type(mod, *mod.own_scope, type, p.range);
                p.sema.frame_offset = allocate_frame_slot(frame_off, type);
                auto* synthetic = make_param_decl(p);

                if (i == 0 || fn.params[i - 1].name != p.name)
                    define_local(*root, synthetic);
            }

            if (!fn.template_params.empty())
            {
                fn.sema.storage = ast::StorageClass::ModuleGlobal;
                return;
            }

            {
                auto ret_ty = fn.return_type ? get_canonical(fn.return_type->sema) : nullptr;
                check_type_valid_for_value(fn.range, ret_ty, "return type");
                if (ret_ty && mod.own_scope)
                    check_type_constraints_in_type(mod, *mod.own_scope, ret_ty, fn.name_range);
            }

            if (fn.body)
            {
                auto res = analyze_block(mod, &fn, *root, *fn.body, 0, frame_off, nullptr, root_consts);
                fn.sema.is_diverging = !res.falls_through;
                auto ret_ty = fn.return_type ? get_canonical(fn.return_type->sema) : nullptr;
                if (ret_ty && ret_ty != m_types.m_voidt() && res.falls_through)
                    error(fn.range, "missing return in function `{}`", fn.name);
            }

            fn.sema.storage = ast::StorageClass::ModuleGlobal;
        }

        void analyze_var(ModuleInfo& mod, ast::VarDecl& var)
        {
            var.sema.storage = ast::StorageClass::ModuleGlobal;

            if (var.type && var.type->kind == ast::TypeKind::Qualified)
            {
                auto const* qt = static_cast<ast::QualifiedType const*>(var.type);
                if (ast::has_qual(qt->quals, ast::Qual::Restrict))
                {
                    auto* resolved = get_canonical(var.type->sema);
                    if (resolved && !types::type_cast<types::PointerType>(resolved) && !types::type_cast<types::SliceType>(resolved))
                        error(var.range, "restrict qualifier is only allowed on pointer or slice types");
                }
            }

            auto* var_type = var.type && var.type->sema.canonical ? get_canonical(var.type->sema) : nullptr;

            if (types::type_cast<types::RuntimeArrayType>(var_type))
            {
                error(var.range, "runtime-sized array is only allowed for local variables, not globals");
                return;
            }

            if (auto const* at = types::type_cast<types::ArrayType>(var_type))
            {
                if (at->count == 0 && var.type && var.type->kind == ast::TypeKind::Array)
                {
                    auto const* arr_type = static_cast<ast::ArrayType const*>(var.type);
                    if (arr_type->size && arr_type->size->kind != ast::ExprKind::IntLiteral)
                    {
                        error(var.range, "global array size must be a constant integer");
                        return;
                    }
                }
            }

            if (!var.is_extern)
                check_type_valid_for_value(var.range, var_type, "variable type");

            if (var_type && mod.own_scope)
                check_type_constraints_in_type(mod, *mod.own_scope, var_type, var.range);

            if (var.init && mod.own_scope)
                std::ignore = analyze_expr(mod, nullptr, *mod.own_scope, *var.init, 0, dummy_offset(), var_type, nullptr);
        }

        void analyze_struct_fields(ModuleInfo& mod, ast::StructDecl& sd)
        {
            if (!mod.own_scope)
                return;

            for (auto& f : sd.fields)
            {
                if (!f.type || !f.type->sema.canonical)
                    continue;

                auto* ft = get_canonical(f.type->sema);
                if (ft)
                    check_type_constraints_in_type(mod, *mod.own_scope, ft, f.type->range);
            }
        }

        void analyze_union_fields(ModuleInfo& mod, ast::UnionDecl& ud)
        {
            if (!mod.own_scope)
                return;

            for (auto& f : ud.fields)
            {
                if (!f.type || !f.type->sema.canonical)
                    continue;

                auto* ft = get_canonical(f.type->sema);
                if (ft)
                    check_type_constraints_in_type(mod, *mod.own_scope, ft, f.type->range);
            }
        }

        void analyze_enum_fields(ModuleInfo& mod, ast::EnumDecl& ed)
        {
            if (!mod.own_scope)
                return;

            for (auto& v : ed.variants)
            {
                for (auto* p : v.payload)
                {
                    if (!p || !p->sema.canonical)
                        continue;

                    auto* pt = get_canonical(p->sema);
                    if (pt)
                        check_type_constraints_in_type(mod, *mod.own_scope, pt, p->range);
                }
            }
        }

        static std::uint32_t& dummy_offset()
        {
            static std::uint32_t x{};
            return x;
        }

        ast::VarDecl* make_param_decl(ast::FuncParam& p)
        {
            if (p.synthetic_decl)
            {
                const_cast<ast::VarDecl*>(p.synthetic_decl)->sema.frame_offset = p.sema.frame_offset;
                const_cast<ast::VarDecl*>(p.synthetic_decl)->sema.param_index = p.sema.param_index;
                return const_cast<ast::VarDecl*>(p.synthetic_decl);
            }
            auto* v = m_ast_ctx.make<ast::VarDecl>(p.range, p.name, p.range);
            v->type = p.type;
            v->sema.storage = ast::StorageClass::Param;
            v->sema.param_index = p.sema.param_index;
            v->sema.frame_offset = p.sema.frame_offset;
            p.synthetic_decl = v;
            return v;
        }

        ast::VarDecl* make_local_decl(std::string_view name, sm::SourceRange range, ast::TypeExpr* type_node, ast::StorageClass storage,
                                      std::uint32_t frame_off)
        {
            auto* v = m_ast_ctx.make<ast::VarDecl>(range, name, range);
            v->type = type_node;
            v->sema.storage = storage;
            v->sema.frame_offset = frame_off;
            return v;
        }

        Scope* make_scope(ScopeKind kind, Scope const* parent)
        {
            auto* p = m_alloc.allocate_object<Scope>();
            return std::construct_at(p, kind, parent, m_alloc);
        }

        void define_local(Scope& scope, ast::VarDecl* decl)
        {
            Symbol s{};
            s.name = decl->name;
            s.kind = SymbolKind::Variable;
            s.decl = decl;
            s.definition_range = decl->range;
            Symbol const* existing = nullptr;
            if (scope.define_variable(s, &existing) == DefineResult::Conflict)
                m_diag.emit(existing ? diag::Diagnostic{diag::Severity::Error, std::format("redefinition of `{}`", decl->name)}
                                           .primary(decl->range)
                                           .secondary(existing->definition_range, "previous declaration here")
                                     : diag::Diagnostic{diag::Severity::Error, std::format("redefinition of `{}`", decl->name)}.primary(decl->range));
        }

        struct PatternBinding
        {
            std::string_view name;
            sm::SourceRange range;
            ast::VarDecl* decl{};
            types::TypePtr type{};
        };

        struct PatternValidation
        {
            bool ok{true};
            std::pmr::vector<PatternBinding> bindings;

            PatternValidation(std::pmr::polymorphic_allocator<> alloc) : bindings(alloc) {}
        };

        struct IntRange
        {
            std::uint64_t lo;
            std::uint64_t hi;
            bool operator==(IntRange const& o) const noexcept { return lo == o.lo && hi == o.hi; }
        };

        struct IntRangeHash
        {
            std::size_t operator()(IntRange const& r) const noexcept { return static_cast<std::size_t>(r.lo) ^ (static_cast<std::size_t>(r.hi) << 16); }
        };

        [[nodiscard]] static int_domain::Domain char_domain() noexcept { return {0, 255}; }

        [[nodiscard]] static std::string ordinal_to_display(std::uint64_t ordinal, types::IntType const& ty)
        {
            if (ty.is_signed)
                return std::format("{}", int_domain::ordinal_to_raw_bits(ordinal, ty));
            else
                return std::format("{}", ordinal & int_domain::mask_for_bits(ty.bits));
        }

        [[nodiscard]] static std::vector<IntRange> normalized_intervals(std::pmr::unordered_set<std::int64_t> const& int_literals,
                                                                        std::pmr::unordered_set<std::uint32_t> const& char_literals,
                                                                        std::pmr::unordered_set<IntRange, IntRangeHash> const& int_ranges,
                                                                        types::TypePtr matched_type)
        {
            std::vector<IntRange> intervals;
            if (matched_type && matched_type->kind == types::TypeKind::Int)
            {
                auto const* it = static_cast<types::IntType const*>(matched_type);
                for (auto v : int_literals)
                {
                    auto ord = int_domain::to_ordinal(v, *it);
                    intervals.push_back({ord, ord});
                }
            }
            else if (matched_type && matched_type->kind == types::TypeKind::Char)
            {
                for (auto v : char_literals)
                    intervals.push_back({static_cast<std::uint64_t>(v), static_cast<std::uint64_t>(v)});
            }

            for (auto const& r : int_ranges)
                intervals.push_back(r);

            if (intervals.empty())
                return intervals;

            std::sort(intervals.begin(), intervals.end(), [](IntRange const& a, IntRange const& b) { return a.lo < b.lo; });

            std::vector<IntRange> merged;
            merged.push_back(intervals[0]);
            for (std::size_t i = 1; i < intervals.size(); ++i)
            {
                auto& last = merged.back();
                if (intervals[i].lo <= last.hi + 1)
                    last.hi = std::max(last.hi, intervals[i].hi);
                else
                    merged.push_back(intervals[i]);
            }

            return merged;
        }

        [[nodiscard]] static bool intervals_cover_domain(std::vector<IntRange> const& intervals, int_domain::Domain domain) noexcept
        {
            std::uint64_t pos = domain.lo;
            for (auto const& r : intervals)
            {
                if (r.lo > pos)
                    return false;

                if (r.hi >= domain.hi)
                    return true;

                if (r.hi == std::numeric_limits<std::uint64_t>::max())
                    return false;

                pos = r.hi + 1;
            }
            return false;
        }

        [[nodiscard]] static std::optional<IntRange> find_first_gap(std::vector<IntRange> const& intervals, int_domain::Domain domain) noexcept
        {
            if (intervals.empty())
            {
                if (domain.lo <= domain.hi)
                    return IntRange{domain.lo, domain.hi};

                return std::nullopt;
            }

            std::uint64_t pos = domain.lo;
            for (auto const& r : intervals)
            {
                if (r.lo > pos)
                    return IntRange{pos, r.lo - 1};

                if (r.hi >= domain.hi)
                    return std::nullopt;

                if (r.hi == std::numeric_limits<std::uint64_t>::max())
                    return std::nullopt;

                pos = r.hi + 1;
                if (pos > domain.hi || pos == 0)
                    return std::nullopt;
            }
            if (pos <= domain.hi)
                return IntRange{pos, domain.hi};

            return std::nullopt;
        }

        struct PatternCoverage
        {
            bool all{};
            bool bool_true{};
            bool bool_false{};
            std::pmr::unordered_set<ast::EnumVariant const*> enum_variants;
            std::pmr::unordered_set<std::int64_t> int_literals;
            std::pmr::unordered_set<std::uint32_t> char_literals;
            std::pmr::unordered_set<IntRange, IntRangeHash> int_ranges;
            bool has_pointer_null{};
            bool has_pointer_nonnull{};

            PatternCoverage(std::pmr::polymorphic_allocator<> alloc) : enum_variants(alloc), int_literals(alloc), char_literals(alloc), int_ranges(alloc) {}
        };

        [[nodiscard]] static bool is_ordered_scalar(types::TypePtr ty) noexcept
        {
            if (!ty)
                return false;

            switch (ty->kind)
            {
                case types::TypeKind::Int:
                case types::TypeKind::Char:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] static std::string_view pattern_name(ast::Pattern const& p) noexcept
        {
            switch (p.kind)
            {
                case ast::PatternKind::Binding:
                    return static_cast<ast::BindingPattern const&>(p).name;
                case ast::PatternKind::EnumDestructure: {
                    auto const& e = static_cast<ast::EnumDestructurePattern const&>(p);
                    return e.variant_path.is_simple() ? e.variant_path.simple_name() : e.variant_path.segments.back().name;
                }
                case ast::PatternKind::StructDestructure: {
                    auto const& s = static_cast<ast::StructDestructurePattern const&>(p);
                    return s.type_path.is_simple() ? s.type_path.simple_name() : s.type_path.segments.back().name;
                }
                default:
                    return {};
            }
        }

        [[nodiscard]] ast::EnumDecl const* enum_decl_of(types::TypePtr ty) const noexcept
        {
            auto const* e = types::type_cast<types::EnumType>(ty);
            return e ? reinterpret_cast<ast::EnumDecl const*>(e->decl) : nullptr;
        }

        [[nodiscard]] bool pattern_irrefutable(ModuleInfo& mod, ast::Pattern const& p, types::TypePtr matched_type, Scope& scope, ConstEnv const* const_env)
        {
            return pattern_coverage(mod, p, matched_type, scope, const_env).all;
        }

        [[nodiscard]] PatternCoverage pattern_coverage(ModuleInfo& mod, ast::Pattern const& p, types::TypePtr matched_type, Scope& scope,
                                                       ConstEnv const* const_env)
        {
            PatternCoverage out{m_alloc};
            if (!matched_type || matched_type->kind == types::TypeKind::Error)
                return out;

            auto accumulate_alt = [&](ast::Pattern const& alt) {
                auto cov = pattern_coverage(mod, alt, matched_type, scope, const_env);
                out.all = out.all || cov.all;
                out.bool_true = out.bool_true || cov.bool_true;
                out.bool_false = out.bool_false || cov.bool_false;
                out.enum_variants.insert(cov.enum_variants.begin(), cov.enum_variants.end());
                out.int_literals.insert(cov.int_literals.begin(), cov.int_literals.end());
                out.char_literals.insert(cov.char_literals.begin(), cov.char_literals.end());
                out.int_ranges.insert(cov.int_ranges.begin(), cov.int_ranges.end());
                out.has_pointer_null = out.has_pointer_null || cov.has_pointer_null;
                out.has_pointer_nonnull = out.has_pointer_nonnull || cov.has_pointer_nonnull;
            };

            switch (p.kind)
            {
                case ast::PatternKind::Wildcard:
                    out.all = true;
                    return out;
                case ast::PatternKind::Binding:
                    out.all = true;
                    if (matched_type && matched_type->kind == types::TypeKind::Pointer)
                        out.has_pointer_nonnull = true;
                    return out;
                case ast::PatternKind::Literal: {
                    auto& lit = static_cast<ast::LiteralPattern const&>(p);
                    if (!lit.value)
                        return out;
                    std::uint32_t tmp{};
                    auto value = analyze_expr(mod, nullptr, scope, *lit.value, 0, tmp, matched_type, const_env);
                    if (!value.constant)
                        return out;

                    if (value.constant->kind() == comptime::Value::Kind::Bool)
                    {
                        if (value.constant->get_bool())
                            out.bool_true = true;
                        else
                            out.bool_false = true;
                    }
                    else if (value.constant->kind() == comptime::Value::Kind::Int)
                        out.int_literals.insert(value.constant->get_int());
                    else if (value.constant->kind() == comptime::Value::Kind::Char)
                        out.char_literals.insert(value.constant->get_char());
                    else if (value.constant->kind() == comptime::Value::Kind::Null)
                        out.has_pointer_null = true;

                    return out;
                }
                case ast::PatternKind::Range: {
                    auto& r = static_cast<ast::RangePattern const&>(p);
                    if (!r.start || !r.end)
                        return out;
                    std::uint32_t tmp{};
                    auto start = analyze_expr(mod, nullptr, scope, *r.start, 0, tmp, matched_type, const_env);
                    auto end = analyze_expr(mod, nullptr, scope, *r.end, 0, tmp, matched_type, const_env);
                    if (!start.constant || !end.constant)
                        return out;
                    std::int64_t lo_raw{}, hi_raw{};
                    if (start.constant->kind() == comptime::Value::Kind::Int)
                    {
                        lo_raw = start.constant->get_int();
                        hi_raw = end.constant->get_int();
                    }
                    else if (start.constant->kind() == comptime::Value::Kind::Char)
                    {
                        lo_raw = static_cast<std::int64_t>(start.constant->get_char());
                        hi_raw = static_cast<std::int64_t>(end.constant->get_char());
                    }
                    else
                        return out;
                    if (!r.inclusive)
                        hi_raw -= 1;

                    if (matched_type && matched_type->kind == types::TypeKind::Int)
                    {
                        auto const* it = static_cast<types::IntType const*>(matched_type);
                        auto lo_ord = int_domain::to_ordinal(lo_raw, *it);
                        auto hi_ord = int_domain::to_ordinal(hi_raw, *it);
                        if (lo_ord <= hi_ord)
                            out.int_ranges.insert({lo_ord, hi_ord});
                    }
                    else if (matched_type && matched_type->kind == types::TypeKind::Char)
                    {
                        auto lo_ch = static_cast<std::uint64_t>(lo_raw);
                        auto hi_ch = static_cast<std::uint64_t>(hi_raw);
                        if (lo_ch <= hi_ch)
                            out.int_ranges.insert({lo_ch, hi_ch});
                    }
                    return out;
                }
                case ast::PatternKind::Or: {
                    auto& o = static_cast<ast::OrPattern const&>(p);
                    for (auto* alt : o.alternatives)
                    {
                        if (!alt)
                            continue;
                        accumulate_alt(*alt);
                        if (out.all)
                            return out;
                    }
                    return out;
                }
                case ast::PatternKind::EnumDestructure: {
                    auto& e = static_cast<ast::EnumDestructurePattern const&>(p);
                    auto const* op_enum = types::type_cast<types::EnumType>(matched_type);
                    if (!op_enum || !e.resolved_variant)
                        return out;
                    auto const* enum_decl = enum_decl_of(matched_type);
                    if (!enum_decl || static_cast<void const*>(op_enum->decl) != static_cast<void const*>(enum_decl))
                        return out;

                    bool payload_irrefutable = true;
                    for (std::size_t i = 0; i < e.payload.size(); ++i)
                    {
                        auto payload_ty = e.resolved_variant->payload[i]
                                              ? resolve_payload_type(*enum_decl, e.resolved_variant->payload[i], matched_type, mod, scope)
                                              : m_types.m_errort();
                        if (!e.payload[i] || !pattern_irrefutable(mod, *e.payload[i], payload_ty, scope, const_env))
                        {
                            payload_irrefutable = false;
                            break;
                        }
                    }
                    if (payload_irrefutable)
                        out.enum_variants.insert(e.resolved_variant);

                    return out;
                }
                case ast::PatternKind::StructDestructure: {
                    auto& s = static_cast<ast::StructDestructurePattern const&>(p);
                    auto const* op_struct = types::type_cast<types::StructType>(matched_type);
                    if (!op_struct)
                        return out;

                    auto const* sym = resolve_type_path(*mod.own_scope, s.type_path);
                    if (!sym || !sym->decl || sym->decl->kind != ast::DeclKind::Struct)
                        return out;

                    auto const* struct_decl = static_cast<ast::StructDecl const*>(sym->decl);
                    if (static_cast<void const*>(op_struct->decl) != static_cast<void const*>(struct_decl))
                        return out;

                    for (auto const& field : s.fields)
                    {
                        auto it =
                            std::find_if(struct_decl->fields.begin(), struct_decl->fields.end(), [&](auto const& f) { return f.name == field.field_name; });
                        if (it == struct_decl->fields.end() || !field.pattern)
                            return out;

                        auto field_ty = substitute_in_nominal_context(resolve_type_node(mod, scope, it->type), matched_type);
                        if (!pattern_irrefutable(mod, *field.pattern, field_ty, scope, const_env))
                            return out;
                    }
                    out.all = true;
                    return out;
                }
                case ast::PatternKind::Ref: {
                    auto& r = static_cast<ast::RefPattern const&>(p);
                    if (r.inner)
                        return pattern_coverage(mod, *r.inner, matched_type, scope, const_env);
                    return out;
                }
            }
            return out;
        }

        [[nodiscard]] bool coverage_covers_all(types::TypePtr matched_type, PatternCoverage const& coverage) const noexcept
        {
            if (!matched_type || matched_type->kind == types::TypeKind::Error)
                return false;

            if (coverage.all)
                return true;

            if (matched_type == m_types.m_boolt())
                return coverage.bool_true && coverage.bool_false;

            if (matched_type->kind == types::TypeKind::Enum)
            {
                auto const* decl = enum_decl_of(matched_type);
                if (!decl)
                    return false;

                return std::all_of(decl->variants.begin(), decl->variants.end(), [&](auto const& v) { return coverage.enum_variants.contains(&v); });
            }

            if (matched_type->kind == types::TypeKind::Pointer)
                return coverage.has_pointer_null && coverage.has_pointer_nonnull;

            if (matched_type->kind == types::TypeKind::Int)
            {
                auto const* it = static_cast<types::IntType const*>(matched_type);
                auto domain = int_domain::domain_for(*it);
                auto intervals = normalized_intervals(coverage.int_literals, coverage.char_literals, coverage.int_ranges, matched_type);
                return intervals_cover_domain(intervals, domain);
            }

            if (matched_type->kind == types::TypeKind::Char)
            {
                auto domain = char_domain();
                auto intervals = normalized_intervals(coverage.int_literals, coverage.char_literals, coverage.int_ranges, matched_type);
                return intervals_cover_domain(intervals, domain);
            }

            return false;
        }

        [[nodiscard]] std::string missing_case_message(types::TypePtr matched_type, PatternCoverage const& coverage) const
        {
            if (!matched_type || matched_type->kind == types::TypeKind::Error || coverage.all)
                return {};

            if (matched_type == m_types.m_boolt())
            {
                if (!coverage.bool_true && coverage.bool_false)
                    return "missing case 'true'";
                if (coverage.bool_true && !coverage.bool_false)
                    return "missing case 'false'";
                return "missing cases 'true' and 'false'";
            }

            if (matched_type->kind == types::TypeKind::Enum)
            {
                auto const* decl = enum_decl_of(matched_type);
                if (!decl)
                    return {};

                std::string missing;
                for (auto const& v : decl->variants)
                {
                    if (!coverage.enum_variants.contains(&v))
                    {
                        if (!missing.empty())
                            missing += ", ";
                        missing += std::format("`{}::{}`", decl->name, v.name);
                    }
                }
                if (!missing.empty())
                    return std::format("missing case{} {}", missing.find(',') != std::string::npos ? "s" : "", missing);
                return {};
            }

            if (matched_type->kind == types::TypeKind::Pointer)
            {
                if (!coverage.has_pointer_null)
                    return "missing case 'null'";
                if (!coverage.has_pointer_nonnull)
                    return "missing non-null binding";
                return {};
            }

            if (matched_type->kind == types::TypeKind::Int)
            {
                auto const* it = static_cast<types::IntType const*>(matched_type);
                auto domain = int_domain::domain_for(*it);
                auto intervals = normalized_intervals(coverage.int_literals, coverage.char_literals, coverage.int_ranges, matched_type);
                auto gap = find_first_gap(intervals, domain);
                if (gap)
                {
                    if (gap->lo == gap->hi)
                        return std::format("missing value {}", ordinal_to_display(gap->lo, *it));

                    return std::format("missing range {}..{}", ordinal_to_display(gap->lo, *it), ordinal_to_display(gap->hi, *it));
                }
                return {};
            }

            if (matched_type->kind == types::TypeKind::Char)
            {
                auto domain = char_domain();
                auto intervals = normalized_intervals(coverage.int_literals, coverage.char_literals, coverage.int_ranges, matched_type);
                auto gap = find_first_gap(intervals, domain);
                if (gap)
                {
                    if (gap->lo == gap->hi)
                    {
                        auto cp = static_cast<std::uint32_t>(gap->lo);

                        if (cp >= 32 && cp <= 126)
                            return std::format("missing value '{}'", static_cast<char>(cp));

                        return std::format("missing value {}", gap->lo);
                    }
                    return std::format("missing range {}..{}", gap->lo, gap->hi);
                }
                return {};
            }

            return {};
        }

        [[nodiscard]] bool coverage_subsumes(types::TypePtr matched_type, PatternCoverage const& prior, PatternCoverage const& current) const noexcept
        {
            if (coverage_covers_all(matched_type, prior))
                return true;

            if (current.all)
                return false;
            if (matched_type == m_types.m_boolt())
            {
                if (current.bool_true && !prior.bool_true)
                    return false;
                if (current.bool_false && !prior.bool_false)
                    return false;
                return current.bool_true || current.bool_false;
            }
            if (matched_type && matched_type->kind == types::TypeKind::Enum)
            {
                if (current.enum_variants.empty())
                    return false;

                for (auto const* v : current.enum_variants)
                    if (!prior.enum_variants.contains(v))
                        return false;
                return true;
            }
            if (matched_type && matched_type->kind == types::TypeKind::Pointer)
            {
                if (current.has_pointer_null && !prior.has_pointer_null)
                    return false;
                if (current.has_pointer_nonnull && !prior.has_pointer_nonnull)
                    return false;
                return current.has_pointer_null || current.has_pointer_nonnull;
            }
            if (matched_type && matched_type->kind == types::TypeKind::Int)
            {
                auto prior_intervals = normalized_intervals(prior.int_literals, prior.char_literals, prior.int_ranges, matched_type);
                auto current_intervals = normalized_intervals(current.int_literals, current.char_literals, current.int_ranges, matched_type);
                if (current_intervals.empty())
                    return false;

                for (auto const& ci : current_intervals)
                {
                    bool covered = false;
                    for (auto const& pi : prior_intervals)
                    {
                        if (pi.lo <= ci.lo && pi.hi >= ci.hi)
                        {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered)
                        return false;
                }
                return true;
            }
            if (matched_type && matched_type->kind == types::TypeKind::Char)
            {
                auto prior_intervals = normalized_intervals(prior.int_literals, prior.char_literals, prior.int_ranges, matched_type);
                auto current_intervals = normalized_intervals(current.int_literals, current.char_literals, current.int_ranges, matched_type);
                if (current_intervals.empty())
                    return false;

                for (auto const& ci : current_intervals)
                {
                    bool covered = false;
                    for (auto const& pi : prior_intervals)
                    {
                        if (pi.lo <= ci.lo && pi.hi >= ci.hi)
                        {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered)
                        return false;
                }
                return true;
            }
            return false;
        }

        [[nodiscard]] bool coverage_redundant(ModuleInfo& mod, types::TypePtr matched_type, PatternCoverage const& prior, ast::Pattern const& p, Scope& scope,
                                              ConstEnv const* const_env)
        {
            auto current = pattern_coverage(mod, p, matched_type, scope, const_env);
            return coverage_subsumes(matched_type, prior, current);
        }

        void add_coverage(types::TypePtr matched_type, PatternCoverage& prior, PatternCoverage const& current)
        {
            if (prior.all || current.all)
            {
                prior.all = true;
                prior.bool_true = prior.bool_false = true;
                prior.enum_variants.clear();
                return;
            }

            if (matched_type == m_types.m_boolt())
            {
                prior.bool_true = prior.bool_true || current.bool_true;
                prior.bool_false = prior.bool_false || current.bool_false;
                return;
            }

            if (matched_type && matched_type->kind == types::TypeKind::Enum)
                prior.enum_variants.insert(current.enum_variants.begin(), current.enum_variants.end());

            if (matched_type && matched_type->kind == types::TypeKind::Pointer)
            {
                prior.has_pointer_null = prior.has_pointer_null || current.has_pointer_null;
                prior.has_pointer_nonnull = prior.has_pointer_nonnull || current.has_pointer_nonnull;
            }

            if (matched_type && (matched_type->kind == types::TypeKind::Int || matched_type->kind == types::TypeKind::Char))
            {
                prior.int_literals.insert(current.int_literals.begin(), current.int_literals.end());
                prior.char_literals.insert(current.char_literals.begin(), current.char_literals.end());
                prior.int_ranges.insert(current.int_ranges.begin(), current.int_ranges.end());
            }
        }

        [[nodiscard]] PatternValidation validate_pattern(ModuleInfo& mod, ast::Pattern& p, types::TypePtr matched_type, Scope& scope, ConstEnv const* const_env)
        {
            PatternValidation out{m_alloc};
            p.matched_type = reinterpret_cast<decltype(p.matched_type)>(matched_type);

            auto validate_child = [&](ast::Pattern& child, types::TypePtr child_type) {
                auto r = validate_pattern(mod, child, child_type, scope, const_env);
                if (!r.ok)
                    out.ok = false;

                out.bindings.insert(out.bindings.end(), r.bindings.begin(), r.bindings.end());
            };

            switch (p.kind)
            {
                case ast::PatternKind::Wildcard:
                    break;
                case ast::PatternKind::Binding: {
                    auto& b = static_cast<ast::BindingPattern&>(p);
                    auto* v = make_local_decl(b.name, b.range, nullptr, ast::StorageClass::Local, 0);
                    if (b.by_reference && matched_type)
                    {
                        types::Qual qual = types::Qual::None;

                        if (auto const* pt = types::type_cast<types::PointerType>(matched_type))
                            qual = pt->pointee_quals;
                        else if (auto const* st = types::type_cast<types::SliceType>(matched_type))
                            qual = st->element_quals;

                        auto ptr_ty = m_types.pointer_to(matched_type, qual);
                        register_inferred_local_type(v, ptr_ty);
                        out.bindings.push_back({b.name, b.range, v, ptr_ty});
                    }
                    else
                    {
                        register_inferred_local_type(v, matched_type ? matched_type : m_types.m_errort());
                        out.bindings.push_back({b.name, b.range, v, matched_type ? matched_type : m_types.m_errort()});
                    }
                    break;
                }
                case ast::PatternKind::Ref: {
                    auto& r = static_cast<ast::RefPattern&>(p);
                    if (!r.inner)
                    {
                        out.ok = false;
                        break;
                    }
                    if (r.inner->kind == ast::PatternKind::Binding)
                    {
                        auto& b = static_cast<ast::BindingPattern&>(*r.inner);
                        b.by_reference = true;
                        auto child = validate_pattern(mod, *r.inner, matched_type, scope, const_env);
                        out.ok = child.ok;
                        out.bindings = std::move(child.bindings);
                    }
                    else
                    {
                        error(r.range, "`&` is only meaningful on binding names within patterns");
                        out.ok = false;

                        std::ignore = validate_pattern(mod, *r.inner, matched_type, scope, const_env);
                    }
                    break;
                }
                case ast::PatternKind::Literal: {
                    auto& lit = static_cast<ast::LiteralPattern&>(p);
                    if (!lit.value)
                        break;

                    std::uint32_t tmp{};
                    auto value = analyze_expr(mod, nullptr, scope, *lit.value, 0, tmp, matched_type, const_env);
                    if (!value.constant || !value.type || value.type->kind == types::TypeKind::Error || (matched_type && value.type != matched_type))
                    {
                        error(lit.range, "literal pattern type mismatch");
                        out.ok = false;
                    }
                    break;
                }
                case ast::PatternKind::Range: {
                    auto& r = static_cast<ast::RangePattern&>(p);
                    if (!is_ordered_scalar(matched_type))
                    {
                        error(r.range, "range pattern requires an ordered scalar operand");
                        out.ok = false;
                    }

                    std::uint32_t tmp{};
                    auto start = r.start ? analyze_expr(mod, nullptr, scope, *r.start, 0, tmp, matched_type, const_env) : detail::ExprResult{};
                    auto end = r.end ? analyze_expr(mod, nullptr, scope, *r.end, 0, tmp, matched_type, const_env) : detail::ExprResult{};
                    if ((r.start && !start.constant) || (r.end && !end.constant))
                    {
                        error(r.range, "range pattern bounds must be constant");
                        out.ok = false;
                    }
                    break;
                }
                case ast::PatternKind::Or: {
                    auto& o = static_cast<ast::OrPattern&>(p);
                    std::optional<std::vector<std::string_view>> baseline;
                    si::InternedPmrHashMap<types::TypePtr> first_binding_types{m_alloc};
                    for (auto* alt : o.alternatives)
                    {
                        if (!alt)
                            continue;
                        auto r = validate_pattern(mod, *alt, matched_type, scope, const_env);
                        if (!r.ok)
                            out.ok = false;

                        std::vector<std::string_view> names;
                        names.reserve(r.bindings.size());
                        for (auto const& b : r.bindings)
                            names.push_back(b.name);
                        std::sort(names.begin(), names.end());
                        if (!baseline)
                        {
                            baseline = std::move(names);
                            for (auto const& b : r.bindings)
                                first_binding_types[b.name] = b.type;
                        }
                        else if (*baseline != names)
                        {
                            error(o.range, "or-pattern alternatives must bind the same names");
                            out.ok = false;
                        }

                        for (auto const& b : r.bindings)
                        {
                            auto it = first_binding_types.find(b.name);
                            if (it != first_binding_types.end() && it->second != b.type)
                            {
                                error(o.range, "or-pattern alternatives bind `{}` with inconsistent types (by-ref vs by-value)", b.name);
                                out.ok = false;
                            }
                        }

                        out.bindings.insert(out.bindings.end(), r.bindings.begin(), r.bindings.end());
                    }

                    if (o.alternatives.size() > 1)
                    {
                        si::InternedPmrHashSet seen{m_alloc};
                        std::pmr::vector<PatternBinding> unique{m_alloc};
                        unique.reserve(out.bindings.size());
                        for (auto& b : out.bindings)
                            if (seen.insert(b.name).second)
                                unique.push_back(b);
                        out.bindings = std::move(unique);
                    }
                    break;
                }
                case ast::PatternKind::EnumDestructure: {
                    auto& e = static_cast<ast::EnumDestructurePattern&>(p);
                    auto const* op_enum = types::type_cast<types::EnumType>(matched_type);
                    if (!op_enum)
                    {
                        error(e.range, "enum pattern requires enum operand");
                        out.ok = false;
                        break;
                    }

                    auto const* sym = resolve_value_path_with_fallback(*mod.own_scope, e.variant_path);
                    if (!sym || sym->kind != SymbolKind::EnumVariant || !sym->decl || sym->decl->kind != ast::DeclKind::Enum)
                    {
                        error(e.range, "unknown enum variant `{}`", pattern_name(e));
                        out.ok = false;
                        break;
                    }

                    auto const* enum_decl = static_cast<ast::EnumDecl const*>(sym->decl);
                    if (static_cast<void const*>(op_enum->decl) != static_cast<void const*>(enum_decl))
                    {
                        error(e.range, "enum variant `{}` does not belong to `{}`", pattern_name(e), enum_decl->name);
                        out.ok = false;
                        break;
                    }

                    e.resolved_variant = nullptr;
                    for (auto const& variant : enum_decl->variants)
                        if (variant.name == sym->name)
                        {
                            e.resolved_variant = &variant;
                            break;
                        }
                    if (!e.resolved_variant)
                    {
                        error(e.range, "unknown enum variant `{}`", pattern_name(e));
                        out.ok = false;
                        break;
                    }

                    std::size_t expected_payload_count = e.resolved_variant->payload.size();
                    if (op_enum && enum_decl->is_tagged && op_enum->tagged_layout && e.resolved_variant->payload.size() <= 1)
                        if (!variant_has_effective_payload(op_enum, e.resolved_variant))
                            expected_payload_count = 0;

                    bool has_eff_payload = (expected_payload_count > 0);

                    if (enum_decl->is_tagged)
                    {
                        if (has_eff_payload && !e.has_parens)
                        {
                            error(e.range, "tagged enum variant with payload requires parenthesized pattern");
                            out.ok = false;
                            break;
                        }
                        if (!has_eff_payload && e.has_parens)
                        {
                            error(e.range, "payloadless tagged enum variant must not use parenthesized pattern");
                            out.ok = false;
                            break;
                        }
                    }

                    if (e.payload.size() != expected_payload_count)
                    {
                        error(e.range, "enum pattern payload arity mismatch");
                        out.ok = false;
                        break;
                    }

                    if (has_eff_payload)
                    {
                        for (std::size_t i = 0; i < e.payload.size(); ++i)
                        {
                            auto* pay_ty_node = e.resolved_variant->payload[i];
                            auto payload_ty = pay_ty_node ? resolve_payload_type(*enum_decl, pay_ty_node, matched_type, mod, scope) : m_types.m_errort();
                            if (e.payload[i])
                                validate_child(*e.payload[i], payload_ty);
                        }
                    }
                    break;
                }
                case ast::PatternKind::StructDestructure: {
                    auto& s = static_cast<ast::StructDestructurePattern&>(p);
                    auto const* op_struct = types::type_cast<types::StructType>(matched_type);
                    if (!op_struct)
                    {
                        error(s.range, "struct pattern requires struct operand");
                        out.ok = false;
                        break;
                    }

                    auto const* sym = resolve_type_path(*mod.own_scope, s.type_path);
                    if (!sym || !sym->decl || sym->decl->kind != ast::DeclKind::Struct)
                    {
                        error(s.range, "struct pattern requires struct operand");
                        out.ok = false;
                        break;
                    }

                    auto const* struct_decl = static_cast<ast::StructDecl const*>(sym->decl);
                    if (static_cast<void const*>(op_struct->decl) != static_cast<void const*>(struct_decl))
                    {
                        error(s.range, "struct pattern requires struct operand");
                        out.ok = false;
                        break;
                    }

                    si::InternedHashSet seen_fields;
                    auto bindings = make_bindings(matched_type);
                    auto fields = record_fields(matched_type, bindings ? &*bindings : nullptr);
                    for (auto& field : s.fields)
                    {
                        if (!seen_fields.insert(field.field_name).second)
                        {
                            error(field.range, "duplicate field `{}` in struct pattern", field.field_name);
                            out.ok = false;
                            continue;
                        }

                        auto it = std::find_if(fields.begin(), fields.end(), [&](auto const& f) { return f.name == field.field_name; });
                        if (it == fields.end())
                        {
                            error(field.range, "unknown field `{}` in struct pattern", field.field_name);
                            out.ok = false;
                            continue;
                        }

                        field.resolved_field_index = static_cast<std::uint32_t>(std::distance(fields.begin(), it));
                        auto field_ty = substitute_in_nominal_context(it->type, matched_type);
                        if (field.pattern)
                            validate_child(*field.pattern, field_ty);
                    }
                    break;
                }
            }

            return out;
        }

        void install_pattern_bindings(Scope& scope, PatternValidation const& validation)
        {
            for (auto const& binding : validation.bindings)
            {
                define_local(scope, binding.decl);
                register_inferred_local_type(binding.decl, binding.type);
            }
        }

        static std::string path_str(ast::Path const& p)
        {
            std::string s;
            for (std::size_t i = 0; i < p.segments.size(); ++i)
            {
                if (i)
                    s += "::";
                s += p.segments[i].name;
            }
            return s;
        }

        std::span<Symbol const> lookup_candidates(ModuleInfo const& mod, Scope const& local_scope, std::string_view name)
        {
            if (auto vs = local_scope.lookup_values(name); !vs.empty())
                return vs;
            if (!mod.own_scope)
                return {};
            auto result = mod.own_scope->lookup_values(name);
            if (!result.empty())
                return result;

            if (m_specialization_defining_module && m_specialization_defining_module->own_scope)
                return m_specialization_defining_module->own_scope->lookup_values(name);

            return {};
        }

        Symbol const* lookup_name(ModuleInfo const& mod, Scope const& local_scope, std::string_view name)
        {
            auto vs = lookup_candidates(mod, local_scope, name);
            return vs.empty() ? nullptr : &vs.front();
        }

        ast::Decl const* nominal_decl(types::TypePtr ty)
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
                    return nominal_decl(static_cast<types::PointerType const*>(ty)->pointee);
                case types::TypeKind::Nominal:
                    return nominal_decl(static_cast<types::NominalType const*>(ty)->underlying);
                default:
                    return nullptr;
            }
        }

        ast::FieldDecl* find_field(ast::Decl const& d, std::string_view name)
        {
            if (auto const* sd = ast::node_cast<ast::StructDecl>(&d))
            {
                for (auto& f : const_cast<ast::StructDecl*>(sd)->fields)
                    if (f.name == name)
                        return &f;
            }
            else if (auto const* ud = ast::node_cast<ast::UnionDecl>(&d))
            {
                for (auto& f : const_cast<ast::UnionDecl*>(ud)->fields)
                    if (f.name == name)
                        return &f;
            }
            return nullptr;
        }

        comptime::Value const* make_value(comptime::Value v) { return m_ast_ctx.own_value(std::move(v)); }

        comptime::Value const* make_int_const(std::int64_t v, types::TypePtr ty) { return make_value(comptime::Value::make_int(v, ty)); }
        comptime::Value const* make_bool_const(bool v, types::TypePtr ty) { return make_value(comptime::Value::make_bool(v, ty)); }
        comptime::Value const* make_char_const(std::uint32_t v, types::TypePtr ty) { return make_value(comptime::Value::make_char(v, ty)); }
        comptime::Value const* make_float_const(double v, types::TypePtr ty) { return make_value(comptime::Value::make_float(v, ty)); }

        comptime::Value const* make_null_const(types::TypePtr ty) { return make_value(comptime::Value::make_null(ty)); }

        comptime::Value const* make_str_const(std::string_view v, types::TypePtr ty) { return make_value(comptime::Value::make_string(std::string{v}, ty)); }

        comptime::Value const* make_u16_str_const(std::u16string_view v, types::TypePtr ty)
        {
            std::string raw;
            raw.resize(v.size() * sizeof(char16_t));
            if (!v.empty())
                std::memcpy(raw.data(), v.data(), v.size() * sizeof(char16_t));

            return make_value(comptime::Value::make_string(std::move(raw), ty));
        }

        comptime::Value const* fold_tagged_enum_construction(ast::EnumVariant const* variant, types::TypePtr enum_type,
                                                             std::span<comptime::Value const* const> arg_consts)
        {
            auto* et = types::type_cast<types::EnumType>(enum_type);
            if (!et || !et->is_tagged || !et->tagged_layout)
                return nullptr;

            complete_tagged_enum(enum_type);
            et = types::type_cast<types::EnumType>(enum_type);
            if (!et || !et->tagged_layout)
                return nullptr;

            auto* layout = et->tagged_layout;

            auto* disc_val = make_int_const(static_cast<std::int64_t>(variant->discriminant), layout->discriminant_type);

            std::vector<comptime::Value> agg_elems;
            agg_elems.push_back(*disc_val);

            if (layout->payload_size > 0 && !arg_consts.empty() && variant->payload.size() == 1 && arg_consts[0])
            {
                agg_elems.push_back(*arg_consts[0]);
            }

            return make_value(comptime::Value::make_aggregate(std::move(agg_elems), enum_type));
        }

        [[nodiscard]] static bool is_const_kind(comptime::Value const& c, comptime::Value::Kind k) noexcept { return const_eval::is_const_kind(c, k); }

        [[nodiscard]] static std::optional<std::int64_t> const_to_int(comptime::Value const& c) noexcept { return const_eval::const_to_int(c); }

        [[nodiscard]] static std::optional<double> const_to_float(comptime::Value const& c) noexcept { return const_eval::const_to_float(c); }

        [[nodiscard]] static std::optional<bool> const_to_bool(comptime::Value const& c) noexcept { return const_eval::const_to_bool(c); }

        [[nodiscard]] static std::optional<std::uint64_t> const_to_bits(comptime::Value const& c) noexcept { return const_eval::const_to_bits(c); }

        comptime::Value const* fold_unary_constant(lex::TokenKind op, comptime::Value const& c, types::TypePtr out_type)
        {
            auto result = const_eval::fold_unary(op, c, out_type);
            if (result)
                return make_value(std::move(*result));

            return nullptr;
        }

        comptime::Value const* fold_binary_constant(lex::TokenKind op, comptime::Value const& lhs, comptime::Value const& rhs, types::TypePtr out_type,
                                                    sm::SourceRange range)
        {
            auto result = const_eval::fold_binary(op, lhs, rhs, out_type);
            if (result)
                return make_value(std::move(*result));

            if (types::type_cast<types::IntType>(out_type) && lhs.kind() == comptime::Value::Kind::Int && rhs.kind() == comptime::Value::Kind::Int)
            {
                if (op == lex::TokenKind::Plus || op == lex::TokenKind::Minus || op == lex::TokenKind::Star)
                    error(range, "integer overflow in constant expression");
                else if (op == lex::TokenKind::LtLt || op == lex::TokenKind::GtGt)
                    error(range, "shift amount out of range in constant expression");
            }

            return nullptr;
        }

        comptime::Value const* fold_binary_cmp(lex::TokenKind op, comptime::Value const& lhs, comptime::Value const& rhs, types::TypePtr out_type)
        {
            auto result = const_eval::fold_cmp(op, lhs, rhs, out_type);
            if (result)
                return make_value(std::move(*result));

            return nullptr;
        }

        comptime::Value const* fold_cast_constant(comptime::Value const& c, types::TypePtr dst)
        {
            auto result = const_eval::fold_cast(c, dst);
            if (result)
                return make_value(std::move(*result));

            return nullptr;
        }

        struct ResolvedType
        {
            types::TypePtr type{};
            types::Qual quals{types::Qual::None};
        };

        [[nodiscard]] static constexpr types::Qual qual_or(types::Qual a, types::Qual b) noexcept
        {
            return static_cast<types::Qual>(std::to_underlying(a) | std::to_underlying(b));
        }

        [[nodiscard]] types::Qual ast_to_type_qual(ast::Qual q) const noexcept
        {
            types::Qual out = types::Qual::None;
            if (ast::has_qual(q, ast::Qual::Const))
                out = qual_or(out, types::Qual::Const);
            if (ast::has_qual(q, ast::Qual::Volatile))
                out = qual_or(out, types::Qual::Volatile);
            if (ast::has_qual(q, ast::Qual::Restrict))
                out = qual_or(out, types::Qual::Restrict);

            return out;
        }

        [[nodiscard]] types::TypePtr materialize_type(ResolvedType const& r)
        {
            if (!r.type)
                return m_types.m_errort();

            if (r.quals != types::Qual::None)
            {
                if (auto const* p = types::type_cast<types::PointerType>(r.type))
                    return m_types.pointer_to(p->pointee, qual_or(p->pointee_quals, r.quals));
                if (auto const* s = types::type_cast<types::SliceType>(r.type))
                    return m_types.slice_t(s->element, qual_or(s->element_quals, r.quals));
            }

            return r.type;
        }

        [[nodiscard]] detail::ExprResult analyze_expr_or_error(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::Expr* expr, int loop_depth,
                                                               std::uint32_t& next_off, types::TypePtr expected_type = nullptr,
                                                               ConstEnv const* const_env = nullptr)
        {
            if (!expr)
                return detail::ExprResult{m_types.m_errort()};
            return analyze_expr(mod, fn, scope, *expr, loop_depth, next_off, expected_type, const_env);
        }

        detail::ExprResult analyze_expr(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::Expr& expr, int loop_depth, std::uint32_t& next_off,
                                        types::TypePtr expected_type = nullptr, ConstEnv const* const_env = nullptr)
        {
            detail::ExprResult out{};

            if (expr.kind == ast::ExprKind::Ident && expr.sema.resolved_type && !expr.sema.resolved_decl && !has_error(get_resolved_type(expr.sema)))
            {
                auto* ident = static_cast<ast::IdentExpr const*>(&expr);
                auto const* sym = lookup_name(mod, scope, ident->name);
                if (sym && sym->decl)
                {
                    out.resolved_decl = sym->decl;
                    track_decl_read(sym->decl);
                    out.is_lvalue = (sym->kind == SymbolKind::Variable);
                }

                out.type = get_resolved_type(expr.sema);
                out.constant = expr.sema.const_value;
                out.is_constant = expr.sema.is_constant;
            }
            else
            {
                switch (expr.kind)
                {
                    case ast::ExprKind::IntLiteral: {
                        auto& e = static_cast<ast::IntLiteralExpr&>(expr);

                        if (expected_type && types::type_cast<types::IntType>(expected_type))
                            out.type = expected_type;
                        else if (e.sema.const_value)
                            out.type = e.sema.const_value->type;
                        else
                            out.type = default_int_type(e.value, expected_type);

                        out.constant = make_int_const(e.value, out.type);
                        out.is_constant = true;
                        break;
                    }
                    case ast::ExprKind::FloatLiteral: {
                        if (auto const* ft = types::type_cast<types::FloatType>(expected_type))
                            out.type = m_types.float_t(ft->bits);
                        else
                            out.type = m_types.float_t(64);

                        out.constant = make_float_const(static_cast<ast::FloatLiteralExpr&>(expr).value, out.type);
                        out.is_constant = true;

                        break;
                    }
                    case ast::ExprKind::StringLiteral: {
                        auto& e = static_cast<ast::StringLiteralExpr&>(expr);
                        if (auto const* st = types::type_cast<types::SliceType>(expected_type))
                        {
                            auto const* el = st->element;
                            if (el && (el->kind == types::TypeKind::Char ||
                                       (types::type_cast<types::IntType>(el) && static_cast<types::IntType const*>(el)->bits == 8)))
                            {
                                out.type = m_types.slice_t(el, st->element_quals);
                                out.constant = make_str_const(std::string_view{e.value}, out.type);
                                out.is_constant = true;
                                break;
                            }
                        }
                        else if (auto const* pt = types::type_cast<types::PointerType>(expected_type))
                        {
                            auto const* pointee = pt->pointee;
                            bool is_char = pointee->kind == types::TypeKind::Char;
                            bool is_u8 = pointee->kind == types::TypeKind::Int &&
                                         static_cast<types::IntType const*>(pointee)->bits == 8 &&
                                         !static_cast<types::IntType const*>(pointee)->is_signed;
                            bool is_const = types::has_qual(pt->pointee_quals, types::Qual::Const);
                            if ((is_char || is_u8) && is_const)
                            {
                                out.type = m_types.pointer_to(pointee, pt->pointee_quals);
                                out.constant = make_str_const(std::string_view{e.value}, out.type);
                                out.is_constant = true;
                                break;
                            }
                        }

                        out.type = m_types.slice_t(m_types.m_chart(), types::Qual::Const);
                        out.constant = make_str_const(std::string_view{e.value}, out.type);
                        out.is_constant = true;
                        break;
                    }
                    case ast::ExprKind::U16StringLiteral: {
                        auto& e = static_cast<ast::U16StringLiteralExpr&>(expr);
                        auto* u16_type = m_types.int_t(16, false);
                        if (auto const* st = types::type_cast<types::SliceType>(expected_type))
                        {
                            auto const* el = st->element;
                            if (el && types::type_cast<types::IntType>(el) && static_cast<types::IntType const*>(el)->bits == 16 &&
                                !static_cast<types::IntType const*>(el)->is_signed)
                            {
                                out.type = m_types.slice_t(el, st->element_quals);
                                out.constant = make_u16_str_const(e.value, out.type);
                                out.is_constant = true;
                                break;
                            }
                        }
                        else if (auto const* pt = types::type_cast<types::PointerType>(expected_type))
                        {
                            auto const* pointee = pt->pointee;
                            bool is_u16 = pointee->kind == types::TypeKind::Int &&
                                          static_cast<types::IntType const*>(pointee)->bits == 16 &&
                                          !static_cast<types::IntType const*>(pointee)->is_signed;
                            bool is_const = types::has_qual(pt->pointee_quals, types::Qual::Const);
                            if (is_u16 && is_const)
                            {
                                out.type = m_types.pointer_to(pointee, pt->pointee_quals);
                                out.constant = make_u16_str_const(e.value, out.type);
                                out.is_constant = true;
                                break;
                            }
                        }

                        out.type = m_types.slice_t(u16_type, types::Qual::Const);
                        out.constant = make_u16_str_const(e.value, out.type);
                        out.is_constant = true;
                        break;
                    }
                    case ast::ExprKind::CharLiteral: {
                        auto& e = static_cast<ast::CharLiteralExpr&>(expr);
                        out.type = m_types.m_chart();
                        out.constant = make_char_const(e.codepoint, out.type);
                        out.is_constant = true;
                        break;
                    }
                    case ast::ExprKind::U16CharLiteral: {
                        auto& e = static_cast<ast::U16CharLiteralExpr&>(expr);
                        out.type = m_types.int_t(16, false);
                        out.constant = make_int_const(e.value, out.type);
                        out.is_constant = true;
                        break;
                    }
                    case ast::ExprKind::BoolLiteral: {
                        auto& e = static_cast<ast::BoolLiteralExpr&>(expr);
                        out.type = m_types.m_boolt();
                        out.constant = make_bool_const(e.value, out.type);
                        out.is_constant = true;
                        break;
                    }
                    case ast::ExprKind::NullLiteral:
                        if (types::type_cast<types::PointerType>(expected_type))
                            out.type = expected_type;
                        else
                            out.type = m_types.m_nullt();

                        out.constant = make_null_const(out.type);
                        out.is_constant = true;
                        break;
                    case ast::ExprKind::Ident:
                        out = analyze_name(mod, scope, static_cast<ast::IdentExpr&>(expr).name, static_cast<ast::IdentExpr&>(expr).range, const_env);
                        break;
                    case ast::ExprKind::PathExpr:
                        out = analyze_path_expr(mod, scope, static_cast<ast::PathExpr&>(expr), expected_type, const_env);
                        break;
                    case ast::ExprKind::Unary:
                        out = analyze_unary(mod, fn, scope, static_cast<ast::UnaryExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Postfix:
                        out = analyze_postfix(mod, fn, scope, static_cast<ast::PostfixExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Binary:
                        out = analyze_binary(mod, fn, scope, static_cast<ast::BinaryExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Call:
                        out = analyze_call(mod, fn, scope, static_cast<ast::CallExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::FieldAccess:
                        out = analyze_field_access(mod, fn, scope, static_cast<ast::FieldAccessExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Index:
                        out = analyze_index(mod, fn, scope, static_cast<ast::IndexExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::PackAccess:
                        out = analyze_pack_access(mod, fn, scope, static_cast<ast::PackAccessExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Cast:
                        out = analyze_cast(mod, fn, scope, static_cast<ast::CastExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Block:
                        out = analyze_block_expr(mod, fn, scope, static_cast<ast::BlockExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::If:
                        out = analyze_if_expr(mod, fn, scope, static_cast<ast::IfExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Match:
                        out = analyze_match_expr(mod, fn, scope, static_cast<ast::MatchExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::StructLiteral:
                        out =
                            analyze_struct_literal(mod, fn, scope, static_cast<ast::StructLiteralExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Sizeof:
                        out = analyze_sizeof(mod, scope, static_cast<ast::SizeofExpr&>(expr));
                        break;
                    case ast::ExprKind::Alignof:
                        out = analyze_alignof(mod, scope, static_cast<ast::AlignofExpr&>(expr));
                        break;
                    case ast::ExprKind::Offsetof:
                        out = analyze_offsetof(mod, scope, static_cast<ast::OffsetofExpr&>(expr));
                        break;
                    case ast::ExprKind::Compiles:
                        out = analyze_compiles(mod, fn, scope, static_cast<ast::CompilesExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::Range: {
                        auto& r = static_cast<ast::RangeExpr&>(expr);

                        if (!m_allow_range_expr)
                            error(r.range, "a range can only be used in a slice index, a for-in, or a match pattern");

                        auto start_result = r.start ? analyze_expr(mod, fn, scope, *r.start, loop_depth, next_off, nullptr, const_env) : detail::ExprResult{};
                        auto end_result = r.end ? analyze_expr(mod, fn, scope, *r.end, loop_depth, next_off, nullptr, const_env) : detail::ExprResult{};

                        if (start_result.type && end_result.type && start_result.type != end_result.type)
                        {
                            if (types::type_cast<types::IntType>(start_result.type) && types::type_cast<types::IntType>(end_result.type))
                            {
                                if (r.start && r.start->kind == ast::ExprKind::IntLiteral)
                                    start_result = analyze_expr(mod, fn, scope, *r.start, loop_depth, next_off, end_result.type, const_env);
                                else if (r.end && r.end->kind == ast::ExprKind::IntLiteral)
                                    end_result = analyze_expr(mod, fn, scope, *r.end, loop_depth, next_off, start_result.type, const_env);
                            }
                        }
                        auto common = start_result.type ? start_result.type : end_result.type;
                        if (start_result.type && end_result.type && start_result.type != end_result.type)
                        {
                            out.type = m_types.m_errort();
                            error(r.range, "range bounds must have the same type");
                        }
                        else if (common && !is_ordered_scalar(common))
                        {
                            out.type = m_types.m_errort();
                            error(r.range, "range bounds must be integer or char type");
                        }
                        else if (common)
                            out.type = r.inclusive ? m_types.range_inclusive_t(common) : m_types.range_t(common);
                        else
                            out.type = m_types.m_errort();
                        break;
                    }
                    case ast::ExprKind::TypeAST: {
                        auto& t = static_cast<ast::TypeASTExpr&>(expr);
                        out.type = resolve_type_node(mod, scope, t.type_node);
                        break;
                    }
                    case ast::ExprKind::TemplateInst:
                        out = analyze_template_inst(mod, fn, scope, static_cast<ast::TemplateInstExpr&>(expr), loop_depth, next_off, expected_type, const_env);
                        break;
                    case ast::ExprKind::SizeofPack:
                        out = analyze_sizeof_pack(mod, scope, static_cast<ast::SizeofPackExpr&>(expr));
                        break;
                    case ast::ExprKind::PackExpansion: {
                        auto& pe = static_cast<ast::PackExpansionExpr&>(expr);
                        if (pe.operand)
                        {
                            auto* ident = ast::node_cast<ast::IdentExpr>(pe.operand);
                            if (ident)
                            {
                                auto pack_result = resolve_pack_info(mod, scope, ident->name, ident->range);
                                if (pack_result.has_value())
                                    out.type = m_types.usize_t();
                                else
                                    error(ident->range, "cannot expand '{}': not a pack", ident->name);
                            }
                            else if (ast::node_cast<ast::IndexExpr>(pe.operand))
                                error(pe.range, "pack-index expression cannot be expanded");
                            else if (ast::node_cast<ast::PackAccessExpr>(pe.operand))
                                error(pe.range, "pack-index expression cannot be expanded");
                            else
                                error(pe.range, "pack expansion requires an identifier");
                        }
                        break;
                    }
                }
            }

            set_resolved_type(expr.sema, out.type);
            expr.sema.const_value = out.constant;
            expr.sema.resolved_decl = out.resolved_decl;
            record_resolved_specialization(expr.sema, out.spec_commit ? &out.spec_commit : nullptr);
            expr.sema.ufcs_callee = out.ufcs_callee;
            expr.sema.construction_kind = out.construction_kind;
            expr.sema.constructed_variant = out.constructed_variant;
            expr.sema.is_lvalue = out.is_lvalue;
            expr.sema.is_constant = out.is_constant;
            expr.sema.is_diverging = out.is_diverging;
            expr.sema.is_type_instantiation = out.is_type_instantiation;
            return out;
        }

        detail::ExprResult analyze_name(ModuleInfo& mod, Scope& scope, std::string_view name, sm::SourceRange range, ConstEnv const* const_env,
                                        types::TypePtr expected_type = nullptr)
        {
            detail::ExprResult out{};
            auto const* sym = lookup_name(mod, scope, name);
            if (!sym)
            {
                out.type = m_types.m_errort();
                error(range, "unknown name `{}`", name);
                return out;
            }

            out.resolved_decl = sym->decl;
            out.type = decl_type(*sym->decl);
            track_decl_read(sym->decl);
            out.is_lvalue = sym->kind == SymbolKind::Variable;
            if (out.is_lvalue)
            {
                if (auto const* c = const_eval::lookup_identifier(name, [&](std::string_view n) { return lookup_constant(const_env, n); }))
                {
                    out.constant = c;
                    out.is_constant = true;
                }
            }

            if (sym->kind == SymbolKind::EnumVariant && sym->decl && sym->decl->kind == ast::DeclKind::Enum)
            {
                auto const* enum_decl = static_cast<ast::EnumDecl const*>(sym->decl);
                auto const* variant = find_enum_variant(*enum_decl, sym->name);
                if (variant)
                {
                    out.construction_kind = ConstructionKind::Enum;
                    out.constructed_variant = variant;

                    if (enum_decl->template_params.empty())
                        out.type = decl_type(*sym->decl);
                    else if (expected_type)
                    {
                        auto const* expected_enum = types::type_cast<types::EnumType>(expected_type);
                        if (expected_enum && static_cast<void const*>(expected_enum->decl) == static_cast<void const*>(sym->decl))
                            out.type = expected_type;
                        else
                        {
                            auto deduced = deduce_enum_type_from_context(*static_cast<ast::EnumDecl const*>(sym->decl), expected_type);
                            if (deduced)
                                out.type = deduced;
                        }
                    }

                    complete_tagged_enum(out.type);

                    bool has_eff_payload = !variant->payload.empty();
                    if (out.type)
                    {
                        auto const* et = types::type_cast<types::EnumType>(out.type);
                        if (et && et->is_tagged && et->tagged_layout)
                            has_eff_payload = variant_has_effective_payload(et, variant);
                    }

                    if (has_eff_payload && !m_analyzing_call_callee)
                    {
                        out.type = m_types.m_errort();
                        error(range, "enum variant `{}::{}` requires a payload argument", enum_decl->name, variant->name);
                        return out;
                    }

                    if (out.type && !has_eff_payload)
                    {
                        out.constant = fold_tagged_enum_construction(variant, out.type, {});
                        if (!out.constant)
                        {
                            auto const* et = types::type_cast<types::EnumType>(out.type);
                            if (et && !et->is_tagged && et->backing && types::type_cast<types::IntType>(et->backing))
                                out.constant = make_int_const(variant->discriminant, et->backing);
                        }

                        if (out.constant)
                            out.is_constant = true;
                    }
                }
            }
            return out;
        }

        detail::ExprResult analyze_path_expr(ModuleInfo& mod, Scope& scope, ast::PathExpr& p, types::TypePtr expected_type, ConstEnv const* const_env)
        {
            if (p.path.is_simple())
                return analyze_name(mod, scope, p.path.simple_name(), p.path.range, const_env, expected_type);

            detail::ExprResult out{};
            if (!mod.own_scope)
            {
                out.type = m_types.m_errort();
                return out;
            }

            auto try_path = [&](Scope& s) -> Symbol const* {
                auto const* sym = resolve_value_path(s, p.path);
                if (sym)
                    return sym;
                return nullptr;
            };

            types::TypePtr explicit_enum_type{};
            if (!p.explicit_enum_args.empty())
            {
                auto const* enum_sym = resolve_type_path_with_fallback(*mod.own_scope, p.path);
                if (enum_sym && enum_sym->decl && enum_sym->decl->kind == ast::DeclKind::Enum)
                {
                    auto const* enum_decl = static_cast<ast::EnumDecl const*>(enum_sym->decl);
                    if (p.explicit_enum_args.size() == enum_decl->template_params.size())
                    {
                        std::vector<types::TypePtr> resolved_args;
                        resolved_args.reserve(p.explicit_enum_args.size());
                        bool all_types = true;
                        for (auto const& arg : p.explicit_enum_args)
                        {
                            if (arg.type)
                                resolved_args.push_back(resolve_type_node(mod, scope, arg.type));
                            else
                            {
                                all_types = false;
                                break;
                            }
                        }
                        if (all_types)
                            explicit_enum_type = m_types.nominal_t(types::TypeKind::Enum, const_cast<ast::EnumDecl*>(enum_decl), resolved_args);
                    }
                }
            }

            auto const* sym = try_path(*mod.own_scope);
            if (!sym && m_specialization_defining_module && m_specialization_defining_module->own_scope)
                sym = try_path(*m_specialization_defining_module->own_scope);
            if (!sym)
            {
                out.type = m_types.m_errort();
                error(p.range, "unknown path `{}`", path_str(p.path));
                return out;
            }

            out.resolved_decl = sym->decl;
            out.type = decl_type(*sym->decl);
            track_decl_read(sym->decl);
            out.is_lvalue = sym->kind == SymbolKind::Variable;

            if (sym->kind == SymbolKind::EnumVariant && sym->decl && sym->decl->kind == ast::DeclKind::Enum)
            {
                auto const* enum_decl = static_cast<ast::EnumDecl const*>(sym->decl);
                auto const* variant = find_enum_variant(*enum_decl, sym->name);
                if (variant)
                {
                    out.construction_kind = ConstructionKind::Enum;
                    out.constructed_variant = variant;

                    if (explicit_enum_type)
                        out.type = explicit_enum_type;
                    else if (enum_decl->template_params.empty())
                        out.type = decl_type(*sym->decl);
                    else if (expected_type)
                    {
                        auto const* expected_enum = types::type_cast<types::EnumType>(expected_type);
                        if (expected_enum && static_cast<void const*>(expected_enum->decl) == static_cast<void const*>(sym->decl))
                            out.type = expected_type;
                        else
                        {
                            auto deduced = deduce_enum_type_from_context(*enum_decl, expected_type);
                            if (deduced)
                                out.type = deduced;
                        }
                    }

                    complete_tagged_enum(out.type);

                    bool has_eff_payload = !variant->payload.empty();
                    if (out.type)
                    {
                        auto const* et = types::type_cast<types::EnumType>(out.type);
                        if (et && et->is_tagged && et->tagged_layout)
                            has_eff_payload = variant_has_effective_payload(et, variant);
                    }

                    if (has_eff_payload && !m_analyzing_call_callee)
                    {
                        out.type = m_types.m_errort();
                        error(p.range, "enum variant `{}::{}` requires a payload argument", enum_decl->name, variant->name);
                        return out;
                    }

                    if (out.type && !has_eff_payload)
                    {
                        out.constant = fold_tagged_enum_construction(variant, out.type, {});
                        if (!out.constant)
                        {
                            auto const* et = types::type_cast<types::EnumType>(out.type);
                            if (et && !et->is_tagged && et->backing && types::type_cast<types::IntType>(et->backing))
                                out.constant = make_int_const(variant->discriminant, et->backing);
                        }

                        if (out.constant)
                            out.is_constant = true;
                    }
                }
            }
            return out;
        }

        detail::ExprResult analyze_unary(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::UnaryExpr& u, int loop_depth, std::uint32_t& next_off,
                                         types::TypePtr expected_type, ConstEnv const* const_env)
        {
            auto op = analyze_expr_or_error(mod, fn, scope, u.operand, loop_depth, next_off, expected_type, const_env);
            detail::ExprResult out = op;
            if (has_error(op.type))
                return out;

            switch (u.op)
            {
                case lex::TokenKind::Plus:
                case lex::TokenKind::Minus:
                case lex::TokenKind::Tilde:
                    if (u.op == lex::TokenKind::Minus)
                    {
                        if (auto const* it = types::type_cast<types::IntType>(op.type); it && !it->is_signed)
                        {
                            out.type = m_types.m_errort();
                            error(u.range, "cannot negate unsigned integer");
                            break;
                        }
                    }
                    if (!types::type_cast<types::IntType>(op.type) && !types::type_cast<types::FloatType>(op.type))
                    {
                        out.type = m_types.m_errort();
                        error(u.range, "unary operand type mismatch");
                    }
                    break;
                case lex::TokenKind::Bang:
                    if (op.type != m_types.m_boolt())
                    {
                        out.type = m_types.m_errort();
                        error(u.range, "unary operand type mismatch");
                    }
                    else
                        out.type = m_types.m_boolt();

                    break;
                case lex::TokenKind::Amp:
                    if (op.resolved_decl && op.resolved_decl->kind == ast::DeclKind::Func && op.type && op.type->kind == types::TypeKind::FuncPtr)
                    {
                        auto const* f = static_cast<ast::FuncDecl const*>(op.resolved_decl);
                        if (!f->template_params.empty() && !op.spec_commit)
                        {
                            out.type = m_types.m_errort();
                            error(u.range, "cannot take address of generic function");
                            break;
                        }

                        out.type = op.type;
                        out.resolved_decl = op.resolved_decl;
                        out.spec_commit = op.spec_commit;
                        out.is_lvalue = false;
                        out.is_constant = false;
                        out.constant = nullptr;
                        break;
                    }
                    if (!op.is_lvalue)
                    {
                        out.type = m_types.m_errort();
                        error(u.range, "unary operand type mismatch");
                        break;
                    }
                    out.type = m_types.pointer_to(op.type, types::Qual::None);
                    break;
                case lex::TokenKind::Star:
                    if (auto const* p = types::type_cast<types::PointerType>(op.type))
                    {
                        out.type = p->pointee;
                        out.is_lvalue = true;
                    }
                    else
                    {
                        out.type = m_types.m_errort();
                        error(u.range, "dereference of non-pointer");
                    }
                    break;
                case lex::TokenKind::Increment:
                case lex::TokenKind::Decrement: {
                    if (!op.is_lvalue)
                    {
                        out.type = m_types.m_errort();
                        error(u.range, "pre-increment/decrement requires an lvalue");
                        return out;
                    }
                    auto const* it = types::type_cast<types::IntType>(op.type);
                    auto const* ft = types::type_cast<types::FloatType>(op.type);
                    auto const* pt = types::type_cast<types::PointerType>(op.type);
                    if (!it && !ft && !pt)
                    {
                        out.type = m_types.m_errort();
                        error(u.range, "pre-increment/decrement requires numeric or pointer type");
                        return out;
                    }
                    out.type = op.type;
                    out.resolved_decl = op.resolved_decl;
                    out.is_lvalue = true;
                    out.constant = nullptr;
                    out.is_constant = false;
                    return out;
                }
                default:
                    out.type = m_types.m_errort();
                    break;
            }
            if (op.constant && out.type && out.type->kind != types::TypeKind::Error)
                out.constant = fold_unary_constant(u.op, *op.constant, out.type);

            out.is_constant = out.constant != nullptr;
            return out;
        }

        detail::ExprResult analyze_postfix(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::PostfixExpr& p, int loop_depth, std::uint32_t& next_off,
                                           types::TypePtr, ConstEnv const* const_env)
        {
            auto op = analyze_expr_or_error(mod, fn, scope, p.operand, loop_depth, next_off, nullptr, const_env);
            detail::ExprResult out{};
            if (has_error(op.type))
            {
                out.type = m_types.m_errort();
                return out;
            }

            switch (p.op)
            {
                case lex::TokenKind::Increment:
                case lex::TokenKind::Decrement: {
                    if (!op.is_lvalue)
                    {
                        out.type = m_types.m_errort();
                        error(p.range, "postfix increment/decrement requires an lvalue");
                        return out;
                    }
                    auto const* it = types::type_cast<types::IntType>(op.type);
                    auto const* ft = types::type_cast<types::FloatType>(op.type);
                    auto const* pt = types::type_cast<types::PointerType>(op.type);
                    if (!it && !ft && !pt)
                    {
                        out.type = m_types.m_errort();
                        error(p.range, "postfix increment/decrement requires numeric or pointer type");
                        return out;
                    }
                    out.type = op.type;
                    out.resolved_decl = op.resolved_decl;
                    out.is_lvalue = false;
                    out.constant = nullptr;
                    out.is_constant = false;
                    return out;
                }
                default:
                    out.type = m_types.m_errort();
                    return out;
            }
        }

        detail::ExprResult analyze_binary(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::BinaryExpr& b, int loop_depth, std::uint32_t& next_off,
                                          types::TypePtr expected_type, ConstEnv const* const_env)
        {
            auto lhs = analyze_expr_or_error(mod, fn, scope, b.lhs, loop_depth, next_off, expected_type, const_env);

            types::TypePtr rhs_expected = expected_type;
            switch (b.op)
            {
                case lex::TokenKind::Eq:
                case lex::TokenKind::PlusEq:
                case lex::TokenKind::MinusEq:
                case lex::TokenKind::StarEq:
                case lex::TokenKind::SlashEq:
                case lex::TokenKind::PercentEq:
                case lex::TokenKind::AmpEq:
                case lex::TokenKind::PipeEq:
                case lex::TokenKind::CaretEq:
                case lex::TokenKind::LtLtEq:
                case lex::TokenKind::GtGtEq:
                    rhs_expected = lhs.type;
                    break;
                default:
                    break;
            }
            auto rhs = analyze_expr_or_error(mod, fn, scope, b.rhs, loop_depth, next_off, rhs_expected, const_env);
            detail::ExprResult out{};
            if (has_error(lhs.type) || has_error(rhs.type))
            {
                out.type = m_types.m_errort();
                return out;
            }

            switch (b.op)
            {
                case lex::TokenKind::Eq:
                case lex::TokenKind::PlusEq:
                case lex::TokenKind::MinusEq:
                case lex::TokenKind::StarEq:
                case lex::TokenKind::SlashEq:
                case lex::TokenKind::PercentEq:
                case lex::TokenKind::AmpEq:
                case lex::TokenKind::PipeEq:
                case lex::TokenKind::CaretEq:
                case lex::TokenKind::LtLtEq:
                case lex::TokenKind::GtGtEq: {
                    if (!lhs.is_lvalue)
                    {
                        out.type = m_types.m_errort();
                        error(b.lhs->range, "assignment target is not assignable");
                        return out;
                    }

                    auto const lhs_type = lhs.type;
                    if (!lhs_type || !rhs.type || lhs_type->kind == types::TypeKind::Error || rhs.type->kind == types::TypeKind::Error)
                    {
                        out.type = m_types.m_errort();
                        return out;
                    }

                    bool ok = b.op == lex::TokenKind::Eq ? can_assign_return(lhs_type, rhs.type) : (lhs_type == rhs.type);
                    if (!ok && b.op == lex::TokenKind::Eq)
                        ok = try_implicit_enum_conversion(lhs_type, rhs.type, mod, scope) != nullptr;
                    if (!ok)
                    {
                        out.type = m_types.m_errort();
                        error(b.range, "assignment type mismatch");
                        return out;
                    }

                    switch (b.op)
                    {
                        case lex::TokenKind::Eq:
                            break;
                        case lex::TokenKind::PlusEq:
                        case lex::TokenKind::MinusEq:
                        case lex::TokenKind::StarEq:
                        case lex::TokenKind::SlashEq:
                        case lex::TokenKind::PercentEq:
                            if (!types::type_cast<types::IntType>(lhs_type) && !types::type_cast<types::FloatType>(lhs_type))
                            {
                                out.type = m_types.m_errort();
                                error(b.range, "binary operand type mismatch");
                                return out;
                            }
                            break;
                        case lex::TokenKind::AmpEq:
                        case lex::TokenKind::PipeEq:
                        case lex::TokenKind::CaretEq:
                        case lex::TokenKind::LtLtEq:
                        case lex::TokenKind::GtGtEq:
                            if (!types::type_cast<types::IntType>(lhs_type))
                            {
                                out.type = m_types.m_errort();
                                error(b.range, "binary operand type mismatch");
                                return out;
                            }
                            break;
                        default:
                            break;
                    }

                    track_decl_write(lhs.resolved_decl);
                    out.type = lhs_type;
                    return out;
                }
                case lex::TokenKind::Plus:
                case lex::TokenKind::Minus:
                case lex::TokenKind::Star:
                case lex::TokenKind::Slash:
                case lex::TokenKind::Percent:
                case lex::TokenKind::Amp:
                case lex::TokenKind::Pipe:
                case lex::TokenKind::Caret:
                case lex::TokenKind::LtLt:
                case lex::TokenKind::GtGt:
                    if (lhs.type && lhs.type->kind != types::TypeKind::Error && rhs.type && rhs.type->kind != types::TypeKind::Error && lhs.type != rhs.type)
                    {
                        if ((b.lhs->kind == ast::ExprKind::IntLiteral || b.lhs->kind == ast::ExprKind::FloatLiteral ||
                             b.lhs->kind == ast::ExprKind::NullLiteral) &&
                            rhs.type)
                            lhs = analyze_expr(mod, fn, scope, *b.lhs, loop_depth, next_off, rhs.type, const_env);
                        if ((b.rhs->kind == ast::ExprKind::IntLiteral || b.rhs->kind == ast::ExprKind::FloatLiteral ||
                             b.rhs->kind == ast::ExprKind::NullLiteral) &&
                            lhs.type)
                            rhs = analyze_expr(mod, fn, scope, *b.rhs, loop_depth, next_off, lhs.type, const_env);
                    }
                    if (lhs.type != rhs.type || (!types::type_cast<types::IntType>(lhs.type) && !types::type_cast<types::FloatType>(lhs.type)))
                    {
                        if (!(b.op == lex::TokenKind::Amp || b.op == lex::TokenKind::Pipe || b.op == lex::TokenKind::Caret || b.op == lex::TokenKind::LtLt ||
                              b.op == lex::TokenKind::GtGt))
                        {
                            out.type = m_types.m_errort();
                            error(b.range, "binary operand type mismatch");
                            return out;
                        }
                    }
                    if (b.op == lex::TokenKind::Amp || b.op == lex::TokenKind::Pipe || b.op == lex::TokenKind::Caret || b.op == lex::TokenKind::LtLt ||
                        b.op == lex::TokenKind::GtGt)
                    {
                        if (lhs.type != rhs.type || !types::type_cast<types::IntType>(lhs.type))
                        {
                            out.type = m_types.m_errort();
                            error(b.range, "binary operand type mismatch");
                            return out;
                        }
                        out.type = lhs.type;
                        if (lhs.constant && rhs.constant)
                            out.constant = fold_binary_constant(b.op, *lhs.constant, *rhs.constant, out.type, b.range);
                        break;
                    }
                    if (lhs.type != rhs.type || (!types::type_cast<types::IntType>(lhs.type) && !types::type_cast<types::FloatType>(lhs.type)))
                    {
                        out.type = m_types.m_errort();
                        error(b.range, "binary operand type mismatch");
                        return out;
                    }
                    out.type = lhs.type;
                    if (lhs.constant && rhs.constant)
                        out.constant = fold_binary_constant(b.op, *lhs.constant, *rhs.constant, out.type, b.range);
                    break;
                case lex::TokenKind::EqEq:
                case lex::TokenKind::BangEq:
                case lex::TokenKind::Lt:
                case lex::TokenKind::LtEq:
                case lex::TokenKind::Gt:
                case lex::TokenKind::GtEq:
                    if ((b.op == lex::TokenKind::EqEq || b.op == lex::TokenKind::BangEq) && lhs.type && lhs.type->kind == types::TypeKind::Slice)
                    {
                        out.type = m_types.m_errort();
                        error(b.range, "slice values cannot be compared for equality (use .ptr/.len comparison or a library function)");
                        return out;
                    }
                    if (lhs.type && rhs.type && lhs.type != rhs.type)
                    {
                        if ((b.lhs->kind == ast::ExprKind::IntLiteral || b.lhs->kind == ast::ExprKind::FloatLiteral ||
                             b.lhs->kind == ast::ExprKind::NullLiteral) &&
                            rhs.type)
                            lhs = analyze_expr(mod, fn, scope, *b.lhs, loop_depth, next_off, rhs.type, const_env);
                        if ((b.rhs->kind == ast::ExprKind::IntLiteral || b.rhs->kind == ast::ExprKind::FloatLiteral ||
                             b.rhs->kind == ast::ExprKind::NullLiteral) &&
                            lhs.type)
                            rhs = analyze_expr(mod, fn, scope, *b.rhs, loop_depth, next_off, lhs.type, const_env);
                    }
                    if (lhs.type != rhs.type || !lhs.type || lhs.type->kind == types::TypeKind::Error)
                    {
                        out.type = m_types.m_errort();
                        error(b.range, "binary operand type mismatch");
                        return out;
                    }
                    out.type = m_types.m_boolt();
                    if (lhs.constant && rhs.constant)
                        out.constant = fold_binary_constant(b.op, *lhs.constant, *rhs.constant, out.type, b.range);
                    break;
                case lex::TokenKind::AmpAmp:
                case lex::TokenKind::PipePipe:
                    if (lhs.type != m_types.m_boolt() || rhs.type != m_types.m_boolt())
                    {
                        out.type = m_types.m_errort();
                        error(b.range, "binary operand type mismatch");
                        return out;
                    }
                    out.type = m_types.m_boolt();
                    if (lhs.constant && lhs.constant->kind() == comptime::Value::Kind::Bool)
                    {
                        bool const lhs_value = lhs.constant->get_bool();
                        if (b.op == lex::TokenKind::AmpAmp)
                            out.constant = lhs_value ? (rhs.constant && rhs.constant->kind() == comptime::Value::Kind::Bool
                                                            ? make_bool_const(rhs.constant->get_bool(), out.type)
                                                            : nullptr)
                                                     : make_bool_const(false, out.type);
                        else
                            out.constant = lhs_value ? make_bool_const(true, out.type)
                                                     : (rhs.constant && rhs.constant->kind() == comptime::Value::Kind::Bool
                                                            ? make_bool_const(rhs.constant->get_bool(), out.type)
                                                            : nullptr);
                    }
                    if (!out.constant && rhs.constant && rhs.constant->kind() == comptime::Value::Kind::Bool)
                    {
                        if (b.op == lex::TokenKind::AmpAmp && rhs.constant->get_bool() && lhs.constant && lhs.constant->kind() == comptime::Value::Kind::Bool)
                            out.constant = make_bool_const(lhs.constant->get_bool(), out.type);
                        else if (b.op == lex::TokenKind::PipePipe && !rhs.constant->get_bool() && lhs.constant &&
                                 lhs.constant->kind() == comptime::Value::Kind::Bool)
                            out.constant = make_bool_const(lhs.constant->get_bool(), out.type);
                    }
                    break;
                default:
                    out.type = m_types.m_errort();
                    break;
            }
            out.is_constant = out.constant != nullptr;
            return out;
        }

        detail::ExprResult analyze_field_access(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::FieldAccessExpr& f, int loop_depth,
                                                std::uint32_t& next_off, types::TypePtr, ConstEnv const* const_env)
        {
            auto obj = analyze_expr_or_error(mod, fn, scope, f.object, loop_depth, next_off, nullptr, const_env);
            detail::ExprResult out = obj;
            if (has_error(obj.type))
                return out;

            if (auto const* slice = types::type_cast<types::SliceType>(obj.type))
            {
                if (f.field == "ptr")
                {
                    auto quals = slice->element_quals;
                    out.type = m_types.pointer_to(slice->element, quals);
                    out.resolved_decl = nullptr;
                    out.is_lvalue = true;
                    if (obj.constant && obj.constant->kind() == comptime::Value::Kind::Slice && obj.constant->size() >= 1)
                    {
                        out.constant = &obj.constant->at(0);
                        out.is_constant = true;
                        out.is_lvalue = false;
                    }

                    return out;
                }
                if (f.field == "len")
                {
                    out.type = m_types.usize_t();
                    out.resolved_decl = nullptr;
                    out.is_lvalue = true;
                    if (obj.constant && obj.constant->kind() == comptime::Value::Kind::Slice && obj.constant->size() >= 2)
                    {
                        out.constant = &obj.constant->at(1);
                        out.is_constant = true;
                        out.is_lvalue = false;
                    }

                    return out;
                }
                out.type = m_types.m_errort();
                error(f.field_range, "slice has no field `{}` (valid fields: .ptr, .len)", f.field);
                return out;
            }

            auto const* nominal = nominal_decl(obj.type);
            if (!nominal)
            {
                out.type = m_types.m_errort();
                error(f.range, "field access on non-record type");
                return out;
            }

            auto* field = find_field(*nominal, f.field);
            if (!field)
            {
                out.type = m_types.m_errort();
                error(f.field_range, "unknown field `{}`", f.field);
                return out;
            }

            if (field->type)
                out.type = substitute_in_nominal_context(get_canonical(field->type->sema), obj.type);

            if (!out.type)
                out.type = m_types.m_errort();

            out.resolved_decl = nominal;
            out.is_lvalue = true;

            if (obj.constant && obj.constant->kind() == comptime::Value::Kind::Aggregate)
            {
                auto const& agg = *obj.constant;
                auto bindings = make_bindings(obj.type);
                auto rfields = record_fields(obj.type, bindings ? &*bindings : nullptr);
                for (std::size_t i = 0; i < rfields.size(); ++i)
                {
                    if (rfields[i].name == f.field && i < agg.size())
                    {
                        out.constant = &agg.at(i);
                        out.is_constant = true;
                        out.is_lvalue = false;
                        break;
                    }
                }
            }

            return out;
        }

        void note_range_slice_index_sema(ast::RangeExpr& r, types::TypePtr common)
        {
            if (common && !has_error(common))
                set_resolved_type(r.sema, r.inclusive ? m_types.range_inclusive_t(common) : m_types.range_t(common));
        }

        void validate_range_bounds_for_array(ast::RangeExpr const& r, std::uint64_t array_len, sm::SourceRange slice_range)
        {
            auto get_const = [&](ast::ExprPtr e) -> std::optional<std::int64_t> {
                if (!e || !e->sema.const_value)
                    return std::nullopt;
                return e->sema.const_value->const_to_int();
            };

            if (r.start)
            {
                auto sv = get_const(r.start);
                if (sv.has_value())
                {
                    if (*sv < 0)
                        error(r.start->range, "range start index must be non-negative");
                    else if (static_cast<std::uint64_t>(*sv) > array_len)
                        error(r.start->range, "range start index exceeds array length");
                }
            }

            if (r.end)
            {
                auto ev = get_const(r.end);
                if (ev.has_value())
                {
                    if (*ev < 0)
                        error(r.end->range, "range end index must be non-negative");
                    else if (r.inclusive && static_cast<std::uint64_t>(*ev) >= array_len)
                        error(r.end->range, "inclusive range end index must be less than array length");
                    else if (!r.inclusive && static_cast<std::uint64_t>(*ev) > array_len)
                        error(r.end->range, "range end index exceeds array length");
                }
            }

            if (r.start && r.end)
            {
                auto sv = get_const(r.start);
                auto ev = get_const(r.end);
                if (sv.has_value() && ev.has_value() && *sv > *ev)
                    error(slice_range, "range start index exceeds end index");
            }
        }

        detail::ExprResult analyze_index(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::IndexExpr& i, int loop_depth, std::uint32_t& next_off,
                                         types::TypePtr, ConstEnv const* const_env)
        {
            auto obj = analyze_expr_or_error(mod, fn, scope, i.object, loop_depth, next_off, nullptr, const_env);
            detail::ExprResult out = obj;
            if (has_error(obj.type))
                return out;

            if (i.index->kind == ast::ExprKind::Range)
            {
                auto& r = static_cast<ast::RangeExpr&>(*i.index);

                types::TypePtr range_expected = nullptr;
                if (obj.type)
                {
                    switch (obj.type->kind)
                    {
                        case types::TypeKind::Array:
                        case types::TypeKind::RuntimeArray:
                        case types::TypeKind::Slice:
                            range_expected = m_types.usize_t();
                            break;
                        default:
                            break;
                    }
                }

                types::TypePtr common = nullptr;
                if (r.start)
                {
                    auto sr = analyze_expr(mod, fn, scope, *r.start, loop_depth, next_off, range_expected, const_env);
                    if (!has_error(sr.type))
                        common = sr.type;
                }
                if (r.end)
                {
                    auto er = analyze_expr(mod, fn, scope, *r.end, loop_depth, next_off, range_expected, const_env);
                    if (!has_error(er.type))
                        common = common ? common : er.type;
                }

                if (r.start && r.end && r.start->sema.resolved_type && r.end->sema.resolved_type)
                {
                    auto st = get_resolved_type(r.start->sema);
                    auto et = get_resolved_type(r.end->sema);
                    if (st && et && st != et && !has_error(st) && !has_error(et))
                    {
                        if (types::type_cast<types::IntType>(st) && types::type_cast<types::IntType>(et))
                        {
                            bool unified = false;
                            if (r.start->kind == ast::ExprKind::IntLiteral)
                            {
                                auto sr2 = analyze_expr(mod, fn, scope, *r.start, loop_depth, next_off, et, const_env);
                                if (!has_error(sr2.type))
                                {
                                    common = sr2.type;
                                    unified = true;
                                }
                            }
                            else if (r.end->kind == ast::ExprKind::IntLiteral)
                            {
                                auto er2 = analyze_expr(mod, fn, scope, *r.end, loop_depth, next_off, st, const_env);
                                if (!has_error(er2.type))
                                {
                                    common = er2.type;
                                    unified = true;
                                }
                            }
                            if (!unified)
                                error(r.range, "range bounds must have the same type");
                        }
                        else
                        {
                            error(r.range, "range bounds must have the same type");
                        }
                    }
                }

                if (r.inclusive && !r.end)
                    error(r.range, "inclusive range slice requires an end index");

                if (common && !has_error(common) && common->kind != types::TypeKind::Int)
                    error(r.range, "range slice bounds must be integer type");

                note_range_slice_index_sema(r, common);

                detail::ExprResult slice_out{};
                if (auto const* a = types::type_cast<types::ArrayType>(obj.type))
                {
                    validate_range_bounds_for_array(r, a->count, i.range);
                    slice_out.type = m_types.slice_t(a->element, types::Qual::None);
                }
                else if (auto const* ra = types::type_cast<types::RuntimeArrayType>(obj.type))
                {
                    slice_out.type = m_types.slice_t(ra->element, types::Qual::None);
                }
                else if (auto const* s = types::type_cast<types::SliceType>(obj.type))
                {
                    slice_out.type = m_types.slice_t(s->element, s->element_quals);
                }
                else if (types::type_cast<types::PointerType>(obj.type))
                {
                    slice_out.type = m_types.m_errort();
                    error(i.range, "cannot range-slice a pointer");
                }
                else
                {
                    slice_out.type = m_types.m_errort();
                    error(i.range, "cannot range-slice non-indexable type");
                }
                return slice_out;
            }

            types::TypePtr index_expected = nullptr;
            if (obj.type)
            {
                switch (obj.type->kind)
                {
                    case types::TypeKind::Array:
                    case types::TypeKind::RuntimeArray:
                    case types::TypeKind::Slice:
                    case types::TypeKind::Pointer:
                    case types::TypeKind::Fam:
                        index_expected = m_types.usize_t();
                        break;
                    default:
                        break;
                }
            }
            auto index_result = analyze_expr_or_error(mod, fn, scope, i.index, loop_depth, next_off, index_expected, const_env);

            if (auto const* a = types::type_cast<types::ArrayType>(obj.type))
                out.type = a->element;
            else if (auto const* ra = types::type_cast<types::RuntimeArrayType>(obj.type))
                out.type = ra->element;
            else if (auto const* s = types::type_cast<types::SliceType>(obj.type))
                out.type = s->element;
            else if (auto const* p = types::type_cast<types::PointerType>(obj.type))
                out.type = p->pointee;
            else if (types::is_fam_type(obj.type))
                out.type = types::fam_element(obj.type);
            else if (types::type_cast<types::TemplateParamType>(obj.type))
                ;
            else if (types::type_cast<types::TypePackType>(obj.type))
                ;
            else
            {
                out.type = m_types.m_errort();
                error(i.range, "indexing non-indexable type");
            }
            out.is_lvalue = true;

            if (obj.constant && index_result.constant &&
                (obj.constant->kind() == comptime::Value::Kind::Aggregate || obj.constant->kind() == comptime::Value::Kind::Slice))
            {
                auto idx_opt = index_result.constant->const_to_int();
                if (idx_opt && *idx_opt >= 0 && static_cast<std::size_t>(*idx_opt) < obj.constant->size())
                {
                    out.constant = &obj.constant->at(static_cast<std::size_t>(*idx_opt));
                    out.is_constant = true;
                    out.is_lvalue = false;
                }
            }

            return out;
        }

        detail::ExprResult analyze_pack_access(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::PackAccessExpr& pa, int loop_depth,
                                               std::uint32_t& next_off, types::TypePtr, ConstEnv const* const_env)
        {
            auto obj = analyze_expr_or_error(mod, fn, scope, pa.object, loop_depth, next_off, nullptr, const_env);
            detail::ExprResult out = obj;
            if (has_error(obj.type))
                return out;

            (void)analyze_expr_or_error(mod, fn, scope, pa.index, loop_depth, next_off, nullptr, const_env);

            if (is_pack_indexable_type(obj.type))
                return out;

            out.type = m_types.m_errort();
            error(pa.range, "cannot pack-index non-pack type");
            return out;
        }

        detail::ExprResult analyze_cast(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::CastExpr& c, int loop_depth, std::uint32_t& next_off,
                                        types::TypePtr expected_type, ConstEnv const* const_env)
        {
            auto op = analyze_expr_or_error(mod, fn, scope, c.operand, loop_depth, next_off, expected_type, const_env);
            detail::ExprResult out = op;
            if (c.target)
            {
                if (!c.target->sema.canonical)
                    std::ignore = resolve_and_substitute_type_expr(mod, scope, c.target);
                out.type = get_canonical(c.target->sema);
            }

            if (out.type && mod.own_scope && c.target)
                check_type_constraints_in_type(mod, *mod.own_scope, out.type, c.target->range);

            if (!out.type)
                out.type = m_types.m_errort();

            if (op.type && out.type && op.type != out.type && !has_error(op.type) && !has_error(out.type))
            {
                if (op.type->kind == types::TypeKind::Pointer && out.type->kind == types::TypeKind::Pointer)
                {
                    auto const* src_ptr = static_cast<types::PointerType const*>(op.type);
                    auto const* dst_ptr = static_cast<types::PointerType const*>(out.type);
                    if (types::has_qual(src_ptr->pointee_quals, types::Qual::Const) && !types::has_qual(dst_ptr->pointee_quals, types::Qual::Const))
                    {
                        error(c.range, "invalid cast from `{}` to `{}`: cannot drop const qualifier", format_type_str(op.type), format_type_str(out.type));
                    }
                }
            }

            if (op.constant)
                out.constant = fold_cast_constant(*op.constant, out.type);

            out.is_constant = out.constant != nullptr;
            return out;
        }

        detail::ExprResult analyze_block_expr(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::BlockExpr& b, int loop_depth, std::uint32_t& next_off,
                                              types::TypePtr expected_type, ConstEnv const* const_env)
        {
            auto expected_kind = expected_type ? construction_kind_for_type(expected_type) : ConstructionKind::None;
            bool has_single_non_struct_tail = b.body.tail && b.body.tail->kind != ast::ExprKind::StructLiteral;
            if (b.body.stmts.empty() && (has_single_non_struct_tail || !b.body.tail) && expected_kind != ConstructionKind::None)
            {
                ast::StructLiteralExpr synthetic{b.range, m_alloc};
                synthetic.type = nullptr;
                if (b.body.tail)
                    synthetic.fields.push_back(ast::StructLiteralField{"", {}, b.body.tail->range, b.body.tail, {}});
                auto result = analyze_struct_literal(mod, fn, scope, synthetic, loop_depth, next_off, expected_type, const_env);
                b.sema.construction_kind = expected_kind;
                set_resolved_type(b.sema, result.type);
                return result;
            }

            auto* inner = make_scope(ScopeKind::Block, &scope);
            auto* inner_consts = make_const_env(const_env);
            auto res = analyze_block(mod, fn, *inner, b.body, loop_depth, next_off, expected_type, inner_consts);
            detail::ExprResult out{};
            if (b.body.tail)
                out.type = get_resolved_type(b.body.tail->sema);
            else
                out.type = m_types.m_voidt();

            out.is_diverging = !res.falls_through;
            if (b.body.tail && res.foldable)
            {
                auto const* tail_const = b.body.tail->sema.const_value;
                if (tail_const)
                {
                    out.constant = tail_const;
                    out.is_constant = true;
                }
            }
            return out;
        }

        detail::ExprResult analyze_if_expr(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::IfExpr& i, int loop_depth, std::uint32_t& next_off,
                                           types::TypePtr expected_type, ConstEnv const* const_env)
        {
            auto cond = analyze_expr_or_error(mod, fn, scope, i.condition, loop_depth, next_off, nullptr, const_env);
            auto* then_scope = make_scope(ScopeKind::Block, &scope);
            auto* then_consts = make_const_env(const_env);
            auto then_res = analyze_block(mod, fn, *then_scope, i.then_block, loop_depth, next_off, expected_type, then_consts);
            detail::ExprResult else_res{};
            if (i.else_branch)
                else_res = analyze_expr(mod, fn, scope, *i.else_branch, loop_depth, next_off, expected_type, const_env);

            detail::ExprResult out{};
            auto const* then_const = i.then_block.tail ? i.then_block.tail->sema.const_value : nullptr;
            auto const* else_const = i.else_branch ? else_res.constant : nullptr;
            if (cond.constant && cond.constant->kind() == comptime::Value::Kind::Bool)
            {
                if (cond.constant->get_bool())
                {
                    if (i.then_block.tail)
                        out.type = get_resolved_type(i.then_block.tail->sema);
                    else
                        out.type = m_types.m_voidt();

                    out.is_diverging = !then_res.falls_through;
                    out.constant = then_const;
                    out.is_constant = out.constant != nullptr;
                    return out;
                }
                if (i.else_branch)
                {
                    out.type = else_res.type;
                    out.is_diverging = else_res.is_diverging;
                    out.constant = else_const;
                    out.is_constant = out.constant != nullptr;
                    return out;
                }

                out.type = m_types.m_voidt();
                return out;
            }

            auto then_type = i.then_block.tail ? get_resolved_type(i.then_block.tail->sema) : m_types.m_voidt();
            if (i.else_branch)
            {
                out.type = then_type ? then_type : else_res.type;
                out.is_diverging = then_res.diverges && else_res.is_diverging;
                if (then_const && else_const && constant_equal(*then_const, *else_const))
                {
                    out.constant = then_const;
                    out.is_constant = true;
                }
            }
            else
            {
                out.type = m_types.m_voidt();
                out.is_diverging = then_res.diverges;
            }
            return out;
        }

        detail::ExprResult analyze_match_expr(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::MatchExpr& m, int loop_depth, std::uint32_t& next_off,
                                              types::TypePtr expected_type, ConstEnv const* const_env)
        {
            auto operand = analyze_expr_or_error(mod, fn, scope, m.operand, loop_depth, next_off, nullptr, const_env);
            detail::ExprResult out{};
            bool coverage_enabled = operand.type && operand.type->kind != types::TypeKind::Error;
            bool all_patterns_valid = coverage_enabled;
            PatternCoverage coverage{m_alloc};
            comptime::Value const* selected_constant{};
            types::TypePtr unified_type{};
            for (auto& arm : m.arms)
            {
                auto* arm_scope = make_scope(ScopeKind::Block, &scope);
                auto* arm_consts = make_const_env(const_env);
                bool guard_is_true = !arm.guard;
                bool pattern_valid = true;
                if (arm.pattern)
                {
                    auto validated = validate_pattern(mod, *arm.pattern, operand.type, *arm_scope, const_env);
                    pattern_valid = validated.ok;
                    all_patterns_valid = all_patterns_valid && validated.ok;
                    if (validated.ok)
                        install_pattern_bindings(*arm_scope, validated);
                }
                if (arm.guard)
                {
                    auto guard = analyze_expr(mod, fn, *arm_scope, *arm.guard, loop_depth, next_off, nullptr, arm_consts);
                    if (guard.type && guard.type->kind != types::TypeKind::Error && guard.type != m_types.m_boolt())
                        error(arm.guard->range, "match guard must be of type `bool`");
                    guard_is_true = guard.constant && guard.constant->kind() == comptime::Value::Kind::Bool && guard.constant->get_bool();
                }
                if (coverage_enabled && pattern_valid && arm.pattern)
                {
                    auto current = pattern_coverage(mod, *arm.pattern, operand.type, *arm_scope, const_env);
                    if (coverage_subsumes(operand.type, coverage, current))
                        warning(arm.pattern->range, "unreachable pattern");

                    if (!arm.guard)
                        add_coverage(operand.type, coverage, current);
                }
                if (arm.body)
                {
                    auto r = analyze_expr(mod, fn, *arm_scope, *arm.body, loop_depth, next_off, expected_type, arm_consts);
                    if (!unified_type)
                        unified_type = r.type ? r.type : m_types.m_voidt();
                    else if (r.type && r.type != unified_type && r.type->kind != types::TypeKind::Error && r.type != m_types.m_voidt() &&
                             unified_type != m_types.m_voidt())
                    {
                        error(arm.body->range, "match arm result type mismatch: expected `{}`, got `{}`", format_type_str(unified_type),
                              format_type_str(r.type));
                    }

                    if (r.type && r.type != m_types.m_voidt())
                        out.type = r.type;

                    if (!selected_constant && operand.constant && arm.pattern && guard_is_true &&
                        (arm.pattern->kind == ast::PatternKind::Literal || arm.pattern->kind == ast::PatternKind::Range ||
                         arm.pattern->kind == ast::PatternKind::Wildcard || arm.pattern->kind == ast::PatternKind::Binding) &&
                        pattern_matches_const(mod, *arm.pattern, operand, *arm_scope) && r.constant)
                        selected_constant = r.constant;
                }
            }
            if (coverage_enabled && all_patterns_valid && !coverage_covers_all(operand.type, coverage))
            {
                auto msg = missing_case_message(operand.type, coverage);
                if (!msg.empty())
                    error(m.range, "match expression not exhaustive: {}", msg);
                else
                    error(m.range, "match expression not exhaustive");
            }

            if (!out.type)
                out.type = m_types.m_voidt();

            out.constant = selected_constant;
            out.is_constant = out.constant != nullptr;
            return out;
        }

        detail::ExprResult analyze_struct_literal(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::StructLiteralExpr& s, int loop_depth,
                                                  std::uint32_t& next_off, types::TypePtr expected_type, ConstEnv const* const_env)
        {
            detail::ExprResult out{};
            auto target = s.type ? resolve_type_node(mod, scope, s.type) : expected_type;
            if (!target)
            {
                out.type = m_types.m_errort();
                error(s.range, "brace literal requires an expected type");
                return out;
            }

            auto kind = construction_kind_for_type(target);
            if (kind == ConstructionKind::None)
            {
                out.type = m_types.m_errort();
                error(s.range, "brace literal requires an aggregate type");
                return out;
            }

            if (s.type && target && mod.own_scope)
                check_type_constraints_in_type(mod, *mod.own_scope, target, s.range);

            auto analyze_record_fields = [&](types::TypePtr record_ty) -> std::optional<detail::ExprResult> {
                if (!nominal_decl(record_ty))
                    return std::nullopt;

                auto bindings = make_bindings(record_ty);
                auto fields = record_fields(record_ty, bindings ? &*bindings : nullptr);
                std::vector<bool> used(fields.size(), false);
                std::size_t next_pos = 0;
                detail::ExprResult result{};
                result.type = record_ty;
                result.construction_kind = kind;

                for (auto& f : s.fields)
                {
                    std::size_t field_index = fields.size();
                    if (!f.name.empty())
                    {
                        for (std::size_t i = 0; i < fields.size(); ++i)
                            if (fields[i].name == f.name)
                            {
                                field_index = i;
                                break;
                            }

                        if (field_index == fields.size())
                        {
                            error(f.range, "unknown field `{}` in brace literal", f.name);
                            return std::nullopt;
                        }
                        if (used[field_index])
                        {
                            error(f.range, "duplicate field `{}` in brace literal", f.name);
                            return std::nullopt;
                        }
                    }
                    else
                    {
                        while (next_pos < fields.size() && used[next_pos])
                            ++next_pos;
                        if (next_pos >= fields.size())
                        {
                            error(f.range, "too many positional fields in brace literal");
                            return std::nullopt;
                        }
                        field_index = next_pos;
                        used[field_index] = true;
                        ++next_pos;
                    }

                    used[field_index] = true;
                    f.resolved_field_index = static_cast<std::uint32_t>(field_index);
                    auto expected_field = fields[field_index].type;

                    if (types::is_fam_type(expected_field))
                    {
                        error(f.range, "cannot initialize flexible array member `{}`", fields[field_index].name);
                        return std::nullopt;
                    }

                    if (f.value)
                    {
                        auto val = analyze_expr(mod, fn, scope, *f.value, loop_depth, next_off, expected_field, const_env);
                        if (expected_field && val.type && val.type != expected_field && val.type->kind != types::TypeKind::Error)
                        {
                            error(f.range, "field type mismatch");
                            return std::nullopt;
                        }
                    }
                }

                if (s.fields.size() != fields.size())
                {
                    for (std::size_t i = 0; i < used.size(); ++i)
                        if (!used[i] && !types::is_fam_type(fields[i].type))
                            error(s.range, "missing initializer for field `{}` in struct literal", fields[i].name);
                }

                return result;
            };

            auto try_implicit_enum_element = [&](ast::Expr& element_expr, types::TypePtr target_enum_type) -> std::optional<detail::ExprResult> {
                auto const* et = types::type_cast<types::EnumType>(target_enum_type);
                if (!et)
                    return std::nullopt;

                auto const* enum_decl = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                if (!enum_decl)
                    return std::nullopt;

                std::optional<detail::ExprResult> chosen;
                ast::EnumVariant const* chosen_variant = nullptr;
                {
                    [[maybe_unused]] ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
                    for (auto const& variant : enum_decl->variants)
                    {
                        if (!variant_has_attr(variant, "implicit_construction"))
                            continue;
                        if (variant.payload.size() != 1)
                            continue;

                        bool var_is_void_normalized = false;
                        if (et->tagged_layout)
                        {
                            std::ptrdiff_t idx = &variant - enum_decl->variants.data();
                            if (idx >= 0 && static_cast<std::size_t>(idx) < et->tagged_layout->variant_count)
                                var_is_void_normalized = (et->tagged_layout->variants[idx].variant_payload_type_or_null == nullptr);
                        }
                        if (var_is_void_normalized)
                        {
                            if (!element_expr.sema.is_from_instantiation)
                                continue;

                            auto val = analyze_expr(mod, fn, scope, element_expr, loop_depth, next_off, nullptr, const_env);
                            if (!val.type || val.type->kind == types::TypeKind::Error)
                                continue;
                            if (!is_concrete_void_type(val.type))
                                continue;

                            detail::ExprResult result{};
                            result.type = target_enum_type;
                            result.construction_kind = ConstructionKind::Enum;
                            result.constructed_variant = &variant;
                            result.constant = val.constant;
                            result.is_constant = val.is_constant;
                            chosen = result;
                            chosen_variant = &variant;
                            break;
                        }

                        auto payload_ty = resolve_payload_type(*enum_decl, variant.payload[0], target_enum_type, mod, scope);
                        if (!payload_ty || has_error(payload_ty))
                            continue;

                        auto val = analyze_expr(mod, fn, scope, element_expr, loop_depth, next_off, payload_ty, const_env);
                        if (!val.type || val.type->kind == types::TypeKind::Error)
                            continue;
                        if (val.type != payload_ty && !can_assign_return(payload_ty, val.type))
                            continue;

                        detail::ExprResult result{};
                        result.type = target_enum_type;
                        result.construction_kind = ConstructionKind::Enum;
                        result.constructed_variant = &variant;
                        result.constant = val.constant;
                        result.is_constant = val.is_constant;
                        chosen = result;
                        chosen_variant = &variant;
                        break;
                    }
                }

                if (!chosen)
                    return std::nullopt;

                bool chosen_is_void_normalized = false;
                if (et->tagged_layout)
                {
                    std::ptrdiff_t idx = chosen_variant - enum_decl->variants.data();
                    if (idx >= 0 && static_cast<std::size_t>(idx) < et->tagged_layout->variant_count)
                        chosen_is_void_normalized = (et->tagged_layout->variants[idx].variant_payload_type_or_null == nullptr);
                }

                if (!chosen_is_void_normalized)
                {
                    auto payload_ty = resolve_payload_type(*enum_decl, chosen_variant->payload[0], target_enum_type, mod, scope);
                    std::ignore = analyze_expr(mod, fn, scope, element_expr, loop_depth, next_off, payload_ty, const_env);
                }

                element_expr.sema.construction_kind = ConstructionKind::Enum;
                element_expr.sema.constructed_variant = chosen_variant;
                set_resolved_type(element_expr.sema, target_enum_type);

                return chosen;
            };

            auto is_shorthand_ident = [](ast::StructLiteralField const& f) -> bool {
                if (f.name.empty())
                    return false;

                auto* ident = ast::node_cast<ast::IdentExpr>(f.value);
                return ident && ident->name == f.name;
            };

            auto analyze_array = [&](types::ArrayType const* arr) -> std::optional<detail::ExprResult> {
                for (auto const& f : s.fields)
                    if (!f.name.empty() && !is_shorthand_ident(f))
                    {
                        error(f.range, "array literal does not accept field names");
                        return std::nullopt;
                    }

                if (static_cast<std::uint64_t>(s.fields.size()) != arr->count)
                {
                    error(s.range, "array literal element count mismatch");
                    return std::nullopt;
                }

                detail::ExprResult result{};
                result.type = target;
                result.construction_kind = ConstructionKind::Array;
                for (std::size_t i = 0; i < s.fields.size(); ++i)
                {
                    auto& f = s.fields[i];
                    f.resolved_field_index = static_cast<std::uint32_t>(i);
                    if (!f.value)
                        continue;

                    auto val = analyze_expr(mod, fn, scope, *f.value, loop_depth, next_off, arr->element, const_env);
                    if (arr->element && val.type && val.type != arr->element && val.type->kind != types::TypeKind::Error)
                    {
                        if (!try_implicit_enum_element(*f.value, arr->element))
                        {
                            error(f.range, "array element type mismatch");
                            return std::nullopt;
                        }
                    }
                }
                return result;
            };

            auto analyze_slice = [&](types::SliceType const* slice) -> std::optional<detail::ExprResult> {
                for (auto const& f : s.fields)
                    if (!f.name.empty() && !is_shorthand_ident(f))
                    {
                        error(f.range, "slice literal does not accept field names");
                        return std::nullopt;
                    }

                detail::ExprResult result{};
                result.type = target;
                result.construction_kind = ConstructionKind::Slice;
                for (std::size_t i = 0; i < s.fields.size(); ++i)
                {
                    auto& f = s.fields[i];
                    f.resolved_field_index = static_cast<std::uint32_t>(i);
                    if (!f.value)
                        continue;

                    auto val = analyze_expr(mod, fn, scope, *f.value, loop_depth, next_off, slice->element, const_env);
                    if (slice->element && val.type && val.type != slice->element && val.type->kind != types::TypeKind::Error)
                    {
                        if (!try_implicit_enum_element(*f.value, slice->element))
                        {
                            error(f.range, "slice element type mismatch");
                            return std::nullopt;
                        }
                    }
                }
                return result;
            };

            auto analyze_enum = [&](types::EnumType const* et) -> std::optional<detail::ExprResult> {
                auto const* enum_decl = enum_decl_of(target);
                if (!enum_decl || static_cast<void const*>(et->decl) != static_cast<void const*>(enum_decl))
                    return std::nullopt;

                bool suppress_nested_enum = !s.type || m_disallow_nested_implicit_enum;
                bool saved_allow_implicit_enum = m_allow_implicit_enum;
                if (suppress_nested_enum)
                    m_allow_implicit_enum = false;

                std::optional<detail::ExprResult> chosen;
                std::vector<comptime::Value const*> chosen_arg_constants;
                bool ambiguous = false;
                {
                    [[maybe_unused]] ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
                    for (auto const& variant : enum_decl->variants)
                    {
                        if (!s.type && !variant_has_attr(variant, "implicit_construction"))
                            continue;

                        if (variant.payload.size() != s.fields.size())
                            continue;

                        bool ok = true;
                        detail::ExprResult result{};
                        result.type = target;
                        result.construction_kind = ConstructionKind::Enum;
                        result.constructed_variant = &variant;
                        std::vector<comptime::Value const*> field_constants;
                        field_constants.reserve(s.fields.size());
                        for (std::size_t i = 0; i < s.fields.size(); ++i)
                        {
                            auto const& f = s.fields[i];

                            auto payload_ty = variant.payload[i] ? resolve_type_node(mod, scope, variant.payload[i]) : m_types.m_errort();
                            payload_ty = substitute_in_nominal_context(payload_ty, target);
                            if (!f.value)
                            {
                                ok = false;
                                break;
                            }

                            auto val = analyze_expr(mod, fn, scope, *f.value, loop_depth, next_off, payload_ty, const_env);
                            field_constants.push_back(val.constant);
                            if (payload_ty && val.type && val.type != payload_ty && val.type->kind != types::TypeKind::Error)
                            {
                                ok = false;
                                break;
                            }
                        }

                        if (!ok)
                            continue;

                        if (chosen)
                        {
                            ambiguous = true;
                            break;
                        }

                        chosen = result;
                        chosen_arg_constants = std::move(field_constants);
                    }
                }

                m_allow_implicit_enum = saved_allow_implicit_enum;
                if (ambiguous)
                {
                    error(s.range, "ambiguous enum construction");
                    return detail::ExprResult{m_types.m_errort()};
                }

                if (!chosen)
                {
                    error(s.range, "no matching enum construction");
                    return detail::ExprResult{m_types.m_errort()};
                }

                complete_tagged_enum(target);

                if (chosen->type)
                {
                    bool all_const = !chosen_arg_constants.empty();
                    for (auto* c : chosen_arg_constants)
                    {
                        if (!c)
                        {
                            all_const = false;
                            break;
                        }
                    }

                    if (all_const && chosen->constructed_variant && !chosen->constructed_variant->payload.empty())
                    {
                        chosen->constant = fold_tagged_enum_construction(chosen->constructed_variant, chosen->type, chosen_arg_constants);
                        if (chosen->constant)
                            chosen->is_constant = true;
                    }
                }

                return *chosen;
            };

            auto maybe_construct = std::optional<detail::ExprResult>{};
            switch (kind)
            {
                case ConstructionKind::Struct:
                    maybe_construct = analyze_record_fields(target);
                    break;
                case ConstructionKind::Array:
                    maybe_construct = analyze_array(static_cast<types::ArrayType const*>(target));
                    break;
                case ConstructionKind::Slice:
                    maybe_construct = analyze_slice(static_cast<types::SliceType const*>(target));
                    break;
                case ConstructionKind::Enum:
                    if (!s.type && !m_allow_implicit_enum)
                    {
                        error(s.range, "brace literal requires an explicit enum type");
                        out.type = m_types.m_errort();
                        return out;
                    }
                    maybe_construct = analyze_enum(static_cast<types::EnumType const*>(target));
                    break;
                case ConstructionKind::None:
                    break;
            }

            if (!maybe_construct)
            {
                out.type = m_types.m_errort();
                return out;
            }

            out = *maybe_construct;

            if (out.type && out.type->kind != types::TypeKind::Error)
            {
                switch (out.construction_kind)
                {
                    case ConstructionKind::Struct: {
                        auto bindings = make_bindings(out.type);
                        auto fields = record_fields(out.type, bindings ? &*bindings : nullptr);
                        std::vector<comptime::Value> agg_elems;
                        agg_elems.reserve(fields.size());

                        bool all_const = true;
                        for (auto const& fld : fields)
                        {
                            auto lit_it = std::find_if(s.fields.begin(), s.fields.end(), [&](ast::StructLiteralField const& sf) {
                                return sf.resolved_field_index < fields.size() && fields[sf.resolved_field_index].name == fld.name;
                            });
                            if (lit_it != s.fields.end() && lit_it->value && lit_it->value->sema.const_value)
                                agg_elems.push_back(*lit_it->value->sema.const_value);
                            else
                            {
                                all_const = false;
                                break;
                            }
                        }

                        if (all_const && agg_elems.size() == fields.size())
                            out.constant = make_value(comptime::Value::make_aggregate(std::move(agg_elems), out.type));

                        break;
                    }
                    case ConstructionKind::Array: {
                        std::vector<comptime::Value> agg_elems;
                        agg_elems.reserve(s.fields.size());
                        bool all_const = true;
                        for (auto& f : s.fields)
                        {
                            if (f.value && f.value->sema.const_value)
                                agg_elems.push_back(*f.value->sema.const_value);
                            else
                            {
                                all_const = false;
                                break;
                            }
                        }
                        if (all_const)
                            out.constant = make_value(comptime::Value::make_aggregate(std::move(agg_elems), out.type));

                        break;
                    }
                    case ConstructionKind::Slice: {
                        std::vector<comptime::Value> slice_elems;
                        slice_elems.reserve(s.fields.size());
                        bool all_const = true;
                        for (auto& f : s.fields)
                        {
                            if (f.value && f.value->sema.const_value)
                                slice_elems.push_back(*f.value->sema.const_value);
                            else
                            {
                                all_const = false;
                                break;
                            }
                        }
                        if (all_const)
                            out.constant = make_value(comptime::Value::make_slice(std::move(slice_elems), out.type));

                        break;
                    }
                    default:
                        break;
                }
                if (out.constant)
                    out.is_constant = true;
            }

            return out;
        }

        detail::ExprResult analyze_sizeof(ModuleInfo& mod, Scope const& scope, ast::SizeofExpr& s)
        {
            detail::ExprResult out{};
            out.type = m_types.usize_t();
            if (s.target)
            {
                auto target = resolve_type_node(mod, scope, s.target);
                if (target && mod.own_scope)
                    check_type_constraints_in_type(mod, *mod.own_scope, target, s.target->range);
                if (auto layout = layout_of(target))
                    out.constant = make_int_const(static_cast<std::int64_t>(layout->size), out.type);
            }

            out.is_constant = true;
            return out;
        }

        struct PackInfo
        {
            std::size_t count{0};
            std::vector<types::TypePtr> element_types;
        };

        [[nodiscard]] std::optional<PackInfo> resolve_pack_info(ModuleInfo& mod, Scope const& scope, std::string_view name, sm::SourceRange range)
        {
            std::ignore = mod;
            std::ignore = range;
            auto const* sym = lookup_name(mod, scope, name);
            if (!sym)
                return std::nullopt;

            if (sym->kind == SymbolKind::TemplateParam)
            {
                PackInfo info;
                info.count = 0;
                return info;
            }

            return std::nullopt;
        }

        detail::ExprResult analyze_sizeof_pack(ModuleInfo& mod, Scope const& scope, ast::SizeofPackExpr& s)
        {
            detail::ExprResult out{};
            out.type = m_types.usize_t();
            out.is_constant = true;

            auto pack = resolve_pack_info(mod, scope, s.pack_name, s.name_range);
            if (pack.has_value())
                out.constant = make_int_const(static_cast<std::int64_t>(pack->count), out.type);
            else
                error(s.name_range, "'{}' is not a pack", s.pack_name);

            return out;
        }

        detail::ExprResult analyze_alignof(ModuleInfo& mod, Scope const& scope, ast::AlignofExpr& s)
        {
            detail::ExprResult out{};
            out.type = m_types.usize_t();
            if (s.target)
            {
                auto target = resolve_type_node(mod, scope, s.target);
                if (target && mod.own_scope)
                    check_type_constraints_in_type(mod, *mod.own_scope, target, s.target->range);
                if (auto layout = layout_of(target))
                    out.constant = make_int_const(static_cast<std::int64_t>(layout->align), out.type);
            }

            out.is_constant = true;
            return out;
        }

        detail::ExprResult analyze_offsetof(ModuleInfo& mod, Scope const& scope, ast::OffsetofExpr& s)
        {
            detail::ExprResult out{};
            out.type = m_types.usize_t();
            auto const* target_ty = s.target ? resolve_type_node(mod, scope, s.target) : nullptr;
            if (target_ty && mod.own_scope && s.target)
                check_type_constraints_in_type(mod, *mod.own_scope, target_ty, s.target->range);
            if (target_ty &&
                (target_ty->kind == types::TypeKind::Struct || target_ty->kind == types::TypeKind::Union || target_ty->kind == types::TypeKind::Enum))
            {
                if (!target_ty->layout_is_default)
                {
                    if (target_ty->kind == types::TypeKind::Struct)
                    {
                        auto const& sd = *reinterpret_cast<ast::StructDecl const*>(static_cast<types::StructType const*>(target_ty)->decl);
                        for (auto const& f : sd.fields)
                            if (f.name == s.field)
                            {
                                out.constant = make_int_const(static_cast<std::int64_t>(f.byte_offset), out.type);
                                break;
                            }
                    }
                    else if (target_ty->kind == types::TypeKind::Union)
                    {
                        auto const& ud = *reinterpret_cast<ast::UnionDecl const*>(static_cast<types::UnionType const*>(target_ty)->decl);
                        for (auto const& f : ud.fields)
                            if (f.name == s.field)
                            {
                                out.constant = make_int_const(static_cast<std::int64_t>(f.byte_offset), out.type);
                                break;
                            }
                    }
                }
                else
                {
                    auto bindings = make_bindings(target_ty);
                    auto fields = record_fields(target_ty, bindings ? &*bindings : nullptr);
                    std::uint64_t offset{};
                    for (auto const& field : fields)
                    {
                        if (field.name == s.field)
                        {
                            if (auto layout = layout_of(field.type))
                                out.constant = make_int_const(static_cast<std::int64_t>(align_up(offset, layout->align)), out.type);
                            else
                                out.constant = make_int_const(static_cast<std::int64_t>(offset), out.type);
                            break;
                        }

                        if (auto layout = layout_of(field.type))
                            offset = align_up(offset, layout->align) + layout->size;
                        else
                            break;
                    }
                }
            }
            out.is_constant = true;
            return out;
        }

        detail::ExprResult analyze_compiles(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::CompilesExpr& c, int loop_depth, std::uint32_t& next_off,
                                            types::TypePtr, ConstEnv const* const_env)
        {
            auto* inner = make_scope(ScopeKind::Block, &scope);
            std::uint32_t tmp_off = next_off;
            [[maybe_unused]] ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
            auto* inner_consts = make_const_env(const_env);
            for (auto const& p : c.params)
            {
                auto type = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : nullptr;
                auto* v = make_local_decl(p.name, p.range, p.type, ast::StorageClass::Local, allocate_frame_slot(tmp_off, type));
                define_local(*inner, v);
            }

            std::ignore = analyze_block(mod, fn, *inner, c.body, loop_depth, tmp_off, nullptr, inner_consts);
            detail::ExprResult out{};
            out.type = m_types.m_boolt();
            auto const success = !suppress.had_suppressed_errors();
            out.constant = make_bool_const(success, out.type);
            out.is_constant = true;
            c.value = success;
            c.resolved = true;
            return out;
        }

        detail::ExprResult analyze_template_inst(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::TemplateInstExpr& t, int loop_depth,
                                                 std::uint32_t& next_off, [[maybe_unused]] types::TypePtr expected_type, ConstEnv const* const_env)
        {
            auto callee = analyze_expr_or_error(mod, fn, scope, t.callee, loop_depth, next_off, nullptr, const_env);
            if (has_error(callee.type))
                return callee;

            if (callee.resolved_decl && callee.resolved_decl->kind == ast::DeclKind::Func && types::type_cast<types::FuncPtrType>(callee.type))
            {
                ExplicitInstFailure failure{};
                ast::FuncDecl const* spec_decl{};
                if (auto fp = instantiate_explicit_function(mod, scope, *static_cast<ast::FuncDecl const*>(callee.resolved_decl), t.template_args, &failure,
                                                            &spec_decl);
                    fp)
                {
                    callee.type = *fp;
                    callee.spec_commit = detail::CommittedSpecialization{spec_decl, *fp};
                    callee.is_type_instantiation = true;
                    return callee;
                }

                if (failure == ExplicitInstFailure::Constraint)
                {
                    diagnose_explicit_constraint_failure(mod, scope, *static_cast<ast::FuncDecl const*>(callee.resolved_decl), t.template_args);
                    emit_constraint_error(
                        t.range, std::format("template constraint not satisfied for `{}`", static_cast<ast::FuncDecl const*>(callee.resolved_decl)->name));
                    return {m_types.m_errort()};
                }
            }

            auto try_direct = [&](std::span<Symbol const> syms, std::string_view display_name) -> detail::ExprResult {
                auto scan = scan_explicit_instantiations(mod, scope, syms, t.template_args);
                if (scan.viable.empty())
                {
                    if (scan.saw_value_arg)
                    {
                        error(t.range, "non-type template argument must be a constant expression");
                        return {m_types.m_errort()};
                    }
                    if (scan.saw_count_mismatch)
                    {
                        error(t.range, "template argument count mismatch for `{}`", display_name);
                        return {m_types.m_errort()};
                    }
                    if (scan.saw_non_template && !scan.saw_template)
                    {
                        error(t.range, "`{}` does not take template arguments", display_name);
                        return {m_types.m_errort()};
                    }
                    if (scan.saw_constraint_failure)
                    {
                        error(t.range, "template constraint not satisfied for `{}`", display_name);
                        return {m_types.m_errort()};
                    }
                    error(t.range, "no matching call for `{}`", display_name);
                    return {m_types.m_errort()};
                }

                if (scan.viable.size() != 1)
                {
                    error(t.range, "ambiguous template instantiation for `{}`", display_name);
                    return {m_types.m_errort()};
                }

                detail::ExprResult out{};
                auto& winner = scan.viable.front();
                if (winner.template_fn && winner.template_fn->name.starts_with("atomic_"))
                    validate_atomic_type_param(*winner.template_fn, winner.bindings, t.template_args);
                auto committed = commit_candidate(mod, winner, t.template_args);
                out.type = winner.fp;
                out.resolved_decl = winner.sym->decl;
                out.spec_commit = detail::CommittedSpecialization{committed.decl, winner.fp};
                out.is_type_instantiation = true;
                return out;
            };

            if (auto* id = ast::node_cast<ast::IdentExpr>(t.callee))
                return try_direct(lookup_candidates(mod, scope, id->name), id->name);

            if (auto* path = ast::node_cast<ast::PathExpr>(t.callee))
            {
                if (path->path.is_simple())
                    return try_direct(lookup_candidates(mod, scope, path->path.simple_name()), path->path.simple_name());

                if (mod.own_scope)
                {
                    auto const* sym = resolve_value_path_with_fallback(*mod.own_scope, path->path);
                    if (sym && sym->decl && sym->decl->kind == ast::DeclKind::Func)
                    {
                        auto const& f = *static_cast<ast::FuncDecl const*>(sym->decl);
                        ExplicitInstFailure failure{};
                        ast::FuncDecl const* spec_decl{};
                        if (auto fp = instantiate_explicit_function(mod, scope, f, t.template_args, &failure, &spec_decl))
                        {
                            detail::ExprResult out{};
                            out.type = *fp;
                            out.resolved_decl = sym->decl;
                            out.spec_commit = detail::CommittedSpecialization{spec_decl, *fp};
                            out.is_type_instantiation = true;
                            return out;
                        }

                        if (failure == ExplicitInstFailure::NotTemplate)
                            error(t.range, "`{}` does not take template arguments", path_str(path->path));
                        else if (failure == ExplicitInstFailure::CountMismatch)
                            error(t.range, "template argument count mismatch for `{}`", path_str(path->path));
                        else if (failure == ExplicitInstFailure::ValueArg)
                            error(t.range, "non-type template argument must be a constant expression");
                        else if (failure == ExplicitInstFailure::Constraint)
                        {
                            diagnose_explicit_constraint_failure(mod, scope, f, t.template_args);
                            emit_constraint_error(t.range, std::format("template constraint not satisfied for `{}`", path_str(path->path)));
                        }
                        else
                            error(t.range, "no matching call for `{}`", path_str(path->path));

                        return {m_types.m_errort()};
                    }
                }
            }

            auto out = std::move(callee);
            if (out.resolved_decl && out.resolved_decl->kind == ast::DeclKind::Func && types::type_cast<types::FuncPtrType>(out.type))
            {
                ExplicitInstFailure failure{};
                ast::FuncDecl const* spec_decl{};
                if (auto fp =
                        instantiate_explicit_function(mod, scope, *static_cast<ast::FuncDecl const*>(out.resolved_decl), t.template_args, &failure, &spec_decl);
                    fp)
                {
                    out.type = *fp;
                    out.spec_commit = detail::CommittedSpecialization{spec_decl, *fp};
                    out.is_type_instantiation = true;
                    return out;
                }
            }

            out.type = m_types.m_errort();
            if (!m_suppress_errors)
                error(t.range, "call target is not callable");

            return out;
        }

        [[nodiscard]] bool receiver_has_field_named(ModuleInfo const& mod, Scope& scope, ast::FieldAccessExpr const& fa, bool* out_is_callable = nullptr)
        {
            if (out_is_callable)
                *out_is_callable = false;

            std::function<types::TypePtr(ast::Expr const*)> get_receiver_type = [&](ast::Expr const* object) -> types::TypePtr {
                if (auto* id = ast::node_cast<ast::IdentExpr>(object))
                {
                    auto const* sym = lookup_name(mod, scope, id->name);
                    if (!sym || !sym->decl)
                        return nullptr;

                    return decl_type(*sym->decl);
                }

                if (auto* inner_fa = ast::node_cast<ast::FieldAccessExpr>(object))
                {
                    auto base_type = get_receiver_type(inner_fa->object);
                    if (!base_type || base_type->kind == types::TypeKind::Error)
                        return nullptr;

                    auto const* nominal = nominal_decl(base_type);
                    if (!nominal)
                        return nullptr;

                    auto* field = find_field(*const_cast<ast::Decl*>(nominal), inner_fa->field);
                    if (!field || !field->type || !field->type->sema.canonical)
                        return nullptr;

                    auto* field_type = get_canonical(field->type->sema);
                    if (!field_type)
                        return nullptr;

                    return substitute_in_nominal_context(field_type, base_type);
                }

                return nullptr;
            };

            auto* receiver_type = get_receiver_type(fa.object);
            if (!receiver_type || receiver_type->kind == types::TypeKind::Error)
                return false;

            auto const* nominal = nominal_decl(receiver_type);
            if (!nominal)
                return false;

            auto* field = find_field(*const_cast<ast::Decl*>(nominal), fa.field);
            if (!field)
                return false;

            if (out_is_callable && field->type && field->type->sema.canonical)
            {
                auto* field_type = get_canonical(field->type->sema);
                *out_is_callable = (types::type_cast<types::FuncPtrType>(field_type) != nullptr);
            }
            return true;
        }

        detail::ExprResult analyze_call(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::CallExpr& c, int loop_depth, std::uint32_t& next_off,
                                        types::TypePtr expected_type, ConstEnv const* const_env)
        {
            auto* generic_callee = c.callee;
            auto* template_callee = ast::node_cast<ast::TemplateInstExpr>(c.callee);
            bool defer_to_generic_resolution = false;
            if (auto* fa = ast::node_cast<ast::FieldAccessExpr>(c.callee))
            {
                bool field_is_callable = false;
                if (receiver_has_field_named(mod, scope, *fa, &field_is_callable))
                {
                    if (field_is_callable)
                        defer_to_generic_resolution = true;
                    else
                    {
                        error(fa->field_range, "field `{}` is not callable", fa->field);
                        return {m_types.m_errort()};
                    }
                }
                else
                    return resolve_ufcs(mod, fn, scope, *fa, c.args, loop_depth, next_off, const_env, expected_type);
            }

            if (auto* t = template_callee)
            {
                bool saw_probe_error = false;
                auto try_filtered = [&](std::span<Symbol const> syms, std::string_view display_name) -> std::optional<detail::ExprResult> {
                    auto scan = scan_explicit_instantiations(mod, scope, syms, t->template_args);
                    if (scan.viable.empty())
                    {
                        if (scan.saw_value_arg)
                        {
                            error(t->range, "value template arguments are not supported in function template instantiation");
                            return detail::ExprResult{m_types.m_errort()};
                        }
                        if (scan.saw_count_mismatch)
                        {
                            error(t->range, "template argument count mismatch for `{}`", display_name);
                            return detail::ExprResult{m_types.m_errort()};
                        }
                        if (scan.saw_non_template && !scan.saw_template)
                        {
                            error(t->range, "`{}` does not take template arguments", display_name);
                            return detail::ExprResult{m_types.m_errort()};
                        }
                        if (scan.saw_constraint_failure)
                        {
                            error(t->range, "template constraint not satisfied for `{}`", display_name);
                            return detail::ExprResult{m_types.m_errort()};
                        }
                        return std::nullopt;
                    }

                    std::vector<RankedCandidate> ranked;
                    ranked.reserve(scan.viable.size());
                    std::vector<CandidateInfo> rejected;
                    bool saw_constraint_failure = false;
                    bool saw_non_constraint_failure = false;
                    for (auto const& cand : scan.viable)
                    {
                        bool probe_error = false;
                        bool probe_constraint_failure = false;
                        bool probe_non_constraint_failure = false;
                        std::string rejection_reason;
                        if (auto probe = probe_candidate_from_params(mod, scope, *cand.sym, cand.fp->params, c.args, next_off, loop_depth, const_env,
                                                                     &probe_error, &probe_constraint_failure, &probe_non_constraint_failure, expected_type,
                                                                     true, &rejection_reason, cand.template_fn ? &cand.bindings : nullptr);
                            probe)
                        {
                            probe->explicit_fp = cand.fp;
                            ranked.push_back(std::move(*probe));
                        }
                        else if (!rejection_reason.empty())
                            collect_candidate_rejection(rejected, *cand.sym, std::move(rejection_reason), cand.fp);

                        saw_probe_error |= probe_error;
                        saw_constraint_failure |= probe_constraint_failure;
                        saw_non_constraint_failure |= probe_non_constraint_failure;
                    }

                    if (ranked.empty())
                    {
                        if (saw_probe_error)
                            return detail::ExprResult{m_types.m_errort()};

                        if (saw_constraint_failure && !saw_non_constraint_failure)
                        {
                            diagnose_implicit_constraint_failure(mod, scope, syms, c.args, next_off, loop_depth, const_env, expected_type);
                            emit_constraint_error(c.range, std::format("template constraint not satisfied for `{}`", display_name));
                            return detail::ExprResult{m_types.m_errort()};
                        }

                        if (!rejected.empty())
                            emit_overload_error(c.range, std::format("no matching call for `{}`", display_name), rejected);
                        else
                            error(c.range, "no matching call for `{}`", display_name);

                        return detail::ExprResult{m_types.m_errort()};
                    };

                    auto winner = choose_best_candidate(ranked);
                    if (!winner)
                    {
                        std::vector<CandidateInfo> ambig_candidates;
                        for (auto const& cand : ranked)
                            if (cand.sym)
                                collect_candidate_rejection(ambig_candidates, *cand.sym, "viable candidate with indistinguishable conversions",
                                                            cand.explicit_fp);

                        emit_overload_error(c.range, std::format("ambiguous call to `{}`", display_name), ambig_candidates);
                        return detail::ExprResult{m_types.m_errort()};
                    }

                    auto const& chosen = ranked[*winner];
                    if (!chosen.explicit_fp)
                        return std::nullopt;

                    std::ignore = analyze_expr(mod, fn, scope, *t->callee, loop_depth, next_off, expected_type, const_env);
                    set_resolved_type(t->sema, chosen.explicit_fp);
                    t->sema.resolved_decl = chosen.sym->decl;
                    t->sema.is_type_instantiation = true;

                    ExplicitInstCandidate const* winning_cand{};
                    for (auto const& cand : scan.viable)
                    {
                        if (cand.sym == chosen.sym)
                        {
                            winning_cand = &cand;
                            break;
                        }
                    }
                    if (!winning_cand)
                        return detail::ExprResult{m_types.m_errort()};
                    if (winning_cand->template_fn && winning_cand->template_fn->name.starts_with("atomic_"))
                        validate_atomic_type_param(*winning_cand->template_fn, winning_cand->bindings, t->template_args);
                    auto committed_spec = commit_candidate(mod, *winning_cand, t->template_args);
                    record_resolved_specialization(t->sema, &committed_spec);

                    auto out = invoke_explicit_ranked_candidate(mod, scope, chosen.sym, chosen.explicit_fp, committed_spec, c.args, c.range, loop_depth,
                                                                next_off, const_env);
                    if (!out)
                        return detail::ExprResult{m_types.m_errort()};
                    return *out;
                };

                if (auto* id = ast::node_cast<ast::IdentExpr>(t->callee))
                {
                    auto syms = lookup_candidates(mod, scope, id->name);
                    if (auto r = try_filtered(syms, id->name); r)
                        return *r;
                }
                else if (auto* path = ast::node_cast<ast::PathExpr>(t->callee))
                {
                    if (path->path.is_simple())
                    {
                        auto syms = lookup_candidates(mod, scope, path->path.simple_name());
                        if (auto r = try_filtered(syms, path->path.simple_name()); r)
                            return *r;
                    }
                    else if (mod.own_scope)
                    {
                        auto syms = resolve_value_overloads(*mod.own_scope, path->path);
                        if (auto r = try_filtered(syms, path_str(path->path)); r)
                            return *r;
                    }
                }
            }

            bool saw_probe_error = false;
            bool saw_constraint_failure = false;
            bool saw_non_constraint_failure = false;
            auto resolve_and_rank = [&](std::span<Symbol const> syms, std::string_view display_name) -> std::optional<detail::ExprResult> {
                std::vector<RankedCandidate> ranked;
                ranked.reserve(syms.size());
                std::vector<CandidateInfo> rejected;
                bool saw_function = false;
                for (auto const& sym : syms)
                {
                    if (!sym.decl || sym.decl->kind != ast::DeclKind::Func)
                        continue;

                    saw_function = true;
                    bool probe_error = false;
                    bool probe_constraint_failure = false;
                    bool probe_non_constraint_failure = false;
                    std::string rejection_reason;
                    if (auto probe = probe_candidate(mod, scope, sym, c.args, next_off, loop_depth, const_env, &probe_error, &probe_constraint_failure,
                                                     &probe_non_constraint_failure, expected_type, &rejection_reason);
                        probe)
                        ranked.push_back(std::move(*probe));
                    else if (!rejection_reason.empty())
                        collect_candidate_rejection(rejected, sym, std::move(rejection_reason));

                    saw_probe_error |= probe_error;
                    saw_constraint_failure |= probe_constraint_failure;
                    saw_non_constraint_failure |= probe_non_constraint_failure;
                }

                if (!saw_function)
                    return std::nullopt;

                if (ranked.empty())
                {
                    if (saw_probe_error)
                        return std::nullopt;

                    if (saw_constraint_failure && !saw_non_constraint_failure)
                    {
                        diagnose_implicit_constraint_failure(mod, scope, syms, c.args, next_off, loop_depth, const_env, expected_type);
                        emit_constraint_error(c.range, std::format("template constraint not satisfied for `{}`", display_name));
                        return detail::ExprResult{m_types.m_errort()};
                    }

                    if (!rejected.empty())
                        emit_overload_error(c.range, std::format("no matching call for `{}`", display_name), rejected);
                    else
                        error(c.range, "no matching call for `{}`", display_name);

                    return detail::ExprResult{m_types.m_errort()};
                }

                auto winner = choose_best_candidate(ranked);
                if (!winner)
                {
                    std::vector<CandidateInfo> ambig_candidates;
                    for (auto const& cand : ranked)
                        if (cand.sym)
                            collect_candidate_rejection(ambig_candidates, *cand.sym, "viable candidate with indistinguishable conversions", cand.explicit_fp);

                    emit_overload_error(c.range, std::format("ambiguous call to `{}`", display_name), ambig_candidates);
                    return detail::ExprResult{m_types.m_errort()};
                }

                return invoke_ranked_candidate(mod, scope, *ranked[*winner].sym, c.args, c.range, loop_depth, next_off, const_env, expected_type);
            };

            if (auto* id = ast::node_cast<ast::IdentExpr>(c.callee))
            {
                auto syms = lookup_candidates(mod, scope, id->name);
                bool has_function = std::ranges::any_of(syms, [](Symbol const& s) { return s.decl && s.decl->kind == ast::DeclKind::Func; });
                if (auto r = resolve_and_rank(syms, id->name); r)
                    return *r;
                if (has_function)
                {
                    if (saw_probe_error)
                        defer_to_generic_resolution = true;
                    else
                    {
                        error(c.range, "no matching call for `{}`", id->name);
                        return {m_types.m_errort()};
                    }
                }
            }

            if (auto* path = ast::node_cast<ast::PathExpr>(c.callee); path && path->path.is_simple())
            {
                auto syms = lookup_candidates(mod, scope, path->path.simple_name());
                bool has_function = std::ranges::any_of(syms, [](Symbol const& s) { return s.decl && s.decl->kind == ast::DeclKind::Func; });
                if (auto r = resolve_and_rank(syms, path->path.simple_name()); r)
                    return *r;

                if (has_function)
                {
                    if (saw_probe_error)
                        defer_to_generic_resolution = true;
                    else
                    {
                        error(c.range, "no matching call for `{}`", path->path.simple_name());
                        return {m_types.m_errort()};
                    }
                }
            }

            if (auto* path = ast::node_cast<ast::PathExpr>(c.callee); path && !path->path.is_simple() && mod.own_scope)
            {
                auto syms = resolve_value_overloads(*mod.own_scope, path->path);
                bool has_function = std::ranges::any_of(syms, [](Symbol const& s) { return s.decl && s.decl->kind == ast::DeclKind::Func; });
                if (auto r = resolve_and_rank(syms, path_str(path->path)); r)
                    return *r;

                if (has_function)
                {
                    if (saw_probe_error)
                        defer_to_generic_resolution = true;
                    else
                    {
                        error(c.range, "no matching call for `{}`", path_str(path->path));
                        return {m_types.m_errort()};
                    }
                }
            }

            if (defer_to_generic_resolution)
                goto generic_call_resolution;

        generic_call_resolution:
            m_analyzing_call_callee = true;
            auto callee = analyze_expr(mod, fn, scope, *generic_callee, loop_depth, next_off, expected_type, const_env);
            m_analyzing_call_callee = false;
            if (has_error(callee.type))
                return callee;

            if (callee.is_type_instantiation && callee.resolved_decl && callee.resolved_decl->kind == ast::DeclKind::Func &&
                types::type_cast<types::FuncPtrType>(callee.type))
                return invoke_funcptr(mod, scope, types::type_cast<types::FuncPtrType>(callee.type), c.args, c.range, loop_depth, next_off, const_env);

            if (callee.resolved_decl && callee.resolved_decl->kind == ast::DeclKind::Func)
                return invoke_function(mod, scope, *static_cast<ast::FuncDecl const*>(callee.resolved_decl), c.args, c.range, loop_depth, next_off, const_env,
                                       false, expected_type);

            if (auto* fp = types::type_cast<types::FuncPtrType>(callee.type))
                return invoke_funcptr(mod, scope, fp, c.args, c.range, loop_depth, next_off, const_env);

            if (callee.resolved_decl && callee.resolved_decl->kind == ast::DeclKind::Enum)
            {
                std::string_view variant_name;
                if (auto* path_expr = ast::node_cast<ast::PathExpr>(generic_callee))
                {
                    auto const& p = path_expr->path;
                    variant_name = p.is_simple() ? p.simple_name() : p.segments.back().name;
                }
                else if (auto* ident_expr = ast::node_cast<ast::IdentExpr>(generic_callee))
                    variant_name = ident_expr->name;

                if (!variant_name.empty())
                {
                    auto const* enum_decl = static_cast<ast::EnumDecl const*>(callee.resolved_decl);
                    auto const* variant = find_enum_variant(*enum_decl, variant_name);
                    if (variant)
                    {
                        bool declared_payloadless = variant->payload.empty();

                        if (declared_payloadless)
                        {
                            error(c.range, "enum variant `{}::{}` is payloadless and cannot be called with '()'", enum_decl->name, variant_name);
                            return {m_types.m_errort()};
                        }

                        types::TypePtr result_type = callee.type;

                        auto opt_bindings = create_template_bindings(*enum_decl, expected_type ? expected_type : callee.type);

                        bool any_arg_error = false;
                        std::vector<comptime::Value const*> arg_constants;
                        arg_constants.reserve(c.args.size());

                        bool is_void_normalized = false;
                        complete_tagged_enum(result_type);
                        if (result_type)
                        {
                            auto const* et = types::type_cast<types::EnumType>(result_type);
                            if (et && et->tagged_layout)
                            {
                                auto* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                                if (ed)
                                {
                                    std::ptrdiff_t idx = variant - ed->variants.data();
                                    if (idx >= 0 && static_cast<std::size_t>(idx) < et->tagged_layout->variant_count)
                                        is_void_normalized = (et->tagged_layout->variants[idx].variant_payload_type_or_null == nullptr);
                                }
                            }
                        }

                        if (is_void_normalized)
                        {
                            if (!c.sema.is_from_instantiation)
                            {
                                error(c.range, "enum variant `{}::{}` has void payload and cannot be called with arguments", enum_decl->name, variant_name);
                                return {m_types.m_errort()};
                            }

                            if (c.args.size() != variant->payload.size())
                            {
                                error(c.range, "enum variant `{}` payload arity mismatch: expected {}, got {}", variant_name, variant->payload.size(),
                                      c.args.size());
                                return {m_types.m_errort()};
                            }
                            for (std::size_t ai = 0; ai < c.args.size(); ++ai)
                            {
                                auto arg = analyze_expr(mod, fn, scope, *c.args[ai], loop_depth, next_off, nullptr, const_env);
                                if (has_error(arg.type))
                                    return {m_types.m_errort()};
                                if (arg.type && !is_concrete_void_type(arg.type) && arg.type->kind != types::TypeKind::Error)
                                {
                                    error(c.args[ai]->range, "enum variant `{}` payload type mismatch", variant_name);
                                    return {m_types.m_errort()};
                                }
                            }
                            detail::ExprResult out{};
                            out.type = result_type;
                            out.resolved_decl = callee.resolved_decl;
                            out.construction_kind = ConstructionKind::Enum;
                            out.constructed_variant = variant;
                            return out;
                        }

                        if (c.args.size() != variant->payload.size())
                        {
                            error(c.range, "enum variant `{}` payload arity mismatch: expected {}, got {}", variant_name, variant->payload.size(),
                                  c.args.size());
                            return {m_types.m_errort()};
                        }

                        for (std::size_t i = 0; i < c.args.size(); ++i)
                        {
                            auto payload_ty = resolve_payload_type(*enum_decl, variant->payload[i], callee.type, mod, scope);
                            if (opt_bindings && !opt_bindings->empty())
                                payload_ty = opt_bindings->substitute(payload_ty);

                            auto arg = analyze_expr(mod, fn, scope, *c.args[i], loop_depth, next_off, payload_ty, const_env);
                            if (has_error(arg.type))
                            {
                                any_arg_error = true;
                                arg_constants.push_back(nullptr);
                                continue;
                            }

                            arg_constants.push_back(arg.constant);

                            if (payload_ty && arg.type && arg.type->kind != types::TypeKind::Error)
                            {
                                if (contains_template_param(payload_ty))
                                {
                                    auto deduced_ty = (!opt_bindings || opt_bindings->empty()) ? payload_ty : opt_bindings->substitute(payload_ty);
                                    if (deduced_ty && contains_template_param(deduced_ty) && arg.type)
                                    {
                                        if (!opt_bindings)
                                            opt_bindings.emplace(m_types);
                                        std::ignore = opt_bindings->deduce(deduced_ty, arg.type);
                                    }

                                    if (opt_bindings && !opt_bindings->empty())
                                    {
                                        auto final_payload = opt_bindings->substitute(payload_ty);
                                        if (final_payload && arg.type && final_payload != arg.type && !contains_template_param(final_payload))
                                        {
                                            error(c.args[i]->range, "enum variant `{}` payload type mismatch", variant_name);
                                            any_arg_error = true;
                                        }
                                    }
                                }
                                else if (payload_ty != arg.type)
                                {
                                    error(c.args[i]->range, "enum variant `{}` payload type mismatch", variant_name);
                                    any_arg_error = true;
                                }
                            }
                        }

                        if (any_arg_error)
                            return {m_types.m_errort()};

                        if (opt_bindings && !opt_bindings->empty() && !enum_decl->template_params.empty())
                        {
                            std::vector<types::TypePtr> concrete_args;
                            concrete_args.reserve(enum_decl->template_params.size());
                            bool all_resolved = true;
                            for (std::size_t i = 0; i < enum_decl->template_params.size(); ++i)
                            {
                                auto const& tp = enum_decl->template_params[i];
                                auto* param_ty =
                                    m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                                auto resolved = opt_bindings->substitute(param_ty);
                                if (resolved && !contains_template_param(resolved))
                                    concrete_args.push_back(resolved);
                                else
                                {
                                    all_resolved = false;
                                    break;
                                }
                            }
                            if (all_resolved)
                                result_type = m_types.nominal_t(types::TypeKind::Enum, const_cast<ast::EnumDecl*>(enum_decl), concrete_args);
                            else
                                result_type = callee.type;
                        }
                        else
                        {
                            result_type = callee.type;
                            if (opt_bindings && !opt_bindings->empty() && contains_template_param(result_type))
                            {
                                auto substituted = opt_bindings->substitute(result_type);
                                if (substituted && !contains_template_param(substituted))
                                    result_type = substituted;
                            }
                        }

                        detail::ExprResult out{};
                        out.type = result_type;
                        out.resolved_decl = callee.resolved_decl;
                        out.construction_kind = ConstructionKind::Enum;
                        out.constructed_variant = variant;

                        complete_tagged_enum(out.type);

                        if (out.type)
                        {
                            bool all_const = !arg_constants.empty();
                            for (auto* arg_c : arg_constants)
                                if (!arg_c)
                                {
                                    all_const = false;
                                    break;
                                }

                            if (all_const)
                            {
                                out.constant = fold_tagged_enum_construction(variant, out.type, arg_constants);
                                if (out.constant)
                                    out.is_constant = true;
                            }
                        }

                        return out;
                    }
                }
            }

            error(c.range, "call target is not callable");
            return {m_types.m_errort()};
        }

        detail::ExprResult resolve_ufcs(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::FieldAccessExpr& f, std::span<ast::Expr* const> args,
                                        int loop_depth, std::uint32_t& next_off, ConstEnv const* const_env, types::TypePtr expected_type = nullptr)
        {
            std::ignore = fn;
            bool saw_probe_error = false;
            bool saw_constraint_failure = false;
            bool saw_non_constraint_failure = false;

            auto collect_unique = [&](Scope const* s, std::pmr::vector<Symbol const*>& out) {
                if (!s)
                    return;

                auto vs = s->lookup_values(f.field);
                for (auto const& sym : vs)
                {
                    if (!sym.decl || sym.decl->kind != ast::DeclKind::Func)
                        continue;

                    bool dup = false;
                    for (auto* existing : out)
                    {
                        if (existing->decl == sym.decl)
                        {
                            dup = true;
                            break;
                        }
                    }

                    if (!dup)
                        out.push_back(&sym);
                }
            };

            std::pmr::vector<Symbol const*> all_syms(m_alloc);
            if (mod.ufcs_scope)
                collect_unique(mod.ufcs_scope, all_syms);
            if (mod.own_scope)
                collect_unique(mod.own_scope, all_syms);

            if (m_specialization_defining_module)
            {
                if (m_specialization_defining_module->ufcs_scope)
                    collect_unique(m_specialization_defining_module->ufcs_scope, all_syms);
                if (m_specialization_defining_module->own_scope)
                    collect_unique(m_specialization_defining_module->own_scope, all_syms);
            }

            collect_unique(&scope, all_syms);
            for (auto const& imp : mod.imports)
            {
                if (imp.target && imp.target->export_scope)
                    collect_unique(imp.target->export_scope, all_syms);
            }

            if (all_syms.empty())
            {
                error(f.range, "no visible UFCS function named `{}` in scope", f.field);
                return {m_types.m_errort()};
            }

            {
                std::vector<RankedCandidate> ranked;
                ranked.reserve(all_syms.size());
                std::vector<CandidateInfo> rejected;

                for (auto const* sym : all_syms)
                {
                    bool probe_error = false;
                    bool probe_constraint_failure = false;
                    bool probe_non_constraint_failure = false;
                    std::string rejection_reason;

                    if (auto probe = probe_ufcs_candidate(mod, scope, *sym, *f.object, args, next_off, loop_depth, const_env, &probe_error,
                                                          &probe_constraint_failure, &probe_non_constraint_failure, expected_type, &rejection_reason);
                        probe)
                        ranked.push_back(std::move(*probe));
                    else if (!rejection_reason.empty())
                        collect_candidate_rejection(rejected, *sym, std::move(rejection_reason));

                    saw_probe_error |= probe_error;
                    saw_constraint_failure |= probe_constraint_failure;
                    saw_non_constraint_failure |= probe_non_constraint_failure;
                }

                if (!ranked.empty())
                {
                    auto winner = choose_best_candidate(ranked);
                    if (!winner)
                    {
                        std::vector<CandidateInfo> ambig_candidates;
                        for (auto const& cand : ranked)
                            if (cand.sym)
                                collect_candidate_rejection(ambig_candidates, *cand.sym, "viable candidate with indistinguishable conversions");

                        emit_overload_error(f.range, std::format("ambiguous UFCS call for `{}`", f.field), ambig_candidates);
                        return detail::ExprResult{m_types.m_errort()};
                    }

                    auto out_opt = invoke_ufcs_candidate(mod, scope, *ranked[*winner].sym, *f.object, args, f.range, loop_depth, next_off, const_env,
                                                         ranked[*winner].receiver_match, expected_type);
                    if (!out_opt)
                        return detail::ExprResult{m_types.m_errort()};

                    auto result = *out_opt;
                    result.ufcs_callee = ranked[*winner].sym->decl;
                    f.sema.ufcs_callee = ranked[*winner].sym->decl;
                    f.sema.resolved_decl = ranked[*winner].sym->decl;
                    return result;
                }

                if (saw_non_constraint_failure)
                {
                    if (!rejected.empty())
                    {
                        auto* probe_scope = make_probe_scope(scope);
                        auto receiver_r = analyze_expr(mod, nullptr, *probe_scope, *f.object, loop_depth, next_off, nullptr, const_env);
                        std::string receiver_ctx;
                        if (receiver_r.type && receiver_r.type->kind != types::TypeKind::Error)
                            receiver_ctx = std::format(" on receiver type `{}`", format_type_str(receiver_r.type));

                        emit_overload_error(f.range, std::format("no matching UFCS function for `{}`{}", f.field, receiver_ctx), rejected);
                    }
                    else
                        error(f.range, "no matching UFCS function for `{}`", f.field);

                    return detail::ExprResult{m_types.m_errort()};
                }
            }

            if (saw_constraint_failure && !saw_non_constraint_failure)
            {
                error(f.range, "template constraint not satisfied for `{}`", f.field);
                return {m_types.m_errort()};
            }

            if (saw_probe_error)
                return {m_types.m_errort()};

            error(f.range, "no matching UFCS function for `{}`", f.field);
            return {m_types.m_errort()};
        }

        detail::ExprResult invoke_function(ModuleInfo& mod, Scope& scope, ast::FuncDecl const& f, std::span<ast::Expr* const> arg_exprs, sm::SourceRange range,
                                           int loop_depth, std::uint32_t& next_off, ConstEnv const* const_env, bool quiet = false,
                                           types::TypePtr expected_type = nullptr)
        {
            std::vector<types::TypePtr> params;
            params.reserve(f.params.size());
            for (auto const& p : f.params)
                params.push_back(p.type ? get_canonical(p.type->sema) : m_types.m_errort());

            std::size_t num_value_tparams = 0;
            for (auto const& tp : f.template_params)
                if (tp.value_type)
                    ++num_value_tparams;

            auto pack_arity = compute_pack_arity(&f, params.size());
            bool has_func_pack = pack_arity.has_pack;
            std::size_t non_pack_func_params = pack_arity.non_pack_count;
            std::size_t min_required = pack_arity.min_required;

            if (!has_func_pack)
            {
                if (params.size() + num_value_tparams != arg_exprs.size())
                {
                    if (!quiet)
                    {
                        auto loc = format_source_location(f.range);
                        error(range, "argument count mismatch for `{}`: expected {}, got {}", f.name, params.size() + num_value_tparams, arg_exprs.size());
                        m_diag.note(f.range, "declared at {}", loc);
                    }
                    return {m_types.m_errort()};
                }
            }
            else
            {
                if (arg_exprs.size() < min_required + num_value_tparams)
                {
                    if (!quiet)
                    {
                        auto loc = format_source_location(f.range);
                        error(range, "argument count mismatch for `{}`: expected at least {} (with pack), got {}", f.name, min_required + num_value_tparams,
                              arg_exprs.size());
                        m_diag.note(f.range, "declared at {}", loc);
                    }
                    return {m_types.m_errort()};
                }
            }

            infer::TemplateBindings b{m_types};
            if (expected_type && !f.template_params.empty() && f.return_type)
            {
                auto return_ty = get_canonical(f.return_type->sema);
                if (return_ty && contains_template_param(return_ty) && !contains_template_param(expected_type))
                    std::ignore = b.deduce(return_ty, expected_type);
            }

            for (std::size_t vi = 0; vi < num_value_tparams; ++vi)
            {
                ast::TemplateParam const* vtparam = nullptr;
                {
                    std::size_t idx = 0;
                    for (auto const& tp : f.template_params)
                        if (tp.value_type)
                        {
                            if (idx == vi)
                            {
                                vtparam = &tp;
                                break;
                            }
                            ++idx;
                        }
                }

                if (!vtparam || !vtparam->value_type || !vtparam->value_type->sema.canonical)
                    return {m_types.m_errort()};

                auto vt_type = get_canonical(vtparam->value_type->sema);
                auto r = analyze_expr(mod, nullptr, scope, *arg_exprs[vi], loop_depth, next_off, vt_type, const_env);
                if (has_error(r.type))
                    return {m_types.m_errort()};

                if (!r.constant)
                {
                    if (!quiet)
                        error(arg_exprs[vi]->range, "non-type template argument must be a constant expression");

                    return {m_types.m_errort()};
                }

                {
                    auto* pty = m_types.template_param_t(const_cast<ast::TemplateParam*>(vtparam), vtparam->name, static_cast<std::uint32_t>(vi));
                    if (pty)
                        b.bind_value(static_cast<types::TemplateParamType const*>(pty), *r.constant);
                }
            }

            std::size_t func_arg_start = num_value_tparams;
            std::size_t func_arg_count = arg_exprs.size() - num_value_tparams;

            std::vector<detail::ExprResult> args;
            args.reserve(func_arg_count);
            std::vector<types::TypePtr> pack_arg_types;
            std::vector<comptime::Value> pack_arg_values;

            for (std::size_t i = 0; i < non_pack_func_params; ++i)
            {
                auto param_ty = b.substitute(params[i]);
                args.push_back(analyze_expr(mod, nullptr, scope, *arg_exprs[func_arg_start + i], loop_depth, next_off, param_ty, const_env));
            }

            if (has_func_pack)
            {
                for (std::size_t i = non_pack_func_params; i < func_arg_count; ++i)
                {
                    auto pack_param_ty = b.substitute(params.back());
                    types::TypePtr expected_ty = nullptr;
                    if (auto const* pt = types::type_cast<types::TypePackType>(pack_param_ty))
                        expected_ty = b.substitute(pt->element);
                    auto r = analyze_expr(mod, nullptr, scope, *arg_exprs[func_arg_start + i], loop_depth, next_off, expected_ty, const_env);
                    pack_arg_types.push_back(r.type);
                    if (r.constant && r.constant->kind() == comptime::Value::Kind::String)
                        pack_arg_values.push_back(*r.constant);
                    else
                        pack_arg_values.push_back(comptime::Value{});
                    args.push_back(std::move(r));
                }
            }

            if (std::ranges::any_of(args, [](detail::ExprResult const& r) { return has_error(r.type); }))
                return {m_types.m_errort()};

            if (f.name.starts_with("atomic_") && !args.empty() && !num_value_tparams)
            {
                auto op = classify_atomic_op(f.name);
                if (op != AtomicOp::Unknown)
                {
                    std::size_t order_idx = args.size() - 1;
                    auto const* order_expr = arg_exprs[func_arg_start + order_idx];
                    validate_atomic_order(f, args[order_idx], order_expr, op);
                }
            }

            std::vector<types::TypePtr> actuals;
            actuals.reserve(args.size());
            for (auto const& a : args)
                actuals.push_back(a.type);

            std::vector<types::TypePtr> deduce_params;
            deduce_params.reserve(non_pack_func_params + (has_func_pack ? 1 : 0));
            for (std::size_t i = 0; i < non_pack_func_params; ++i)
                deduce_params.push_back(params[i]);
            if (has_func_pack)
            {
                auto pack_elem = params.back();
                if (auto const* pt = types::type_cast<types::TypePackType>(params.back()))
                    pack_elem = pt->element;
                deduce_params.push_back(m_types.type_pack_t(pack_elem));
            }

            auto deduce_result = b.deduce_function(deduce_params, actuals);
            if (!deduce_result)
            {
                if (!quiet)
                {
                    bool reported = false;
                    for (std::size_t i = 0; i < actuals.size() && i < deduce_params.size(); ++i)
                    {
                        auto param_ty = b.substitute(deduce_params[i]);
                        if (actuals[i] != param_ty && !has_error(actuals[i]) && !has_error(param_ty))
                        {
                            error(range, "argument {} of call to `{}` has wrong type: expected `{}`, found `{}`", i + 1, f.name, format_type_str(param_ty),
                                  format_type_str(actuals[i]));

                            reported = true;
                            break;
                        }
                    }
                    if (!reported)
                    {
                        auto loc = format_source_location(f.range);
                        error(range, "call argument mismatch for `{}`: {}", f.name,
                              deduce_result.detail.empty() ? "template argument deduction failed" : deduce_result.detail);
                        m_diag.note(f.range, "declared at {}", loc);
                    }
                    else
                    {
                        auto loc = format_source_location(f.range);
                        m_diag.note(f.range, "declared at {}", loc);
                    }
                }

                return {m_types.m_errort()};
            }

            if (f.name.starts_with("atomic_"))
            {
                sm::SourceRange type_arg_range = range;
                if (func_arg_start < arg_exprs.size())
                    type_arg_range = arg_exprs[func_arg_start]->range;
                validate_atomic_type_param(f, b, {}, type_arg_range);
            }

            if (has_func_pack && !pack_arg_types.empty())
            {
                if (!f.template_params.empty() && f.template_params.back().is_pack)
                {
                    auto const& tp = f.template_params.back();
                    auto* pack_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name,
                                                             static_cast<std::uint32_t>(f.template_params.size() - 1));
                    if (pack_ty)
                    {
                        bool has_any_value = std::ranges::any_of(pack_arg_values, [](comptime::Value const& v) { return v.kind() != comptime::Value::Kind::Null; });
                        if (has_any_value)
                            std::ignore = b.bind_pack(static_cast<types::TemplateParamType const*>(pack_ty), pack_arg_types, pack_arg_values);
                        else
                            std::ignore = b.bind_pack(static_cast<types::TemplateParamType const*>(pack_ty), pack_arg_types);
                    }
                }
            }

            detail::CommittedSpecialization committed_spec = commit_specialization(mod, f, b, range);

            detail::ExprResult out{};
            out.type = b.substitute(f.return_type ? get_canonical(f.return_type->sema) : m_types.m_voidt());
            out.resolved_decl = &f;
            out.spec_commit = committed_spec;
            return out;
        }

        detail::ExprResult invoke_funcptr(ModuleInfo& mod, Scope& scope, types::FuncPtrType const* fp, std::span<ast::Expr* const> arg_exprs,
                                          sm::SourceRange range, int loop_depth, std::uint32_t& next_off, ConstEnv const* const_env, bool quiet = false)
        {
            if (fp->params.size() != arg_exprs.size())
            {
                if (!quiet)
                {
                    error(range, "argument count mismatch for function pointer call: expected {}, got {}", fp->params.size(), arg_exprs.size());
                    m_diag.note(range, "callee type is `{}`", format_type_str(types::TypePtr{fp}));
                }

                return {m_types.m_errort()};
            }

            std::vector<detail::ExprResult> args;
            args.reserve(arg_exprs.size());
            for (std::size_t i = 0; i < arg_exprs.size(); ++i)
                args.push_back(analyze_expr(mod, nullptr, scope, *arg_exprs[i], loop_depth, next_off, fp->params[i], const_env));

            if (std::ranges::any_of(args, [](detail::ExprResult const& r) { return has_error(r.type); }))
                return {m_types.m_errort()};

            std::vector<types::TypePtr> actuals;
            actuals.reserve(args.size());
            for (auto const& a : args)
                actuals.push_back(a.type);

            infer::TemplateBindings b{m_types};
            if (!b.deduce_function(fp->params, actuals))
            {
                if (!quiet)
                {
                    bool reported = false;
                    for (std::size_t i = 0; i < actuals.size(); ++i)
                    {
                        if (actuals[i] != fp->params[i] && !has_error(actuals[i]) && !has_error(fp->params[i]))
                        {
                            error(range, "argument {} of function pointer call has wrong type: expected `{}`, found `{}`", i + 1,
                                  format_type_str(fp->params[i]), format_type_str(actuals[i]));

                            reported = true;
                            break;
                        }
                    }
                    if (!reported)
                        error(range, "function pointer call argument mismatch");

                    m_diag.note(range, "callee type is `{}`", format_type_str(types::TypePtr{fp}));
                }

                return {m_types.m_errort()};
            }

            detail::ExprResult out{};
            out.type = b.substitute(fp->return_type);
            return out;
        }

        detail::StmtResult analyze_static_type_match(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::StaticMatchStmt& sm, int loop_depth,
                                                     std::uint32_t& next_off, ConstEnv const* const_env)
        {
            detail::StmtResult out{};

            if (!fn || fn->template_params.empty())
            {
                error(sm.range, "static type match requires a template parameter");
                return out;
            }

            std::string_view param_name;
            if (auto const* ident = ast::node_cast<ast::IdentExpr>(sm.operand))
                param_name = ident->name;
            if (param_name.empty())
            {
                error(sm.range, "static type match scrutinee must name a template parameter");
                return out;
            }

            types::TypePtr scrutinee_type = nullptr;
            std::uint32_t param_index = 0;
            for (auto const& tp : fn->template_params)
            {
                if (tp.name == param_name)
                {
                    scrutinee_type = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, param_index);
                    break;
                }
                ++param_index;
            }

            if (!scrutinee_type)
            {
                error(sm.range, "template parameter `{}` not found", param_name);
                return out;
            }

            sm.taken_arm = -1;
            for (std::size_t i = 0; i < sm.arms.size(); ++i)
            {
                auto& arm = sm.arms[i];
                bool matched = false;

                if (arm.is_wildcard)
                    matched = true;
                else if (arm.type_pattern)
                {
                    auto arm_type = resolve_type_node(mod, scope, arm.type_pattern);
                    if (!arm_type || arm_type->kind == types::TypeKind::Error)
                        continue;

                    if (arm_type == scrutinee_type)
                        matched = true;
                    else if (auto const* named = ast::node_cast<ast::NamedType>(arm.type_pattern))
                    {
                        std::string_view concept_name = named->path.is_simple() ? named->path.simple_name() : std::string_view{};
                        if (!concept_name.empty())
                        {
                            if (auto const* sym = scope.lookup_type(concept_name))
                            {
                                if (auto const* use = ast::node_cast<ast::UsingDecl>(sym->decl); use && use->using_kind == ast::UsingKind::Concept)
                                {
                                    infer::TemplateBindings concept_bindings{m_types};
                                    if (!use->template_params.empty())
                                    {
                                        std::uint32_t ci = 0;
                                        for (auto const& ctp : use->template_params)
                                        {
                                            auto param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(ctp)), ctp.name, ci);
                                            std::ignore = concept_bindings.deduce(param_ty, scrutinee_type);
                                            ++ci;
                                        }
                                    }

                                    [[maybe_unused]] ErrorSuppressionGuard suppress{m_suppress_errors, m_suppressed_error_count};
                                    if (fn)
                                    {
                                        auto result = evaluate_concept_decl(mod, scope, fn, *use, concept_bindings);
                                        if (result.value_or(false) && !suppress.had_suppressed_errors())
                                            matched = true;
                                    }
                                }
                            }
                        }
                    }
                }
                else
                    continue;

                bool guard_is_true = true;
                if (arm.guard)
                {
                    auto* inner = make_scope(ScopeKind::Block, &scope);
                    auto* inner_consts = make_const_env(const_env);
                    auto guard = analyze_expr(mod, fn, *inner, *arm.guard, loop_depth, next_off, nullptr, inner_consts);
                    guard_is_true = guard.constant && guard.constant->kind() == comptime::Value::Kind::Bool && guard.constant->get_bool();
                }

                if (matched && guard_is_true)
                {
                    sm.taken_arm = static_cast<std::int32_t>(i);
                    if (arm.body)
                    {
                        auto* inner = make_scope(ScopeKind::Block, &scope);
                        auto* inner_consts = make_const_env(const_env);
                        std::ignore = analyze_expr(mod, fn, *inner, *arm.body, loop_depth, next_off, nullptr, inner_consts);
                    }
                    return out;
                }
            }

            error(sm.range, "static type match is not exhaustive for `{}`", param_name);
            return out;
        }

        detail::StmtResult analyze_static_type_if(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::StaticIfStmt& si, int loop_depth,
                                                  std::uint32_t& next_off, ConstEnv const* const_env)
        {
            detail::StmtResult out{};

            auto const* bin = ast::node_cast<ast::BinaryExpr>(si.condition);
            if (!bin || bin->op != lex::TokenKind::EqEq)
            {
                si.taken_branch = 0;
                auto* inner = make_scope(ScopeKind::Block, &scope);
                auto* inner_consts = make_const_env(const_env);
                out = analyze_block(mod, fn, *inner, si.then_block, loop_depth, next_off, nullptr, inner_consts);

                out.foldable = false;
                return out;
            }

            auto const* lhs_ident = ast::node_cast<ast::IdentExpr>(bin->lhs);
            if (!lhs_ident)
            {
                si.taken_branch = 0;
                auto* inner = make_scope(ScopeKind::Block, &scope);
                auto* inner_consts = make_const_env(const_env);
                out = analyze_block(mod, fn, *inner, si.then_block, loop_depth, next_off, nullptr, inner_consts);

                out.foldable = false;
                return out;
            }

            std::string_view param_name = lhs_ident->name;
            types::TypePtr scrutinee_type = nullptr;
            if (fn)
            {
                std::uint32_t param_index = 0;
                for (auto const& tp : fn->template_params)
                {
                    if (tp.name == param_name)
                    {
                        scrutinee_type = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, param_index);
                        break;
                    }
                    ++param_index;
                }
            }

            if (!scrutinee_type)
            {
                if (!fn || !fn->template_params.empty())
                    si.taken_branch = 0;

                if (si.taken_branch == 0)
                {
                    auto* inner = make_scope(ScopeKind::Block, &scope);
                    auto* inner_consts = make_const_env(const_env);
                    out = analyze_block(mod, fn, *inner, si.then_block, loop_depth, next_off, nullptr, inner_consts);
                }
                else if (si.else_branch)
                {
                    out = analyze_stmt(mod, fn, scope, *si.else_branch, loop_depth, next_off, const_env);
                    if (auto* es = ast::node_cast<ast::ExprStmt>(si.else_branch))
                    {
                        if (es->expr && es->expr->sema.is_diverging)
                            out.falls_through = false;
                    }
                }

                out.foldable = false;
                return out;
            }

            auto const* rhs_ident = ast::node_cast<ast::IdentExpr>(bin->rhs);
            types::TypePtr target_type = nullptr;
            if (rhs_ident)
            {
                ast::Path path(m_ast_ctx.allocator());
                path.segments.push_back({rhs_ident->name, rhs_ident->range});
                path.range = rhs_ident->range;
                auto* type_expr = m_ast_ctx.make<ast::NamedType>(rhs_ident->range, std::move(path));
                target_type = resolve_type_node(mod, scope, type_expr);
            }

            bool scrutinee_unresolved =
                !scrutinee_type || scrutinee_type->kind == types::TypeKind::TemplateParam || scrutinee_type->kind == types::TypeKind::Error;
            bool types_match =
                !scrutinee_unresolved && target_type && scrutinee_type && (target_type == scrutinee_type || target_type->kind == scrutinee_type->kind);
            bool take_then = scrutinee_unresolved || types_match;

            si.taken_branch = take_then ? 0 : 1;

            if (take_then)
            {
                auto* inner = make_scope(ScopeKind::Block, &scope);
                auto* inner_consts = make_const_env(const_env);
                out = analyze_block(mod, fn, *inner, si.then_block, loop_depth, next_off, nullptr, inner_consts);
            }
            else if (si.else_branch)
            {
                out = analyze_stmt(mod, fn, scope, *si.else_branch, loop_depth, next_off, const_env);
                if (auto* es = ast::node_cast<ast::ExprStmt>(si.else_branch))
                {
                    if (es->expr && es->expr->sema.is_diverging)
                        out.falls_through = false;
                }
            }

            out.foldable = false;
            return out;
        }

        detail::StmtResult analyze_stmt(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::Stmt& s, int loop_depth, std::uint32_t& next_off,
                                        ConstEnv const* const_env)
        {
            detail::StmtResult out{};
            auto is_const_true = [](detail::ExprResult const& r) {
                return r.constant && r.constant->kind() == comptime::Value::Kind::Bool && r.constant->get_bool();
            };
            switch (s.kind)
            {
                case ast::StmtKind::Expr: {
                    auto* expr = static_cast<ast::ExprStmt&>(s).expr;
                    if (expr)
                        std::ignore = analyze_expr(mod, fn, scope, *expr, loop_depth, next_off, nullptr, const_env);
                    out.foldable = false;
                    return out;
                }
                case ast::StmtKind::DeclStmt:
                    out = analyze_decl_stmt(mod, fn, scope, *static_cast<ast::DeclStmt&>(s).decl, loop_depth, next_off, const_env);
                    return out;
                case ast::StmtKind::Return:
                    static_cast<ast::ReturnStmt&>(s).exit_defers.clear();
                    if (fn)
                    {
                        auto& r = static_cast<ast::ReturnStmt&>(s);
                        auto expected = fn->return_type ? get_canonical(fn->return_type->sema) : m_types.m_voidt();
                        if (r.value)
                        {
                            auto got = analyze_expr(mod, fn, scope, *r.value, loop_depth, next_off, expected, const_env);
                            if (has_error(got.type))
                                ;
                            else if (expected == m_types.m_voidt())
                                error(s.range, "void function cannot return a value");
                            else if (!can_assign_return(expected, got.type) && !try_implicit_enum_conversion(expected, got.type, mod, scope))
                                error(s.range, "return type mismatch");
                        }
                        else if (expected != m_types.m_voidt())
                            error(s.range, "missing return value");
                    }
                    if (m_defer_depth)
                    {
                        error(s.range, "deferred statement must not transfer control");
                        return {.foldable = false};
                    }
                    static_cast<ast::ReturnStmt&>(s).exit_defers = snapshot_exit_defers();
                    return {.falls_through = false, .diverges = true, .foldable = false};
                case ast::StmtKind::Break:
                    static_cast<ast::BreakStmt&>(s).exit_defers.clear();
                    if (m_defer_depth)
                    {
                        error(s.range, "deferred statement must not transfer control");
                        return {.foldable = false};
                    }
                    if (loop_depth == 0)
                        error(s.range, "break outside loop");
                    static_cast<ast::BreakStmt&>(s).exit_defers = snapshot_exit_defers();
                    return {.falls_through = false, .foldable = false};
                case ast::StmtKind::Continue:
                    static_cast<ast::ContinueStmt&>(s).exit_defers.clear();
                    if (m_defer_depth)
                    {
                        error(s.range, "deferred statement must not transfer control");
                        return {.foldable = false};
                    }
                    if (loop_depth == 0)
                        error(s.range, "continue outside loop");
                    static_cast<ast::ContinueStmt&>(s).exit_defers = snapshot_exit_defers();
                    return {.falls_through = false, .foldable = false};
                case ast::StmtKind::While: {
                    auto& w = static_cast<ast::WhileStmt&>(s);
                    auto cond = analyze_expr_or_error(mod, fn, scope, w.condition, loop_depth, next_off, nullptr, const_env);
                    auto* inner = make_scope(ScopeKind::Block, &scope);
                    auto* inner_consts = make_const_env(const_env);
                    auto body = analyze_block(mod, fn, *inner, w.body, loop_depth + 1, next_off, nullptr, inner_consts);
                    if (is_const_true(cond) && body.diverges)
                    {
                        out.diverges = true;
                        out.falls_through = false;
                    }
                    out.foldable = false;
                    return out;
                }
                case ast::StmtKind::DoWhile: {
                    auto& w = static_cast<ast::DoWhileStmt&>(s);
                    auto* inner = make_scope(ScopeKind::Block, &scope);
                    auto* inner_consts = make_const_env(const_env);
                    auto body = analyze_block(mod, fn, *inner, w.body, loop_depth + 1, next_off, nullptr, inner_consts);
                    std::ignore = analyze_expr_or_error(mod, fn, scope, w.condition, loop_depth, next_off, nullptr, const_env);
                    if (body.diverges)
                    {
                        out.diverges = true;
                        out.falls_through = false;
                    }
                    out.foldable = false;
                    return out;
                }
                case ast::StmtKind::For: {
                    auto& f = static_cast<ast::ForStmt&>(s);
                    auto* inner = make_scope(ScopeKind::Block, &scope);
                    auto* inner_consts = make_const_env(const_env);
                    if (f.init)
                        std::ignore = analyze_stmt(mod, fn, *inner, *f.init, loop_depth, next_off, inner_consts);
                    auto cond = f.cond ? analyze_expr(mod, fn, *inner, *f.cond, loop_depth, next_off, nullptr, inner_consts) : detail::ExprResult{};
                    if (f.update)
                        std::ignore = analyze_expr(mod, fn, *inner, *f.update, loop_depth, next_off, nullptr, inner_consts);
                    auto body = analyze_block(mod, fn, *inner, f.body, loop_depth + 1, next_off, nullptr, inner_consts);
                    if ((!f.cond || is_const_true(cond)) && body.diverges)
                    {
                        out.diverges = true;
                        out.falls_through = false;
                    }
                    out.foldable = false;
                    return out;
                }
                case ast::StmtKind::ForIn: {
                    auto& f = static_cast<ast::ForInStmt&>(s);

                    bool const is_direct_range = f.iterable && f.iterable->kind == ast::ExprKind::Range;
                    bool const saved_allow_range = m_allow_range_expr;
                    if (is_direct_range)
                        m_allow_range_expr = true;

                    auto iterable_result = analyze_expr_or_error(mod, fn, scope, f.iterable, loop_depth, next_off, nullptr, const_env);

                    m_allow_range_expr = saved_allow_range;

                    if (!has_error(iterable_result.type) && iterable_result.type &&
                        (iterable_result.type->kind == types::TypeKind::Range || iterable_result.type->kind == types::TypeKind::RangeInclusive) &&
                        !is_direct_range)
                    {
                        error(f.iterable->range, "cannot iterate a range value; iterate an array or slice, or use a literal range");
                    }

                    auto* inner = make_scope(ScopeKind::Block, &scope);
                    auto* inner_consts = make_const_env(const_env);
                    if (!f.item_name.empty())
                    {
                        if (f.item_type && !f.item_type->sema.canonical)
                            std::ignore = resolve_and_substitute_type_expr(mod, scope, f.item_type);

                        types::TypePtr element_type = nullptr;
                        types::Qual element_quals = types::Qual::None;
                        if (iterable_result.type)
                        {
                            switch (iterable_result.type->kind)
                            {
                                case types::TypeKind::Array:
                                    element_type = static_cast<types::ArrayType const*>(iterable_result.type)->element;
                                    break;
                                case types::TypeKind::RuntimeArray:
                                    element_type = static_cast<types::RuntimeArrayType const*>(iterable_result.type)->element;
                                    break;
                                case types::TypeKind::Slice:
                                    element_type = static_cast<types::SliceType const*>(iterable_result.type)->element;
                                    element_quals = static_cast<types::SliceType const*>(iterable_result.type)->element_quals;
                                    break;
                                case types::TypeKind::Range:
                                    element_type = static_cast<types::RangeType const*>(iterable_result.type)->element;
                                    break;
                                case types::TypeKind::RangeInclusive:
                                    element_type = static_cast<types::RangeInclusiveType const*>(iterable_result.type)->element;
                                    break;
                                default:
                                    break;
                            }
                        }

                        types::TypePtr binding_type = element_type;
                        if (f.by_reference && element_type)
                            binding_type = m_types.pointer_to(element_type, element_quals);

                        auto* v = make_local_decl(
                            f.item_name, f.name_range, f.item_type, ast::StorageClass::Local,
                            allocate_frame_slot(next_off, f.item_type && f.item_type->sema.canonical ? get_canonical(f.item_type->sema) : binding_type));

                        if (!f.item_type || !f.item_type->sema.canonical)
                            register_inferred_local_type(v, binding_type ? binding_type : m_types.m_errort());

                        define_local(*inner, v);
                        track_decl_write(v);
                    }
                    std::ignore = analyze_block(mod, fn, *inner, f.body, loop_depth + 1, next_off, nullptr, inner_consts);
                    out.foldable = false;
                    return out;
                }
                case ast::StmtKind::Defer:
                    if (m_defer_depth)
                    {
                        error(s.range, "defer is not allowed inside a deferred statement");
                        return {.foldable = false};
                    }
                    if (static_cast<ast::DeferStmt&>(s).body)
                    {
                        ++m_defer_depth;
                        std::ignore = analyze_stmt(mod, fn, scope, *static_cast<ast::DeferStmt&>(s).body, loop_depth, next_off, const_env);
                        --m_defer_depth;
                    }
                    m_active_defers.push_back(s.range);
                    out.foldable = false;
                    return out;
                case ast::StmtKind::StaticIf: {
                    auto& si = static_cast<ast::StaticIfStmt&>(s);
                    if (si.is_type_if)
                        return analyze_static_type_if(mod, fn, scope, si, loop_depth, next_off, const_env);

                    auto cond = analyze_expr_or_error(mod, fn, scope, si.condition, loop_depth, next_off, nullptr, const_env);
                    bool take_then = !(cond.constant && cond.constant->kind() == comptime::Value::Kind::Bool) || cond.constant->get_bool();
                    si.taken_branch = take_then ? 0 : 1;
                    if (take_then)
                    {
                        auto* inner = make_scope(ScopeKind::Block, &scope);
                        auto* inner_consts = make_const_env(const_env);
                        out = analyze_block(mod, fn, *inner, si.then_block, loop_depth, next_off, nullptr, inner_consts);
                    }
                    else if (si.else_branch)
                    {
                        out = analyze_stmt(mod, fn, scope, *si.else_branch, loop_depth, next_off, const_env);
                        if (auto* es = ast::node_cast<ast::ExprStmt>(si.else_branch))
                            if (es->expr && es->expr->sema.is_diverging)
                                out.falls_through = false;
                    }
                    out.foldable = false;
                    return out;
                }
                case ast::StmtKind::StaticMatch: {
                    auto& sm = static_cast<ast::StaticMatchStmt&>(s);
                    if (sm.is_type_match)
                        return analyze_static_type_match(mod, fn, scope, sm, loop_depth, next_off, const_env);

                    auto operand = analyze_expr_or_error(mod, fn, scope, sm.operand, loop_depth, next_off, nullptr, const_env);
                    if (!operand.constant && sm.operand)
                        error(sm.operand->range, "static match operand must be constant");

                    sm.taken_arm = -1;
                    for (std::size_t i = 0; i < sm.arms.size(); ++i)
                    {
                        auto* inner = make_scope(ScopeKind::Block, &scope);
                        auto* inner_consts = make_const_env(const_env);
                        bool pattern_ok = true;
                        bool guard_is_true = !sm.arms[i].guard;
                        if (sm.arms[i].pattern)
                        {
                            auto validated = validate_pattern(mod, *sm.arms[i].pattern, operand.type, *inner, const_env);
                            pattern_ok = validated.ok;
                            if (validated.ok)
                                install_pattern_bindings(*inner, validated);
                        }
                        if (sm.arms[i].guard)
                        {
                            auto guard = analyze_expr(mod, fn, *inner, *sm.arms[i].guard, loop_depth, next_off, nullptr, inner_consts);
                            guard_is_true = guard.constant && guard.constant->kind() == comptime::Value::Kind::Bool && guard.constant->get_bool();
                        }
                        if (pattern_ok && operand.constant && sm.arms[i].pattern && guard_is_true &&
                            pattern_matches_const(mod, *sm.arms[i].pattern, operand, *inner))
                        {
                            sm.taken_arm = static_cast<std::int32_t>(i);
                            if (sm.arms[i].body)
                                std::ignore = analyze_expr(mod, fn, *inner, *sm.arms[i].body, loop_depth, next_off, nullptr, inner_consts);

                            break;
                        }
                    }
                    out.foldable = false;
                    return out;
                }
                case ast::StmtKind::StaticFor: {
                    auto& sf = static_cast<ast::StaticForStmt&>(s);

                    if (sf.is_range_for)
                    {
                        if (sf.pack_expr)
                            std::ignore = analyze_expr(mod, fn, scope, *sf.pack_expr, loop_depth, next_off, nullptr, const_env);

                        auto* inner = make_scope(ScopeKind::Block, &scope);
                        auto* inner_consts = make_const_env(const_env);
                        auto* v =
                            make_local_decl(sf.item_name, sf.name_range, nullptr, ast::StorageClass::Local, allocate_frame_slot(next_off, m_types.usize_t()));
                        define_local(*inner, v);
                        std::ignore = analyze_block(mod, fn, *inner, sf.body, loop_depth, next_off, nullptr, inner_consts);
                    }
                    else if (sf.pack_expr)
                    {
                        if (ast::node_cast<ast::IndexExpr>(sf.pack_expr))
                        {
                            auto src = extract_source_text(sf.pack_expr->range);
                            error(sf.pack_expr->range, "'{}' is not a pack", src.empty() ? "<expr>" : src);
                        }
                        else
                        {
                            std::ignore = analyze_expr(mod, fn, scope, *sf.pack_expr, loop_depth, next_off, nullptr, const_env);
                            if (auto* ident = ast::node_cast<ast::IdentExpr>(sf.pack_expr))
                            {
                                auto pack_info = resolve_pack_info(mod, scope, ident->name, ident->range);
                                if (!pack_info.has_value())
                                    error(sf.pack_expr->range, "'{}' is not a pack", ident->name);
                            }
                        }
                    }

                    if (!sf.is_range_for && ast::node_cast<ast::IdentExpr>(sf.pack_expr))
                    {
                        auto* inner = make_scope(ScopeKind::Block, &scope);
                        auto* inner_consts = make_const_env(const_env);

                        auto* v = make_local_decl(sf.item_name, sf.name_range, nullptr, ast::StorageClass::Local, allocate_frame_slot(next_off, nullptr));

                        define_local(*inner, v);

                        std::ignore = analyze_block(mod, fn, *inner, sf.body, loop_depth, next_off, nullptr, inner_consts);
                    }

                    out.foldable = false;
                    return out;
                }
                case ast::StmtKind::Ambiguous: {
                    auto& a = static_cast<ast::AmbiguousStmt&>(s);
                    if (a.as_decl)
                    {
                        a.resolution = ast::AmbiguousStmt::Resolution::AsDecl;
                        out = analyze_decl_stmt(mod, fn, scope, *a.as_decl, loop_depth, next_off, const_env);
                    }
                    else if (a.as_expr)
                    {
                        a.resolution = ast::AmbiguousStmt::Resolution::AsExpr;
                        std::ignore = analyze_expr(mod, fn, scope, *a.as_expr, loop_depth, next_off, nullptr, const_env);
                    }
                    out.foldable = false;
                    return out;
                }
            }
            return out;
        }

        detail::StmtResult analyze_decl_stmt(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::Decl& d, int loop_depth, std::uint32_t& next_off,
                                             ConstEnv const* const_env)
        {
            detail::StmtResult out{};
            if (auto* v = ast::node_cast<ast::VarDecl>(&d))
            {
                v->sema.storage = fn ? ast::StorageClass::Local : ast::StorageClass::ModuleGlobal;
                if (v->type && !v->type->sema.canonical)
                    std::ignore = resolve_and_substitute_type_expr(mod, scope, v->type, fn, &next_off, const_env);
                if (v->type && v->type->sema.canonical)
                {
                    auto* canon = get_canonical(v->type->sema);
                    v->sema.frame_offset = allocate_frame_slot(next_off, canon);

                    if (types::type_cast<types::RuntimeArrayType>(canon))
                        if (!fn)
                            error(v->range, "runtime-sized array is only allowed for local variables, not globals");

                    if (!v->is_extern)
                        check_type_valid_for_value(v->range, canon, "variable type");

                    if (mod.own_scope)
                        check_type_constraints_in_type(mod, *mod.own_scope, canon, v->range);
                }

                if (v->type && v->type->kind == ast::TypeKind::Qualified)
                {
                    auto const* qt = static_cast<ast::QualifiedType const*>(v->type);
                    if (ast::has_qual(qt->quals, ast::Qual::Restrict))
                    {
                        auto* resolved = get_canonical(v->type->sema);
                        if (resolved && !types::type_cast<types::PointerType>(resolved) && !types::type_cast<types::SliceType>(resolved))
                            error(v->range, "restrict qualifier is only allowed on pointer or slice types");
                    }
                }

                define_local(scope, v);
                if (v->init)
                {
                    track_decl_write(v);
                    auto expected = v->type && v->type->sema.canonical ? get_canonical(v->type->sema) : nullptr;
                    auto init = analyze_expr(mod, fn, scope, *v->init, loop_depth, next_off, expected, const_env);
                    if (expected && !has_error(init.type) && init.type != expected)
                    {
                        bool implicit_decay_ok = false;
                        if (auto const* exp_ptr = types::type_cast<types::PointerType>(expected))
                        {
                            if (auto const* got_ra = types::type_cast<types::RuntimeArrayType>(init.type))
                                implicit_decay_ok = (got_ra->element == exp_ptr->pointee);
                            else if (auto const* got_arr = types::type_cast<types::ArrayType>(init.type))
                                implicit_decay_ok = (got_arr->element == exp_ptr->pointee);
                        }

                        if (!implicit_decay_ok && !can_assign_return(expected, init.type) && !try_implicit_enum_conversion(expected, init.type, mod, scope))
                            error(v->range, "initializer type mismatch");
                    }

                    if (init.constant && const_env && fn)
                        define_constant(*const_cast<ConstEnv*>(const_env), v->name, init.constant);

                    out.foldable = init.constant != nullptr;
                }
                else
                    out.foldable = false;
            }
            else if (auto* fd = ast::node_cast<ast::FuncDecl>(&d))
            {
                Symbol sym{};
                sym.name = fd->name;
                sym.kind = SymbolKind::Function;
                sym.decl = fd;
                sym.definition_range = fd->range;
                Symbol const* existing = nullptr;
                auto r = scope.add_function_overload(sym, &existing);
                if (r == DefineResult::Conflict)
                    m_diag.emit(existing ? diag::Diagnostic{diag::Severity::Error, std::format("name `{}` already declared as a variable", fd->name)}
                                               .primary(fd->range)
                                               .secondary(existing->definition_range, "previous declaration here")
                                         : diag::Diagnostic{diag::Severity::Error, std::format("name `{}` already declared as a variable", fd->name)}.primary(
                                               fd->range));

                if (mod.ufcs_scope)
                    std::ignore = mod.ufcs_scope->add_function_overload(sym);

                fd->sema.storage = ast::StorageClass::Local;
                out.foldable = false;
            }
            return out;
        }

        detail::StmtResult analyze_block(ModuleInfo& mod, ast::FuncDecl* fn, Scope& scope, ast::Block& block, int loop_depth, std::uint32_t& next_off,
                                         types::TypePtr expected_type, ConstEnv const* const_env)
        {
            detail::StmtResult out{};
            auto const saved_defer_depth = m_active_defers.size();
            block.exit_defers.clear();
            bool reachable = true;
            for (std::size_t i = 0; i < block.stmts.size(); ++i)
            {
                auto* s = block.stmts[i];
                if (!reachable)
                    error(s->range, "unreachable statement");

                auto r = analyze_stmt(mod, fn, scope, *s, loop_depth, next_off, const_env);

                if (s->kind == ast::StmtKind::Ambiguous)
                {
                    auto& ambig = static_cast<ast::AmbiguousStmt&>(*s);
                    if (ambig.resolution == ast::AmbiguousStmt::Resolution::AsDecl && ambig.as_decl)
                        block.stmts[i] = m_ast_ctx.make<ast::DeclStmt>(ambig.range, ambig.as_decl);
                    else if (ambig.resolution == ast::AmbiguousStmt::Resolution::AsExpr && ambig.as_expr)
                        block.stmts[i] = m_ast_ctx.make<ast::ExprStmt>(ambig.range, ambig.as_expr);
                }

                if (!r.falls_through)
                    reachable = false;

                out.diverges = out.diverges || r.diverges;
                out.foldable = out.foldable && r.foldable;
            }
            if (block.tail)
            {
                auto tr = analyze_expr(mod, fn, scope, *block.tail, loop_depth, next_off, expected_type, const_env);
                out.diverges = out.diverges || tr.is_diverging;
                out.falls_through = reachable && !tr.is_diverging;
                out.foldable = out.foldable && tr.is_constant;
            }
            else
                out.falls_through = reachable;

            if (out.falls_through)
                block.exit_defers = snapshot_exit_defers();

            m_active_defers.resize(saved_defer_depth);
            return out;
        }

        bool pattern_matches_const(ModuleInfo& mod, ast::Pattern& p, detail::ExprResult const& operand, Scope& scope)
        {
            switch (p.kind)
            {
                case ast::PatternKind::Wildcard:
                    return true;
                case ast::PatternKind::Binding: {
                    std::ignore = scope;
                    return true;
                }
                case ast::PatternKind::Ref: {
                    auto& r = static_cast<ast::RefPattern&>(p);
                    return r.inner && pattern_matches_const(mod, *r.inner, operand, scope);
                }
                case ast::PatternKind::Literal: {
                    auto& lit = static_cast<ast::LiteralPattern&>(p);
                    if (!lit.value)
                        return false;
                    std::uint32_t tmp{};
                    auto res = analyze_expr(mod, nullptr, scope, *lit.value, 0, tmp);
                    return operand.constant && res.constant && constant_equal(*operand.constant, *res.constant);
                }
                case ast::PatternKind::Range: {
                    auto& r = static_cast<ast::RangePattern&>(p);
                    if (!operand.constant)
                        return false;

                    std::uint32_t tmp{};
                    auto start = r.start ? analyze_expr(mod, nullptr, scope, *r.start, 0, tmp) : detail::ExprResult{};
                    auto end = r.end ? analyze_expr(mod, nullptr, scope, *r.end, 0, tmp) : detail::ExprResult{};
                    if (!start.constant || !end.constant)
                        return false;

                    if (operand.constant->kind() != comptime::Value::Kind::Int && operand.constant->kind() != comptime::Value::Kind::Char)
                        return false;

                    auto const value = operand.constant->kind() == comptime::Value::Kind::Char ? static_cast<std::int64_t>(operand.constant->get_char())
                                                                                               : operand.constant->get_int();
                    auto const lo = start.constant->kind() == comptime::Value::Kind::Char ? static_cast<std::int64_t>(start.constant->get_char())
                                                                                          : start.constant->get_int();
                    auto const hi =
                        end.constant->kind() == comptime::Value::Kind::Char ? static_cast<std::int64_t>(end.constant->get_char()) : end.constant->get_int();

                    return r.inclusive ? (value >= lo && value <= hi) : (value >= lo && value < hi);
                }
                case ast::PatternKind::Or: {
                    auto& o = static_cast<ast::OrPattern&>(p);
                    for (auto* alt : o.alternatives)
                        if (alt && pattern_matches_const(mod, *alt, operand, scope))
                            return true;
                    return false;
                }
                case ast::PatternKind::EnumDestructure:
                case ast::PatternKind::StructDestructure:
                    return false;
            }
            return false;
        }

        bool constant_equal(comptime::Value const& a, comptime::Value const& b) const { return const_eval::values_equal(a, b); }

        detail::ExprResult analyze_if_branch_result(detail::ExprResult then_result, detail::ExprResult else_result)
        {
            detail::ExprResult out{};
            out.type = then_result.type ? then_result.type : else_result.type;
            out.is_diverging = then_result.is_diverging && else_result.is_diverging;
            return out;
        }

        [[nodiscard]] types::TypePtr try_implicit_enum_conversion(types::TypePtr target_type, types::TypePtr source_type, ModuleInfo& mod, Scope& scope)
        {
            if (!target_type || !source_type || target_type->kind != types::TypeKind::Enum)
                return nullptr;

            auto const* et = types::type_cast<types::EnumType>(target_type);
            if (!et)
                return nullptr;

            auto const* enum_decl = reinterpret_cast<ast::EnumDecl const*>(et->decl);
            if (!enum_decl)
                return nullptr;

            for (auto const& variant : enum_decl->variants)
            {
                if (!variant_has_attr(variant, "implicit_construction"))
                    continue;

                if (variant.payload.size() != 1)
                    continue;

                auto payload_ty = resolve_payload_type(*enum_decl, variant.payload[0], target_type, mod, scope);

                if (can_assign_return(payload_ty, source_type))
                    return target_type;
            }

            return nullptr;
        }

        [[nodiscard]] types::TypePtr resolve_payload_type(ast::EnumDecl const& enum_decl, ast::TypeExpr const* payload_type, types::TypePtr context_enum_type,
                                                          ModuleInfo& mod, Scope& scope)
        {
            if (!payload_type)
                return m_types.m_errort();

            if (payload_type->kind == ast::TypeKind::Named)
            {
                auto const* nt = static_cast<ast::NamedType const*>(payload_type);
                if (nt->path.is_simple() && nt->template_args.empty())
                {
                    auto name = nt->path.simple_name();
                    for (std::size_t i = 0; i < enum_decl.template_params.size(); ++i)
                        if (enum_decl.template_params[i].name == name)
                        {
                            auto* param_ty =
                                m_types.template_param_t(const_cast<ast::TemplateParam*>(&enum_decl.template_params[i]), name, static_cast<std::uint32_t>(i));
                            if (param_ty && context_enum_type)
                                return substitute_in_nominal_context(param_ty, context_enum_type);
                            return param_ty;
                        }
                }
            }

            auto ty = resolve_type_node(mod, scope, payload_type);
            if (ty && context_enum_type)
                return substitute_in_nominal_context(ty, context_enum_type);

            return ty;
        }

        [[nodiscard]] static ast::EnumVariant const* find_enum_variant(ast::EnumDecl const& decl, std::string_view name) noexcept
        {
            for (auto const& v : decl.variants)
                if (v.name == name)
                    return &v;
            return nullptr;
        }

        [[nodiscard]] std::optional<infer::TemplateBindings> create_template_bindings(ast::EnumDecl const& enum_decl, types::TypePtr context_type)
        {
            if (enum_decl.template_params.empty())
                return std::nullopt;

            if (context_type)
            {
                if (auto const* ctx_enum = types::type_cast<types::EnumType>(context_type))
                {
                    if (static_cast<void const*>(ctx_enum->decl) == static_cast<void const*>(&enum_decl) &&
                        ctx_enum->template_args.size() == enum_decl.template_params.size())
                    {
                        infer::TemplateBindings bindings{m_types};
                        for (std::size_t i = 0; i < enum_decl.template_params.size(); ++i)
                        {
                            auto const& tp = enum_decl.template_params[i];
                            auto* param_ty =
                                m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                            if (!param_ty)
                                return std::nullopt;
                            if (auto r = bindings.deduce(param_ty, ctx_enum->template_args[i]); !r)
                                return std::nullopt;
                        }
                        return bindings;
                    }
                }
            }

            infer::TemplateBindings bindings{m_types};
            return bindings;
        }

        [[nodiscard]] types::TypePtr deduce_enum_type_from_context(ast::EnumDecl const& enum_decl, types::TypePtr context)
        {
            if (!context)
                return nullptr;

            if (auto const* ctx_enum = types::type_cast<types::EnumType>(context))
            {
                if (static_cast<void const*>(ctx_enum->decl) == static_cast<void const*>(&enum_decl))
                    return context;
            }

            auto bindings = make_bindings(context);
            if (!bindings)
                return nullptr;

            std::vector<types::TypePtr> pattern_args;
            pattern_args.reserve(enum_decl.template_params.size());
            for (std::size_t i = 0; i < enum_decl.template_params.size(); ++i)
            {
                auto const& tp = enum_decl.template_params[i];
                auto* param_ty = m_types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
                pattern_args.push_back(param_ty);
            }

            if (pattern_args.empty())
                return nullptr;

            auto pattern_enum = m_types.nominal_t(types::TypeKind::Enum, &enum_decl, pattern_args);
            auto result = bindings->substitute(pattern_enum);
            if (result && !contains_template_param(result))
                return result;

            return nullptr;
        }

        [[nodiscard]] types::TypePtr resolve_and_substitute_type_expr(ModuleInfo& mod, Scope const& scope, ast::TypeExpr const* t, ast::FuncDecl* fn = nullptr,
                                                                      std::uint32_t* next_off_ptr = nullptr, ConstEnv const* const_env = nullptr)
        {
            auto saved = sema::get_canonical(t->sema);
            auto resolved = resolve_type_node(mod, scope, t, fn, next_off_ptr, const_env);
            if (m_concept_bindings && resolved)
            {
                auto sub = m_concept_bindings->substitute(resolved);
                if (sub && sub != resolved)
                    resolved = sub;
            }
            sema::set_canonical(const_cast<ast::TypeSema&>(t->sema), resolved);

            if (m_concept_canon_guards && (resolved != saved))
            {
                auto& g = m_concept_canon_guards->emplace_back();
                g.sema = const_cast<ast::TypeSema*>(std::addressof(t->sema));
                g.saved = saved;
            }
            return resolved;
        }

        types::TypePtr decl_type(ast::Decl const& d)
        {
            switch (d.kind)
            {
                case ast::DeclKind::Var:
                    if (auto ty = inferred_local_type(&d))
                        return ty;
                    if (static_cast<ast::VarDecl const&>(d).type)
                        return get_canonical(static_cast<ast::VarDecl const&>(d).type->sema);
                    return m_types.m_errort();
                case ast::DeclKind::Func: {
                    auto const& f = static_cast<ast::FuncDecl const&>(d);
                    std::vector<types::TypePtr> params;
                    for (auto const& p : f.params)
                        if (p.type)
                            params.push_back(get_canonical(p.type->sema));
                        else
                            params.push_back(m_types.m_errort());

                    auto ret = m_types.m_voidt();
                    if (f.return_type)
                        ret = get_canonical(f.return_type->sema);

                    return m_types.funcptr_t(ret, params);
                }
                case ast::DeclKind::Struct:
                    return m_types.nominal_t(types::TypeKind::Struct, &static_cast<ast::StructDecl const&>(d));
                case ast::DeclKind::Union:
                    return m_types.nominal_t(types::TypeKind::Union, &static_cast<ast::UnionDecl const&>(d));
                case ast::DeclKind::Enum:
                    return m_types.nominal_t(types::TypeKind::Enum, &static_cast<ast::EnumDecl const&>(d));
                default:
                    return m_types.m_errort();
            }
        }

        struct ConstSizeResult
        {
            enum class Tag
            {
                Folded,
                NonInteger,
                NotConstant
            };

            Tag tag;
            std::int64_t value{};

            static ConstSizeResult folded(std::int64_t v)
            {
                ConstSizeResult r;
                r.tag = Tag::Folded;
                r.value = v;
                return r;
            }

            static ConstSizeResult non_integer() { return {Tag::NonInteger, {}}; }
            static ConstSizeResult not_constant() { return {Tag::NotConstant, {}}; }
        };

        [[nodiscard]] ConstSizeResult try_eval_const_size_expr(ModuleInfo& mod, Scope const& scope, ast::Expr const* e)
        {
            if (!e)
                return ConstSizeResult::not_constant();

            switch (e->kind)
            {
                case ast::ExprKind::IntLiteral: {
                    auto val = static_cast<ast::IntLiteralExpr const*>(e)->value;
                    return ConstSizeResult::folded(val);
                }
                case ast::ExprKind::FloatLiteral:
                case ast::ExprKind::CharLiteral:
                case ast::ExprKind::U16CharLiteral:
                case ast::ExprKind::BoolLiteral:
                case ast::ExprKind::StringLiteral:
                case ast::ExprKind::U16StringLiteral:
                case ast::ExprKind::NullLiteral:
                    return ConstSizeResult::non_integer();
                case ast::ExprKind::Ident: {
                    auto const* id = static_cast<ast::IdentExpr const*>(e);
                    auto const* sym = lookup_name(mod, scope, id->name);
                    if (!sym || !sym->decl)
                        return ConstSizeResult::not_constant();

                    auto const* vd = ast::node_cast<ast::VarDecl>(sym->decl);
                    if (!vd || !vd->init)
                        return ConstSizeResult::not_constant();

                    if (vd->init->sema.const_value)
                    {
                        auto maybe_int = vd->init->sema.const_value->const_to_int();
                        if (maybe_int.has_value())
                            return ConstSizeResult::folded(*maybe_int);
                    }

                    return ConstSizeResult::not_constant();
                }
                case ast::ExprKind::Binary: {
                    auto const* be = static_cast<ast::BinaryExpr const*>(e);
                    auto lhs = try_eval_const_size_expr(mod, scope, be->lhs);
                    auto rhs = try_eval_const_size_expr(mod, scope, be->rhs);
                    if (lhs.tag == ConstSizeResult::Tag::NonInteger || rhs.tag == ConstSizeResult::Tag::NonInteger)
                        return ConstSizeResult::non_integer();
                    if (lhs.tag != ConstSizeResult::Tag::Folded || rhs.tag != ConstSizeResult::Tag::Folded)
                        return ConstSizeResult::not_constant();

                    auto* fold_ty = m_types.usize_t();
                    switch (be->op)
                    {
                        case lex::TokenKind::Plus: {
                            auto result = comptime::Value::fold_int_binary(comptime::BinaryOp::Add, lhs.value, rhs.value, fold_ty);
                            if (result)
                                return ConstSizeResult::folded(result->get_int());
                            return ConstSizeResult::not_constant();
                        }
                        case lex::TokenKind::Minus: {
                            auto result = comptime::Value::fold_int_binary(comptime::BinaryOp::Sub, lhs.value, rhs.value, fold_ty);
                            if (result)
                                return ConstSizeResult::folded(result->get_int());
                            return ConstSizeResult::not_constant();
                        }
                        case lex::TokenKind::Star: {
                            auto result = comptime::Value::fold_int_binary(comptime::BinaryOp::Mul, lhs.value, rhs.value, fold_ty);
                            if (result)
                                return ConstSizeResult::folded(result->get_int());
                            return ConstSizeResult::not_constant();
                        }
                        case lex::TokenKind::Slash: {
                            if (rhs.value == 0)
                                return ConstSizeResult::not_constant();
                            auto result = comptime::Value::fold_int_binary(comptime::BinaryOp::Div, lhs.value, rhs.value, fold_ty);
                            if (result)
                                return ConstSizeResult::folded(result->get_int());
                            return ConstSizeResult::not_constant();
                        }
                        case lex::TokenKind::LtLt: {
                            if (rhs.value < 0)
                                return ConstSizeResult::not_constant();
                            auto result = comptime::Value::fold_int_binary(comptime::BinaryOp::Shl, lhs.value, rhs.value, fold_ty);
                            if (result)
                                return ConstSizeResult::folded(result->get_int());
                            return ConstSizeResult::not_constant();
                        }
                        case lex::TokenKind::GtGt: {
                            if (rhs.value < 0)
                                return ConstSizeResult::not_constant();
                            auto result = comptime::Value::fold_int_binary(comptime::BinaryOp::Shr, lhs.value, rhs.value, fold_ty);
                            if (result)
                                return ConstSizeResult::folded(result->get_int());
                            return ConstSizeResult::not_constant();
                        }
                        default:
                            return ConstSizeResult::not_constant();
                    }
                }
                case ast::ExprKind::Unary: {
                    auto const* ue = static_cast<ast::UnaryExpr const*>(e);
                    auto inner = try_eval_const_size_expr(mod, scope, ue->operand);
                    if (inner.tag == ConstSizeResult::Tag::NonInteger)
                        return ConstSizeResult::non_integer();
                    if (inner.tag != ConstSizeResult::Tag::Folded)
                        return ConstSizeResult::not_constant();

                    auto* fold_ty = m_types.usize_t();
                    switch (ue->op)
                    {
                        case lex::TokenKind::Plus:
                            return ConstSizeResult::folded(inner.value);
                        case lex::TokenKind::Minus: {
                            auto result = comptime::Value::fold_int_binary(comptime::BinaryOp::Sub, static_cast<std::int64_t>(0), inner.value, fold_ty);
                            if (result)
                                return ConstSizeResult::folded(result->get_int());
                            return ConstSizeResult::not_constant();
                        }
                        default:
                            return ConstSizeResult::not_constant();
                    }
                }
                default:
                    return ConstSizeResult::not_constant();
            }
        }

        ResolvedType resolve_type_node_resolved(ModuleInfo& mod, Scope const& scope, ast::TypeExpr const* t, ast::FuncDecl* fn = nullptr,
                                                std::uint32_t* next_off_ptr = nullptr, ConstEnv const* const_env = nullptr)
        {
            if (!t)
                return {.type = m_types.m_errort()};

            switch (t->kind)
            {
                case ast::TypeKind::Primitive: {
                    auto which = static_cast<ast::PrimitiveType const*>(t)->which;
                    switch (which)
                    {
                        case lex::TokenKind::KwVoid:
                            return {.type = m_types.m_voidt()};
                        case lex::TokenKind::KwBool:
                            return {.type = m_types.m_boolt()};
                        case lex::TokenKind::KwChar:
                            return {.type = m_types.m_chart()};
                        case lex::TokenKind::Kwi8:
                            return {.type = m_types.int_t(8, true)};
                        case lex::TokenKind::Kwi16:
                            return {.type = m_types.int_t(16, true)};
                        case lex::TokenKind::Kwi32:
                            return {.type = m_types.int_t(32, true)};
                        case lex::TokenKind::Kwu8:
                            return {.type = m_types.int_t(8, false)};
                        case lex::TokenKind::Kwu16:
                            return {.type = m_types.int_t(16, false)};
                        case lex::TokenKind::Kwu32:
                            return {.type = m_types.int_t(32, false)};
                        case lex::TokenKind::Kwi64:
                            return {.type = m_types.int_t(64, true)};
                        case lex::TokenKind::Kwu64:
                            return {.type = m_types.int_t(64, false)};
                        case lex::TokenKind::Kwf32:
                            return {.type = m_types.float_t(32)};
                        case lex::TokenKind::Kwf64:
                            return {.type = m_types.float_t(64)};
                        case lex::TokenKind::KwUsize:
                            return {.type = m_types.usize_t()};
                        case lex::TokenKind::KwIsize:
                            return {.type = m_types.isize_t()};
                        default:
                            return {.type = m_types.m_errort()};
                    }
                }
                case ast::TypeKind::Named: {
                    auto const* nt = static_cast<ast::NamedType const*>(t);
                    auto instantiate = [&](ast::Decl const* decl) -> types::TypePtr {
                        if (auto const* u = ast::node_cast<ast::UsingDecl>(decl))
                        {
                            if (u->using_kind == ast::UsingKind::Alias && u->target_type && u->target_type->sema.canonical)
                            {
                                auto underlying = get_canonical(u->target_type->sema);
                                if (u->sema.is_nominal && underlying && underlying->kind != types::TypeKind::Error)
                                    return m_types.nominal_alias_t(underlying, u);
                                return underlying;
                            }

                            return m_types.m_errort();
                        }
                        if (nt->template_args.empty())
                            return decl_type(*decl);

                        std::vector<types::TypePtr> resolved_args;
                        resolved_args.reserve(nt->template_args.size());
                        auto const* void_reject_decl = decl;
                        for (auto const& arg : nt->template_args)
                        {
                            if (!arg.type)
                                return m_types.m_errort();

                            auto arg_ty =
                                arg.type->sema.canonical ? get_canonical(arg.type->sema) : resolve_type_node(mod, scope, arg.type, fn, next_off_ptr, const_env);
                            if (arg_ty && is_concrete_void_type(arg_ty) && void_reject_decl)
                            {
                                if (void_reject_decl->kind != ast::DeclKind::Enum)
                                {
                                    error(arg.type->range, "void is not allowed as a type argument to this struct/union");
                                    return m_types.m_errort();
                                }
                            }
                            resolved_args.push_back(arg_ty);
                        }

                        if (auto const* sd = ast::node_cast<ast::StructDecl>(decl))
                        {
                            auto ty = m_types.nominal_t(types::TypeKind::Struct, sd, resolved_args);
                            std::ignore = check_type_constraint(mod, scope, sd, resolved_args, nt->range);
                            return ty;
                        }
                        if (auto const* ud = ast::node_cast<ast::UnionDecl>(decl))
                            return m_types.nominal_t(types::TypeKind::Union, ud, resolved_args);
                        if (auto const* ed = ast::node_cast<ast::EnumDecl>(decl))
                        {
                            auto ty = m_types.nominal_t(types::TypeKind::Enum, ed, resolved_args);
                            std::ignore = check_type_constraint(mod, scope, ed, resolved_args, nt->range);
                            return ty;
                        }

                        return m_types.m_errort();
                    };

                    if (nt->path.is_simple())
                    {
                        if (auto const* sym = scope.lookup_type(nt->path.simple_name()))
                            return {.type = instantiate(sym->decl)};

                        if (mod.own_scope)
                            if (auto const* sym = mod.own_scope->lookup_type(nt->path.simple_name()))
                                return {.type = instantiate(sym->decl)};

                        if (m_specialization_defining_module && m_specialization_defining_module->own_scope)
                            if (auto const* sym = m_specialization_defining_module->own_scope->lookup_type(nt->path.simple_name()))
                                return {.type = instantiate(sym->decl)};
                    }

                    if (mod.own_scope)
                        if (auto const* sym = resolve_type_path(*mod.own_scope, nt->path))
                            return {.type = instantiate(sym->decl)};

                    if (m_specialization_defining_module && m_specialization_defining_module->own_scope)
                        if (auto const* sym = resolve_type_path(*m_specialization_defining_module->own_scope, nt->path))
                            return {.type = instantiate(sym->decl)};

                    return {.type = m_types.m_errort()};
                }
                case ast::TypeKind::Pointer: {
                    auto inner = resolve_type_node_resolved(mod, scope, static_cast<ast::PointerType const*>(t)->pointee, fn, next_off_ptr, const_env);
                    return {.type = m_types.pointer_to(materialize_type(inner), inner.quals)};
                }
                case ast::TypeKind::Array: {
                    auto inner = resolve_type_node_resolved(mod, scope, static_cast<ast::ArrayType const*>(t)->element, fn, next_off_ptr, const_env);
                    if (inner.type && is_concrete_void_type(inner.type))
                    {
                        error(static_cast<ast::ArrayType const*>(t)->element->range, "array element type cannot be void");
                        return {.type = m_types.m_errort()};
                    }
                    auto const* arr = static_cast<ast::ArrayType const*>(t);

                    auto result = try_eval_const_size_expr(mod, scope, arr->size);
                    switch (result.tag)
                    {
                        case ConstSizeResult::Tag::Folded: {
                            auto val = result.value;
                            if (val < 0)
                            {
                                error(arr->size ? arr->size->range : arr->range, "array size must be non-negative");
                                return {.type = m_types.m_errort()};
                            }
                            return {.type = m_types.array_t(materialize_type(inner), static_cast<std::uint64_t>(val))};
                        }
                        case ConstSizeResult::Tag::NonInteger: {
                            error(arr->size ? arr->size->range : arr->range, "array size must be an integer constant");
                            return {.type = m_types.m_errort()};
                        }
                        case ConstSizeResult::Tag::NotConstant: {
                            if (arr->size && fn)
                            {
                                auto* size_expr = const_cast<ast::Expr*>(arr->size);
                                auto& scope_ref = const_cast<Scope&>(scope);
                                std::uint32_t& off_ref = next_off_ptr ? *next_off_ptr : dummy_offset();
                                auto* expected_ty = m_types.usize_t();
                                std::ignore = analyze_expr(mod, fn, scope_ref, *size_expr, 0, off_ref, expected_ty, const_env);
                            }
                            else if (arr->size && arr->size->kind == ast::ExprKind::Ident)
                            {
                                auto const* id = static_cast<ast::IdentExpr const*>(arr->size);
                                if (auto const* sym = lookup_name(mod, scope, id->name))
                                    const_cast<ast::Expr*>(arr->size)->sema.resolved_decl = sym->decl;
                            }
                            return {.type = m_types.runtime_array_t(materialize_type(inner))};
                        }
                    }
                    return {.type = m_types.m_errort()};
                }
                case ast::TypeKind::Slice: {
                    auto inner = resolve_type_node_resolved(mod, scope, static_cast<ast::SliceType const*>(t)->element, fn, next_off_ptr, const_env);
                    if (inner.type && is_concrete_void_type(inner.type))
                    {
                        error(t->range, "slice element type cannot be void");
                        return {.type = m_types.m_errort()};
                    }
                    return {.type = m_types.slice_t(materialize_type(inner), inner.quals)};
                }
                case ast::TypeKind::Fam: {
                    auto inner = resolve_type_node_resolved(mod, scope, static_cast<ast::FamType const*>(t)->element, fn, next_off_ptr, const_env);
                    return {.type = m_types.fam_t(materialize_type(inner))};
                }
                case ast::TypeKind::FuncPtr: {
                    auto const* fp = static_cast<ast::FuncPtrType const*>(t);
                    std::vector<types::TypePtr> params;
                    for (auto* p : fp->params)
                        params.push_back(resolve_type_node(mod, scope, p, fn, next_off_ptr, const_env));

                    return {.type = m_types.funcptr_t(resolve_type_node(mod, scope, fp->return_type, fn, next_off_ptr, const_env), params)};
                }
                case ast::TypeKind::Qualified: {
                    auto inner = resolve_type_node_resolved(mod, scope, static_cast<ast::QualifiedType const*>(t)->inner, fn, next_off_ptr, const_env);
                    inner.quals = qual_or(inner.quals, ast_to_type_qual(static_cast<ast::QualifiedType const*>(t)->quals));
                    return inner;
                }
                case ast::TypeKind::PackIndex: {
                    auto const* pi = static_cast<ast::PackIndexType const*>(t);
                    auto base = resolve_type_node(mod, scope, pi->base, fn, next_off_ptr, const_env);
                    if (!base || base->kind == types::TypeKind::Error)
                        return ResolvedType{.type = m_types.m_errort()};

                    if (!is_pack_indexable_type(base))
                    {
                        error(pi->range, "cannot index non-pack type with .N syntax");
                        return ResolvedType{.type = m_types.m_errort()};
                    }

                    std::optional<std::int64_t> idx_val;
                    if (auto* lit = ast::node_cast<ast::IntLiteralExpr>(pi->index))
                        idx_val = lit->value;
                    else if (pi->index && pi->index->sema.const_value)
                        idx_val = pi->index->sema.const_value->const_to_int();

                    if (!idx_val || *idx_val < 0)
                        return ResolvedType{.type = base};

                    if (types::type_cast<types::TemplateParamType>(base))
                        return ResolvedType{.type = m_types.type_pack_t(base, static_cast<std::uint32_t>(*idx_val))};

                    if (auto const* type_pack = types::type_cast<types::TypePackType>(base))
                        return ResolvedType{.type = m_types.type_pack_t(type_pack->element, type_pack->pack_index + static_cast<std::uint32_t>(*idx_val))};

                    return ResolvedType{.type = base};
                }
            }
            return {.type = m_types.m_errort()};
        }

        [[nodiscard]] types::TypePtr resolve_type_node(ModuleInfo& mod, Scope const& scope, ast::TypeExpr const* t, ast::FuncDecl* fn = nullptr,
                                                       std::uint32_t* next_off_ptr = nullptr, ConstEnv const* const_env = nullptr)
        {
            return materialize_type(resolve_type_node_resolved(mod, scope, t, fn, next_off_ptr, const_env));
        }
    };

    void analyze_bodies(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, ast::AstContext& ast_ctx,
                        types::TypeContext& type_ctx, std::pmr::polymorphic_allocator<> alloc, SpecializationRegistry& reg)
    {
        BodyAnalyzer{modules, diag, ast_ctx, type_ctx, alloc, reg}.run();
    }

    void analyze_instantiated_body(ModuleInfo& mod, ast::FuncDecl& fn, diag::DiagnosticEngine& diag, ast::AstContext& ast_ctx, types::TypeContext& type_ctx,
                                   std::pmr::polymorphic_allocator<> alloc)
    {
        std::vector<std::unique_ptr<ModuleInfo>> dummy;
        std::span<std::unique_ptr<ModuleInfo> const> empty_span{dummy};
        static SpecializationRegistry reg;
        reg = SpecializationRegistry{};
        BodyAnalyzer{empty_span, diag, ast_ctx, type_ctx, alloc, reg}.analyze_single_function(mod, fn);
    }

    [[nodiscard]] std::uint64_t align_up_enum(std::uint64_t n, std::uint64_t align)
    {
        if (align <= 1)
            return n;

        auto rem = n % align;
        return rem ? (n + (align - rem)) : n;
    }

    [[nodiscard]] std::optional<infer::TemplateBindings> make_bindings(types::TypeContext& types, types::TypePtr ty)
    {
        if (!ty)
            return std::nullopt;

        std::vector<types::TypePtr> args;
        std::vector<ast::TemplateParam const*> template_params;

        if (ty->kind == types::TypeKind::Nominal)
            ty = static_cast<types::NominalType const*>(ty)->underlying;

        switch (ty->kind)
        {
            case types::TypeKind::Struct: {
                auto const* st = static_cast<types::StructType const*>(ty);
                auto const* decl = reinterpret_cast<ast::StructDecl const*>(st->decl);
                template_params.reserve(decl->template_params.size());
                for (auto const& tp : decl->template_params)
                    template_params.push_back(&tp);

                args.assign(st->template_args.begin(), st->template_args.end());
                break;
            }
            case types::TypeKind::Enum: {
                auto const* et = static_cast<types::EnumType const*>(ty);
                auto const* decl = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                template_params.reserve(decl->template_params.size());
                for (auto const& tp : decl->template_params)
                    template_params.push_back(&tp);

                args.assign(et->template_args.begin(), et->template_args.end());
                break;
            }
            default:
                return std::nullopt;
        }

        if (template_params.empty())
            return std::nullopt;
        if (template_params.size() != args.size())
            return std::nullopt;

        std::optional<infer::TemplateBindings> bindings;
        bindings.emplace(types);
        for (std::size_t i = 0; i < template_params.size(); ++i)
        {
            auto const& tp = *template_params[i];
            auto param_ty = types.template_param_t(const_cast<ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));
            if (auto r = bindings->deduce(param_ty, args[i]); !r)
                return std::nullopt;
        }

        return bindings;
    }

    [[nodiscard]] types::TypePtr substitute_in_nominal_context(types::TypeContext& types, types::TypePtr ty, types::TypePtr context)
    {
        if (!context)
            return ty;

        auto bindings = make_bindings(types, context);
        if (!bindings)
            return ty;

        return bindings->substitute(ty);
    }

    void complete_templated_union(types::TypeContext& types, std::pmr::polymorphic_allocator<> alloc, types::UnionType const* ut);

    void complete_templated_struct(types::TypeContext& types, std::pmr::polymorphic_allocator<> alloc, types::StructType const* st)
    {
        if (!st || st->is_complete || st->template_args.empty())
            return;

        static thread_local std::vector<types::StructType const*> s_struct_stack;
        if (std::find(s_struct_stack.begin(), s_struct_stack.end(), st) != s_struct_stack.end())
            return;
        s_struct_stack.push_back(st);
        struct Guard
        {
            ~Guard() { s_struct_stack.pop_back(); }
        } guard;

        auto* sd = reinterpret_cast<ast::StructDecl const*>(st->decl);
        if (!sd || sd->template_params.empty())
            return;

        auto bindings = make_bindings(types, st);
        if (!bindings)
            return;

        std::uint64_t size = 0;
        std::uint32_t align = 1;
        bool ok = true;

        for (auto const& f : sd->fields)
        {
            if (!f.type || !f.type->sema.canonical)
            {
                ok = false;
                break;
            }

            auto* ft = get_canonical(f.type->sema);
            auto concrete_ft = bindings->substitute(ft);
            if (!concrete_ft)
            {
                ok = false;
                break;
            }

            if (!concrete_ft->is_complete)
            {
                if (auto* ft_et = types::type_cast<types::EnumType>(concrete_ft))
                {
                    if (!ft_et->tagged_layout && !ft_et->template_args.empty())
                        ensure_tagged_enum_complete(types, alloc, concrete_ft);
                }
                else if (auto* ft_st = types::type_cast<types::StructType>(concrete_ft))
                    complete_templated_struct(types, alloc, ft_st);
                else if (auto* ft_ut = types::type_cast<types::UnionType>(concrete_ft))
                    complete_templated_union(types, alloc, ft_ut);
            }

            if (!concrete_ft->is_complete)
            {
                ok = false;
                break;
            }

            auto field_align = concrete_ft->byte_align;
            if (field_align > align)
                align = field_align;
            size = align_up_enum(size, field_align);
            size += concrete_ft->byte_size;
        }

        if (ok)
        {
            size = align_up_enum(size, align);
            auto* mut_st = const_cast<types::StructType*>(st);
            mut_st->byte_size = size;
            mut_st->byte_align = align;
            mut_st->is_zero_sized = (size == 0);
            mut_st->is_complete = true;
        }
    }

    void complete_templated_union(types::TypeContext& types, std::pmr::polymorphic_allocator<> alloc, types::UnionType const* ut)
    {
        if (!ut || ut->is_complete || ut->template_args.empty())
            return;

        static thread_local std::vector<types::UnionType const*> s_union_stack;
        if (std::find(s_union_stack.begin(), s_union_stack.end(), ut) != s_union_stack.end())
            return;
        s_union_stack.push_back(ut);
        struct Guard
        {
            ~Guard() { s_union_stack.pop_back(); }
        } guard;

        auto* ud = reinterpret_cast<ast::UnionDecl const*>(ut->decl);
        if (!ud)
            return;

        auto bindings = make_bindings(types, ut);
        if (!bindings)
            return;

        std::uint64_t max_field_size = 0;
        std::uint32_t max_field_align = 1;
        bool ok = true;

        for (auto const& f : ud->fields)
        {
            if (!f.type || !f.type->sema.canonical)
            {
                ok = false;
                break;
            }

            auto* ft = get_canonical(f.type->sema);
            auto concrete_ft = bindings->substitute(ft);
            if (!concrete_ft)
            {
                ok = false;
                break;
            }

            if (!concrete_ft->is_complete)
            {
                if (auto* ft_et = types::type_cast<types::EnumType>(concrete_ft))
                {
                    if (!ft_et->tagged_layout && !ft_et->template_args.empty())
                        ensure_tagged_enum_complete(types, alloc, concrete_ft);
                }
                else if (auto* ft_st = types::type_cast<types::StructType>(concrete_ft))
                    complete_templated_struct(types, alloc, ft_st);
                else if (auto* ft_ut = types::type_cast<types::UnionType>(concrete_ft))
                    complete_templated_union(types, alloc, ft_ut);
            }

            if (!concrete_ft->is_complete)
            {
                ok = false;
                break;
            }

            if (concrete_ft->byte_align > max_field_align)
                max_field_align = concrete_ft->byte_align;
            if (concrete_ft->byte_size > max_field_size)
                max_field_size = concrete_ft->byte_size;
        }

        if (ok)
        {
            max_field_size = align_up_enum(max_field_size, max_field_align);
            auto* mut_ut = const_cast<types::UnionType*>(ut);
            mut_ut->byte_size = max_field_size;
            mut_ut->byte_align = max_field_align;
            mut_ut->is_zero_sized = (max_field_size == 0);
            mut_ut->is_complete = true;
        }
    }

    void ensure_tagged_enum_complete(types::TypeContext& types, std::pmr::polymorphic_allocator<> alloc, types::TypePtr ty)
    {
        auto* et = const_cast<types::EnumType*>(types::type_cast<types::EnumType>(ty));
        if (!et || et->tagged_layout || et->template_args.empty())
            return;

        static thread_local std::vector<void const*> s_completing_stack;
        if (std::find(s_completing_stack.begin(), s_completing_stack.end(), et) != s_completing_stack.end())
            return;
        s_completing_stack.push_back(et);
        struct CompletingGuard
        {
            std::vector<void const*>& stack;
            ~CompletingGuard() { stack.pop_back(); }
        } guard{s_completing_stack};

        auto* ed = const_cast<ast::EnumDecl*>(reinterpret_cast<ast::EnumDecl const*>(et->decl));
        if (!ed || !ed->is_tagged || ed->template_params.empty())
            return;

        et->is_tagged = true;

        auto variant_count = ed->variants.size();

        auto disc_magnitude = [](std::int64_t stored_val, bool is_negative) -> std::uint64_t {
            if (!is_negative)
                return static_cast<std::uint64_t>(stored_val);
            if (stored_val == std::numeric_limits<std::int64_t>::min())
                return static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
            return static_cast<std::uint64_t>(-stored_val);
        };

        std::uint64_t max_positive = 0;
        std::uint64_t max_negative_magnitude = 0;
        bool has_negative_disc = false;
        for (auto& v : ed->variants)
        {
            auto mag = disc_magnitude(v.discriminant, v.discriminant_is_negative);
            if (v.discriminant_is_negative)
            {
                has_negative_disc = true;
                if (mag > max_negative_magnitude)
                    max_negative_magnitude = mag;
            }
            else if (mag > max_positive)
                max_positive = mag;
        }

        bool needs_signed = has_negative_disc;
        types::IntType const* disc_type = nullptr;
        std::uint64_t disc_size = 0;

        auto signed_fits = [&](std::uint8_t bits) -> bool {
            if (bits >= 64)
            {
                if (max_positive > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    return false;
            }
            else
            {
                std::int64_t pos_max = (std::int64_t(1) << (bits - 1)) - 1;
                if (static_cast<std::int64_t>(max_positive) > pos_max)
                    return false;
            }

            if (bits >= 64)
                return max_negative_magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
            else
            {
                std::uint64_t neg_max_mag = std::uint64_t(1) << (bits - 1);
                return max_negative_magnitude <= neg_max_mag;
            }
        };

        if (needs_signed)
        {
            if (signed_fits(8))
            {
                disc_type = static_cast<types::IntType const*>(types.int_t(8, true));
                disc_size = 1;
            }
            else if (signed_fits(16))
            {
                disc_type = static_cast<types::IntType const*>(types.int_t(16, true));
                disc_size = 2;
            }
            else if (signed_fits(32))
            {
                disc_type = static_cast<types::IntType const*>(types.int_t(32, true));
                disc_size = 4;
            }
            else
            {
                disc_type = static_cast<types::IntType const*>(types.int_t(64, true));
                disc_size = 8;
            }
        }
        else
        {
            std::uint64_t needed = max_positive > static_cast<std::uint64_t>(variant_count) - 1 ? max_positive : static_cast<std::uint64_t>(variant_count) - 1;

            if (needed <= 255)
            {
                disc_type = static_cast<types::IntType const*>(types.int_t(8, false));
                disc_size = 1;
            }
            else if (needed <= 65535)
            {
                disc_type = static_cast<types::IntType const*>(types.int_t(16, false));
                disc_size = 2;
            }
            else if (needed <= 4294967295)
            {
                disc_type = static_cast<types::IntType const*>(types.int_t(32, false));
                disc_size = 4;
            }
            else
            {
                disc_type = static_cast<types::IntType const*>(types.int_t(64, false));
                disc_size = 8;
            }
        }

        std::uint32_t disc_align = static_cast<std::uint32_t>(disc_size);
        std::uint32_t max_payload_align = 1;
        std::uint64_t max_payload_size = 0;

        auto* variant_layouts = static_cast<types::TaggedEnumVariantLayout*>(
            alloc.resource()->allocate(sizeof(types::TaggedEnumVariantLayout) * variant_count, alignof(types::TaggedEnumVariantLayout)));

        for (std::size_t i = 0; i < variant_count; ++i)
        {
            auto& v = ed->variants[i];
            types::TypePtr payload_type = nullptr;
            if (v.payload.size() == 1)
            {
                auto const& ts = v.payload[0]->sema;
                payload_type = get_canonical(ts);

                auto concrete = substitute_in_nominal_context(types, payload_type, ty);
                if (concrete && concrete != payload_type)
                    payload_type = concrete;

                if (payload_type && is_concrete_void_type(payload_type))
                    payload_type = nullptr;

                if (payload_type && !payload_type->is_complete)
                {
                    if (auto* payload_et = types::type_cast<types::EnumType>(payload_type))
                    {
                        if (!payload_et->tagged_layout && !payload_et->template_args.empty())
                        {
                            auto* payload_ed = reinterpret_cast<ast::EnumDecl const*>(payload_et->decl);
                            if (payload_ed && payload_ed->is_tagged && !payload_ed->template_params.empty())
                                ensure_tagged_enum_complete(types, alloc, payload_type);
                        }
                    }
                    else if (auto* payload_st = types::type_cast<types::StructType>(payload_type))
                        complete_templated_struct(types, alloc, payload_st);
                    else if (auto* payload_ut = types::type_cast<types::UnionType>(payload_type))
                        complete_templated_union(types, alloc, payload_ut);
                }

                if (payload_type && payload_type->is_complete)
                {
                    if (payload_type->byte_align > max_payload_align)
                        max_payload_align = payload_type->byte_align;
                    if (payload_type->byte_size > max_payload_size)
                        max_payload_size = payload_type->byte_size;
                }
            }

            ::new (&variant_layouts[i]) types::TaggedEnumVariantLayout{v.name, payload_type, v.discriminant};
        }

        std::uint64_t disc_offset = 0;
        std::uint64_t payload_offset = align_up_enum(disc_size, max_payload_align);
        std::uint64_t total_size_raw = payload_offset + max_payload_size;

        std::uint32_t total_align = disc_align > max_payload_align ? disc_align : max_payload_align;
        if (total_align < 1)
            total_align = 1;

        std::uint64_t total_size = align_up_enum(total_size_raw, total_align);

        void* p = alloc.resource()->allocate(sizeof(types::TaggedEnumLayout), alignof(types::TaggedEnumLayout));
        auto* layout = ::new (p) types::TaggedEnumLayout{
            .discriminant_offset = disc_offset,
            .discriminant_size = disc_size,
            .discriminant_type = disc_type,
            .payload_offset = payload_offset,
            .payload_size = max_payload_size,
            .total_size = total_size,
            .total_align = total_align,
            .variants = variant_layouts,
            .variant_count = variant_count,
        };

        et->tagged_layout = layout;
        et->byte_size = total_size;
        et->byte_align = total_align;
        et->is_zero_sized = (total_size == 0);
        et->is_complete = true;
    }

    void complete_all_templated_tagged_enums(types::TypeContext& types, std::pmr::polymorphic_allocator<> alloc)
    {
        for (std::size_t i = 0; i < types.enums().size(); ++i)
        {
            auto* et = const_cast<types::EnumType*>(types.enums()[i]);
            if (!et || et->tagged_layout || et->template_args.empty())
                continue;

            auto* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
            if (!ed || !ed->is_tagged || ed->template_params.empty())
                continue;

            ensure_tagged_enum_complete(types, alloc, et);
        }
    }

} // namespace dcc::sema
