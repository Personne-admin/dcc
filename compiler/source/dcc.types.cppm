export module dcc.types;

import std;
import dcc.target;

namespace dcc::ast
{
    struct Decl;
    struct TemplateParam;

} // namespace dcc::ast

export namespace dcc::types
{
    enum class TypeKind : std::uint8_t
    {
        Void,
        Bool,
        Int,
        Float,
        Char,
        NullT,
        Pointer,
        Array,
        Slice,
        Fam,
        FuncPtr,
        Lambda,
        Struct,
        Union,
        Enum,
        TemplateParam,
        Range,
        RangeInclusive,
        RuntimeArray,
        TypePack,
        Nominal,
        Error,
    };

    enum class Qual : std::uint8_t
    {
        None = 0,
        Const = 1 << 0,
        Volatile = 1 << 1,
        Restrict = 1 << 2,
    };

    [[nodiscard]] constexpr Qual operator|(Qual a, Qual b) noexcept
    {
        return static_cast<Qual>(std::to_underlying(a) | std::to_underlying(b));
    }

    [[nodiscard]] constexpr bool has_qual(Qual a, Qual b) noexcept
    {
        return (std::to_underlying(a) & std::to_underlying(b)) != 0;
    }

    struct Type
    {
        TypeKind kind;
        std::uint64_t byte_size{};
        std::uint32_t byte_align{};
        bool is_complete : 1 {true};
        bool is_zero_sized : 1 {};
        bool layout_is_default : 1 {true};

    protected:
        Type(TypeKind k) : kind(k) {}
    };

    using TypePtr = Type const*;

    struct VoidType : Type
    {
        static constexpr auto Kind = TypeKind::Void;

        VoidType() : Type(Kind) { is_zero_sized = true; }
    };

    struct BoolType : Type
    {
        static constexpr auto Kind = TypeKind::Bool;

        BoolType() : Type(Kind)
        {
            byte_size = 1;
            byte_align = 1;
        }
    };

    struct IntType : Type
    {
        static constexpr auto Kind = TypeKind::Int;

        std::uint8_t bits;
        bool is_signed;
        bool is_pointer_sized;

        IntType(std::uint8_t b, bool s, bool ps = false) : Type(Kind), bits(b), is_signed(s), is_pointer_sized(ps)
        {
            byte_size = b / 8;
            byte_align = std::uint32_t(byte_size);
        }
    };

    struct FloatType : Type
    {
        static constexpr auto Kind = TypeKind::Float;

        std::uint8_t bits;

        FloatType(std::uint8_t b) : Type(Kind), bits(b)
        {
            byte_size = b / 8;
            byte_align = std::uint32_t(byte_size);
        }
    };

    struct CharType : Type
    {
        static constexpr auto Kind = TypeKind::Char;

        CharType() : Type(Kind)
        {
            byte_size = 1;
            byte_align = 1;
        }
    };

    struct NullTType : Type
    {
        static constexpr auto Kind = TypeKind::NullT;

        NullTType(std::uint8_t pb = 64, std::uint8_t pa = 8) : Type(Kind)
        {
            byte_size = pb / 8;
            byte_align = pa;
        }
    };

    struct PointerType : Type
    {
        static constexpr auto Kind = TypeKind::Pointer;

        TypePtr pointee;
        Qual pointee_quals;

        PointerType(TypePtr p, Qual q, std::uint8_t pb = 64, std::uint8_t pa = 8) : Type(Kind), pointee(p), pointee_quals(q)
        {
            byte_size = pb / 8;
            byte_align = pa;
        }
    };

    struct ArrayType : Type
    {
        static constexpr auto Kind = TypeKind::Array;

        TypePtr element;
        std::uint64_t count;

        ArrayType(TypePtr e, std::uint64_t c) : Type(Kind), element(e), count(c) {}
    };

    struct SliceType : Type
    {
        static constexpr auto Kind = TypeKind::Slice;

        TypePtr element;
        Qual element_quals;

        SliceType(TypePtr e, Qual q, std::uint8_t pointer_bits = 64, std::uint8_t pointer_align = 8) : Type(Kind), element(e), element_quals(q)
        {
            byte_size = 2 * (static_cast<std::uint64_t>(pointer_bits) / 8);
            byte_align = pointer_align;
        }
    };

