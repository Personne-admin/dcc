export module dcc.sema.public_validator;

import std;
import dcc.ast;
import dcc.diag;
import dcc.sm;
import dcc.types;
import dcc.sema.scope;
import dcc.sema.type_helpers;

export namespace dcc::sema
{
    class PublicValidator
    {
    public:
        PublicValidator(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag) : m_modules{modules}, m_diag{diag} {}

        void run()
        {
            for (auto const& m : m_modules)
            {
                if (!m->tu)
                    continue;

                validate_module(*m);
                if (m->state < ModuleState::Validated)
                    m->state = ModuleState::Validated;
            }
        }

    private:
        std::span<std::unique_ptr<ModuleInfo> const> m_modules;
        diag::DiagnosticEngine& m_diag;

        void validate_module(ModuleInfo const& mod)
        {
            for (auto* d : mod.tu->decls)
            {
                if (!d->is_public)
                    continue;

                validate_decl(mod, *d);
            }
        }

        void validate_decl(ModuleInfo const& mod, ast::Decl const& d)
        {
            switch (d.kind)
            {
                case ast::DeclKind::Struct:
                    validate_struct(mod, static_cast<ast::StructDecl const&>(d));
                    break;
                case ast::DeclKind::Union:
                    validate_union(mod, static_cast<ast::UnionDecl const&>(d));
                    break;
                case ast::DeclKind::Enum:
                    validate_enum(mod, static_cast<ast::EnumDecl const&>(d));
                    break;
                case ast::DeclKind::Func:
                    validate_func(mod, static_cast<ast::FuncDecl const&>(d));
                    break;
                case ast::DeclKind::Var:
                    validate_var(mod, static_cast<ast::VarDecl const&>(d));
                    break;
                case ast::DeclKind::Using:
                    validate_using(mod, static_cast<ast::UsingDecl const&>(d));
                    break;
                case ast::DeclKind::Module:
                case ast::DeclKind::Import:
                case ast::DeclKind::StaticIfGroup:
                    break;
            }
        }

        void validate_struct(ModuleInfo const& mod, ast::StructDecl const& d)
        {
            for (auto const& tp : d.template_params)
                validate_type(mod, tp.value_type);

            for (auto const& f : d.fields)
                validate_type(mod, f.type);
        }

        void validate_union(ModuleInfo const& mod, ast::UnionDecl const& d)
        {
            for (auto const& f : d.fields)
                validate_type(mod, f.type);
        }

        void validate_enum(ModuleInfo const& mod, ast::EnumDecl const& d)
        {
            for (auto const& tp : d.template_params)
                validate_type(mod, tp.value_type);

            validate_type(mod, d.backing_type);

            for (auto const& v : d.variants)
                for (auto const* p : v.payload)
                    validate_type(mod, p);
        }

        void validate_func(ModuleInfo const& mod, ast::FuncDecl const& d)
        {
            for (auto const& tp : d.template_params)
                validate_type(mod, tp.value_type);

            validate_type(mod, d.return_type);

            for (auto const& p : d.params)
                validate_type(mod, p.type);

            validate_expr(mod, d.constraint);
        }

        void validate_var(ModuleInfo const& mod, ast::VarDecl const& d) { validate_type(mod, d.type); }

        void validate_using(ModuleInfo const& mod, ast::UsingDecl const& d)
        {
            for (auto const& tp : d.template_params)
                validate_type(mod, tp.value_type);

            validate_type(mod, d.target_type);
            validate_expr(mod, d.target_expr);
        }

