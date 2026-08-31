export module dcc.sema.collect;

import std;
import dcc.ast;
import dcc.diag;
import dcc.sm;
import dcc.comptime;
import dcc.const_eval;
import dcc.types;
import dcc.lex.tokens;
import dcc.sema.scope;

export namespace dcc::sema
{
    class ModuleConstEvaluator
    {
    public:
        ModuleConstEvaluator(types::TypeContext& types, diag::DiagnosticEngine& diag) : m_types{types}, m_diag{diag} {}

        void register_var(ast::VarDecl const* v)
        {
            if (v && !v->name.empty())
                m_vars.insert_or_assign(v->name, v);
        }

        void register_value_alias(ast::UsingDecl const* u)
        {
            if (u && u->using_kind == ast::UsingKind::ValueAlias && !u->alias_path.segments.empty())
                m_value_aliases.insert_or_assign(u->alias_path.segments.back().name, u);
        }

        void register_enum(ast::EnumDecl const* e)
        {
            if (e && !e->name.empty())
                m_enums.insert_or_assign(e->name, e);
        }

        std::optional<comptime::Value> eval_condition(ast::Expr const* e) { return eval(e); }

    private:
        types::TypeContext& m_types;
        diag::DiagnosticEngine& m_diag;
        std::unordered_map<std::string_view, ast::VarDecl const*> m_vars;
        std::unordered_map<std::string_view, ast::UsingDecl const*> m_value_aliases;
        std::unordered_map<std::string_view, ast::EnumDecl const*> m_enums;
        std::unordered_map<ast::EnumDecl const*, types::TypePtr> m_enum_backing;
        std::unordered_map<ast::EnumDecl const*, std::vector<std::int64_t>> m_enum_discriminants;
        std::unordered_set<ast::EnumDecl const*> m_enum_in_progress;
        std::unordered_map<std::string_view, comptime::Value> m_cache;
        std::unordered_set<std::string_view> m_in_progress;

        static bool is_const_var(ast::VarDecl const* v)
        {
            auto const* q = ast::node_cast<ast::QualifiedType>(v->type);
            return q && ast::has_qual(q->quals, ast::Qual::Const);
        }

        std::optional<bool> require_bool(comptime::Value const& v, sm::SourceRange range)
        {
            if (v.kind() == comptime::Value::Kind::Bool)
                return v.get_bool();
            m_diag.error(range, "operand of a logical operator must be a compile-time boolean");
            return std::nullopt;
        }

        types::TypePtr unary_out_type(lex::TokenKind op, comptime::Value const& v) const
        {
            using K = comptime::Value::Kind;
            switch (op)
            {
                case lex::TokenKind::Bang:
                    return v.kind() == K::Bool ? v.type : nullptr;
                case lex::TokenKind::Minus:
                case lex::TokenKind::Plus:
                    return (v.kind() == K::Int || v.kind() == K::Float) ? v.type : nullptr;
                case lex::TokenKind::Tilde:
                    return v.kind() == K::Int ? v.type : nullptr;
                default:
                    return nullptr;
            }
        }

        types::TypePtr binary_out_type(lex::TokenKind op, comptime::Value const& l, comptime::Value const& r)
        {
            using K = comptime::Value::Kind;
            switch (op)
            {
                case lex::TokenKind::EqEq:
                case lex::TokenKind::BangEq:
                case lex::TokenKind::Lt:
                case lex::TokenKind::LtEq:
                case lex::TokenKind::Gt:
                case lex::TokenKind::GtEq:
                    return m_types.m_boolt();
                case lex::TokenKind::Plus:
                case lex::TokenKind::Minus:
                case lex::TokenKind::Star:
                case lex::TokenKind::Slash:
                case lex::TokenKind::Percent:
                    if (l.kind() == K::Float && r.kind() == K::Float)
                        return l.type;
                    if (l.kind() == K::Int && r.kind() == K::Int)
                        return l.type;
                    return nullptr;
                case lex::TokenKind::Amp:
                case lex::TokenKind::Pipe:
                case lex::TokenKind::Caret:
                case lex::TokenKind::LtLt:
                case lex::TokenKind::GtGt:
                    return (l.kind() == K::Int && r.kind() == K::Int) ? l.type : nullptr;
                default:
                    return nullptr;
            }
        }