    struct RangeType : Type
    {
        static constexpr auto Kind = TypeKind::Range;

        TypePtr element;

        RangeType(TypePtr e, std::uint8_t pointer_bits = 64, std::uint8_t pointer_align = 8) : Type(Kind), element(e)
        {
            byte_size = 2 * (static_cast<std::uint64_t>(pointer_bits) / 8);
            byte_align = pointer_align;
        }
    };

    struct RangeInclusiveType : Type
    {
        static constexpr auto Kind = TypeKind::RangeInclusive;

        TypePtr element;

        RangeInclusiveType(TypePtr e, std::uint8_t pointer_bits = 64, std::uint8_t pointer_align = 8) : Type(Kind), element(e)
        {
            byte_size = 2 * (static_cast<std::uint64_t>(pointer_bits) / 8);
            byte_align = pointer_align;
        }
    };

    struct FamType : Type
    {
        static constexpr auto Kind = TypeKind::Fam;

        TypePtr element;

        FamType(TypePtr e) : Type(Kind), element(e) { is_complete = false; }
    };

    struct RuntimeArrayType : Type
    {
        static constexpr auto Kind = TypeKind::RuntimeArray;

        TypePtr element;

        RuntimeArrayType(TypePtr e) : Type(Kind), element(e)
        {
            is_complete = false;
            is_zero_sized = true;
        }
    };

    struct TypePackType : Type
    {
        static constexpr auto Kind = TypeKind::TypePack;

        TypePtr element;
        std::uint32_t pack_index{};

        TypePackType(TypePtr e) : Type(Kind), element(e)
        {
            is_complete = false;
            is_zero_sized = true;
        }
    };

    struct NominalType : Type
    {
        static constexpr auto Kind = TypeKind::Nominal;

        TypePtr underlying;
        void const* decl;

        NominalType(TypePtr u, void const* d) : Type(Kind), underlying(u), decl(d)
        {
            byte_size = u->byte_size;
            byte_align = u->byte_align;
            is_complete = u->is_complete;
            is_zero_sized = u->is_zero_sized;
            layout_is_default = u->layout_is_default;
        }
    };

    struct FuncPtrType : Type
    {
        static constexpr auto Kind = TypeKind::FuncPtr;

        TypePtr return_type;
        std::pmr::vector<TypePtr> params;

        FuncPtrType(TypePtr r, std::pmr::polymorphic_allocator<> a, std::uint8_t pb = 64, std::uint8_t pa = 8) : Type(Kind), return_type(r), params(a)
        {
            byte_size = pb / 8;
            byte_align = pa;
        }
    };

    struct LambdaType : Type
    {
        static constexpr auto Kind = TypeKind::Lambda;

        void const* expr;

        LambdaType(void const* e, std::uint8_t pb = 64, std::uint8_t pa = 8) : Type(Kind), expr(e)
        {
            byte_size = pb / 8;
            byte_align = pa;
        }
    };

    struct UserType : Type
    {
        ast::Decl const* decl;
        std::pmr::vector<TypePtr> template_args;

        UserType(TypeKind k, void const* d, std::pmr::polymorphic_allocator<> a) : Type(k), decl(static_cast<ast::Decl const*>(d)), template_args(a) {}
    };

    struct StructType : UserType
    {
        static constexpr auto Kind = TypeKind::Struct;

        StructType(void const* d, std::pmr::polymorphic_allocator<> a) : UserType(Kind, d, a) { is_complete = false; }

        bool has_fam : 1 {};
    };

    struct UnionType : UserType
    {
        static constexpr auto Kind = TypeKind::Union;

        UnionType(void const* d, std::pmr::polymorphic_allocator<> a) : UserType(Kind, d, a) { is_complete = false; }
    };

    struct TaggedEnumVariantLayout
    {
        std::string_view variant_name;
        TypePtr variant_payload_type_or_null{};
        std::int64_t discriminant_value{};
    };

    struct TaggedEnumLayout
    {
        std::uint64_t discriminant_offset{};
        std::uint64_t discriminant_size{};
        IntType const* discriminant_type{};
        std::uint64_t payload_offset{};
        std::uint64_t payload_size{};
        std::uint64_t total_size{};
        std::uint32_t total_align{};
        TaggedEnumVariantLayout const* variants{};
        std::size_t variant_count{};
    };

