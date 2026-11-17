export module dcc.sema.type_helpers;

import std;
import dcc.ast;
import dcc.diag;
import dcc.sm;
import dcc.types;
import dcc.sema.infer;

export namespace dcc::sema
{
    [[nodiscard]] bool is_unselected_default_allocator(types::TypeContext& types, types::TypePtr type)
    {
        if (!types.unselected_default_allocator_decl || !type)
            return false;

        auto const* struct_type = types::type_cast<types::StructType>(type);
        return struct_type && static_cast<void const*>(struct_type->decl) == types.unselected_default_allocator_decl;
    }

    [[nodiscard]] bool diagnose_unselected_default_allocator(types::TypeContext& types, diag::DiagnosticEngine& diag, types::TypePtr type, sm::SourceRange range)
    {
        if (!is_unselected_default_allocator(types, type))
            return false;

        diag.error(range, "no default allocator selected; pass -fdefault-allocator=<module>::<type> or give the allocator argument explicitly");
        return true;
    }

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
            case types::TypeKind::Restricted: {
                auto const* rt = static_cast<types::RestrictedType const*>(ty);
                auto format_ordinal = [&](std::uint64_t ordinal) {
                    auto raw = int_domain::ordinal_to_raw_bits(ordinal, *rt->underlying);
                    if (rt->underlying->is_signed)
                        return std::to_string(raw);
                    return std::to_string(static_cast<std::uint64_t>(raw) & int_domain::mask_for_bits(rt->underlying->bits));
                };

                auto out = format_dcc_type(rt->underlying);
                out += '{';
                for (std::size_t i = 0; i < rt->intervals.size(); ++i)
                {
                    if (i)
                        out += ", ";
                    auto const& interval = rt->intervals[i];
                    out += format_ordinal(interval.lo);
                    if (interval.lo != interval.hi)
                    {
                        out += "..";
                        out += format_ordinal(interval.hi);
                    }
                }
                out += '}';
                return out;
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
                std::string suffix = "*";
                if (p->flavor == types::PointerFlavor::Far)
                    suffix = "^";
                else if (p->flavor == types::PointerFlavor::Based)
                    suffix = std::format("^{}", types::TypeContext::seg_reg_name(p->segment));
                return std::format("{}{}{}", quals, format_dcc_type(p->pointee), suffix);
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
                std::string prefix = "[]";
                if (s->flavor == types::PointerFlavor::Far)
                    prefix = "[^]";
                else if (s->flavor == types::PointerFlavor::Based)
                    prefix = std::format("[^{}]", types::TypeContext::seg_reg_name(s->segment));
                return std::format("{}{}{}", prefix, quals, format_dcc_type(s->element));
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
                if (f->is_far)
                    return std::format("{}(^)({})", format_dcc_type(f->return_type), params);
                return std::format("{}(*)({})", format_dcc_type(f->return_type), params);
            }
            case types::TypeKind::Lambda:
                return "lambda";
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
            case types::TypeKind::Deferred:
                return "typeof(<dependent>)";
            case types::TypeKind::TemplateParam:
                return std::string{static_cast<types::TemplateParamType const*>(ty)->name};
            case types::TypeKind::TypePack:
                return std::format("pack({})", format_dcc_type(static_cast<types::TypePackType const*>(ty)->element));
            case types::TypeKind::Nominal: {
                auto const* nt = static_cast<types::NominalType const*>(ty);
                auto const* dd = reinterpret_cast<ast::Decl const*>(nt->decl);
                if (dd && dd->kind == ast::DeclKind::Using)
                {
                    auto const* ud = static_cast<ast::UsingDecl const*>(dd);
                    if (!ud->alias_path.is_empty())
                        return std::string{ud->alias_path.tail_name()};
                }
                return format_dcc_type(nt->underlying);
            }
            case types::TypeKind::Error:
                return "<error>";
        }
        return "<type>";
    }

    [[nodiscard]] inline std::string format_dcc_qualifiers(ast::Qual quals)
    {
        std::string out;
        if (ast::has_qual(quals, ast::Qual::Const))
            out += "const ";
        if (ast::has_qual(quals, ast::Qual::Volatile))
            out += "volatile ";
        if (ast::has_qual(quals, ast::Qual::Restrict))
            out += "restrict ";
        return out;
    }

    [[nodiscard]] inline std::string format_dcc_type_with_quals(types::TypePtr type, ast::Qual quals)
    {
        return format_dcc_qualifiers(quals) + format_dcc_type(type);
    }

    [[nodiscard]] inline bool type_contains_template_param(types::TypePtr type) noexcept
    {
        if (!type)
            return false;

        switch (type->kind)
        {
            case types::TypeKind::TemplateParam:
            case types::TypeKind::Deferred:
                return true;
            case types::TypeKind::Nominal:
                return type_contains_template_param(static_cast<types::NominalType const*>(type)->underlying);
            case types::TypeKind::Pointer:
                return type_contains_template_param(static_cast<types::PointerType const*>(type)->pointee);
            case types::TypeKind::Array:
                return type_contains_template_param(static_cast<types::ArrayType const*>(type)->element);
            case types::TypeKind::Slice:
                return type_contains_template_param(static_cast<types::SliceType const*>(type)->element);
            case types::TypeKind::Fam:
                return type_contains_template_param(static_cast<types::FamType const*>(type)->element);
            case types::TypeKind::RuntimeArray:
                return type_contains_template_param(static_cast<types::RuntimeArrayType const*>(type)->element);
            case types::TypeKind::FuncPtr: {
                auto const* fp = static_cast<types::FuncPtrType const*>(type);
                if (type_contains_template_param(fp->return_type))
                    return true;
                for (auto const* param : fp->params)
                    if (type_contains_template_param(param))
                        return true;
                return false;
            }
            case types::TypeKind::Struct:
            case types::TypeKind::Union:
            case types::TypeKind::Enum: {
                auto const* user = static_cast<types::UserType const*>(type);
                for (auto const* arg : user->template_args)
                    if (type_contains_template_param(arg))
                        return true;
                return false;
            }
            default:
                return false;
        }
    }

    struct TypeArgumentProjection
    {
        types::TypeResolution result{};
        std::string error;
    };

    [[nodiscard]] inline TypeArgumentProjection project_type_argument(types::TypeContext& types, types::TypePtr type, std::uint64_t index)
    {
        while (auto const* nominal = types::type_cast<types::NominalType>(type))
            type = nominal->underlying;

        if (!type || type->kind == types::TypeKind::Error)
            return {{types.m_errort(), types::Qual::None}, {}};

        types::UserType const* user = nullptr;
        switch (type->kind)
        {
            case types::TypeKind::Struct:
                user = static_cast<types::StructType const*>(type);
                break;
            case types::TypeKind::Union:
                user = static_cast<types::UnionType const*>(type);
                break;
            case types::TypeKind::Enum:
                user = static_cast<types::EnumType const*>(type);
                break;
            default:
                break;
        }

        if (!user)
            return {{types.m_errort(), types::Qual::None}, std::format("cannot project type argument {} from `{}`", index, format_dcc_type(type))};

        if (index >= user->template_args.size())
            return {{types.m_errort(), types::Qual::None},
                    std::format("type argument index {} is out of range for `{}`", index, format_dcc_type(type))};

        return {{user->template_args[static_cast<std::size_t>(index)], types::Qual::None}, {}};
    }

    [[nodiscard]] inline bool is_meta_type_query(std::string_view name) noexcept
    {
        return name == "type_argument" || name == "pointee" || name == "element" || name == "underlying" || name == "field_type" ||
               name == "variant_payload" || name == "parameter_type" || name == "return_type";
    }

    [[nodiscard]] inline std::size_t meta_type_query_arity(std::string_view name) noexcept
    {
        return name == "type_argument" || name == "field_type" || name == "variant_payload" || name == "parameter_type" ? 2 : 1;
    }

    struct MetaVariant
    {
        std::string_view name;
        ast::EnumVariant const* decl{};
        types::TypePtr payload{};
    };

    [[nodiscard]] inline std::optional<std::vector<MetaVariant>> meta_enum_variants(types::TypeContext& types, types::TypePtr type)
    {
        while (auto const* nominal = types::type_cast<types::NominalType>(type))
            type = nominal->underlying;
        auto const* enumeration = types::type_cast<types::EnumType>(type);
        if (!enumeration)
            return std::nullopt;
        auto const* decl = reinterpret_cast<ast::EnumDecl const*>(enumeration->decl);
        if (!decl)
            return std::nullopt;
        auto bindings = infer::make_bindings(types, type);
        std::vector<MetaVariant> variants;
        for (auto const& variant : decl->variants)
        {
            types::TypePtr payload = nullptr;
            if (!variant.payload.empty() && variant.payload.front())
            {
                payload = get_canonical(variant.payload.front()->sema);
                if (bindings)
                    payload = bindings->substitute(payload);
            }
            variants.push_back({variant.name, &variant, payload});
        }
        return variants;
    }

    struct MetaField
    {
        std::string_view name;
        types::TypePtr type{};
        ast::FieldDecl const* decl{};
        std::uint64_t expanded_offset{};
    };

    [[nodiscard]] inline std::optional<std::vector<MetaField>> meta_record_fields(types::TypeContext& types, types::TypePtr type)
    {
        while (auto const* nominal = types::type_cast<types::NominalType>(type))
            type = nominal->underlying;
        std::vector<MetaField> fields;
        std::span<ast::FieldDecl const> declared;
        if (auto const* structure = types::type_cast<types::StructType>(type))
        {
            if (structure->has_expansion)
            {
                for (std::size_t i = 0; i < structure->expanded_field_count; ++i)
                    fields.push_back({structure->expanded_fields[i].name, structure->expanded_fields[i].type, nullptr, structure->expanded_fields[i].byte_offset});
                return fields;
            }
            auto const* decl = reinterpret_cast<ast::StructDecl const*>(structure->decl);
            if (!decl)
                return std::nullopt;
            declared = decl->fields;
        }
        else if (auto const* unified = types::type_cast<types::UnionType>(type))
        {
            auto const* decl = reinterpret_cast<ast::UnionDecl const*>(unified->decl);
            if (!decl)
                return std::nullopt;
            declared = decl->fields;
        }
        else
            return std::nullopt;
        auto bindings = infer::make_bindings(types, type);
        for (auto const& field : declared)
        {
            auto field_type = field.type ? get_canonical(field.type->sema) : nullptr;
            if (bindings)
                field_type = bindings->substitute(field_type);
            fields.push_back({field.name, field_type, &field, 0});
        }
        return fields;
    }

    [[nodiscard]] inline TypeArgumentProjection project_meta_type(types::TypeContext& types, std::string_view query, types::TypePtr type, std::uint64_t index)
    {
        if (query == "type_argument")
            return project_type_argument(types, type, index);
        if (!type || type->kind == types::TypeKind::Error)
            return {{types.m_errort(), types::Qual::None}, {}};
        auto fail = [&](std::string_view what) -> TypeArgumentProjection {
            return {{types.m_errort(), types::Qual::None}, std::format("`{}` has no {}", format_dcc_type(type), what)};
        };
        if (query == "underlying")
        {
            if (auto const* nominal = types::type_cast<types::NominalType>(type))
                return {{nominal->underlying, types::Qual::None}, {}};
            if (auto const* restricted = types::type_cast<types::RestrictedType>(type))
                return {{restricted->underlying, types::Qual::None}, {}};
            if (auto const* enumeration = types::type_cast<types::EnumType>(type); enumeration && enumeration->backing && !enumeration->is_tagged)
                return {{enumeration->backing, types::Qual::None}, {}};
            return fail("underlying type");
        }
        while (auto const* nominal = types::type_cast<types::NominalType>(type))
            type = nominal->underlying;
        if (query == "parameter_type" || query == "return_type")
        {
            auto const* function = types::type_cast<types::FuncPtrType>(type);
            if (!function)
                return fail("parameters");
            if (query == "return_type")
                return {{function->return_type, types::Qual::None}, {}};
            if (index >= function->params.size())
                return {{types.m_errort(), types::Qual::None}, std::format("parameter index {} is out of range for `{}`", index, format_dcc_type(type))};
            return {{function->params[static_cast<std::size_t>(index)], types::Qual::None}, {}};
        }
        if (query == "variant_payload")
        {
            auto variants = meta_enum_variants(types, type);
            if (!variants)
                return fail("variants");
            if (index >= variants->size())
                return {{types.m_errort(), types::Qual::None}, std::format("variant index {} is out of range for `{}`", index, format_dcc_type(type))};
            auto const& variant = (*variants)[static_cast<std::size_t>(index)];
            if (!variant.payload)
                return {{types.m_errort(), types::Qual::None}, std::format("variant `{}` of `{}` has no payload", variant.name, format_dcc_type(type))};
            return {{variant.payload, types::Qual::None}, {}};
        }
        if (query == "field_type")
        {
            auto fields = meta_record_fields(types, type);
            if (!fields)
                return fail("fields");
            if (index >= fields->size())
                return {{types.m_errort(), types::Qual::None}, std::format("field index {} is out of range for `{}`", index, format_dcc_type(type))};
            return {{(*fields)[static_cast<std::size_t>(index)].type, types::Qual::None}, {}};
        }
        if (query == "pointee")
        {
            if (auto const* pointer = types::type_cast<types::PointerType>(type))
                return {{pointer->pointee, pointer->pointee_quals}, {}};
            return fail("pointee type");
        }
        if (auto const* array = types::type_cast<types::ArrayType>(type))
            return {{array->element, types::Qual::None}, {}};
        if (auto const* slice = types::type_cast<types::SliceType>(type))
            return {{slice->element, slice->element_quals}, {}};
        return fail("element type");
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
            return tpt->is_pack;

        if (auto const* type_pack = types::type_cast<types::TypePackType>(type))
            return is_pack_indexable_type(type_pack->element);

        return false;
    }

    [[nodiscard]] inline types::TypePtr variant_effective_payload_type(types::EnumType const* et, ast::EnumVariant const* variant) noexcept
    {
        if (!et || !variant || !et->tagged_layout)
            return nullptr;

        auto* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
        if (!ed)
            return nullptr;

        std::ptrdiff_t idx = variant - ed->variants.data();
        if (idx < 0 || static_cast<std::size_t>(idx) >= et->tagged_layout->variant_count)
            return nullptr;

        return et->tagged_layout->variants[idx].variant_payload_type_or_null;
    }

    [[nodiscard]] inline bool variant_has_effective_payload(types::EnumType const* et, ast::EnumVariant const* variant) noexcept
    {
        return variant_effective_payload_type(et, variant) != nullptr;
    }

    [[nodiscard]] inline bool is_concrete_void_type(types::TypePtr ty) noexcept
    {
        if (!ty)
            return false;

        while (ty->kind == types::TypeKind::Nominal)
            ty = static_cast<types::NominalType const*>(ty)->underlying;

        return ty->kind == types::TypeKind::Void;
    }

} // namespace dcc::sema