        std::optional<comptime::Value> eval_ident(ast::IdentExpr const* id)
        {
            if (auto it = m_cache.find(id->name); it != m_cache.end())
                return it->second;

            if (m_in_progress.contains(id->name))
            {
                m_diag.error(id->range, "`{}` has a cyclic compile-time initializer", id->name);
                return std::nullopt;
            }

            if (auto vit = m_vars.find(id->name); vit != m_vars.end())
            {
                auto const* v = vit->second;
                if (!is_const_var(v))
                {
                    m_diag.error(id->range, "`{}` is not a compile-time constant", id->name);
                    return std::nullopt;
                }
                if (!v->init)
                {
                    m_diag.error(id->range, "`{}` has no initializer to evaluate", id->name);
                    return std::nullopt;
                }

                m_in_progress.insert(id->name);
                auto val = eval(v->init);
                m_in_progress.erase(id->name);
                if (val)
                    m_cache.insert_or_assign(id->name, *val);
                return val;
            }

            if (auto ait = m_value_aliases.find(id->name); ait != m_value_aliases.end())
            {
                auto const* u = ait->second;
                if (!u->target_expr)
                {
                    m_diag.error(id->range, "`{}` has no initializer to evaluate", id->name);
                    return std::nullopt;
                }

                m_in_progress.insert(id->name);
                auto val = eval(u->target_expr);
                m_in_progress.erase(id->name);
                if (val)
                    m_cache.insert_or_assign(id->name, *val);
                return val;
            }

            m_diag.error(id->range, "unknown identifier `{}` in `static if` condition", id->name);
            return std::nullopt;
        }

        [[nodiscard]] types::TypePtr primitive_type(lex::TokenKind kind) const
        {
            switch (kind)
            {
                case lex::TokenKind::KwVoid:
                    return m_types.m_voidt();
                case lex::TokenKind::KwBool:
                    return m_types.m_boolt();
                case lex::TokenKind::Kwi8:
                    return m_types.int_t(8, true);
                case lex::TokenKind::Kwi16:
                    return m_types.int_t(16, true);
                case lex::TokenKind::Kwi32:
                    return m_types.int_t(32, true);
                case lex::TokenKind::Kwi64:
                    return m_types.int_t(64, true);
                case lex::TokenKind::Kwu8:
                    return m_types.int_t(8, false);
                case lex::TokenKind::Kwu16:
                    return m_types.int_t(16, false);
                case lex::TokenKind::Kwu32:
                    return m_types.int_t(32, false);
                case lex::TokenKind::Kwu64:
                    return m_types.int_t(64, false);
                case lex::TokenKind::Kwf32:
                    return m_types.float_t(32);
                case lex::TokenKind::Kwf64:
                    return m_types.float_t(64);
                case lex::TokenKind::KwChar:
                    return m_types.m_chart();
                case lex::TokenKind::KwUsize:
                    return m_types.usize_t();
                case lex::TokenKind::KwIsize:
                    return m_types.isize_t();
                default:
                    return nullptr;
            }
        }

