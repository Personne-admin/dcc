export module dcc.sema.infer;

import std;
import dcc.types;
import dcc.comptime;

export namespace dcc::infer
{
    enum class DeductionError : std::uint8_t
    {
        None,
        Conflict,
        ArityMismatch,
        KindMismatch,
        OccursCheck,
        MissingField,
        DuplicateField,
    };

    struct DeductionResult
    {
        DeductionError error{DeductionError::None};
        std::string_view detail{""};

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return error == DeductionError::None; }
    };

    struct RecordField
    {
        std::string_view name;
        types::TypePtr type{};
    };

    class TemplateBindings
    {
    public:
        explicit TemplateBindings(types::TypeContext& types) : m_types(types) {}

        [[nodiscard]] bool empty() const noexcept
        {
            return m_bindings.empty() && m_value_bindings.empty() && m_pack_bindings.empty() && m_value_pack_bindings.empty();
        }
        [[nodiscard]] std::size_t size() const noexcept { return m_bindings.size(); }

        void clear()
        {
            m_bindings.clear();
            m_value_bindings.clear();
            m_pack_bindings.clear();
            m_value_pack_bindings.clear();
        }

        [[nodiscard]] types::TypePtr lookup(types::TemplateParamType const* param) const
        {
            if (!param)
                return nullptr;

            auto resolved = resolve(param);
            return resolved == param ? nullptr : resolved;
        }

        [[nodiscard]] types::TypePtr resolve(types::TypePtr type) const
        {
            std::unordered_set<types::TemplateParamType const*> seen;
            return resolve_impl(type, seen);
        }

        [[nodiscard]] bool bind_pack(types::TemplateParamType const* param, std::vector<types::TypePtr> types)
        {
            if (!param)
                return false;

            for (auto& pb : m_pack_bindings)
                if (pb.param == param)
                {
                    if (pb.types.size() != types.size())
                        return false;

                    for (std::size_t i = 0; i < types.size(); ++i)
                        if (pb.types[i] != types[i])
                            return false;

                    return true;
                }

            m_pack_bindings.push_back({param, std::move(types)});
            return true;
        }

        [[nodiscard]] std::vector<types::TypePtr> const* lookup_pack(types::TemplateParamType const* param) const
        {
            if (!param)
                return nullptr;

            for (auto const& pb : m_pack_bindings)
                if (pb.param == param)
                    return &pb.types;

            return nullptr;
        }

        [[nodiscard]] std::size_t pack_size(types::TemplateParamType const* param) const
        {
            auto* p = lookup_pack(param);
            return p ? p->size() : 0;
        }

        [[nodiscard]] bool has_pack_binding(types::TemplateParamType const* param) const { return lookup_pack(param) != nullptr; }

        [[nodiscard]] bool bind_value_pack(types::TemplateParamType const* param, std::vector<comptime::Value> values)
        {
            if (!param)
                return false;

            for (auto& pb : m_value_pack_bindings)
                if (pb.param == param)
                {
                    if (pb.values.size() != values.size())
                        return false;

                    for (std::size_t i = 0; i < values.size(); ++i)
                        if (!(pb.values[i] == values[i]))
                            return false;

                    return true;
                }

            m_value_pack_bindings.push_back({param, std::move(values)});
            return true;
        }

        [[nodiscard]] std::vector<comptime::Value> const* lookup_value_pack(types::TemplateParamType const* param) const
        {
            if (!param)
                return nullptr;

            for (auto const& pb : m_value_pack_bindings)
                if (pb.param == param)
                    return &pb.values;

            return nullptr;
        }

        [[nodiscard]] std::size_t value_pack_size(types::TemplateParamType const* param) const
        {
            auto* p = lookup_value_pack(param);
            return p ? p->size() : 0;
        }

        [[nodiscard]] bool has_value_pack_binding(types::TemplateParamType const* param) const { return lookup_value_pack(param) != nullptr; }

        [[nodiscard]] std::size_t total_pack_elements() const noexcept
        {
            std::size_t total = 0;
            for (auto const& pb : m_pack_bindings)
                total += pb.types.size();

            for (auto const& pb : m_value_pack_bindings)
                total += pb.values.size();

            return total;
        }

        void bind_value(types::TemplateParamType const* param, comptime::Value value)
        {
            if (!param)
                return;

            for (auto& vb : m_value_bindings)
                if (vb.param == param)
                {
                    vb.value = std::move(value);
                    return;
                }

            m_value_bindings.push_back({param, std::move(value)});
        }

        [[nodiscard]] comptime::Value const* lookup_value(types::TemplateParamType const* param) const
        {
            if (!param)
                return nullptr;

            for (auto const& vb : m_value_bindings)
                if (vb.param == param)
                    return &vb.value;

            return nullptr;
        }

        [[nodiscard]] bool has_value_binding(types::TemplateParamType const* param) const { return lookup_value(param) != nullptr; }

        [[nodiscard]] std::size_t value_binding_count() const noexcept { return m_value_bindings.size(); }

        [[nodiscard]] auto const& value_bindings() const noexcept { return m_value_bindings; }

        [[nodiscard]] DeductionResult deduce(types::TypePtr pattern, types::TypePtr actual) { return unify(pattern, actual); }

        [[nodiscard]] DeductionResult deduce_function(std::span<types::TypePtr const> params, std::span<types::TypePtr const> args)
        {
            bool has_pack_param = false;
            std::size_t non_pack_count = params.size();
            if (!params.empty())
            {
                auto const* last = params.back();
                if (last && last->kind == types::TypeKind::TypePack)
                {
                    has_pack_param = true;
                    non_pack_count = params.size() - 1;
                }
            }

            if (!has_pack_param)
            {
                if (params.size() != args.size())
                    return fail(DeductionError::ArityMismatch, "function argument count mismatch");

                for (std::size_t i = 0; i < params.size(); ++i)
                    if (auto r = deduce(params[i], args[i]); !r)
                        return r;

                return ok();
            }

            if (args.size() < non_pack_count)
                return fail(DeductionError::ArityMismatch, "not enough arguments for function with pack parameter");

            for (std::size_t i = 0; i < non_pack_count; ++i)
                if (auto r = deduce(params[i], args[i]); !r)
                    return r;

            auto const* pack_type = types::type_cast<types::TypePackType>(params.back());
            if (!pack_type)
                return fail(DeductionError::KindMismatch, "expected type pack");

            std::vector<types::TypePtr> pack_elements;
            for (std::size_t i = non_pack_count; i < args.size(); ++i)
                pack_elements.push_back(args[i]);

            if (auto const* tp = types::type_cast<types::TemplateParamType>(pack_type->element))
            {
                if (!has_pack_binding(tp))
                    if (!bind_pack(tp, pack_elements))
                        return fail(DeductionError::Conflict, "conflicting pack binding");
            }
            else
                for (std::size_t i = non_pack_count; i < args.size(); ++i)
                {
                    auto r = unify(pack_type->element, args[i]);
                    if (!r)
                        return r;
                }

            return ok();
        }

        [[nodiscard]] DeductionResult deduce_record(std::span<RecordField const> pattern, std::span<RecordField const> actual)
        {
            if (pattern.size() != actual.size())
                return fail(DeductionError::ArityMismatch, "record field count mismatch");

            std::unordered_map<std::string_view, types::TypePtr> actual_fields;
            actual_fields.reserve(actual.size());

            for (auto const& field : actual)
            {
                auto [it, inserted] = actual_fields.emplace(field.name, field.type);
                if (!inserted)
                    return fail(DeductionError::DuplicateField, "duplicate field in actual record shape");
            }

            for (auto const& field : pattern)
            {
                auto it = actual_fields.find(field.name);
                if (it == actual_fields.end())
                    return fail(DeductionError::MissingField, "missing field in actual record shape");

                if (auto r = deduce(field.type, it->second); !r)
                    return r;

                actual_fields.erase(it);
            }

            return actual_fields.empty() ? ok() : fail(DeductionError::MissingField, "extra field in actual record shape");
        }

        [[nodiscard]] types::TypePtr substitute(types::TypePtr type) const
        {
            std::unordered_map<types::TypePtr, types::TypePtr> memo;
            return substitute_impl(type, memo);
        }

        [[nodiscard]] std::vector<RecordField> substitute_record(std::span<RecordField const> fields) const
        {
            std::vector<RecordField> out;
            out.reserve(fields.size());

            for (auto const& field : fields)
                out.push_back({field.name, substitute(field.type)});

            return out;
        }

    private:
        struct Binding
        {
            types::TemplateParamType const* param{};
            types::TypePtr type{};
        };

        struct ValueBinding
        {
            types::TemplateParamType const* param{};
            comptime::Value value;
        };

        struct PackBinding
        {
            types::TemplateParamType const* param{};
            std::vector<types::TypePtr> types;
        };

        struct ValuePackBinding
        {
            types::TemplateParamType const* param{};
            std::vector<comptime::Value> values;
        };

        types::TypeContext& m_types;
        std::vector<Binding> m_bindings;
        std::vector<ValueBinding> m_value_bindings;
        std::vector<PackBinding> m_pack_bindings;
        std::vector<ValuePackBinding> m_value_pack_bindings;

        [[nodiscard]] static DeductionResult ok() { return {}; }

        [[nodiscard]] static DeductionResult fail(DeductionError error, std::string_view detail) { return DeductionResult{error, detail}; }

        [[nodiscard]] Binding* find_binding(types::TemplateParamType const* param)
        {
            for (auto& binding : m_bindings)
                if (binding.param == param)
                    return &binding;

            return nullptr;
        }

        [[nodiscard]] Binding const* find_binding(types::TemplateParamType const* param) const
        {
            for (auto const& binding : m_bindings)
                if (binding.param == param)
                    return &binding;

            return nullptr;
        }

        [[nodiscard]] types::TypePtr resolve_impl(types::TypePtr type, std::unordered_set<types::TemplateParamType const*>& seen) const
        {
            if (!type)
                return nullptr;

            if (auto const* param = types::type_cast<types::TemplateParamType>(type))
            {
                if (!seen.insert(param).second)
                    return m_types.m_errort();

                if (auto const* binding = find_binding(param))
                    return resolve_impl(binding->type, seen);
            }

            return type;
        }

        [[nodiscard]] bool contains_param(types::TypePtr type, types::TemplateParamType const* target) const
        {
            if (!type)
                return false;

            if (auto const* param = types::type_cast<types::TemplateParamType>(type))
                return param == target;

            switch (type->kind)
            {
                case types::TypeKind::Pointer: {
                    auto const* t = static_cast<types::PointerType const*>(type);
                    return contains_param(t->pointee, target);
                }
                case types::TypeKind::Array: {
                    auto const* t = static_cast<types::ArrayType const*>(type);
                    return contains_param(t->element, target);
                }
                case types::TypeKind::RuntimeArray: {
                    auto const* t = static_cast<types::RuntimeArrayType const*>(type);
                    return contains_param(t->element, target);
                }
                case types::TypeKind::Slice: {
                    auto const* t = static_cast<types::SliceType const*>(type);
                    return contains_param(t->element, target);
                }
                case types::TypeKind::Range: {
                    auto const* t = static_cast<types::RangeType const*>(type);
                    return contains_param(t->element, target);
                }
                case types::TypeKind::RangeInclusive: {
                    auto const* t = static_cast<types::RangeInclusiveType const*>(type);
                    return contains_param(t->element, target);
                }
                case types::TypeKind::Fam: {
                    auto const* t = static_cast<types::FamType const*>(type);
                    return contains_param(t->element, target);
                }
                case types::TypeKind::FuncPtr: {
                    auto const* t = static_cast<types::FuncPtrType const*>(type);
                    if (contains_param(t->return_type, target))
                        return true;
                    for (auto const* p : t->params)
                        if (contains_param(p, target))
                            return true;
                    return false;
                }
                case types::TypeKind::Nominal: {
                    auto const* t = static_cast<types::NominalType const*>(type);
                    return contains_param(t->underlying, target);
                }
                case types::TypeKind::Struct:
                case types::TypeKind::Union:
                case types::TypeKind::Enum: {
                    auto const* t = static_cast<types::UserType const*>(type);
                    for (auto const* arg : t->template_args)
                        if (contains_param(arg, target))
                            return true;
                    if (auto const* e = types::type_cast<types::EnumType>(type); e && contains_param(e->backing, target))
                        return true;
                    return false;
                }
                case types::TypeKind::TypePack: {
                    auto const* t = static_cast<types::TypePackType const*>(type);
                    return contains_param(t->element, target);
                }
                case types::TypeKind::Void:
                case types::TypeKind::Bool:
                case types::TypeKind::Int:
                case types::TypeKind::Float:
                case types::TypeKind::Char:
                case types::TypeKind::NullT:
                case types::TypeKind::TemplateParam:
                case types::TypeKind::Error:
                    return false;
            }

            return false;
        }

        [[nodiscard]] DeductionResult bind(types::TemplateParamType const* param, types::TypePtr value)
        {
            value = substitute(value);

            if (value == param)
                return ok();

            if (contains_param(value, param))
                return fail(DeductionError::OccursCheck, "occurs-check failed");

            if (auto* existing = find_binding(param))
                return deduce(existing->type, value);

            m_bindings.push_back({param, value});
            return ok();
        }

        [[nodiscard]] DeductionResult unify(types::TypePtr lhs, types::TypePtr rhs)
        {
            lhs = substitute(lhs);
            rhs = substitute(rhs);

            if (lhs == rhs)
                return ok();

            if (auto const* lp = types::type_cast<types::TemplateParamType>(lhs))
                return bind(lp, rhs);

            if (auto const* rp = types::type_cast<types::TemplateParamType>(rhs))
                return bind(rp, lhs);

            if (lhs && rhs && (lhs->kind == types::TypeKind::Slice || rhs->kind == types::TypeKind::Slice) &&
                (lhs->kind == types::TypeKind::Array || rhs->kind == types::TypeKind::Array || lhs->kind == types::TypeKind::RuntimeArray ||
                 rhs->kind == types::TypeKind::RuntimeArray))
            {
                auto const* slice =
                    (lhs->kind == types::TypeKind::Slice) ? static_cast<types::SliceType const*>(lhs) : static_cast<types::SliceType const*>(rhs);

                types::TypePtr array_elem = nullptr;
                if (lhs->kind == types::TypeKind::Array)
                    array_elem = static_cast<types::ArrayType const*>(lhs)->element;
                else if (lhs->kind == types::TypeKind::RuntimeArray)
                    array_elem = static_cast<types::RuntimeArrayType const*>(lhs)->element;
                else if (rhs->kind == types::TypeKind::Array)
                    array_elem = static_cast<types::ArrayType const*>(rhs)->element;
                else if (rhs->kind == types::TypeKind::RuntimeArray)
                    array_elem = static_cast<types::RuntimeArrayType const*>(rhs)->element;

                if (slice->element == array_elem)
                    return ok();
            }

            if (!lhs || !rhs || lhs->kind != rhs->kind)
                return fail(DeductionError::Conflict, "type kind mismatch");

            switch (lhs->kind)
            {
                case types::TypeKind::Void:
                case types::TypeKind::Bool:
                case types::TypeKind::Char:
                case types::TypeKind::NullT:
                case types::TypeKind::TemplateParam:
                case types::TypeKind::TypePack:
                case types::TypeKind::Error:
                    return ok();

                case types::TypeKind::Int: {
                    auto const* a = static_cast<types::IntType const*>(lhs);
                    auto const* b = static_cast<types::IntType const*>(rhs);
                    return (a->bits == b->bits && a->is_signed == b->is_signed && a->is_pointer_sized == b->is_pointer_sized)
                               ? ok()
                               : fail(DeductionError::Conflict, "integer type mismatch");
                }

                case types::TypeKind::Float: {
                    auto const* a = static_cast<types::FloatType const*>(lhs);
                    auto const* b = static_cast<types::FloatType const*>(rhs);
                    return (a->bits == b->bits) ? ok() : fail(DeductionError::Conflict, "float type mismatch");
                }

                case types::TypeKind::Pointer: {
                    auto const* a = static_cast<types::PointerType const*>(lhs);
                    auto const* b = static_cast<types::PointerType const*>(rhs);
                    if ((std::to_underlying(a->pointee_quals) & std::to_underlying(b->pointee_quals)) !=
                        std::to_underlying(a->pointee_quals))
                        return fail(DeductionError::Conflict, "pointer pointee qualifier mismatch");
                    return deduce(a->pointee, b->pointee);
                }

                case types::TypeKind::Array: {
                    auto const* a = static_cast<types::ArrayType const*>(lhs);
                    auto const* b = static_cast<types::ArrayType const*>(rhs);
                    if (a->count != b->count)
                        return fail(DeductionError::ArityMismatch, "array bound mismatch");
                    return deduce(a->element, b->element);
                }

                case types::TypeKind::RuntimeArray: {
                    auto const* a = static_cast<types::RuntimeArrayType const*>(lhs);
                    auto const* b = static_cast<types::RuntimeArrayType const*>(rhs);
                    return deduce(a->element, b->element);
                }

                case types::TypeKind::Slice: {
                    auto const* a = static_cast<types::SliceType const*>(lhs);
                    auto const* b = static_cast<types::SliceType const*>(rhs);
                    if (a->element_quals != b->element_quals)
                        return fail(DeductionError::Conflict, "slice qualifier mismatch");
                    return deduce(a->element, b->element);
                }

                case types::TypeKind::Range: {
                    auto const* a = static_cast<types::RangeType const*>(lhs);
                    auto const* b = static_cast<types::RangeType const*>(rhs);
                    return deduce(a->element, b->element);
                }

                case types::TypeKind::RangeInclusive: {
                    auto const* a = static_cast<types::RangeInclusiveType const*>(lhs);
                    auto const* b = static_cast<types::RangeInclusiveType const*>(rhs);
                    return deduce(a->element, b->element);
                }

                case types::TypeKind::Fam: {
                    auto const* a = static_cast<types::FamType const*>(lhs);
                    auto const* b = static_cast<types::FamType const*>(rhs);
                    return deduce(a->element, b->element);
                }

                case types::TypeKind::FuncPtr: {
                    auto const* a = static_cast<types::FuncPtrType const*>(lhs);
                    auto const* b = static_cast<types::FuncPtrType const*>(rhs);
                    if (a->params.size() != b->params.size())
                        return fail(DeductionError::ArityMismatch, "function parameter count mismatch");

                    if (auto r = deduce(a->return_type, b->return_type); !r)
                        return r;

                    for (std::size_t i = 0; i < a->params.size(); ++i)
                        if (auto r = deduce(a->params[i], b->params[i]); !r)
                            return r;

                    return ok();
                }

                case types::TypeKind::Nominal: {
                    auto const* a = static_cast<types::NominalType const*>(lhs);
                    auto const* b = static_cast<types::NominalType const*>(rhs);
                    if (a->decl != b->decl)
                        return fail(DeductionError::KindMismatch, "nominal alias type mismatch");
                    return deduce(a->underlying, b->underlying);
                }
                case types::TypeKind::Struct:
                case types::TypeKind::Union:
                case types::TypeKind::Enum: {
                    auto const* a = static_cast<types::UserType const*>(lhs);
                    auto const* b = static_cast<types::UserType const*>(rhs);
                    if (a->kind != b->kind || a->decl != b->decl)
                        return fail(DeductionError::KindMismatch, "nominal type mismatch");

                    if (a->template_args.size() != b->template_args.size())
                        return fail(DeductionError::ArityMismatch, "nominal template argument count mismatch");

                    for (std::size_t i = 0; i < a->template_args.size(); ++i)
                        if (auto r = deduce(a->template_args[i], b->template_args[i]); !r)
                            return r;

                    if (lhs->kind == types::TypeKind::Enum)
                    {
                        auto const* ae = static_cast<types::EnumType const*>(lhs);
                        auto const* be = static_cast<types::EnumType const*>(rhs);
                        if (ae->backing && be->backing)
                            return deduce(ae->backing, be->backing);

                        if (ae->backing || be->backing)
                            return fail(DeductionError::KindMismatch, "enum backing type mismatch");
                    }

                    return ok();
                }
            }

            return fail(DeductionError::KindMismatch, "unsupported type form");
        }

        [[nodiscard]] types::TypePtr substitute_impl(types::TypePtr type, std::unordered_map<types::TypePtr, types::TypePtr>& memo) const
        {
            if (!type)
                return nullptr;

            if (auto it = memo.find(type); it != memo.end())
                return it->second;

            auto resolved = resolve(type);
            if (resolved != type)
            {
                auto out = substitute_impl(resolved, memo);
                memo.emplace(type, out);
                return out;
            }

            types::TypePtr out = type;

            switch (type->kind)
            {
                case types::TypeKind::Void:
                case types::TypeKind::Bool:
                case types::TypeKind::Int:
                case types::TypeKind::Float:
                case types::TypeKind::Char:
                case types::TypeKind::NullT:
                case types::TypeKind::TemplateParam:
                case types::TypeKind::Error:
                    break;

                case types::TypeKind::TypePack: {
                    auto const* t = static_cast<types::TypePackType const*>(type);
                    if (auto const* tp = types::type_cast<types::TemplateParamType>(t->element))
                    {
                        auto const* pack = lookup_pack(tp);
                        if (pack && t->pack_index < pack->size())
                        {
                            auto result = (*pack)[t->pack_index];
                            memo.emplace(type, result);
                            return result;
                        }
                    }
                    auto element = substitute_impl(t->element, memo);
                    if (auto const* tp2 = types::type_cast<types::TemplateParamType>(element))
                    {
                        auto const* pack = lookup_pack(tp2);
                        if (pack && t->pack_index < pack->size())
                        {
                            auto result = (*pack)[t->pack_index];
                            memo.emplace(type, result);
                            return result;
                        }
                    }
                    if (element != t->element)
                        out = m_types.type_pack_t(element, t->pack_index);
                    break;
                }

                case types::TypeKind::Pointer: {
                    auto const* t = static_cast<types::PointerType const*>(type);
                    auto pointee = substitute_impl(t->pointee, memo);
                    if (pointee != t->pointee)
                        out = m_types.pointer_to(pointee, t->pointee_quals);

                    break;
                }

                case types::TypeKind::Array: {
                    auto const* t = static_cast<types::ArrayType const*>(type);
                    auto element = substitute_impl(t->element, memo);
                    if (element != t->element)
                        out = m_types.array_t(element, t->count);

                    break;
                }

                case types::TypeKind::RuntimeArray: {
                    auto const* t = static_cast<types::RuntimeArrayType const*>(type);
                    auto element = substitute_impl(t->element, memo);
                    if (element != t->element)
                        out = m_types.runtime_array_t(element);

                    break;
                }

                case types::TypeKind::Slice: {
                    auto const* t = static_cast<types::SliceType const*>(type);
                    auto element = substitute_impl(t->element, memo);
                    if (element != t->element)
                        out = m_types.slice_t(element, t->element_quals);

                    break;
                }

                case types::TypeKind::Range: {
                    auto const* t = static_cast<types::RangeType const*>(type);
                    auto element = substitute_impl(t->element, memo);
                    if (element != t->element)
                        out = m_types.range_t(element);

                    break;
                }

                case types::TypeKind::RangeInclusive: {
                    auto const* t = static_cast<types::RangeInclusiveType const*>(type);
                    auto element = substitute_impl(t->element, memo);
                    if (element != t->element)
                        out = m_types.range_inclusive_t(element);

                    break;
                }

                case types::TypeKind::Fam: {
                    auto const* t = static_cast<types::FamType const*>(type);
                    auto element = substitute_impl(t->element, memo);
                    if (element != t->element)
                        out = m_types.fam_t(element);

                    break;
                }

                case types::TypeKind::FuncPtr: {
                    auto const* t = static_cast<types::FuncPtrType const*>(type);
                    auto ret = substitute_impl(t->return_type, memo);
                    std::vector<types::TypePtr> params;
                    params.reserve(t->params.size());
                    bool changed = ret != t->return_type;
                    for (auto const* p : t->params)
                    {
                        auto sub = substitute_impl(p, memo);
                        changed |= sub != p;
                        params.push_back(sub);
                    }

                    if (changed)
                        out = m_types.funcptr_t(ret, params);

                    break;
                }

                case types::TypeKind::Nominal: {
                    auto const* t = static_cast<types::NominalType const*>(type);
                    auto underlying = substitute_impl(t->underlying, memo);
                    if (underlying != t->underlying)
                        out = m_types.nominal_alias_t(underlying, t->decl);
                    break;
                }
                case types::TypeKind::Struct:
                case types::TypeKind::Union:
                case types::TypeKind::Enum: {
                    auto const* t = static_cast<types::UserType const*>(type);
                    std::vector<types::TypePtr> args;
                    args.reserve(t->template_args.size());
                    bool changed = false;
                    for (auto const* arg : t->template_args)
                    {
                        auto sub = substitute_impl(arg, memo);
                        changed |= sub != arg;
                        args.push_back(sub);
                    }

                    if (type->kind == types::TypeKind::Enum)
                    {
                        auto const* e = static_cast<types::EnumType const*>(type);
                        auto backing = substitute_impl(e->backing, memo);
                        changed |= backing != e->backing;
                        if (changed)
                        {
                            auto* out_enum = static_cast<types::EnumType const*>(m_types.nominal_t(type->kind, t->decl, args));
                            const_cast<types::EnumType*>(out_enum)->backing = backing;
                            out = out_enum;
                        }
                    }
                    else if (changed)
                        out = m_types.nominal_t(type->kind, t->decl, args);

                    break;
                }
            }

            memo.emplace(type, out);
            return out;
        }
    };

} // namespace dcc::infer