        void validate_expr(ModuleInfo const& mod, ast::Expr const* expr)
        {
            if (!expr)
                return;

            switch (expr->kind)
            {
                case ast::ExprKind::Compiles: {
                    auto const& c = *static_cast<ast::CompilesExpr const*>(expr);
                    for (auto const& p : c.params)
                        validate_type(mod, p.type);
                    break;
                }
                case ast::ExprKind::Unary:
                    validate_expr(mod, static_cast<ast::UnaryExpr const*>(expr)->operand);
                    break;
                case ast::ExprKind::Postfix:
                    validate_expr(mod, static_cast<ast::PostfixExpr const*>(expr)->operand);
                    break;
                case ast::ExprKind::Binary: {
                    auto const& b = *static_cast<ast::BinaryExpr const*>(expr);
                    validate_expr(mod, b.lhs);
                    validate_expr(mod, b.rhs);
                    break;
                }
                case ast::ExprKind::Call: {
                    auto const& c = *static_cast<ast::CallExpr const*>(expr);
                    validate_expr(mod, c.callee);
                    for (auto const* arg : c.args)
                        validate_expr(mod, arg);

                    break;
                }
                case ast::ExprKind::FieldAccess:
                    validate_expr(mod, static_cast<ast::FieldAccessExpr const*>(expr)->object);
                    break;
                case ast::ExprKind::Index: {
                    auto const& i = *static_cast<ast::IndexExpr const*>(expr);
                    validate_expr(mod, i.object);
                    validate_expr(mod, i.index);
                    break;
                }
                case ast::ExprKind::PackAccess: {
                    auto const& pa = *static_cast<ast::PackAccessExpr const*>(expr);
                    validate_expr(mod, pa.object);
                    validate_expr(mod, pa.index);
                    break;
                }
                case ast::ExprKind::Cast: {
                    auto const& c = *static_cast<ast::CastExpr const*>(expr);
                    validate_expr(mod, c.operand);
                    validate_type(mod, c.target);
                    break;
                }
                case ast::ExprKind::StructLiteral: {
                    auto const& s = *static_cast<ast::StructLiteralExpr const*>(expr);
                    validate_type(mod, s.type);
                    for (auto const& f : s.fields)
                        validate_expr(mod, f.value);

                    break;
                }
                case ast::ExprKind::Sizeof:
                    validate_type(mod, static_cast<ast::SizeofExpr const*>(expr)->target);
                    break;
                case ast::ExprKind::Alignof:
                    validate_type(mod, static_cast<ast::AlignofExpr const*>(expr)->target);
                    break;
                case ast::ExprKind::Offsetof:
                    validate_type(mod, static_cast<ast::OffsetofExpr const*>(expr)->target);
                    break;
                case ast::ExprKind::TypeAST:
                    validate_type(mod, static_cast<ast::TypeASTExpr const*>(expr)->type_node);
                    break;
                case ast::ExprKind::Range: {
                    auto const& r = *static_cast<ast::RangeExpr const*>(expr);
                    validate_expr(mod, r.start);
                    validate_expr(mod, r.end);
                    break;
                }
                case ast::ExprKind::TemplateInst: {
                    auto const& t = *static_cast<ast::TemplateInstExpr const*>(expr);
                    validate_expr(mod, t.callee);
                    for (auto const& arg : t.template_args)
                    {
                        validate_type(mod, arg.type);
                        validate_expr(mod, arg.expr);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        void validate_type(ModuleInfo const& mod, ast::TypeExpr const* node)
        {
            if (!node || !node->sema.canonical)
                return;

            auto const* ty = get_canonical(node->sema);

            if (ty->kind == types::TypeKind::Error)
                return;

            if (auto const* tp = types::type_cast<types::TemplateParamType>(ty))
            {
                std::ignore = tp;
                return;
            }

            switch (node->kind)
            {
                case ast::TypeKind::Primitive:
                    return;
                case ast::TypeKind::Named: {
                    auto const* named = static_cast<ast::NamedType const*>(node);
                    if (auto const* decl = nominal_decl(ty))
                    {
                        if (!is_publicly_visible(mod, decl))
                        {
                            m_diag.error(node->range, "public declaration exposes private type");
                            return;
                        }

                        if (!named->template_args.empty())
                        {
                            for (auto const& arg : named->template_args)
                                validate_type(mod, arg.type);
                        }
                        else
                            validate_canonical_args(mod, nominal_args(ty), node->range);

                        return;
                    }

                    validate_canonical(mod, ty, node->range);
                    for (auto const& arg : named->template_args)
                        validate_type(mod, arg.type);

                    return;
                }
                case ast::TypeKind::Pointer:
                    validate_type(mod, static_cast<ast::PointerType const*>(node)->pointee);
                    return;
                case ast::TypeKind::Array: {
                    auto const* a = static_cast<ast::ArrayType const*>(node);
                    validate_type(mod, a->element);
                    return;
                }
                case ast::TypeKind::Slice:
                    validate_type(mod, static_cast<ast::SliceType const*>(node)->element);
                    return;
                case ast::TypeKind::Fam:
                    validate_type(mod, static_cast<ast::FamType const*>(node)->element);
                    return;
                case ast::TypeKind::FuncPtr: {
                    auto const* f = static_cast<ast::FuncPtrType const*>(node);
                    validate_type(mod, f->return_type);
                    for (auto const* p : f->params)
                        validate_type(mod, p);

                    return;
                }
                case ast::TypeKind::Qualified:
                    validate_type(mod, static_cast<ast::QualifiedType const*>(node)->inner);
                    return;
                case ast::TypeKind::PackIndex: {
                    auto const* pi = static_cast<ast::PackIndexType const*>(node);
                    validate_type(mod, pi->base);
                    return;
                }
            }
        }

        void validate_canonical_args(ModuleInfo const& mod, std::span<types::TypePtr const> args, sm::SourceRange range)
        {
            for (auto const* arg : args)
                validate_canonical(mod, arg, range);
        }

        void validate_canonical(ModuleInfo const& mod, types::Type const* ty, sm::SourceRange range)
        {
            if (!ty)
                return;

            if (ty->kind == types::TypeKind::Error)
            {
                m_diag.error(range, "unresolved type in public signature");
                return;
            }

            if (types::type_cast<types::TemplateParamType>(ty))
                return;

            if (auto const* decl = nominal_decl(ty))
            {
                if (!is_publicly_visible(mod, decl))
                {
                    m_diag.error(range, "public declaration exposes private type");
                    return;
                }

                validate_canonical_args(mod, nominal_args(ty), range);
                return;
            }

            switch (ty->kind)
            {
                case types::TypeKind::Void:
                case types::TypeKind::Bool:
                case types::TypeKind::Int:
                case types::TypeKind::Float:
                case types::TypeKind::Char:
                case types::TypeKind::NullT:
                    return;
                case types::TypeKind::Pointer:
                    validate_canonical(mod, static_cast<types::PointerType const*>(ty)->pointee, range);
                    return;
                case types::TypeKind::Array:
                    validate_canonical(mod, static_cast<types::ArrayType const*>(ty)->element, range);
                    return;
                case types::TypeKind::RuntimeArray:
                    validate_canonical(mod, static_cast<types::RuntimeArrayType const*>(ty)->element, range);
                    return;
                case types::TypeKind::Slice:
                    validate_canonical(mod, static_cast<types::SliceType const*>(ty)->element, range);
                    return;
                case types::TypeKind::Range:
                    validate_canonical(mod, static_cast<types::RangeType const*>(ty)->element, range);
                    return;
                case types::TypeKind::RangeInclusive:
                    validate_canonical(mod, static_cast<types::RangeInclusiveType const*>(ty)->element, range);
                    return;
                case types::TypeKind::Fam:
                    validate_canonical(mod, static_cast<types::FamType const*>(ty)->element, range);
                    return;
                case types::TypeKind::FuncPtr: {
                    auto const* f = static_cast<types::FuncPtrType const*>(ty);
                    validate_canonical(mod, f->return_type, range);
                    for (auto const* p : f->params)
                        validate_canonical(mod, p, range);

                    return;
                }
                case types::TypeKind::Nominal:
                    validate_canonical(mod, static_cast<types::NominalType const*>(ty)->underlying, range);
                    return;
                case types::TypeKind::Struct:
                case types::TypeKind::Union:
                case types::TypeKind::Enum:
                case types::TypeKind::TemplateParam:
                case types::TypeKind::TypePack:
                case types::TypeKind::Error:
                    return;
            }
        }

        [[nodiscard]] bool is_publicly_visible(ModuleInfo const& mod, void const* decl) const
        {
            if (!decl)
                return false;

            return find_decl(mod, decl);
        }

        [[nodiscard]] bool find_decl(ModuleInfo const& mod, void const* decl) const
        {
            if (!mod.export_scope)
                return false;

            return scope_contains_decl(*mod.export_scope, decl);
        }

        [[nodiscard]] bool scope_contains_decl(Scope const& scope, void const* decl) const
        {
            std::unordered_set<Scope const*> seen;
            return scope_contains_decl_impl(scope, decl, seen);
        }

        [[nodiscard]] bool scope_contains_decl_impl(Scope const& scope, void const* decl, std::unordered_set<Scope const*>& seen) const
        {
            if (!seen.insert(&scope).second)
                return false;

            for (auto const& [_, b] : scope.bindings())
            {
                if (b.has_type && static_cast<void const*>(b.type_sym.decl) == decl)
                    return true;

                for (auto const& s : b.value_syms)
                    if (static_cast<void const*>(s.decl) == decl)
                        return true;

                if (b.has_namespace && b.namespace_scope && scope_contains_decl_impl(*b.namespace_scope, decl, seen))
                    return true;
            }

            return false;
        }

        [[nodiscard]] void const* nominal_decl(types::Type const* ty) const noexcept
        {
            if (!ty)
                return nullptr;

            if (ty->kind == types::TypeKind::Nominal)
                ty = static_cast<types::NominalType const*>(ty)->underlying;

            switch (ty->kind)
            {
                case types::TypeKind::Struct:
                    return static_cast<void const*>(static_cast<types::StructType const*>(ty)->decl);
                case types::TypeKind::Union:
                    return static_cast<void const*>(static_cast<types::UnionType const*>(ty)->decl);
                case types::TypeKind::Enum:
                    return static_cast<void const*>(static_cast<types::EnumType const*>(ty)->decl);
                default:
                    return nullptr;
            }
        }

        [[nodiscard]] std::span<types::TypePtr const> nominal_args(types::Type const* ty) const noexcept
        {
            if (!ty)
                return {};

            if (ty->kind == types::TypeKind::Nominal)
                ty = static_cast<types::NominalType const*>(ty)->underlying;

            switch (ty->kind)
            {
                case types::TypeKind::Struct:
                    return static_cast<types::StructType const*>(ty)->template_args;
                case types::TypeKind::Union:
                    return static_cast<types::UnionType const*>(ty)->template_args;
                case types::TypeKind::Enum:
                    return static_cast<types::EnumType const*>(ty)->template_args;
                default:
                    return {};
            }
        }
    };

    void validate_public_signatures(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag)
    {
        PublicValidator{modules, diag}.run();
    }

} // namespace dcc::sema