        [[nodiscard]] std::optional<std::int64_t> variant_discriminant(ast::EnumDecl const* e, std::size_t index)
        {
            if (auto it = m_enum_discriminants.find(e); it != m_enum_discriminants.end())
            {
                if (index < it->second.size())
                    return it->second[index];
                return std::nullopt;
            }

            if (m_enum_in_progress.contains(e))
                return std::nullopt;

            m_enum_in_progress.insert(e);
            std::vector<std::int64_t> discs;
            discs.reserve(e->variants.size());
            for (auto const& v : e->variants)
            {
                if (v.explicit_value)
                {
                    auto val = eval(v.explicit_value);
                    if (!val || val->kind() != comptime::Value::Kind::Int)
                    {
                        m_enum_in_progress.erase(e);
                        return std::nullopt;
                    }
                    discs.push_back(val->get_int());
                }
                else
                {
                    discs.push_back(discs.empty() ? 0 : discs.back() + 1);
                }
            }
            m_enum_in_progress.erase(e);
            m_enum_discriminants.insert_or_assign(e, std::move(discs));

            auto const& cached = m_enum_discriminants[e];
            if (index < cached.size())
                return cached[index];
            return std::nullopt;
        }

        [[nodiscard]] types::TypePtr enum_backing_type(ast::EnumDecl const* e)
        {
            if (!e)
                return nullptr;

            if (auto it = m_enum_backing.find(e); it != m_enum_backing.end())
                return it->second;

            types::TypePtr backing = nullptr;
            if (e->backing_type)
            {
                if (auto const* pt = ast::node_cast<ast::PrimitiveType>(e->backing_type))
                {
                    auto const* int_ty = types::type_cast<types::IntType>(primitive_type(pt->which));
                    if (int_ty)
                        backing = int_ty;
                }
            }

            if (!backing)
            {
                bool has_negative = false;
                std::uint64_t max_pos = 0;
                std::uint64_t max_neg_mag = 0;
                for (std::size_t i = 0; i < e->variants.size(); ++i)
                {
                    auto d = variant_discriminant(e, i);
                    if (!d)
                        return nullptr;

                    if (*d < 0)
                    {
                        has_negative = true;
                        auto mag = (*d == std::numeric_limits<std::int64_t>::min())
                                       ? (std::uint64_t{1} << 63)
                                       : static_cast<std::uint64_t>(-*d);
                        max_neg_mag = std::max(max_neg_mag, mag);
                    }
                    else
                    {
                        max_pos = std::max(max_pos, static_cast<std::uint64_t>(*d));
                    }
                }

                std::uint8_t bits = 8;
                if (has_negative)
                {
                    while (bits < 64)
                    {
                        auto max_mag = std::uint64_t{1} << (bits - 1);
                        if (max_pos <= max_mag - 1 && max_neg_mag <= max_mag)
                            break;
                        bits *= 2;
                    }
                    backing = m_types.int_t(bits, true);
                }
                else
                {
                    while (bits < 64)
                    {
                        if (max_pos <= (std::uint64_t{1} << bits) - 1)
                            break;
                        bits *= 2;
                    }
                    backing = m_types.int_t(bits, false);
                }
            }

            if (backing && types::type_cast<types::IntType>(backing))
                m_enum_backing.insert_or_assign(e, backing);
            return backing;
        }

        [[nodiscard]] types::TypePtr resolve_cast_target_type(ast::TypeExpr const* t)
        {
            if (!t)
                return nullptr;

            switch (t->kind)
            {
                case ast::TypeKind::Primitive:
                    return primitive_type(static_cast<ast::PrimitiveType const*>(t)->which);
                case ast::TypeKind::Named: {
                    auto const* nt = static_cast<ast::NamedType const*>(t);
                    if (nt->path.is_simple())
                    {
                        if (auto eit = m_enums.find(nt->path.simple_name()); eit != m_enums.end())
                            return enum_backing_type(eit->second);
                    }
                    return nullptr;
                }
                default:
                    return nullptr;
            }
        }