    struct EnumType : UserType
    {
        static constexpr auto Kind = TypeKind::Enum;

        TypePtr backing{};
        bool is_tagged{};
        TaggedEnumLayout* tagged_layout{};

        EnumType(void const* d, std::pmr::polymorphic_allocator<> a) : UserType(Kind, d, a) { is_complete = false; }
    };

    struct TemplateParamType : Type
    {
        static constexpr auto Kind = TypeKind::TemplateParam;

        void* param{};
        std::string_view name;
        std::uint32_t index;

        TemplateParamType(void* p, std::string_view n, std::uint32_t i) : Type(Kind), param(p), name(n), index(i) { is_complete = false; }
    };

    struct ErrorType : Type
    {
        static constexpr auto Kind = TypeKind::Error;

        ErrorType() : Type(Kind) { is_complete = false; }
    };

    template <typename To, typename From> [[nodiscard]] To const* type_cast(From const* t) noexcept
    {
        return (t && t->kind == To::Kind) ? static_cast<To const*>(t) : nullptr;
    }

    [[nodiscard]] bool is_fam_type(TypePtr t) noexcept
    {
        return t && t->kind == TypeKind::Fam;
    }

    [[nodiscard]] TypePtr fam_element(TypePtr t) noexcept
    {
        if (auto const* ft = type_cast<FamType>(t))
            return ft->element;

        return nullptr;
    }

    [[nodiscard]] bool type_has_fam_struct(TypePtr t) noexcept
    {
        if (!t)
            return false;

        if (auto const* st = type_cast<StructType>(t))
            return st->has_fam;

        if (auto const* at = type_cast<ArrayType>(t))
            return type_has_fam_struct(at->element);

        if (auto const* rt = type_cast<RuntimeArrayType>(t))
            return type_has_fam_struct(rt->element);

        return false;
    }

    class TypeContext
    {
    public:
        explicit TypeContext(std::size_t initial = 32 * 1024, target::TargetConfig const* target = nullptr) : m_buffer(initial), m_arena(&m_buffer)
        {
            if (target)
            {
                m_pointer_bits = target->pointer_bits;
                m_pointer_align = target->pointer_align;
            }
        }

        [[nodiscard]] std::uint8_t pointer_bits() const noexcept { return m_pointer_bits; }
        [[nodiscard]] std::uint8_t pointer_align() const noexcept { return m_pointer_align; }

        TypeContext(TypeContext const&) = delete;
        TypeContext& operator=(TypeContext const&) = delete;

