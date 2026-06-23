export module dcc.sema.type_helpers;

import std;
import dcc.ast;
import dcc.types;

export namespace dcc::sema
{
    [[nodiscard]] types::TypePtr get_canonical(ast::TypeSema const& ts) noexcept
    {
        return reinterpret_cast<types::TypePtr>(ts.canonical);
    }

    void set_canonical(ast::TypeSema& ts, types::TypePtr tp) noexcept
    {
        ts.canonical = reinterpret_cast<decltype(ts.canonical)>(tp);
    }

    [[nodiscard]] types::TypePtr get_resolved_type(ast::ExprSema const& es) noexcept
    {
        return reinterpret_cast<types::TypePtr>(es.resolved_type);
    }

    void set_resolved_type(ast::ExprSema& es, types::TypePtr tp) noexcept
    {
        es.resolved_type = reinterpret_cast<decltype(es.resolved_type)>(tp);
    }

    [[nodiscard]] std::string format_dcc_type(types::TypePtr ty)
    {
        if (!ty)
            return "<unresolved>";

        switch (ty->kind)
        {
            case types::TypeKind::Void:
                return "void";
            case types::TypeKind::Bool:
                return "bool";
            case types::TypeKind::Int: {
                auto const* it = static_cast<types::IntType const*>(ty);
                if (it->is_pointer_sized)
                    return std::string(it->is_signed ? "isize" : "usize");
                return std::format("{}{}", it->is_signed ? 'i' : 'u', unsigned(it->bits));
            }
            case types::TypeKind::Float: {
                auto const* ft = static_cast<types::FloatType const*>(ty);
                return std::format("f{}", unsigned(ft->bits));
            }
            case types::TypeKind::Char:
                return "char";
            case types::TypeKind::NullT:
                return "null_t";
            case types::TypeKind::Pointer: {
                auto const* p = static_cast<types::PointerType const*>(ty);
                std::string quals;
                if (types::has_qual(p->pointee_quals, types::Qual::Const))
                    quals += "const ";
                if (types::has_qual(p->pointee_quals, types::Qual::Volatile))
                    quals += "volatile ";
                if (types::has_qual(p->pointee_quals, types::Qual::Restrict))
                    quals += "restrict ";
                return std::format("{}{}*", quals, format_dcc_type(p->pointee));
            }
            case types::TypeKind::Array: {
                auto const* a = static_cast<types::ArrayType const*>(ty);
                return std::format("{}[{}]", format_dcc_type(a->element), a->count);
            }
            case types::TypeKind::Slice: {
                auto const* s = static_cast<types::SliceType const*>(ty);
                std::string quals;
                if (types::has_qual(s->element_quals, types::Qual::Const))
                    quals += "const ";
                if (types::has_qual(s->element_quals, types::Qual::Volatile))
                    quals += "volatile ";
                if (types::has_qual(s->element_quals, types::Qual::Restrict))
                    quals += "restrict ";
                return std::format("[]{}{}", quals, format_dcc_type(s->element));
            }
            case types::TypeKind::Fam:
                return std::format("{}[]", format_dcc_type(static_cast<types::FamType const*>(ty)->element));
            case types::TypeKind::RuntimeArray:
                return std::format("runtime[{}]", format_dcc_type(static_cast<types::RuntimeArrayType const*>(ty)->element));
            case types::TypeKind::FuncPtr: {
                auto const* f = static_cast<types::FuncPtrType const*>(ty);
                std::string params;
                for (std::size_t i = 0; i < f->params.size(); ++i)
                {
                    if (i > 0)
                        params += ", ";
                    params += format_dcc_type(f->params[i]);
                }
                return std::format("{}(*)({})", format_dcc_type(f->return_type), params);
            }
            case types::TypeKind::Struct:
            case types::TypeKind::Union:
            case types::TypeKind::Enum: {
                auto const* ut = static_cast<types::UserType const*>(ty);
                auto const* dd = reinterpret_cast<ast::Decl const*>(ut->decl);
                std::string name = "<null>";
                if (dd)
                {
                    switch (dd->kind)
                    {
                        case ast::DeclKind::Struct:
                            name = std::string{static_cast<ast::StructDecl const*>(dd)->name};
                            break;
                        case ast::DeclKind::Union:
                            name = std::string{static_cast<ast::UnionDecl const*>(dd)->name};
                            break;
                        case ast::DeclKind::Enum:
                            name = std::string{static_cast<ast::EnumDecl const*>(dd)->name};
                            break;
                        case ast::DeclKind::Using:
                            if (!static_cast<ast::UsingDecl const*>(dd)->alias_path.is_empty())
                                name = std::string{static_cast<ast::UsingDecl const*>(dd)->alias_path.tail_name()};
                            else
                                name = "<using>";
                            break;
                        default:
                            break;
                    }
                }
                if (!ut->template_args.empty())
                {
                    std::string args;
                    for (std::size_t i = 0; i < ut->template_args.size(); ++i)
                    {
                        if (i > 0)
                            args += ", ";
                        args += format_dcc_type(ut->template_args[i]);
                    }
                    return std::format("{}({})", name, args);
                }
                return name;
            }
            case types::TypeKind::Range:
                return std::format("range({})", format_dcc_type(static_cast<types::RangeType const*>(ty)->element));
            case types::TypeKind::RangeInclusive:
                return std::format("range_inclusive({})", format_dcc_type(static_cast<types::RangeInclusiveType const*>(ty)->element));
            case types::TypeKind::TemplateParam:
                return std::string{static_cast<types::TemplateParamType const*>(ty)->name};
            case types::TypeKind::TypePack:
                return std::format("pack({})", format_dcc_type(static_cast<types::TypePackType const*>(ty)->element));
            case types::TypeKind::Nominal: {
                auto const* nt = static_cast<types::NominalType const*>(ty);
                auto const* dd = reinterpret_cast<ast::Decl const*>(nt->decl);
                std::string name = "<nominal>";
                if (dd && dd->kind == ast::DeclKind::Using)
                {
                    auto const* ud = static_cast<ast::UsingDecl const*>(dd);
                    if (!ud->alias_path.is_empty())
                        name = std::string{ud->alias_path.tail_name()};
                }
                return std::format("nominal({}, {})", name, format_dcc_type(nt->underlying));
            }
            case types::TypeKind::Error:
                return "<error>";
        }
        return "<type>";
    }

    [[nodiscard]] inline bool is_func_param_sema_pack(ast::FuncParam const& p, ast::FuncDecl const& fn)
    {
        if (p.is_pack)
            return true;

        if (p.type && p.type->kind == ast::TypeKind::PackIndex)
            return false;

        auto type = p.type && p.type->sema.canonical ? get_canonical(p.type->sema) : nullptr;
        if (!type)
            return false;

        if (types::type_cast<types::TypePackType>(type))
            return true;

        if (auto const* tpt = types::type_cast<types::TemplateParamType>(type))
            for (auto const& tp : fn.template_params)
                if (tp.name == tpt->name && tp.is_pack)
                    return true;

        return false;
    }

    [[nodiscard]] inline bool is_pack_indexable_type(types::TypePtr type) noexcept
    {
        if (!type)
            return false;

        if (auto const* tpt = types::type_cast<types::TemplateParamType>(type))
        {
            if (auto* tp = static_cast<ast::TemplateParam*>(tpt->param))
                return tp->is_pack;
            return false;
        }

        if (auto const* type_pack = types::type_cast<types::TypePackType>(type))
            return is_pack_indexable_type(type_pack->element);

        return false;
    }

} // namespace dcc::sema