        std::optional<comptime::Value> eval_path(ast::PathExpr const* pe)
        {
            if (pe->path.segments.size() != 2)
            {
                m_diag.error(pe->range, "unsupported path in `static if` condition");
                return std::nullopt;
            }

            auto enum_name = pe->path.segments[0].name;
            auto variant_name = pe->path.segments[1].name;

            auto eit = m_enums.find(enum_name);
            if (eit == m_enums.end())
            {
                m_diag.error(pe->range, "unknown identifier `{}` in `static if` condition", enum_name);
                return std::nullopt;
            }

            auto const* e = eit->second;
            for (std::size_t i = 0; i < e->variants.size(); ++i)
            {
                if (e->variants[i].name != variant_name)
                    continue;

                auto disc = variant_discriminant(e, i);
                if (!disc)
                {
                    m_diag.error(pe->range, "cannot evaluate enum variant `{}::{}` in `static if` condition", enum_name, variant_name);
                    return std::nullopt;
                }

                auto backing = enum_backing_type(e);
                if (!backing)
                {
                    m_diag.error(pe->range, "cannot determine backing type of enum `{}` in `static if` condition", enum_name);
                    return std::nullopt;
                }

                return comptime::Value::make_int(*disc, backing);
            }

            m_diag.error(pe->range, "unknown enum variant `{}::{}`", enum_name, variant_name);
            return std::nullopt;
        }

        std::optional<comptime::Value> eval(ast::Expr const* e)
        {
            switch (e->kind)
            {
                case ast::ExprKind::BoolLiteral:
                    return comptime::Value::make_bool(static_cast<ast::BoolLiteralExpr const*>(e)->value, m_types.m_boolt());
                case ast::ExprKind::IntLiteral:
                    return comptime::Value::make_int(static_cast<ast::IntLiteralExpr const*>(e)->value, m_types.int_t(32, true));
                case ast::ExprKind::FloatLiteral:
                    return comptime::Value::make_float(static_cast<ast::FloatLiteralExpr const*>(e)->value, m_types.float_t(64));
                case ast::ExprKind::CharLiteral:
                    return comptime::Value::make_char(static_cast<ast::CharLiteralExpr const*>(e)->codepoint, m_types.m_chart());
                case ast::ExprKind::Ident:
                    return eval_ident(static_cast<ast::IdentExpr const*>(e));
                case ast::ExprKind::PathExpr:
                    return eval_path(static_cast<ast::PathExpr const*>(e));
                case ast::ExprKind::Cast: {
                    auto const* c = static_cast<ast::CastExpr const*>(e);
                    auto operand = eval(c->operand);
                    if (!operand)
                        return std::nullopt;

                    auto dst = resolve_cast_target_type(c->target);
                    if (!dst)
                    {
                        m_diag.error(e->range, "unsupported cast target in `static if` condition");
                        return std::nullopt;
                    }

                    auto r = const_eval::fold_cast(*operand, dst);
                    if (!r)
                        m_diag.error(e->range, "`static if` condition is not a compile-time constant expression");
                    return r;
                }
                case ast::ExprKind::Unary: {
                    auto const* u = static_cast<ast::UnaryExpr const*>(e);
                    auto operand = eval(u->operand);
                    if (!operand)
                        return std::nullopt;
                    auto out = unary_out_type(u->op, *operand);
                    if (!out)
                    {
                        m_diag.error(e->range, "unsupported operand in `static if` condition");
                        return std::nullopt;
                    }
                    auto r = const_eval::fold_unary(u->op, *operand, out);
                    if (!r)
                        m_diag.error(e->range, "`static if` condition is not a compile-time constant expression");
                    return r;
                }
                case ast::ExprKind::Binary: {
                    auto const* b = static_cast<ast::BinaryExpr const*>(e);
                    if (b->op == lex::TokenKind::AmpAmp || b->op == lex::TokenKind::PipePipe)
                    {
                        auto l = eval(b->lhs);
                        if (!l)
                            return std::nullopt;
                        auto lb = require_bool(*l, b->lhs->range);
                        if (!lb)
                            return std::nullopt;
                        if (b->op == lex::TokenKind::AmpAmp && !*lb)
                            return comptime::Value::make_bool(false, m_types.m_boolt());
                        if (b->op == lex::TokenKind::PipePipe && *lb)
                            return comptime::Value::make_bool(true, m_types.m_boolt());
                        auto r = eval(b->rhs);
                        if (!r)
                            return std::nullopt;
                        auto rb = require_bool(*r, b->rhs->range);
                        if (!rb)
                            return std::nullopt;
                        return comptime::Value::make_bool(*rb, m_types.m_boolt());
                    }

                    auto l = eval(b->lhs);
                    if (!l)
                        return std::nullopt;
                    auto r = eval(b->rhs);
                    if (!r)
                        return std::nullopt;
                    auto out = binary_out_type(b->op, *l, *r);
                    if (!out)
                    {
                        m_diag.error(e->range, "unsupported operands in `static if` condition");
                        return std::nullopt;
                    }
                    auto res = const_eval::fold_binary(b->op, *l, *r, out);
                    if (!res)
                        m_diag.error(e->range, "`static if` condition is not a compile-time constant expression");
                    return res;
                }
                default:
                    m_diag.error(e->range, "`static if` condition is not a compile-time constant expression");
                    return std::nullopt;
            }
        }
    };