        [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() noexcept { return m_arena; }

        [[nodiscard]] TypePtr m_voidt()
        {
            return ensure_singleton(m_void, [&] { return make<VoidType>(); });
        }

        [[nodiscard]] TypePtr m_boolt()
        {
            return ensure_singleton(m_bool, [&] { return make<BoolType>(); });
        }

        [[nodiscard]] TypePtr m_chart()
        {
            return ensure_singleton(m_char, [&] { return make<CharType>(); });
        }

        [[nodiscard]] TypePtr m_nullt()
        {
            return ensure_singleton(m_null, [&] { return make<NullTType>(m_pointer_bits, m_pointer_align); });
        }

        [[nodiscard]] TypePtr m_errort()
        {
            return ensure_singleton(m_error, [&] { return make<ErrorType>(); });
        }

        [[nodiscard]] TypePtr int_t(std::uint8_t bits, bool is_signed, bool is_pointer_sized = false)
        {
            for (auto const* t : m_ints)
                if (t->bits == bits && t->is_signed == is_signed && t->is_pointer_sized == is_pointer_sized)
                    return t;

            auto* t = make<IntType>(bits, is_signed, is_pointer_sized);
            m_ints.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr usize_t() { return int_t(m_pointer_bits, false, true); }
        [[nodiscard]] TypePtr isize_t() { return int_t(m_pointer_bits, true, true); }

        [[nodiscard]] TypePtr float_t(std::uint8_t bits)
        {
            for (auto const* t : m_floats)
                if (t->bits == bits)
                    return t;

            auto* t = make<FloatType>(bits);
            m_floats.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr pointer_to(TypePtr pointee, Qual quals)
        {
            for (auto const* t : m_pointers)
                if (t->pointee == pointee && t->pointee_quals == quals)
                    return t;

            auto* t = make<PointerType>(pointee, quals, m_pointer_bits, m_pointer_align);
            m_pointers.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr array_t(TypePtr element, std::uint64_t count)
        {
            for (auto const* t : m_arrays)
                if (t->element == element && t->count == count)
                    return t;

            auto* t = make<ArrayType>(element, count);
            if (element && element->is_complete)
            {
                t->byte_align = element->byte_align;
                t->byte_size = element->byte_size * count;
                t->is_zero_sized = (t->byte_size == 0);
            }
            else
                t->is_complete = false;

            m_arrays.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr slice_t(TypePtr element, Qual quals)
        {
            for (auto const* t : m_slices)
                if (t->element == element && t->element_quals == quals)
                    return t;

            auto* t = make<SliceType>(element, quals, m_pointer_bits, m_pointer_align);
            m_slices.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr range_t(TypePtr element)
        {
            for (auto const* t : m_ranges)
                if (t->element == element)
                    return t;

            auto* t = make<RangeType>(element, m_pointer_bits, m_pointer_align);
            m_ranges.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr range_inclusive_t(TypePtr element)
        {
            for (auto const* t : m_range_inclusives)
                if (t->element == element)
                    return t;

            auto* t = make<RangeInclusiveType>(element, m_pointer_bits, m_pointer_align);
            m_range_inclusives.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr fam_t(TypePtr element)
        {
            for (auto const* t : m_fams)
                if (t->element == element)
                    return t;

            auto* t = make<FamType>(element);
            m_fams.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr runtime_array_t(TypePtr element)
        {
            for (auto const* t : m_runtime_arrays)
                if (t->element == element)
                    return t;

            auto* t = make<RuntimeArrayType>(element);
            m_runtime_arrays.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr type_pack_t(TypePtr element, std::uint32_t pack_index = 0)
        {
            for (auto const* t : m_type_packs)
                if (t->element == element && t->pack_index == pack_index)
                    return t;

            auto* t = make<TypePackType>(element);
            t->pack_index = pack_index;
            m_type_packs.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr funcptr_t(TypePtr ret, std::span<TypePtr const> params)
        {
            for (auto const* t : m_funcptrs)
                if (t->return_type == ret && same_span(t->params, params))
                    return t;

            auto* t = make<FuncPtrType>(ret, m_arena, m_pointer_bits, m_pointer_align);
            t->params.assign(params.begin(), params.end());
            m_funcptrs.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr lambda_t(void const* expr)
        {
            for (auto const* t : m_lambdas)
                if (t->expr == expr)
                    return t;

            auto* t = make<LambdaType>(expr, m_pointer_bits, m_pointer_align);
            m_lambdas.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr nominal_t(TypeKind kind, void const* decl, std::span<TypePtr const> args = {})
        {
            auto matches = [&](UserType const* u) { return u->kind == kind && u->decl == decl && same_span(u->template_args, args); };

            switch (kind)
            {
                case TypeKind::Struct:
                    for (auto const* t : m_structs)
                        if (matches(t))
                            return t;
                    {
                        auto* t = make<StructType>(decl, m_arena);
                        t->template_args.assign(args.begin(), args.end());
                        m_structs.push_back(t);
                        return t;
                    }
                case TypeKind::Union:
                    for (auto const* t : m_unions)
                        if (matches(t))
                            return t;
                    {
                        auto* t = make<UnionType>(decl, m_arena);
                        t->template_args.assign(args.begin(), args.end());
                        m_unions.push_back(t);
                        return t;
                    }
                case TypeKind::Enum:
                    for (auto const* t : m_enums)
                        if (matches(t))
                            return t;
                    {
                        auto* t = make<EnumType>(decl, m_arena);
                        t->template_args.assign(args.begin(), args.end());
                        m_enums.push_back(t);
                        return t;
                    }
                default:
                    return m_errort();
            }
        }

        [[nodiscard]] TypePtr nominal_alias_t(TypePtr underlying, void const* decl)
        {
            for (auto const* t : m_nominal_aliases)
                if (t->underlying == underlying && t->decl == decl)
                    return t;

            auto* t = make<NominalType>(underlying, decl);
            m_nominal_aliases.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr template_param_t(void* param, std::string_view name, std::uint32_t index)
        {
            for (auto const* t : m_template_params)
                if (t->param == param)
                    return t;

            auto* t = make<TemplateParamType>(param, name, index);
            m_template_params.push_back(t);
            return t;
        }

        [[nodiscard]] std::vector<EnumType const*> const& enums() const noexcept { return m_enums; }

        template <typename T, typename... Args> [[nodiscard]] T* make(Args&&... args)
        {
            void* p = m_buffer.allocate(sizeof(T), alignof(T));
            if constexpr (std::is_constructible_v<T, Args..., std::pmr::polymorphic_allocator<>>)
                return ::new (p) T(std::forward<Args>(args)..., m_arena);
            else
                return ::new (p) T(std::forward<Args>(args)...);
        }

    private:
        std::pmr::monotonic_buffer_resource m_buffer;
        std::pmr::polymorphic_allocator<> m_arena;

        VoidType const* m_void{};
        BoolType const* m_bool{};
        CharType const* m_char{};
        NullTType const* m_null{};
        ErrorType const* m_error{};

        std::uint8_t m_pointer_bits{64};
        std::uint8_t m_pointer_align{8};

        std::vector<IntType const*> m_ints;
        std::vector<FloatType const*> m_floats;
        std::vector<PointerType const*> m_pointers;
        std::vector<ArrayType const*> m_arrays;
        std::vector<SliceType const*> m_slices;
        std::vector<RangeType const*> m_ranges;
        std::vector<RangeInclusiveType const*> m_range_inclusives;
        std::vector<FamType const*> m_fams;
        std::vector<RuntimeArrayType const*> m_runtime_arrays;
        std::vector<TypePackType const*> m_type_packs;
        std::vector<FuncPtrType const*> m_funcptrs;
        std::vector<LambdaType const*> m_lambdas;
        std::vector<StructType const*> m_structs;
        std::vector<UnionType const*> m_unions;
        std::vector<EnumType const*> m_enums;
        std::vector<TemplateParamType const*> m_template_params;
        std::vector<NominalType const*> m_nominal_aliases;

        template <typename T, typename F> TypePtr ensure_singleton(T const*& slot, F&& factory)
        {
            if (!slot)
                slot = factory();

            return slot;
        }

        template <typename A, typename B> static bool same_span(A const& a, B const& b) noexcept
        {
            if (a.size() != b.size())
                return false;

            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i] != b[i])
                    return false;

            return true;
        }
    };

} // namespace dcc::types

export namespace dcc::int_domain
{
    struct Domain
    {
        std::uint64_t lo;
        std::uint64_t hi;
    };

    [[nodiscard]] constexpr std::uint64_t mask_for_bits(std::uint8_t bits) noexcept
    {
        if (bits >= 64)
            return ~std::uint64_t{};

        return (std::uint64_t{1} << bits) - 1;
    }

    [[nodiscard]] constexpr Domain domain_for(types::IntType const& ty) noexcept
    {
        return {0, mask_for_bits(ty.bits)};
    }

    [[nodiscard]] constexpr std::uint64_t to_ordinal(std::int64_t raw_value, types::IntType const& ty) noexcept
    {
        auto raw = static_cast<std::uint64_t>(raw_value) & mask_for_bits(ty.bits);
        if (ty.is_signed)
            raw ^= std::uint64_t{1} << (ty.bits - 1);

        return raw;
    }

    [[nodiscard]] constexpr std::int64_t ordinal_to_raw_bits(std::uint64_t ordinal, types::IntType const& ty) noexcept
    {
        auto mask = mask_for_bits(ty.bits);
        ordinal &= mask;
        if (!ty.is_signed)
            return static_cast<std::int64_t>(ordinal);

        auto sign_bit = std::uint64_t{1} << (ty.bits - 1);
        auto val = (ordinal ^ sign_bit) & mask;
        if (val & sign_bit)
            val |= ~mask;

        return static_cast<std::int64_t>(val);
    }

} // namespace dcc::int_domain
