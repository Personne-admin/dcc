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
        Restricted,
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

    struct RestrictionInterval
    {
        std::uint64_t lo{};
        std::uint64_t hi{};

        friend bool operator==(RestrictionInterval const&, RestrictionInterval const&) = default;
    };

    struct RestrictedType : Type
    {
        static constexpr auto Kind = TypeKind::Restricted;

        IntType const* underlying;
        std::pmr::vector<RestrictionInterval> intervals;

        RestrictedType(IntType const* u, std::span<RestrictionInterval const> rs, std::pmr::polymorphic_allocator<> alloc)
            : Type(Kind), underlying(u), intervals(rs.begin(), rs.end(), alloc)
        {
            byte_size = u->byte_size;
            byte_align = u->byte_align;
            is_complete = u->is_complete;
            is_zero_sized = u->is_zero_sized;
            layout_is_default = u->layout_is_default;
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

    enum class SegReg : std::uint8_t
    {
        None,
        CS,
        DS,
        ES,
        SS,
        FS,
        GS,
    };

    enum class PointerFlavor : std::uint8_t
    {
        Near,
        Far,
        Based,
    };

    struct PointerType : Type
    {
        static constexpr auto Kind = TypeKind::Pointer;

        TypePtr pointee;
        Qual pointee_quals;
        PointerFlavor flavor = PointerFlavor::Near;
        SegReg segment = SegReg::None;

        PointerType(TypePtr p, Qual q, std::uint8_t pb = 64, std::uint8_t pa = 8, PointerFlavor f = PointerFlavor::Near, SegReg s = SegReg::None)
            : Type(Kind), pointee(p), pointee_quals(q), flavor(f), segment(s)
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
        PointerFlavor flavor = PointerFlavor::Near;
        SegReg segment = SegReg::None;

        SliceType(TypePtr e, Qual q, std::uint8_t pointer_bits = 64, std::uint8_t pointer_align = 8, PointerFlavor f = PointerFlavor::Near,
                  SegReg s = SegReg::None, std::uint8_t length_bits = 0)
            : Type(Kind), element(e), element_quals(q), flavor(f), segment(s)
        {
            if (length_bits == 0)
                length_bits = pointer_bits;
            auto const unaligned = (static_cast<std::uint64_t>(pointer_bits) + length_bits) / 8;
            byte_align = pointer_align;
            byte_size = (unaligned + pointer_align - 1) / pointer_align * pointer_align;
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
        bool is_far = false;

        FuncPtrType(TypePtr r, std::pmr::polymorphic_allocator<> a, std::uint8_t pb = 64, std::uint8_t pa = 8, bool far = false)
            : Type(Kind), return_type(r), params(a), is_far(far)
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

    struct StructExpandedField
    {
        std::string_view name;
        TypePtr type{};
        std::uint32_t byte_offset{};
    };

    struct StructType : UserType
    {
        static constexpr auto Kind = TypeKind::Struct;

        StructType(void const* d, std::pmr::polymorphic_allocator<> a) : UserType(Kind, d, a) { is_complete = false; }

        bool has_fam : 1 {};
        bool is_specialization : 1 {};
        bool has_expansion : 1 {};
        StructExpandedField const* expanded_fields{};
        std::size_t expanded_field_count{};
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
                m_far_offset_bits = target->far_offset_bits();
                m_i386_segments = target->has_i386_segments();
                m_arch = target->arch;
                m_target_triple = target->triple;
            }
        }

        [[nodiscard]] std::uint8_t pointer_bits() const noexcept { return m_pointer_bits; }
        [[nodiscard]] std::uint8_t far_offset_bits() const noexcept { return m_far_offset_bits; }
        [[nodiscard]] std::uint8_t pointer_align() const noexcept { return m_pointer_align; }
        [[nodiscard]] target::Arch arch() const noexcept { return m_arch; }
        [[nodiscard]] std::string_view target_triple() const noexcept { return m_target_triple; }

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

        [[nodiscard]] TypePtr restricted_t(IntType const* underlying, std::span<RestrictionInterval const> intervals)
        {
            std::vector<RestrictionInterval> normalized(intervals.begin(), intervals.end());
            std::sort(normalized.begin(), normalized.end(), [](auto const& a, auto const& b) { return a.lo < b.lo || (a.lo == b.lo && a.hi < b.hi); });

            std::vector<RestrictionInterval> merged;
            merged.reserve(normalized.size());
            for (auto interval : normalized)
            {
                if (!merged.empty() && interval.lo <= merged.back().hi)
                    merged.back().hi = std::max(merged.back().hi, interval.hi);
                else
                    merged.push_back(interval);
            }

            for (auto const* t : m_restricted)
                if (t->underlying == underlying && same_span(t->intervals, merged))
                    return t;

            auto* t = make<RestrictedType>(underlying, std::span<RestrictionInterval const>{merged}, allocator());
            m_restricted.push_back(t);
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
            return pointer_with_flavor(pointee, quals, PointerFlavor::Near, SegReg::None);
        }

        [[nodiscard]] TypePtr far_pointer_to(TypePtr pointee, Qual quals)
        {
            return pointer_with_flavor(pointee, quals, PointerFlavor::Far, SegReg::None);
        }

        [[nodiscard]] TypePtr based_pointer_to(TypePtr pointee, Qual quals, SegReg seg)
        {
            return pointer_with_flavor(pointee, quals, PointerFlavor::Based, seg);
        }

        [[nodiscard]] TypePtr pointer_with_flavor(TypePtr pointee, Qual quals, PointerFlavor flavor, SegReg seg)
        {
            for (auto const* t : m_pointers)
                if (t->pointee == pointee && t->pointee_quals == quals && t->flavor == flavor && t->segment == seg)
                    return t;

            std::uint8_t bits = m_pointer_bits;
            std::uint8_t align = m_pointer_align;
            if (flavor == PointerFlavor::Based)
            {
                bits = m_far_offset_bits;
                align = bits / 8;
            }
            if (flavor == PointerFlavor::Far)
                far_layout(bits, align);

            auto* t = make<PointerType>(pointee, quals, bits, align, flavor, seg);
            m_pointers.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr rebuild_pointer(PointerType const* p, TypePtr pointee, Qual quals)
        {
            return pointer_with_flavor(pointee, quals, p->flavor, p->segment);
        }

        void far_layout(std::uint8_t& bits, std::uint8_t& align) const noexcept
        {
            if (m_far_offset_bits <= 16)
            {
                bits = 32;
                align = 2;
            }
            else if (m_far_offset_bits <= 32)
            {
                bits = 64;
                align = 4;
            }
            else
            {
                bits = 128;
                align = 8;
            }
        }

        [[nodiscard]] static bool dynamic_far_allowed(target::Arch arch) noexcept
        {
            return arch == target::Arch::I8086 || arch == target::Arch::X86;
        }

        [[nodiscard]] static bool based_register_allowed(target::Arch arch, SegReg seg, bool i386_segments = true) noexcept
        {
            if (seg == SegReg::None)
                return false;
            switch (arch)
            {
                case target::Arch::X86_64:
                    return seg == SegReg::FS || seg == SegReg::GS;
                case target::Arch::I8086:
                    return i386_segments || seg == SegReg::CS || seg == SegReg::DS || seg == SegReg::ES || seg == SegReg::SS;
                case target::Arch::X86:
                    return true;
            }
            return false;
        }

        [[nodiscard]] static SegReg seg_reg_from_name(std::string_view name) noexcept
        {
            if (name == "CS")
                return SegReg::CS;
            if (name == "DS")
                return SegReg::DS;
            if (name == "ES")
                return SegReg::ES;
            if (name == "SS")
                return SegReg::SS;
            if (name == "FS")
                return SegReg::FS;
            if (name == "GS")
                return SegReg::GS;
            return SegReg::None;
        }

        [[nodiscard]] std::optional<std::string> check_far_pointer(bool is_far, SegReg seg, std::string_view noun = "pointers") const
        {
            if (!is_far)
                return std::nullopt;
            if (seg == SegReg::None)
            {
                if (dynamic_far_allowed(m_arch))
                    return std::nullopt;
                return std::format("dynamic far {} are not available on target '{}'", noun, m_target_triple);
            }
            if (based_register_allowed(m_arch, seg, m_i386_segments))
                return std::nullopt;
            if (m_arch == target::Arch::X86_64)
                return std::format("segment register '{}' is not available on target '{}'; x86-64 based {} allow only FS and GS", seg_reg_name(seg),
                                   m_target_triple, noun);
            if (m_arch == target::Arch::I8086)
                return std::format("segment register '{}' is not available on target '{}'; the 8086 has no FS or GS", seg_reg_name(seg),
                                   m_target_triple);
            return std::format("segment register '{}' is not available on target '{}'", seg_reg_name(seg), m_target_triple);
        }

        [[nodiscard]] static std::string_view seg_reg_name(SegReg seg) noexcept
        {
            switch (seg)
            {
                case SegReg::CS:
                    return "CS";
                case SegReg::DS:
                    return "DS";
                case SegReg::ES:
                    return "ES";
                case SegReg::SS:
                    return "SS";
                case SegReg::FS:
                    return "FS";
                case SegReg::GS:
                    return "GS";
                case SegReg::None:
                    return "";
            }
            return "";
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

        [[nodiscard]] TypePtr slice_t(TypePtr element, Qual quals, PointerFlavor flavor = PointerFlavor::Near, SegReg seg = SegReg::None)
        {
            for (auto const* t : m_slices)
                if (t->element == element && t->element_quals == quals && t->flavor == flavor && t->segment == seg)
                    return t;

            std::uint8_t bits = m_pointer_bits;
            std::uint8_t align = m_pointer_align;
            if (flavor == PointerFlavor::Based)
            {
                bits = m_far_offset_bits;
                align = bits / 8;
            }
            if (flavor == PointerFlavor::Far)
                far_layout(bits, align);

            auto* t = make<SliceType>(element, quals, bits, align, flavor, seg, m_pointer_bits);
            m_slices.push_back(t);
            return t;
        }

        [[nodiscard]] TypePtr rebuild_slice(SliceType const* s, TypePtr element, Qual quals)
        {
            return slice_t(element, quals, s->flavor, s->segment);
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

        [[nodiscard]] TypePtr funcptr_t(TypePtr ret, std::span<TypePtr const> params, bool is_far = false)
        {
            for (auto const* t : m_funcptrs)
                if (t->return_type == ret && same_span(t->params, params) && t->is_far == is_far)
                    return t;

            std::uint8_t bits = m_pointer_bits;
            std::uint8_t align = m_pointer_align;
            if (is_far)
                far_layout(bits, align);

            auto* t = make<FuncPtrType>(ret, m_arena, bits, align, is_far);
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

        [[nodiscard]] TypePtr nominal_t(TypeKind kind, void const* decl, std::span<TypePtr const> args = {}, bool is_specialization = false)
        {
            if (kind == TypeKind::Struct && !args.empty())
                is_specialization = true;

            auto matches = [&](UserType const* u) {
                if (u->kind != kind || u->decl != decl || !same_span(u->template_args, args))
                    return false;
                if (kind == TypeKind::Struct && static_cast<StructType const*>(u)->is_specialization != is_specialization)
                    return false;
                return true;
            };

            switch (kind)
            {
                case TypeKind::Struct:
                    for (auto const* t : m_structs)
                        if (matches(t))
                            return t;
                    {
                        auto* t = make<StructType>(decl, m_arena);
                        t->template_args.assign(args.begin(), args.end());
                        t->is_specialization = is_specialization;
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
        [[nodiscard]] std::vector<StructType const*> const& structs() const noexcept { return m_structs; }

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

        std::uint8_t m_far_offset_bits{64};
        bool m_i386_segments{true};
        std::uint8_t m_pointer_bits{64};
        std::uint8_t m_pointer_align{8};
        target::Arch m_arch{target::Arch::X86_64};
        std::string m_target_triple{"x86_64-elf"};

        std::vector<IntType const*> m_ints;
        std::vector<RestrictedType const*> m_restricted;
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

    [[nodiscard]] constexpr bool contains(types::RestrictedType const& rt, std::uint64_t ordinal) noexcept
    {
        for (auto const& interval : rt.intervals)
            if (ordinal >= interval.lo && ordinal <= interval.hi)
                return true;

        return false;
    }

    [[nodiscard]] constexpr bool contains(types::RestrictedType const& rt, std::int64_t raw_value) noexcept
    {
        return contains(rt, to_ordinal(raw_value, *rt.underlying));
    }

    [[nodiscard]] constexpr bool domain_contains(types::RestrictedType const& outer, types::RestrictedType const& inner) noexcept
    {
        if (outer.underlying != inner.underlying)
            return false;

        for (auto const& inner_interval : inner.intervals)
        {
            bool covered = false;
            for (auto const& outer_interval : outer.intervals)
                if (inner_interval.lo >= outer_interval.lo && inner_interval.hi <= outer_interval.hi)
                {
                    covered = true;
                    break;
                }

            if (!covered)
                return false;
        }
        return true;
    }

} // namespace dcc::int_domain