    class DeclCollector
    {
    public:
        DeclCollector(ModuleInfo& mod, diag::DiagnosticEngine& diag, types::TypeContext& types, std::pmr::polymorphic_allocator<> a)
            : m_mod{mod}, m_diag{diag}, m_alloc{a}, m_eval{types, diag}
        {
        }

        void run()
        {
            if (m_mod.state >= ModuleState::Collected)
                return;

            m_mod.own_scope = make_scope(ScopeKind::Module, nullptr);
            m_mod.export_scope = make_scope(ScopeKind::Module, nullptr);
            m_mod.ufcs_scope = make_scope(ScopeKind::Module, nullptr);

            for (auto* d : m_mod.tu->decls)
            {
                if (auto* v = ast::node_cast<ast::VarDecl>(d))
                    m_eval.register_var(v);
                else if (auto* u = ast::node_cast<ast::UsingDecl>(d))
                    m_eval.register_value_alias(u);
                else if (auto* e = ast::node_cast<ast::EnumDecl>(d))
                    m_eval.register_enum(e);
            }

            std::vector<ast::Decl*> flat;
            for (auto* d : m_mod.tu->decls)
                collect_top_level(d, flat);

            m_mod.tu->decls.clear();
            for (auto* d : flat)
                m_mod.tu->decls.push_back(d);

            m_mod.state = ModuleState::Collected;
        }

    private:
        ModuleInfo& m_mod;
        diag::DiagnosticEngine& m_diag;
        std::pmr::polymorphic_allocator<> m_alloc;
        ModuleConstEvaluator m_eval;

        Scope* make_scope(ScopeKind k, Scope const* parent)
        {
            auto* p = m_alloc.allocate_object<Scope>();
            return std::construct_at(p, k, parent, m_alloc);
        }

        void collect_top_level(ast::Decl* d, std::vector<ast::Decl*>& flat)
        {
            switch (d->kind)
            {
                case ast::DeclKind::Struct:
                    register_type(d, ast::node_cast<ast::StructDecl>(d)->name, SymbolKind::Struct);
                    break;
                case ast::DeclKind::Union:
                    register_type(d, ast::node_cast<ast::UnionDecl>(d)->name, SymbolKind::Union);
                    break;
                case ast::DeclKind::Enum:
                    register_enum(static_cast<ast::EnumDecl*>(d));
                    break;
                case ast::DeclKind::Func:
                    register_function(static_cast<ast::FuncDecl*>(d));
                    break;
                case ast::DeclKind::Var:
                    register_variable(static_cast<ast::VarDecl*>(d));
                    break;
                case ast::DeclKind::Using:
                    m_mod.using_worklist.push_back(static_cast<ast::UsingDecl*>(d));
                    break;
                case ast::DeclKind::StaticIfGroup:
                    collect_static_if_group(static_cast<ast::StaticIfGroup*>(d), flat);
                    return;
                case ast::DeclKind::Module:
                case ast::DeclKind::Import:
                    break;
            }

            flat.push_back(d);
        }

        void collect_static_if_group(ast::StaticIfGroup* g, std::vector<ast::Decl*>& flat)
        {
            std::int8_t idx = 0;
            for (ast::StaticIfGroup* cur = g; cur; cur = cur->else_group, ++idx)
            {
                if (!cur->condition)
                {
                    g->taken_branch = idx;
                    collect_branch(cur, flat);
                    return;
                }

                auto val = m_eval.eval_condition(cur->condition);
                if (!val)
                    return;

                if (val->kind() != comptime::Value::Kind::Bool)
                {
                    m_diag.error(cur->condition->range, "`static if` condition must be a compile-time boolean");
                    return;
                }

                if (val->get_bool())
                {
                    g->taken_branch = idx;
                    collect_branch(cur, flat);
                    return;
                }
            }
        }

        void collect_branch(ast::StaticIfGroup* branch, std::vector<ast::Decl*>& flat)
        {
            for (auto* inner : branch->then_decls)
            {
                if (auto* v = ast::node_cast<ast::VarDecl>(inner))
                    m_eval.register_var(v);
                else if (auto* u = ast::node_cast<ast::UsingDecl>(inner))
                    m_eval.register_value_alias(u);
                else if (auto* e = ast::node_cast<ast::EnumDecl>(inner))
                    m_eval.register_enum(e);
            }

            for (auto* inner : branch->then_decls)
                collect_top_level(inner, flat);
        }

        Symbol make_symbol(ast::Decl* d, std::string_view name, SymbolKind kind)
        {
            Symbol s{};

            s.name = name;
            s.kind = kind;
            s.decl = d;
            s.module = &m_mod;
            s.is_exported = d->is_public;
            s.definition_range = decl_name_range(d);

            return s;
        }

        [[nodiscard]] static sm::SourceRange decl_name_range(ast::Decl const* d) noexcept
        {
            if (!d)
                return {};

            switch (d->kind)
            {
                case ast::DeclKind::Func:
                    return static_cast<ast::FuncDecl const*>(d)->name_range;
                case ast::DeclKind::Var:
                    return static_cast<ast::VarDecl const*>(d)->name_range;
                case ast::DeclKind::Struct:
                    return static_cast<ast::StructDecl const*>(d)->name_range;
                case ast::DeclKind::Union:
                    return static_cast<ast::UnionDecl const*>(d)->name_range;
                case ast::DeclKind::Enum:
                    return static_cast<ast::EnumDecl const*>(d)->name_range;
                case ast::DeclKind::Using:
                    return static_cast<ast::UsingDecl const*>(d)->name_range;
                case ast::DeclKind::Module:
                    return static_cast<ast::ModuleDecl const*>(d)->name_range;
                case ast::DeclKind::Import:
                    return static_cast<ast::ImportDecl const*>(d)->name_range;
                default:
                    return d->range;
            }
        }

        void register_type(ast::Decl* d, std::string_view name, SymbolKind kind)
        {
            if (name.empty())
                return;

            auto s = make_symbol(d, name, kind);
            install_type(*m_mod.own_scope, s, d);
            if (d->is_public)
            {
                d->sema.exported = true;
                install_type(*m_mod.export_scope, s, d);
            }
        }

        void register_function(ast::FuncDecl* d)
        {
            if (d->name.empty())
                return;

            auto s = make_symbol(d, d->name, SymbolKind::Function);
            install_function(*m_mod.own_scope, s, d);
            install_function(*m_mod.ufcs_scope, s, d);
            if (d->is_public)
            {
                d->sema.exported = true;
                install_function(*m_mod.export_scope, s, d);
            }
        }

        void register_variable(ast::VarDecl* d)
        {
            if (d->name.empty())
                return;

            auto s = make_symbol(d, d->name, SymbolKind::Variable);
            install_variable(*m_mod.own_scope, s, d);
            if (d->is_public)
            {
                d->sema.exported = true;
                install_variable(*m_mod.export_scope, s, d);
            }
        }

        void register_enum(ast::EnumDecl* d)
        {
            if (d->name.empty())
                return;

            auto type_sym = make_symbol(d, d->name, SymbolKind::Enum);
            install_type(*m_mod.own_scope, type_sym, d);
            if (d->is_public)
            {
                d->sema.exported = true;
                install_type(*m_mod.export_scope, type_sym, d);
            }

            install_variant_scope(*m_mod.own_scope, d, type_sym);
            if (d->is_public)
                install_variant_scope(*m_mod.export_scope, d, type_sym);
        }

        void install_variant_scope(Scope& scope, ast::EnumDecl* d, Symbol const& anchor)
        {
            auto* variant_scope = scope.ensure_namespace(d->name, anchor, ScopeKind::Enum);
            for (auto& v : d->variants)
            {
                Symbol vs{};
                vs.name = v.name;
                vs.kind = SymbolKind::EnumVariant;
                vs.decl = d;
                vs.module = &m_mod;
                vs.is_exported = d->is_public;
                vs.definition_range = v.range;

                Symbol const* existing = nullptr;
                auto r = variant_scope->define_variable(vs, &existing);
                if (r == DefineResult::Conflict)
                {
                    auto diag_obj = diag::Diagnostic{diag::Severity::Error, std::format("duplicate enum variant `{}`", v.name)}.primary(v.range);
                    if (existing && existing->definition_range.valid())
                        std::move(diag_obj).secondary(existing->definition_range, "previous variant was here");
                    m_diag.emit(std::move(diag_obj));
                }
            }
        }

        void install_type(Scope& scope, Symbol const& s, ast::Decl* d)
        {
            Symbol const* existing = nullptr;
            auto r = scope.define_type(s, &existing);
            if (r == DefineResult::Conflict)
            {
                auto diag_obj = diag::Diagnostic{diag::Severity::Error, std::format("redefinition of type `{}`", s.name)}.primary(
                    decl_name_range(d).valid() ? decl_name_range(d) : d->range);
                if (existing && existing->definition_range.valid())
                    std::move(diag_obj).secondary(existing->definition_range, "previous definition was here");
                m_diag.emit(std::move(diag_obj));
            }
        }

        void install_function(Scope& scope, Symbol const& s, ast::Decl* d)
        {
            Symbol const* existing = nullptr;
            auto r = scope.add_function_overload(s, &existing);
            if (r == DefineResult::Conflict)
            {
                auto diag_obj = diag::Diagnostic{diag::Severity::Error, std::format("name `{}` already declared as a variable", s.name)}.primary(
                    decl_name_range(d).valid() ? decl_name_range(d) : d->range);
                if (existing && existing->definition_range.valid())
                    std::move(diag_obj).secondary(existing->definition_range, "previous definition was here");
                m_diag.emit(std::move(diag_obj));
            }
        }

        void install_variable(Scope& scope, Symbol const& s, ast::Decl* d)
        {
            Symbol const* existing = nullptr;
            auto r = scope.define_variable(s, &existing);
            if (r == DefineResult::Conflict)
            {
                auto diag_obj = diag::Diagnostic{diag::Severity::Error, std::format("redefinition of `{}`", s.name)}.primary(
                    decl_name_range(d).valid() ? decl_name_range(d) : d->range);
                if (existing && existing->definition_range.valid())
                    std::move(diag_obj).secondary(existing->definition_range, "previous definition was here");
                m_diag.emit(std::move(diag_obj));
            }
        }
    };

    void collect_all(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, types::TypeContext& types,
                     std::pmr::polymorphic_allocator<> alloc)
    {
        for (auto const& m : modules)
        {
            if (!m->tu || m->state >= ModuleState::Collected)
                continue;

            DeclCollector{*m, diag, types, alloc}.run();
        }
    }

} // namespace dcc::sema
