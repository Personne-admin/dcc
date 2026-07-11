export module dcc.ir;

import std;
import dcc.sm;
import dcc.target;

export namespace dcc::ir
{
    enum class IrTypeKind : std::uint8_t
    {
        Void,
        Bool,
        Int,
        Float,
        Pointer,
        Aggregate,
        Array,
        Slice,
        Func,
    };

    enum class Segment : std::uint8_t
    {
        None,
        Cs,
        Ds,
        Es,
        Fs,
        Gs,
        Ss,
    };

    struct IrType
    {
        IrTypeKind kind;
        std::uint64_t byte_size{};
        std::uint64_t byte_align{};

    protected:
        explicit IrType(IrTypeKind k) : kind(k) {}
    };

    struct IrVoidType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Void;

        IrVoidType() : IrType(Kind)
        {
            byte_size = 0;
            byte_align = 1;
        }
    };

    struct IrBoolType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Bool;

        IrBoolType() : IrType(Kind)
        {
            byte_size = 1;
            byte_align = 1;
        }
    };

    struct IrIntType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Int;

        std::uint8_t bits;
        bool is_signed;
        bool is_pointer_sized;

        IrIntType(std::uint8_t b, bool s, bool ps = false) : IrType(Kind), bits(b), is_signed(s), is_pointer_sized(ps)
        {
            byte_size = b / 8;
            byte_align = byte_size;
        }
    };

    struct IrFloatType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Float;

        std::uint8_t bits;

        explicit IrFloatType(std::uint8_t b) : IrType(Kind), bits(b)
        {
            byte_size = b / 8;
            byte_align = byte_size;
        }
    };

    struct IrPointerType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Pointer;

        IrType const* pointee;
        Segment seg{Segment::None};

        IrPointerType(IrType const* p, Segment s = Segment::None, std::uint8_t pb = 64, std::uint8_t pa = 8) : IrType(Kind), pointee(p), seg(s)
        {
            byte_size = pb / 8;
            byte_align = pa;
        }
    };

    struct IrAggregateType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Aggregate;

        std::pmr::vector<IrType const*> members;
        std::pmr::vector<std::uint64_t> member_offsets;
        bool has_trailing_fam : 1 {};
        std::uint32_t fam_member_index{};

        IrAggregateType(std::pmr::polymorphic_allocator<> a) : IrType(Kind), members(a), member_offsets(a) {}
    };

    struct IrArrayType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Array;

        IrType const* element;
        std::uint64_t count;

        IrArrayType(IrType const* el, std::uint64_t c) : IrType(Kind), element(el), count(c)
        {
            byte_size = (el ? el->byte_size : 0) * c;
            byte_align = (el ? el->byte_align : 1);
        }
    };

    struct IrSliceType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Slice;

        IrType const* element;
        Segment seg{Segment::None};

        IrSliceType(IrType const* el, Segment s = Segment::None, std::uint8_t pointer_bits = 64, std::uint8_t pointer_align = 8)
            : IrType(Kind), element(el), seg(s)
        {
            byte_size = 2 * (static_cast<std::uint64_t>(pointer_bits) / 8);
            byte_align = pointer_align;
        }
    };

    struct IrFuncType : IrType
    {
        static constexpr auto Kind = IrTypeKind::Func;

        IrType const* return_type;
        std::pmr::vector<IrType const*> params;

        IrFuncType(IrType const* ret, std::pmr::polymorphic_allocator<> a) : IrType(Kind), return_type(ret), params(a)
        {
            byte_size = 0;
            byte_align = 1;
        }
    };

    template <typename To, typename From> [[nodiscard]] To const* ir_type_cast(From const* t) noexcept
    {
        return (t && t->kind == To::Kind) ? static_cast<To const*>(t) : nullptr;
    }

    enum class IrMemoryOrdering : std::uint8_t
    {
        Relaxed,
        Acquire,
        Release,
        AcqRel,
        SeqCst,
    };

    enum class IrAtomicRmwOp : std::uint8_t
    {
        Xchg,
        Add,
        Sub,
        And,
        Or,
        Xor,
    };

    enum class IrNodeKind : std::uint8_t
    {
        IntConstant,
        FloatConstant,
        BoolConstant,
        NullConstant,
        StringConstant,

        Local,
        GlobalRef,

        Add,
        Sub,
        Mul,
        UDiv,
        SDiv,
        URem,
        SRem,
        FDiv,
        FRem,
        And,
        Or,
        Xor,
        Shl,
        LShr,
        AShr,
        Neg,
        Not,

        CmpEq,
        CmpNe,
        CmpLt,
        CmpLe,
        CmpGt,
        CmpGe,
        CmpOLt,
        CmpOLe,
        CmpOGt,
        CmpOGe,
        CmpULt,
        CmpULe,
        CmpUGt,
        CmpUGe,

        Alloca,
        Load,
        LoadVolatile,
        Store,
        StoreVolatile,
        Gep,

        Zext,
        Sext,
        Trunc,
        FpExt,
        FpTrunc,
        FpToI,
        IToFp,
        PtrToI,
        IToPtr,
        Bitcast,
        Segcast,

        Extract,
        Insert,
        Aggregate,

        Br,
        BrCond,
        Ret,
        Unreachable,
        Switch,

        Phi,

        Call,
        CallTail,

        AtomicLoad,
        AtomicStore,
        AtomicRmw,
        Fence,

        BasicBlock,
        Function,
        Global,
    };

    struct IrBasicBlock;
    struct IrFunction;
    struct IrGlobal;

    struct SourceLoc
    {
        std::uint32_t file_id;
        std::uint32_t line;
        std::uint32_t column;
        std::uint32_t scope_id;
    };

    struct IrDebugLocation
    {
        std::uint32_t block_id;
        std::uint32_t instruction_index;
        bool is_terminator;
        SourceLoc loc;
    };

    struct IrNode
    {
        IrNodeKind kind;
        sm::SourceRange range;

    protected:
        explicit IrNode(IrNodeKind k, sm::SourceRange r) : kind(k), range(r) {}
        explicit IrNode(IrNodeKind k) : kind(k) {}
    };

    struct IrValue : IrNode
    {
        IrType const* type{};
        std::string_view name;

    protected:
        explicit IrValue(IrNodeKind k) : IrNode(k) {}
        explicit IrValue(IrNodeKind k, sm::SourceRange r) : IrNode(k, r) {}
    };

    struct IrIntConstant : IrValue
    {
        static constexpr auto Kind = IrNodeKind::IntConstant;
        std::int64_t value;
        IrIntConstant(IrType const* t, std::int64_t v) : IrValue(Kind), value(v) { type = t; }
    };

    struct IrFloatConstant : IrValue
    {
        static constexpr auto Kind = IrNodeKind::FloatConstant;
        double value;
        IrFloatConstant(IrType const* t, double v) : IrValue(Kind), value(v) { type = t; }
    };

    struct IrBoolConstant : IrValue
    {
        static constexpr auto Kind = IrNodeKind::BoolConstant;
        bool value;
        IrBoolConstant(IrType const* t, bool v) : IrValue(Kind), value(v) { type = t; }
    };

    struct IrNullConstant : IrValue
    {
        static constexpr auto Kind = IrNodeKind::NullConstant;
        explicit IrNullConstant(IrType const* t) : IrValue(Kind) { type = t; }
    };

    struct IrStringConstant : IrValue
    {
        static constexpr auto Kind = IrNodeKind::StringConstant;
        std::string_view value;
        IrStringConstant(IrType const* t, std::string_view v) : IrValue(Kind), value(v) { type = t; }
    };

    struct IrLocal : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Local;
        std::uint32_t id{};

        explicit IrLocal(std::string_view n, std::uint32_t i, IrType const* t = nullptr) : IrValue(Kind), id(i)
        {
            name = n;
            type = t;
        }
    };

    template <typename To, typename From> [[nodiscard]] To const* ir_cast(From const* n) noexcept
    {
        return (n && n->kind == To::Kind) ? static_cast<To const*>(n) : nullptr;
    }

    template <typename To, typename From> [[nodiscard]] To* ir_cast(From* n) noexcept
    {
        return (n && n->kind == To::Kind) ? static_cast<To*>(n) : nullptr;
    }

    struct IrAddInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Add;

        IrValue* lhs;
        IrValue* rhs;

        IrAddInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrSubInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Sub;

        IrValue* lhs;
        IrValue* rhs;

        IrSubInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrMulInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Mul;

        IrValue* lhs;
        IrValue* rhs;

        IrMulInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrUDivInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::UDiv;

        IrValue* lhs;
        IrValue* rhs;

        IrUDivInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrSDivInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::SDiv;

        IrValue* lhs;
        IrValue* rhs;

        IrSDivInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrURemInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::URem;

        IrValue* lhs;
        IrValue* rhs;

        IrURemInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrSRemInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::SRem;

        IrValue* lhs;
        IrValue* rhs;

        IrSRemInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrFDivInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::FDiv;

        IrValue* lhs;
        IrValue* rhs;

        IrFDivInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrFRemInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::FRem;

        IrValue* lhs;
        IrValue* rhs;

        IrFRemInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrAndInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::And;

        IrValue* lhs;
        IrValue* rhs;

        IrAndInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrOrInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Or;

        IrValue* lhs;
        IrValue* rhs;

        IrOrInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrXorInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Xor;

        IrValue* lhs;
        IrValue* rhs;

        IrXorInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrShlInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Shl;

        IrValue* lhs;
        IrValue* rhs;

        IrShlInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrLShrInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::LShr;

        IrValue* lhs;
        IrValue* rhs;

        IrLShrInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrAShrInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::AShr;

        IrValue* lhs;
        IrValue* rhs;

        IrAShrInst(IrType const* t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = t; }
    };

    struct IrNegInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Neg;

        IrValue* operand;

        IrNegInst(IrType const* t, IrValue* o) : IrValue(Kind), operand(o) { type = t; }
    };

    struct IrNotInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Not;

        IrValue* operand;

        IrNotInst(IrType const* t, IrValue* o) : IrValue(Kind), operand(o) { type = t; }
    };

    struct IrCmpEqInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpEq;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpEqInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpNeInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpNe;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpNeInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpLtInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpLt;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpLtInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpLeInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpLe;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpLeInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpGtInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpGt;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpGtInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpGeInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpGe;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpGeInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpOLtInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpOLt;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpOLtInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpOLeInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpOLe;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpOLeInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpOGtInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpOGt;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpOGtInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpOGeInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpOGe;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpOGeInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpULtInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpULt;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpULtInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpULeInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpULe;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpULeInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpUGtInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpUGt;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpUGtInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrCmpUGeInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CmpUGe;

        IrValue* lhs;
        IrValue* rhs;

        IrCmpUGeInst(IrType const* bool_t, IrValue* l, IrValue* r) : IrValue(Kind), lhs(l), rhs(r) { type = bool_t; }
    };

    struct IrAllocaInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Alloca;

        IrType const* allocated_type;
        IrValue* count{};
        std::uint32_t alignment{};

        IrAllocaInst(IrType const* result_ptr_t, IrType const* alloc_t, IrValue* c = nullptr) : IrValue(Kind), allocated_type(alloc_t), count(c)
        {
            type = result_ptr_t;
        }
    };

    struct IrLoadInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Load;

        IrValue* pointer;

        IrLoadInst(IrType const* result_t, IrValue* ptr) : IrValue(Kind), pointer(ptr) { type = result_t; }
    };

    struct IrLoadVolatileInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::LoadVolatile;

        IrValue* pointer;

        IrLoadVolatileInst(IrType const* result_t, IrValue* ptr) : IrValue(Kind), pointer(ptr) { type = result_t; }
    };

    struct IrStoreInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Store;

        IrValue* value;
        IrValue* pointer;

        IrStoreInst(IrValue* v, IrValue* p) : IrValue(Kind), value(v), pointer(p) {}
    };

    struct IrStoreVolatileInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::StoreVolatile;

        IrValue* value;
        IrValue* pointer;

        IrStoreVolatileInst(IrValue* v, IrValue* p) : IrValue(Kind), value(v), pointer(p) {}
    };

    struct IrAtomicLoadInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::AtomicLoad;

        IrValue* pointer;
        IrMemoryOrdering ordering;

        IrAtomicLoadInst(IrType const* result_t, IrValue* ptr, IrMemoryOrdering ord) : IrValue(Kind), pointer(ptr), ordering(ord) { type = result_t; }
    };

    struct IrAtomicStoreInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::AtomicStore;

        IrValue* value;
        IrValue* pointer;
        IrMemoryOrdering ordering;

        IrAtomicStoreInst(IrValue* v, IrValue* p, IrMemoryOrdering ord) : IrValue(Kind), value(v), pointer(p), ordering(ord) {}
    };

    struct IrAtomicRmwInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::AtomicRmw;

        IrAtomicRmwOp op;
        IrValue* pointer;
        IrValue* value;
        IrMemoryOrdering ordering;

        IrAtomicRmwInst(IrType const* result_t, IrAtomicRmwOp o, IrValue* ptr, IrValue* v, IrMemoryOrdering ord)
            : IrValue(Kind), op(o), pointer(ptr), value(v), ordering(ord)
        {
            type = result_t;
        }
    };

    struct IrFenceInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Fence;

        IrMemoryOrdering ordering;

        explicit IrFenceInst(IrMemoryOrdering ord) : IrValue(Kind), ordering(ord) {}
    };

    struct IrGepInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Gep;

        enum class IndexKind : std::uint8_t
        {
            Array,
            Field,
        };

        struct Index
        {
            IndexKind kind;
            IrValue* dynamic_index{};
            std::uint32_t field_index{};
        };

        IrValue* base;
        std::pmr::vector<Index> indices;

        IrGepInst(IrType const* result_t, IrValue* b, std::pmr::polymorphic_allocator<> a) : IrValue(Kind), base(b), indices(a) { type = result_t; }
    };

    struct IrZextInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Zext;

        IrValue* operand;

        IrZextInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrSextInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Sext;

        IrValue* operand;

        IrSextInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrTruncInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Trunc;

        IrValue* operand;

        IrTruncInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrFpExtInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::FpExt;

        IrValue* operand;

        IrFpExtInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrFpTruncInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::FpTrunc;

        IrValue* operand;

        IrFpTruncInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrFpToIInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::FpToI;

        IrValue* operand;

        IrFpToIInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrIToFpInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::IToFp;

        IrValue* operand;

        IrIToFpInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrPtrToIInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::PtrToI;

        IrValue* operand;

        IrPtrToIInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrIToPtrInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::IToPtr;

        IrValue* operand;

        IrIToPtrInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrBitcastInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Bitcast;

        IrValue* operand;

        IrBitcastInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrSegcastInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Segcast;

        IrValue* operand;

        IrSegcastInst(IrType const* dst_t, IrValue* o) : IrValue(Kind), operand(o) { type = dst_t; }
    };

    struct IrExtractInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Extract;

        IrValue* aggregate;
        std::uint32_t field_index;

        IrExtractInst(IrType const* field_t, IrValue* agg, std::uint32_t fi) : IrValue(Kind), aggregate(agg), field_index(fi) { type = field_t; }
    };

    struct IrInsertInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Insert;

        IrValue* aggregate;
        std::uint32_t field_index;
        IrValue* value;

        IrInsertInst(IrType const* result_t, IrValue* agg, std::uint32_t fi, IrValue* v) : IrValue(Kind), aggregate(agg), field_index(fi), value(v)
        {
            type = result_t;
        }
    };

    struct IrAggregateInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Aggregate;

        std::pmr::vector<IrValue*> values;

        IrAggregateInst(IrType const* result_t, std::pmr::polymorphic_allocator<> a) : IrValue(Kind), values(a) { type = result_t; }
    };

    struct IrBrInst : IrNode
    {
        static constexpr auto Kind = IrNodeKind::Br;

        IrBasicBlock* target;

        explicit IrBrInst(IrBasicBlock* t) : IrNode(Kind), target(t) {}
    };

    struct IrBrCondInst : IrNode
    {
        static constexpr auto Kind = IrNodeKind::BrCond;

        IrValue* condition;
        IrBasicBlock* true_target;
        IrBasicBlock* false_target;

        IrBrCondInst(IrValue* cond, IrBasicBlock* tt, IrBasicBlock* ft) : IrNode(Kind), condition(cond), true_target(tt), false_target(ft) {}
    };

    struct IrRetInst : IrNode
    {
        static constexpr auto Kind = IrNodeKind::Ret;

        IrValue* value{};

        explicit IrRetInst(IrValue* v = nullptr) : IrNode(Kind), value(v) {}
    };

    struct IrUnreachableInst : IrNode
    {
        static constexpr auto Kind = IrNodeKind::Unreachable;

        IrUnreachableInst() : IrNode(Kind) {}
    };

    struct IrSwitchCase
    {
        std::int64_t start;
        std::int64_t end;
        IrBasicBlock* target;
    };

    struct IrSwitchInst : IrNode
    {
        static constexpr auto Kind = IrNodeKind::Switch;

        IrValue* value;
        std::pmr::vector<IrSwitchCase> cases;
        IrBasicBlock* default_target;

        IrSwitchInst(IrValue* v, IrBasicBlock* def_t, std::pmr::polymorphic_allocator<> a) : IrNode(Kind), value(v), cases(a), default_target(def_t) {}
    };

    struct IrPhiInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Phi;

        struct Pred
        {
            IrValue* value;
            IrBasicBlock* block;
        };

        std::pmr::vector<Pred> incoming;

        IrPhiInst(IrType const* t, std::pmr::polymorphic_allocator<> a) : IrValue(Kind), incoming(a) { type = t; }
    };

    struct IrCallInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Call;

        IrValue* callee;
        std::pmr::vector<IrValue*> args;

        IrCallInst(IrType const* result_t, IrValue* callee, std::pmr::polymorphic_allocator<> a) : IrValue(Kind), callee(callee), args(a) { type = result_t; }
    };

    struct IrCallTailInst : IrValue
    {
        static constexpr auto Kind = IrNodeKind::CallTail;

        IrValue* callee;
        std::pmr::vector<IrValue*> args;

        IrCallTailInst(IrType const* result_t, IrValue* callee, std::pmr::polymorphic_allocator<> a) : IrValue(Kind), callee(callee), args(a)
        {
            type = result_t;
        }
    };

    enum class Linkage : std::uint8_t
    {
        Internal,
        External,
        LinkOnceODR,
        WeakODR,
    };

    enum class IrFuncAttr : std::uint8_t
    {
        Inline,
        NoInline,
        NoMangle,
        Section,
        CallingConv,
    };

    enum class CallingConv : std::uint8_t
    {
        Cdecl,
        Stdcall,
        Fastcall,
        Vectorcall,
        SystemV,
        Win64,
    };

    struct IrFuncAttribute
    {
        IrFuncAttr kind;
        std::string_view value{};
    };

    struct IrBasicBlock : IrValue
    {
        static constexpr auto Kind = IrNodeKind::BasicBlock;

        std::uint32_t id{};

        std::pmr::vector<IrValue*> instructions;
        IrNode* terminator{};

        IrFunction* parent{};

        std::pmr::vector<IrValue*> params;

        IrBasicBlock(std::uint32_t i, std::pmr::polymorphic_allocator<> a) : IrValue(Kind), id(i), instructions(a), params(a) { name = {}; }

        IrBasicBlock(std::string_view n, std::uint32_t i, std::pmr::polymorphic_allocator<> a) : IrValue(Kind), id(i), instructions(a), params(a) { name = n; }

        [[nodiscard]] bool has_name() const noexcept { return !name.empty(); }
    };

    struct IrFunction : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Function;

        IrFuncType const* func_type{};
        std::pmr::vector<IrFuncAttribute> attrs;
        std::pmr::vector<IrBasicBlock*> blocks;
        IrBasicBlock* entry_block{};

        std::pmr::vector<IrDebugLocation> debug_locations;

        std::string_view source_name;
        std::uint32_t decl_file_id{static_cast<std::uint32_t>(sm::FileId::Invalid)};
        std::uint32_t decl_line{};

        bool is_dll_import{};
        bool is_dll_export{};
        Linkage linkage{Linkage::Internal};
        std::uint32_t alignment{};

        IrFunction(std::string_view n, IrFuncType const* ft, std::pmr::polymorphic_allocator<> a)
            : IrValue(Kind), func_type(ft), attrs(a), blocks(a), debug_locations(a)
        {
            name = n;
            type = ft;
        }
    };

    struct IrGlobal : IrValue
    {
        static constexpr auto Kind = IrNodeKind::Global;

        IrValue* init{};
        bool is_constant{false};
        bool is_dll_import{};
        bool is_dll_export{};
        Linkage linkage{Linkage::Internal};
        std::uint32_t alignment{};
        std::string_view section{};

        IrGlobal(std::string_view n, IrType const* t, IrValue* i = nullptr, bool c = false) : IrValue(Kind), init(i), is_constant(c)
        {
            name = n;
            type = t;
        }
    };

    struct IrModule
    {
        std::string_view name;
        std::pmr::vector<IrGlobal*> globals;
        std::pmr::vector<IrFunction*> functions;
        std::uint32_t source_file_id{static_cast<std::uint32_t>(sm::FileId::Invalid)};

        explicit IrModule(std::pmr::polymorphic_allocator<> a) : globals(a), functions(a) {}

        IrModule(std::string_view n, std::pmr::polymorphic_allocator<> a) : name(n), globals(a), functions(a) {}
    };

    struct IrGlobalRef : IrValue
    {
        static constexpr auto Kind = IrNodeKind::GlobalRef;

        IrGlobal const* global{};
        IrFunction const* function{};

        explicit IrGlobalRef(IrGlobal const* g, IrType const* t) : IrValue(Kind), global(g)
        {
            type = t;
            name = g->name;
        }

        explicit IrGlobalRef(IrFunction const* f, IrType const* t) : IrValue(Kind), function(f)
        {
            type = t;
            name = f->name;
        }

        explicit IrGlobalRef(std::string_view n, IrType const* t) : IrValue(Kind)
        {
            name = n;
            type = t;
        }
    };

    class IrContext
    {
    public:
        explicit IrContext(std::size_t initial = 256 * 1024, target::TargetConfig const* target = nullptr) : m_buffer(initial), m_arena(&m_buffer)
        {
            if (target)
            {
                m_pointer_bits = target->pointer_bits;
                m_pointer_align = target->pointer_align;
            }
        }

        [[nodiscard]] std::uint8_t pointer_bits() const noexcept { return m_pointer_bits; }
        [[nodiscard]] std::uint8_t pointer_align() const noexcept { return m_pointer_align; }

        IrContext(IrContext const&) = delete;
        IrContext& operator=(IrContext const&) = delete;
        IrContext(IrContext&&) = delete;
        IrContext& operator=(IrContext&&) = delete;

        [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() noexcept { return m_arena; }

        [[nodiscard]] IrType const* void_t()
        {
            return ensure_singleton(m_void, [&] { return make<IrVoidType>(); });
        }

        [[nodiscard]] IrType const* bool_t()
        {
            return ensure_singleton(m_bool, [&] { return make<IrBoolType>(); });
        }

        [[nodiscard]] IrType const* int_t(std::uint8_t bits, bool is_signed, bool is_pointer_sized = false)
        {
            for (auto const* t : m_ints)
                if (t->bits == bits && t->is_signed == is_signed && t->is_pointer_sized == is_pointer_sized)
                    return t;

            auto* t = make<IrIntType>(bits, is_signed, is_pointer_sized);
            m_ints.push_back(t);
            return t;
        }

        [[nodiscard]] IrType const* usize_t() { return int_t(m_pointer_bits, false, true); }
        [[nodiscard]] IrType const* isize_t() { return int_t(m_pointer_bits, true, true); }

        [[nodiscard]] IrType const* float_t(std::uint8_t bits)
        {
            for (auto const* t : m_floats)
                if (t->bits == bits)
                    return t;

            auto* t = make<IrFloatType>(bits);
            m_floats.push_back(t);
            return t;
        }

        [[nodiscard]] IrType const* pointer_to(IrType const* pointee, Segment seg = Segment::None)
        {
            for (auto const* t : m_pointers)
                if (t->pointee == pointee && t->seg == seg)
                    return t;

            auto* t = make<IrPointerType>(pointee, seg, m_pointer_bits, m_pointer_align);
            m_pointers.push_back(t);
            return t;
        }

        [[nodiscard]] IrType const* aggregate_t(std::span<IrType const*> members, std::span<std::uint64_t const> member_offsets, std::uint64_t byte_size,
                                                std::uint64_t byte_align, bool default_layout = true)
        {
#ifndef NDEBUG
            if (default_layout)
                verify_aggregate_layout(members, member_offsets, byte_size, byte_align);
#endif

            for (auto const* t : m_aggregates)
            {
                auto* at = static_cast<IrAggregateType const*>(t);
                if (at->byte_size == byte_size && at->byte_align == byte_align && same_span(at->members, members) &&
                    same_span(at->member_offsets, member_offsets))
                    return t;
            }

            if (members.size() != member_offsets.size())
                std::abort();

            auto* t = make<IrAggregateType>(m_arena);
            t->members.assign(members.begin(), members.end());
            t->member_offsets.assign(member_offsets.begin(), member_offsets.end());
            t->byte_size = byte_size;
            t->byte_align = byte_align;
            m_aggregates.push_back(t);
            return t;
        }

        [[nodiscard]] IrType const* array_t(IrType const* element, std::uint64_t count)
        {
            for (auto const* t : m_arrays)
                if (t->element == element && t->count == count)
                    return t;

            auto* t = make<IrArrayType>(element, count);
            m_arrays.push_back(t);
            return t;
        }

        [[nodiscard]] IrType const* slice_t(IrType const* element, Segment seg = Segment::None)
        {
            for (auto const* t : m_slices)
                if (t->element == element && t->seg == seg)
                    return t;

            auto* t = make<IrSliceType>(element, seg, m_pointer_bits, m_pointer_align);
            m_slices.push_back(t);
            return t;
        }

        [[nodiscard]] IrType const* func_t(IrType const* ret, std::span<IrType const*> params)
        {
            for (auto const* t : m_funcs)
                if (t->return_type == ret && same_span(t->params, params))
                    return t;

            auto* t = make<IrFuncType>(ret, m_arena);
            t->params.assign(params.begin(), params.end());
            m_funcs.push_back(t);
            return t;
        }

        [[nodiscard]] IrIntConstant* int_const(IrType const* t, std::int64_t v) { return make<IrIntConstant>(t, v); }

        [[nodiscard]] IrFloatConstant* float_const(IrType const* t, double v) { return make<IrFloatConstant>(t, v); }

        [[nodiscard]] IrBoolConstant* bool_const(bool v) { return make<IrBoolConstant>(bool_t(), v); }

        [[nodiscard]] IrNullConstant* null_const(IrType const* ptr_t) { return make<IrNullConstant>(ptr_t); }

        [[nodiscard]] IrStringConstant* string_const(IrType const* t, std::string_view v) { return make<IrStringConstant>(t, v); }

        [[nodiscard]] IrLocal* local(std::string_view name, std::uint32_t id, IrType const* t = nullptr) { return make<IrLocal>(name, id, t); }

        [[nodiscard]] IrGlobalRef* global_ref(IrGlobal const* g, IrType const* t) { return make<IrGlobalRef>(g, t); }
        [[nodiscard]] IrGlobalRef* func_ref(IrFunction const* f) { return make<IrGlobalRef>(f, pointer_to(f->func_type)); }
        [[nodiscard]] IrGlobalRef* symbol_ref(std::string_view name, IrType const* t) { return make<IrGlobalRef>(name, t); }

        [[nodiscard]] IrAddInst* add(IrType const* t, IrValue* l, IrValue* r) { return make<IrAddInst>(t, l, r); }
        [[nodiscard]] IrSubInst* sub(IrType const* t, IrValue* l, IrValue* r) { return make<IrSubInst>(t, l, r); }
        [[nodiscard]] IrMulInst* mul(IrType const* t, IrValue* l, IrValue* r) { return make<IrMulInst>(t, l, r); }
        [[nodiscard]] IrUDivInst* udiv(IrType const* t, IrValue* l, IrValue* r) { return make<IrUDivInst>(t, l, r); }
        [[nodiscard]] IrSDivInst* sdiv(IrType const* t, IrValue* l, IrValue* r) { return make<IrSDivInst>(t, l, r); }
        [[nodiscard]] IrURemInst* urem(IrType const* t, IrValue* l, IrValue* r) { return make<IrURemInst>(t, l, r); }
        [[nodiscard]] IrSRemInst* srem(IrType const* t, IrValue* l, IrValue* r) { return make<IrSRemInst>(t, l, r); }
        [[nodiscard]] IrFDivInst* fdiv(IrType const* t, IrValue* l, IrValue* r) { return make<IrFDivInst>(t, l, r); }
        [[nodiscard]] IrFRemInst* frem(IrType const* t, IrValue* l, IrValue* r) { return make<IrFRemInst>(t, l, r); }
        [[nodiscard]] IrAndInst* and_(IrType const* t, IrValue* l, IrValue* r) { return make<IrAndInst>(t, l, r); }
        [[nodiscard]] IrOrInst* or_(IrType const* t, IrValue* l, IrValue* r) { return make<IrOrInst>(t, l, r); }
        [[nodiscard]] IrXorInst* xor_(IrType const* t, IrValue* l, IrValue* r) { return make<IrXorInst>(t, l, r); }
        [[nodiscard]] IrShlInst* shl(IrType const* t, IrValue* l, IrValue* r) { return make<IrShlInst>(t, l, r); }
        [[nodiscard]] IrLShrInst* lshr(IrType const* t, IrValue* l, IrValue* r) { return make<IrLShrInst>(t, l, r); }
        [[nodiscard]] IrAShrInst* ashr(IrType const* t, IrValue* l, IrValue* r) { return make<IrAShrInst>(t, l, r); }
        [[nodiscard]] IrNegInst* neg(IrType const* t, IrValue* o) { return make<IrNegInst>(t, o); }
        [[nodiscard]] IrNotInst* not_(IrType const* t, IrValue* o) { return make<IrNotInst>(t, o); }

        [[nodiscard]] IrCmpEqInst* cmp_eq(IrValue* l, IrValue* r) { return make<IrCmpEqInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpNeInst* cmp_ne(IrValue* l, IrValue* r) { return make<IrCmpNeInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpLtInst* cmp_lt(IrValue* l, IrValue* r) { return make<IrCmpLtInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpLeInst* cmp_le(IrValue* l, IrValue* r) { return make<IrCmpLeInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpGtInst* cmp_gt(IrValue* l, IrValue* r) { return make<IrCmpGtInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpGeInst* cmp_ge(IrValue* l, IrValue* r) { return make<IrCmpGeInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpOLtInst* cmp_olt(IrValue* l, IrValue* r) { return make<IrCmpOLtInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpOLeInst* cmp_ole(IrValue* l, IrValue* r) { return make<IrCmpOLeInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpOGtInst* cmp_ogt(IrValue* l, IrValue* r) { return make<IrCmpOGtInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpOGeInst* cmp_oge(IrValue* l, IrValue* r) { return make<IrCmpOGeInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpULtInst* cmp_ult(IrValue* l, IrValue* r) { return make<IrCmpULtInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpULeInst* cmp_ule(IrValue* l, IrValue* r) { return make<IrCmpULeInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpUGtInst* cmp_ugt(IrValue* l, IrValue* r) { return make<IrCmpUGtInst>(bool_t(), l, r); }
        [[nodiscard]] IrCmpUGeInst* cmp_uge(IrValue* l, IrValue* r) { return make<IrCmpUGeInst>(bool_t(), l, r); }

        [[nodiscard]] IrAllocaInst* alloca(IrType const* ptr_t, IrType const* alloc_t, IrValue* count = nullptr)
        {
            return make<IrAllocaInst>(ptr_t, alloc_t, count);
        }
        [[nodiscard]] IrLoadInst* load(IrType const* result_t, IrValue* ptr) { return make<IrLoadInst>(result_t, ptr); }
        [[nodiscard]] IrLoadVolatileInst* load_volatile(IrType const* result_t, IrValue* ptr) { return make<IrLoadVolatileInst>(result_t, ptr); }
        [[nodiscard]] IrStoreInst* store(IrValue* val, IrValue* ptr) { return make<IrStoreInst>(val, ptr); }
        [[nodiscard]] IrStoreVolatileInst* store_volatile(IrValue* val, IrValue* ptr) { return make<IrStoreVolatileInst>(val, ptr); }

        [[nodiscard]] IrAtomicLoadInst* atomic_load(IrType const* result_t, IrValue* ptr, IrMemoryOrdering ord)
        {
            return make<IrAtomicLoadInst>(result_t, ptr, ord);
        }
        [[nodiscard]] IrAtomicStoreInst* atomic_store(IrValue* val, IrValue* ptr, IrMemoryOrdering ord)
        {
            return make<IrAtomicStoreInst>(val, ptr, ord);
        }
        [[nodiscard]] IrAtomicRmwInst* atomic_rmw(IrType const* result_t, IrAtomicRmwOp op, IrValue* ptr, IrValue* val, IrMemoryOrdering ord)
        {
            return make<IrAtomicRmwInst>(result_t, op, ptr, val, ord);
        }
        [[nodiscard]] IrFenceInst* fence(IrMemoryOrdering ord) { return make<IrFenceInst>(ord); }

        [[nodiscard]] IrGepInst* gep(IrType const* result_t, IrValue* base) { return make<IrGepInst>(result_t, base, m_arena); }

        [[nodiscard]] IrZextInst* zext(IrType const* dst, IrValue* o) { return make<IrZextInst>(dst, o); }
        [[nodiscard]] IrSextInst* sext(IrType const* dst, IrValue* o) { return make<IrSextInst>(dst, o); }
        [[nodiscard]] IrTruncInst* trunc(IrType const* dst, IrValue* o) { return make<IrTruncInst>(dst, o); }
        [[nodiscard]] IrFpExtInst* fpext(IrType const* dst, IrValue* o) { return make<IrFpExtInst>(dst, o); }
        [[nodiscard]] IrFpTruncInst* fptrunc(IrType const* dst, IrValue* o) { return make<IrFpTruncInst>(dst, o); }
        [[nodiscard]] IrFpToIInst* fptoi(IrType const* dst, IrValue* o) { return make<IrFpToIInst>(dst, o); }
        [[nodiscard]] IrIToFpInst* itofp(IrType const* dst, IrValue* o) { return make<IrIToFpInst>(dst, o); }
        [[nodiscard]] IrPtrToIInst* ptrtoi(IrType const* dst, IrValue* o) { return make<IrPtrToIInst>(dst, o); }
        [[nodiscard]] IrIToPtrInst* itoptr(IrType const* dst, IrValue* o) { return make<IrIToPtrInst>(dst, o); }
        [[nodiscard]] IrBitcastInst* bitcast(IrType const* dst, IrValue* o) { return make<IrBitcastInst>(dst, o); }
        [[nodiscard]] IrSegcastInst* segcast(IrType const* dst, IrValue* o) { return make<IrSegcastInst>(dst, o); }

        [[nodiscard]] IrExtractInst* extract(IrType const* field_t, IrValue* agg, std::uint32_t fi) { return make<IrExtractInst>(field_t, agg, fi); }
        [[nodiscard]] IrInsertInst* insert(IrType const* result_t, IrValue* agg, std::uint32_t fi, IrValue* v)
        {
            return make<IrInsertInst>(result_t, agg, fi, v);
        }
        [[nodiscard]] IrAggregateInst* aggregate(IrType const* result_t) { return make<IrAggregateInst>(result_t, m_arena); }

        [[nodiscard]] IrBrInst* br(IrBasicBlock* target) { return make<IrBrInst>(target); }
        [[nodiscard]] IrBrCondInst* br_cond(IrValue* cond, IrBasicBlock* tt, IrBasicBlock* ft) { return make<IrBrCondInst>(cond, tt, ft); }
        [[nodiscard]] IrRetInst* ret(IrValue* v = nullptr) { return make<IrRetInst>(v); }
        [[nodiscard]] IrUnreachableInst* unreachable() { return make<IrUnreachableInst>(); }
        [[nodiscard]] IrSwitchInst* switch_(IrValue* v, IrBasicBlock* def_t) { return make<IrSwitchInst>(v, def_t, m_arena); }

        [[nodiscard]] IrPhiInst* phi(IrType const* t) { return make<IrPhiInst>(t, m_arena); }

        [[nodiscard]] IrCallInst* call(IrType const* result_t, IrValue* callee) { return make<IrCallInst>(result_t, callee, m_arena); }

        [[nodiscard]] IrCallTailInst* call_tail(IrType const* result_t, IrValue* callee) { return make<IrCallTailInst>(result_t, callee, m_arena); }

        [[nodiscard]] IrBasicBlock* basic_block(std::uint32_t id) { return make<IrBasicBlock>(id, m_arena); }
        [[nodiscard]] IrBasicBlock* basic_block(std::string_view name, std::uint32_t id) { return make<IrBasicBlock>(name, id, m_arena); }

        [[nodiscard]] IrFunction* function(std::string_view name, IrFuncType const* ft) { return make<IrFunction>(name, ft, m_arena); }

        [[nodiscard]] IrGlobal* global(std::string_view name, IrType const* t, IrValue* init = nullptr, bool constant = false)
        {
            return make<IrGlobal>(name, t, init, constant);
        }

        [[nodiscard]] IrModule* module(std::string_view name = {}) { return make<IrModule>(name, m_arena); }

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

        IrVoidType const* m_void{};
        IrBoolType const* m_bool{};

        std::uint8_t m_pointer_bits{64};
        std::uint8_t m_pointer_align{8};

        std::vector<IrIntType const*> m_ints;
        std::vector<IrFloatType const*> m_floats;
        std::vector<IrPointerType const*> m_pointers;
        std::vector<IrAggregateType const*> m_aggregates;
        std::vector<IrArrayType const*> m_arrays;
        std::vector<IrSliceType const*> m_slices;
        std::vector<IrFuncType const*> m_funcs;

        template <typename T, typename F> T const* ensure_singleton(T const*& slot, F&& factory)
        {
            if (!slot)
                slot = factory();

            return slot;
        }

        static void verify_aggregate_layout(std::span<IrType const*> members, std::span<std::uint64_t const> member_offsets, std::uint64_t byte_size,
                                            std::uint64_t byte_align)
        {
            std::ignore = byte_size;
            std::ignore = byte_align;
            if (members.size() != member_offsets.size())
                std::abort();

            std::uint64_t expected_offset = 0;
            std::uint64_t expected_max_align = 1;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                auto* member = members[i];
                if (!member)
                    continue;

                std::uint64_t align = member->byte_align;
                if (align > expected_max_align)
                    expected_max_align = align;

                expected_offset = (expected_offset + align - 1) / align * align;
                if (member_offsets[i] != expected_offset)
                    std::abort();

                expected_offset += member->byte_size;
            }
            if (byte_align != expected_max_align)
                std::abort();

            if (byte_size != (expected_offset + expected_max_align - 1) / expected_max_align * expected_max_align)
                std::abort();
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

    class IrSerializer
    {
    public:
        [[nodiscard]] static std::string dump(IrModule const* m)
        {
            IrSerializer s;
            s.print_module(m);
            return s.m_out;
        }

        [[nodiscard]] static std::string dump(IrFunction const* f)
        {
            IrSerializer s;
            s.print_function(f);
            return s.m_out;
        }

        [[nodiscard]] static std::string dump(IrType const* t)
        {
            IrSerializer s;
            s.print_type(t);
            return s.m_out;
        }

    private:
        std::string m_out;
        std::size_t m_indent = 0;
        std::unordered_set<IrType const*> m_printing_types;

        struct Indent
        {
            std::size_t& level;
            explicit Indent(std::size_t& l) : level(l) { ++level; }
            ~Indent() { --level; }
            Indent(Indent const&) = delete;
            Indent& operator=(Indent const&) = delete;
        };

        void pad() { m_out.append(m_indent * 2, ' '); }

        void write(std::string_view s) { m_out += s; }

        void line(std::string_view s)
        {
            pad();
            m_out += s;
            m_out += '\n';
        }

        template <typename... Args> void line_fmt(std::format_string<Args...> fmt, Args&&... args)
        {
            pad();
            std::format_to(std::back_inserter(m_out), fmt, std::forward<Args>(args)...);
            m_out += '\n';
        }

        static std::string seg_str(Segment seg)
        {
            switch (seg)
            {
                case Segment::Cs:
                    return "cs";
                case Segment::Ds:
                    return "ds";
                case Segment::Es:
                    return "es";
                case Segment::Fs:
                    return "fs";
                case Segment::Gs:
                    return "gs";
                case Segment::Ss:
                    return "ss";
                default:
                    return "";
            }
        }

        void print_type(IrType const* t)
        {
            if (!t)
            {
                write("<null-type>");
                return;
            }

            if (t->kind == IrTypeKind::Aggregate || t->kind == IrTypeKind::Pointer || t->kind == IrTypeKind::Func || t->kind == IrTypeKind::Array ||
                t->kind == IrTypeKind::Slice)
            {
                if (!m_printing_types.insert(t).second)
                {
                    std::format_to(std::back_inserter(m_out), "<cycle {}>", static_cast<void const*>(t));
                    return;
                }
            }

            switch (t->kind)
            {
                case IrTypeKind::Void:
                    write("void");
                    break;
                case IrTypeKind::Bool:
                    write("bool");
                    break;
                case IrTypeKind::Int: {
                    auto* it = static_cast<IrIntType const*>(t);
                    if (it->is_pointer_sized)
                        write(it->is_signed ? "isize" : "usize");
                    else
                        std::format_to(std::back_inserter(m_out), "{}{}", it->is_signed ? 'i' : 'u', it->bits);
                    break;
                }
                case IrTypeKind::Float: {
                    auto* ft = static_cast<IrFloatType const*>(t);
                    std::format_to(std::back_inserter(m_out), "f{}", ft->bits);
                    break;
                }
                case IrTypeKind::Pointer: {
                    auto* pt = static_cast<IrPointerType const*>(t);
                    write("ptr<");
                    print_type(pt->pointee);
                    if (pt->seg != Segment::None)
                    {
                        write(", seg=");
                        write(seg_str(pt->seg));
                    }
                    write(">");
                    break;
                }
                case IrTypeKind::Aggregate: {
                    auto* at = static_cast<IrAggregateType const*>(t);
                    write("{");
                    for (std::size_t i = 0; i < at->members.size(); ++i)
                    {
                        if (i > 0)
                            write(", ");

                        if (i < at->member_offsets.size())
                            std::format_to(std::back_inserter(m_out), "@{} ", at->member_offsets[i]);

                        print_type(at->members[i]);
                    }
                    write("}");
                    break;
                }
                case IrTypeKind::Array: {
                    auto* at = static_cast<IrArrayType const*>(t);
                    write("[");
                    std::format_to(std::back_inserter(m_out), "{}", at->count);
                    write(" x ");
                    print_type(at->element);
                    write("]");
                    break;
                }
                case IrTypeKind::Slice: {
                    auto* st = static_cast<IrSliceType const*>(t);
                    write("[]");
                    if (st->seg != Segment::None)
                    {
                        write(seg_str(st->seg));
                        write(" ");
                    }
                    print_type(st->element);
                    break;
                }
                case IrTypeKind::Func: {
                    auto* ft = static_cast<IrFuncType const*>(t);
                    write("fn(");
                    for (std::size_t i = 0; i < ft->params.size(); ++i)
                    {
                        if (i > 0)
                            write(", ");

                        print_type(ft->params[i]);
                    }
                    write(") -> ");
                    print_type(ft->return_type);
                    break;
                }
            }

            if (t->kind == IrTypeKind::Aggregate || t->kind == IrTypeKind::Pointer || t->kind == IrTypeKind::Func || t->kind == IrTypeKind::Array ||
                t->kind == IrTypeKind::Slice)
            {
                m_printing_types.erase(t);
            }
        }

        void print_value(IrValue const* v)
        {
            if (!v)
            {
                write("<null>");
                return;
            }

            switch (v->kind)
            {
                case IrNodeKind::IntConstant: {
                    auto* c = static_cast<IrIntConstant const*>(v);
                    bool is_unsigned = false;
                    if (c->type && c->type->kind == IrTypeKind::Int)
                    {
                        auto* it = static_cast<IrIntType const*>(c->type);
                        if (!it->is_signed)
                            is_unsigned = true;
                    }
                    if (is_unsigned)
                        std::format_to(std::back_inserter(m_out), "#{}", static_cast<std::uint64_t>(c->value));
                    else
                        std::format_to(std::back_inserter(m_out), "#{}", c->value);
                    break;
                }
                case IrNodeKind::FloatConstant: {
                    auto* c = static_cast<IrFloatConstant const*>(v);
                    std::format_to(std::back_inserter(m_out), "#{}", c->value);
                    break;
                }
                case IrNodeKind::BoolConstant: {
                    auto* c = static_cast<IrBoolConstant const*>(v);
                    write(c->value ? "#true" : "#false");
                    break;
                }
                case IrNodeKind::NullConstant: {
                    write("#null");
                    break;
                }
                case IrNodeKind::StringConstant: {
                    auto* c = static_cast<IrStringConstant const*>(v);
                    std::format_to(std::back_inserter(m_out), "\"{}\"", c->value);
                    break;
                }
                case IrNodeKind::Local: {
                    auto* l = static_cast<IrLocal const*>(v);
                    if (!l->name.empty())
                        std::format_to(std::back_inserter(m_out), "%{}", l->name);
                    else
                        std::format_to(std::back_inserter(m_out), "%{}", l->id);
                    break;
                }
                case IrNodeKind::GlobalRef: {
                    auto* g = static_cast<IrGlobalRef const*>(v);
                    std::format_to(std::back_inserter(m_out), "@{}", g->name);
                    break;
                }
                case IrNodeKind::BasicBlock: {
                    auto* bb = static_cast<IrBasicBlock const*>(v);
                    if (bb->has_name())
                        std::format_to(std::back_inserter(m_out), "label %{}", bb->name);
                    else
                        std::format_to(std::back_inserter(m_out), "label %bb{}", bb->id);
                    break;
                }
                case IrNodeKind::Aggregate: {
                    auto* agg = static_cast<IrAggregateInst const*>(v);
                    if (!agg->name.empty())
                    {
                        std::format_to(std::back_inserter(m_out), "%{}", agg->name);
                        break;
                    }

                    write("aggregate ");
                    print_type(agg->type);
                    for (auto* elem : agg->values)
                    {
                        write(", ");
                        print_value(elem);
                    }
                    break;
                }
                default: {
                    if (!v->name.empty())
                        std::format_to(std::back_inserter(m_out), "%{}", v->name);
                    else
                        write("%?");
                    break;
                }
            }
        }

        void print_value_with_type(IrValue const* v)
        {
            if (!v)
            {
                write("<null>");
                return;
            }

            switch (v->kind)
            {
                case IrNodeKind::IntConstant:
                case IrNodeKind::FloatConstant:
                case IrNodeKind::BoolConstant:
                case IrNodeKind::NullConstant:
                case IrNodeKind::StringConstant:
                case IrNodeKind::GlobalRef:
                case IrNodeKind::Local:
                case IrNodeKind::BasicBlock:
                case IrNodeKind::Function:
                case IrNodeKind::Global:
                    print_value(v);
                    break;
                default:
                    if (!v->name.empty())
                        std::format_to(std::back_inserter(m_out), "%{}", v->name);
                    else
                        write("%?");
                    break;
            }
        }

        void print_typed_value(IrValue const* v)
        {
            print_type(v->type);
            write(" ");
            print_value(v);
        }

        void print_label(IrBasicBlock const* bb)
        {
            if (bb->has_name())
                std::format_to(std::back_inserter(m_out), "label %{}", bb->name);
            else
                std::format_to(std::back_inserter(m_out), "label %bb{}", bb->id);
        }

        static bool is_terminator(IrNodeKind k)
        {
            return k == IrNodeKind::Br || k == IrNodeKind::BrCond || k == IrNodeKind::Ret || k == IrNodeKind::Unreachable || k == IrNodeKind::Switch;
        }

        void print_inst(IrNode const* inst, std::string_view result_name)
        {
            if (!inst)
                return;

            switch (inst->kind)
            {
                case IrNodeKind::Add:
                    print_binop("add", static_cast<IrAddInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Sub:
                    print_binop("sub", static_cast<IrSubInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Mul:
                    print_binop("mul", static_cast<IrMulInst const*>(inst), result_name);
                    break;
                case IrNodeKind::UDiv:
                    print_binop("udiv", static_cast<IrUDivInst const*>(inst), result_name);
                    break;
                case IrNodeKind::SDiv:
                    print_binop("sdiv", static_cast<IrSDivInst const*>(inst), result_name);
                    break;
                case IrNodeKind::URem:
                    print_binop("urem", static_cast<IrURemInst const*>(inst), result_name);
                    break;
                case IrNodeKind::SRem:
                    print_binop("srem", static_cast<IrSRemInst const*>(inst), result_name);
                    break;
                case IrNodeKind::FDiv:
                    print_binop("fdiv", static_cast<IrFDivInst const*>(inst), result_name);
                    break;
                case IrNodeKind::FRem:
                    print_binop("frem", static_cast<IrFRemInst const*>(inst), result_name);
                    break;
                case IrNodeKind::And:
                    print_binop("and", static_cast<IrAndInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Or:
                    print_binop("or", static_cast<IrOrInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Xor:
                    print_binop("xor", static_cast<IrXorInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Shl:
                    print_binop("shl", static_cast<IrShlInst const*>(inst), result_name);
                    break;
                case IrNodeKind::LShr:
                    print_binop("lshr", static_cast<IrLShrInst const*>(inst), result_name);
                    break;
                case IrNodeKind::AShr:
                    print_binop("ashr", static_cast<IrAShrInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Neg:
                    print_unop("neg", static_cast<IrNegInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Not:
                    print_unop("not", static_cast<IrNotInst const*>(inst), result_name);
                    break;

                case IrNodeKind::CmpEq:
                    print_cmp("cmp.eq", static_cast<IrCmpEqInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpNe:
                    print_cmp("cmp.ne", static_cast<IrCmpNeInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpLt:
                    print_cmp("cmp.lt", static_cast<IrCmpLtInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpLe:
                    print_cmp("cmp.le", static_cast<IrCmpLeInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpGt:
                    print_cmp("cmp.gt", static_cast<IrCmpGtInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpGe:
                    print_cmp("cmp.ge", static_cast<IrCmpGeInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpOLt:
                    print_cmp("cmp.olt", static_cast<IrCmpOLtInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpOLe:
                    print_cmp("cmp.ole", static_cast<IrCmpOLeInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpOGt:
                    print_cmp("cmp.ogt", static_cast<IrCmpOGtInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpOGe:
                    print_cmp("cmp.oge", static_cast<IrCmpOGeInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpULt:
                    print_cmp("cmp.ult", static_cast<IrCmpULtInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpULe:
                    print_cmp("cmp.ule", static_cast<IrCmpULeInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpUGt:
                    print_cmp("cmp.ugt", static_cast<IrCmpUGtInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CmpUGe:
                    print_cmp("cmp.uge", static_cast<IrCmpUGeInst const*>(inst), result_name);
                    break;

                case IrNodeKind::Load:
                    print_load(false, static_cast<IrLoadInst const*>(inst), result_name);
                    break;
                case IrNodeKind::LoadVolatile:
                    print_load(true, static_cast<IrLoadVolatileInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Alloca:
                    print_alloca(static_cast<IrAllocaInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Store:
                    print_store(false, static_cast<IrStoreInst const*>(inst));
                    break;
                case IrNodeKind::StoreVolatile:
                    print_store(true, static_cast<IrStoreVolatileInst const*>(inst));
                    break;
                case IrNodeKind::Gep:
                    print_gep(static_cast<IrGepInst const*>(inst), result_name);
                    break;

                case IrNodeKind::Zext:
                    print_cast("zext", static_cast<IrZextInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Sext:
                    print_cast("sext", static_cast<IrSextInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Trunc:
                    print_cast("trunc", static_cast<IrTruncInst const*>(inst), result_name);
                    break;
                case IrNodeKind::FpExt:
                    print_cast("fpext", static_cast<IrFpExtInst const*>(inst), result_name);
                    break;
                case IrNodeKind::FpTrunc:
                    print_cast("fptrunc", static_cast<IrFpTruncInst const*>(inst), result_name);
                    break;
                case IrNodeKind::FpToI:
                    print_cast("fptoi", static_cast<IrFpToIInst const*>(inst), result_name);
                    break;
                case IrNodeKind::IToFp:
                    print_cast("itofp", static_cast<IrIToFpInst const*>(inst), result_name);
                    break;
                case IrNodeKind::PtrToI:
                    print_cast("ptrtoi", static_cast<IrPtrToIInst const*>(inst), result_name);
                    break;
                case IrNodeKind::IToPtr:
                    print_cast("itoptr", static_cast<IrIToPtrInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Bitcast:
                    print_cast("bitcast", static_cast<IrBitcastInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Segcast:
                    print_cast("segcast", static_cast<IrSegcastInst const*>(inst), result_name);
                    break;

                case IrNodeKind::Extract:
                    print_extract(static_cast<IrExtractInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Insert:
                    print_insert(static_cast<IrInsertInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Aggregate:
                    print_aggregate(static_cast<IrAggregateInst const*>(inst), result_name);
                    break;

                case IrNodeKind::Br:
                    print_br(static_cast<IrBrInst const*>(inst));
                    break;
                case IrNodeKind::BrCond:
                    print_brcond(static_cast<IrBrCondInst const*>(inst));
                    break;
                case IrNodeKind::Ret:
                    print_ret(static_cast<IrRetInst const*>(inst));
                    break;
                case IrNodeKind::Unreachable:
                    line("unreachable");
                    break;
                case IrNodeKind::Switch:
                    print_switch(static_cast<IrSwitchInst const*>(inst));
                    break;

                case IrNodeKind::Phi:
                    print_phi(static_cast<IrPhiInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Call:
                    print_call(false, static_cast<IrCallInst const*>(inst), result_name);
                    break;
                case IrNodeKind::CallTail:
                    print_call(true, static_cast<IrCallTailInst const*>(inst), result_name);
                    break;

                case IrNodeKind::AtomicLoad:
                    print_atomic_load(static_cast<IrAtomicLoadInst const*>(inst), result_name);
                    break;
                case IrNodeKind::AtomicStore:
                    print_atomic_store(static_cast<IrAtomicStoreInst const*>(inst));
                    break;
                case IrNodeKind::AtomicRmw:
                    print_atomic_rmw(static_cast<IrAtomicRmwInst const*>(inst), result_name);
                    break;
                case IrNodeKind::Fence:
                    print_fence(static_cast<IrFenceInst const*>(inst));
                    break;

                default:
                    break;
            }
        }

        void print_op(IrValue const* v)
        {
            if (v->type)
            {
                print_type(v->type);
                write(" ");
            }
            print_value(v);
        }

        template <typename I> void print_binop(std::string_view mnemonic, I const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write(mnemonic);
            write(" ");
            print_op(inst->lhs);
            write(", ");
            print_op(inst->rhs);
            m_out += '\n';
        }

        template <typename I> void print_unop(std::string_view mnemonic, I const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write(mnemonic);
            write(" ");
            print_op(inst->operand);
            m_out += '\n';
        }

        template <typename I> void print_cmp(std::string_view mnemonic, I const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write(mnemonic);
            write(" ");
            print_op(inst->lhs);
            write(", ");
            print_value(inst->rhs);
            m_out += '\n';
        }

        void print_alloca(IrAllocaInst const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write("alloca ");
            print_type(inst->allocated_type);
            if (inst->count)
            {
                write(", ");
                print_op(inst->count);
            }
            if (inst->alignment)
                std::format_to(std::back_inserter(m_out), ", align={}", inst->alignment);

            m_out += '\n';
        }

        void print_load(bool is_volatile, IrValue const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            IrValue* ptr;
            if (is_volatile)
                ptr = static_cast<IrLoadVolatileInst const*>(inst)->pointer;
            else
                ptr = static_cast<IrLoadInst const*>(inst)->pointer;

            write(is_volatile ? "load.volatile " : "load ");

            print_type(inst->type);
            write(", ");
            print_op(ptr);
            m_out += '\n';
        }

        void print_store(bool is_volatile, IrNode const* inst)
        {
            pad();
            IrValue* val;
            IrValue* ptr;
            if (is_volatile)
            {
                auto* si = static_cast<IrStoreVolatileInst const*>(inst);
                val = si->value;
                ptr = si->pointer;
                write("store.volatile ");
            }
            else
            {
                auto* si = static_cast<IrStoreInst const*>(inst);
                val = si->value;
                ptr = si->pointer;
                write("store ");
            }

            print_op(val);
            write(", ");
            print_op(ptr);
            m_out += '\n';
        }

        static std::string_view memory_ordering_str(IrMemoryOrdering ord)
        {
            switch (ord)
            {
                case IrMemoryOrdering::Relaxed:
                    return "relaxed";
                case IrMemoryOrdering::Acquire:
                    return "acquire";
                case IrMemoryOrdering::Release:
                    return "release";
                case IrMemoryOrdering::AcqRel:
                    return "acq_rel";
                case IrMemoryOrdering::SeqCst:
                    return "seq_cst";
            }
            return "unknown";
        }

        static std::string_view atomic_rmw_op_str(IrAtomicRmwOp op)
        {
            switch (op)
            {
                case IrAtomicRmwOp::Xchg:
                    return "xchg";
                case IrAtomicRmwOp::Add:
                    return "add";
                case IrAtomicRmwOp::Sub:
                    return "sub";
                case IrAtomicRmwOp::And:
                    return "and";
                case IrAtomicRmwOp::Or:
                    return "or";
                case IrAtomicRmwOp::Xor:
                    return "xor";
            }
            return "unknown";
        }

        void print_atomic_load(IrAtomicLoadInst const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write("atomic_load ");
            print_type(inst->type);
            write(", ");
            print_op(inst->pointer);
            std::format_to(std::back_inserter(m_out), " \"{}\"", memory_ordering_str(inst->ordering));
            m_out += '\n';
        }

        void print_atomic_store(IrAtomicStoreInst const* inst)
        {
            pad();
            write("atomic_store ");
            print_op(inst->value);
            write(", ");
            print_op(inst->pointer);
            std::format_to(std::back_inserter(m_out), " \"{}\"", memory_ordering_str(inst->ordering));
            m_out += '\n';
        }

        void print_atomic_rmw(IrAtomicRmwInst const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write("atomic_rmw ");
            write(atomic_rmw_op_str(inst->op));
            write(" ");
            print_op(inst->pointer);
            write(", ");
            print_op(inst->value);
            std::format_to(std::back_inserter(m_out), " \"{}\"", memory_ordering_str(inst->ordering));
            m_out += '\n';
        }

        void print_fence(IrFenceInst const* inst)
        {
            pad();
            write("fence ");
            std::format_to(std::back_inserter(m_out), "\"{}\"", memory_ordering_str(inst->ordering));
            m_out += '\n';
        }

        void print_gep(IrGepInst const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write("gep ");
            print_op(inst->base);
            for (auto const& idx : inst->indices)
                if (idx.kind == IrGepInst::IndexKind::Field)
                    std::format_to(std::back_inserter(m_out), ", field #{}", idx.field_index);
                else
                {
                    write(", ");
                    print_value(idx.dynamic_index);
                }
            m_out += '\n';
        }

        template <typename I> void print_cast(std::string_view mnemonic, I const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write(mnemonic);
            write(" ");
            print_op(inst->operand);
            write(" to ");
            print_type(inst->type);
            m_out += '\n';
        }

        void print_extract(IrExtractInst const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write("extract ");
            print_op(inst->aggregate);
            std::format_to(std::back_inserter(m_out), ", field #{}", inst->field_index);
            m_out += '\n';
        }

        void print_insert(IrInsertInst const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write("insert ");
            print_op(inst->aggregate);
            std::format_to(std::back_inserter(m_out), ", field #{}", inst->field_index);
            write(", ");
            print_op(inst->value);
            m_out += '\n';
        }

        void print_aggregate(IrAggregateInst const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write("aggregate ");
            print_type(inst->type);
            for (auto* v : inst->values)
            {
                write(", ");
                print_value(v);
            }
            m_out += '\n';
        }

        void print_br(IrBrInst const* inst)
        {
            pad();
            write("br ");
            print_label(inst->target);
            m_out += '\n';
        }

        void print_brcond(IrBrCondInst const* inst)
        {
            pad();
            write("br.cond ");
            print_value(inst->condition);
            write(", ");
            print_label(inst->true_target);
            write(", ");
            print_label(inst->false_target);
            m_out += '\n';
        }

        void print_ret(IrRetInst const* inst)
        {
            pad();
            if (inst->value)
            {
                write("ret ");
                print_op(inst->value);
            }
            else
                write("ret void");

            m_out += '\n';
        }

        void print_switch(IrSwitchInst const* inst)
        {
            bool is_unsigned = false;
            if (inst->value && inst->value->type && inst->value->type->kind == IrTypeKind::Int)
            {
                auto* it = static_cast<IrIntType const*>(inst->value->type);
                if (!it->is_signed)
                    is_unsigned = true;
            }

            pad();
            write("switch ");
            print_op(inst->value);
            write(" {\n");
            {
                Indent i(m_indent);
                for (auto const& c : inst->cases)
                {
                    pad();
                    if (c.start == c.end)
                    {
                        if (is_unsigned)
                            std::format_to(std::back_inserter(m_out), "#{}", static_cast<std::uint64_t>(c.start));
                        else
                            std::format_to(std::back_inserter(m_out), "#{}", c.start);
                    }
                    else
                    {
                        if (is_unsigned)
                            std::format_to(std::back_inserter(m_out), "#{}..=#{}", static_cast<std::uint64_t>(c.start), static_cast<std::uint64_t>(c.end));
                        else
                            std::format_to(std::back_inserter(m_out), "#{}..=#{}", c.start, c.end);
                    }

                    write(" => ");
                    print_label(c.target);
                    m_out += '\n';
                }

                pad();
                write("default => ");
                print_label(inst->default_target);
                m_out += '\n';
            }

            pad();
            write("}");
            m_out += '\n';
        }

        void print_phi(IrPhiInst const* inst, std::string_view name)
        {
            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write("phi ");
            print_type(inst->type);
            for (std::size_t i = 0; i < inst->incoming.size(); ++i)
            {
                if (i > 0)
                    write(", ");

                write(" [ ");
                print_op(inst->incoming[i].value);
                write(", ");
                print_label(inst->incoming[i].block);
                write(" ]");
            }
            m_out += '\n';
        }

        void print_call(bool is_tail, IrValue const* call_inst, std::string_view name)
        {
            IrValue const* callee;
            std::span<IrValue* const> args;

            if (is_tail)
            {
                auto* ct = static_cast<IrCallTailInst const*>(call_inst);
                callee = ct->callee;
                args = std::span<IrValue* const>(ct->args.data(), ct->args.size());
            }
            else
            {
                auto* cc = static_cast<IrCallInst const*>(call_inst);
                callee = cc->callee;
                args = std::span<IrValue* const>(cc->args.data(), cc->args.size());
            }

            pad();
            if (!name.empty())
                std::format_to(std::back_inserter(m_out), "%{} = ", name);

            write(is_tail ? "call.tail fn(" : "call fn(");
            print_value(callee);
            write(")(");
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                if (i > 0)
                    write(", ");

                print_op(args[i]);
            }
            write(")");

            if (call_inst->type && call_inst->type->kind != IrTypeKind::Void)
            {
                write(" -> ");
                print_type(call_inst->type);
            }

            m_out += '\n';
        }

        static std::string linkage_str(Linkage l)
        {
            switch (l)
            {
                case Linkage::Internal:
                    return "internal";
                case Linkage::External:
                    return "external";
                case Linkage::LinkOnceODR:
                    return "linkonce_odr";
                case Linkage::WeakODR:
                    return "weak_odr";
            }
            return "internal";
        }

        void print_global(IrGlobal const* g)
        {
            pad();
            if (g->linkage == Linkage::LinkOnceODR)
                write("linkonce_odr ");
            else if (g->linkage == Linkage::WeakODR)
                write("weak_odr ");
            if (g->is_dll_import)
                write("import ");
            if (g->is_dll_export)
                write("export ");
            write(g->is_constant ? "const " : "");
            write("global ");
            write("@");
            write(g->name);
            write(" : ");
            print_type(g->type);
            if (g->alignment)
                std::format_to(std::back_inserter(m_out), " align={}", g->alignment);
            if (!g->section.empty())
                std::format_to(std::back_inserter(m_out), " section=\"{}\"", g->section);

            if (g->init)
            {
                write(" = ");
                print_value(g->init);
            }
            m_out += '\n';
        }

        void print_function(IrFunction const* f)
        {
            for (auto const& attr : f->attrs)
            {
                pad();
                std::format_to(std::back_inserter(m_out), "@{}", attr_name(attr.kind));
                if (!attr.value.empty())
                    std::format_to(std::back_inserter(m_out), "(\"{}\")", attr.value);

                m_out += '\n';
            }

            pad();
            if (f->linkage == Linkage::LinkOnceODR)
                write("linkonce_odr ");
            else if (f->linkage == Linkage::WeakODR)
                write("weak_odr ");
            if (f->is_dll_import)
                write("import ");
            if (f->is_dll_export)
                write("export ");

            auto const* ft = f->func_type;
            if (ft && ft->return_type)
                print_type(ft->return_type);
            else
                write("void ");

            write("@");
            write(f->name);
            write("(");
            if (ft)
            {
                for (std::size_t i = 0; i < ft->params.size(); ++i)
                {
                    if (i > 0)
                        write(", ");

                    print_type(ft->params[i]);
                }
            }
            write(")");
            if (f->alignment)
                std::format_to(std::back_inserter(m_out), " align={}", f->alignment);

            write(" {\n");

            auto const* entry = f->entry_block;
            if (entry)
                print_basic_block(entry);

            for (auto* bb : f->blocks)
                if (bb != entry)
                    print_basic_block(bb);

            pad();
            write("}\n");
        }

        void print_basic_block(IrBasicBlock const* bb)
        {
            pad();
            if (bb->has_name())
                std::format_to(std::back_inserter(m_out), "%{}", bb->name);
            else
                std::format_to(std::back_inserter(m_out), "%bb{}", bb->id);

            if (!bb->params.empty())
            {
                write("(");
                for (std::size_t i = 0; i < bb->params.size(); ++i)
                {
                    if (i > 0)
                        write(", ");

                    auto* p = bb->params[i];
                    if (p->type)
                    {
                        print_type(p->type);
                        write(" ");
                    }
                    print_value(p);
                }
                write(")");
            }

            write(":\n");

            Indent i(m_indent);

            for (auto* inst_val : bb->instructions)
            {
                if (!inst_val)
                    continue;

                if (is_terminator(inst_val->kind))
                    continue;

                print_inst(inst_val, inst_val->name);
            }

            if (bb->terminator)
                print_inst(bb->terminator, {});
        }

        void print_module(IrModule const* m)
        {
            if (!m->name.empty())
                line_fmt("module \"{}\"", m->name);

            if (!m->globals.empty())
                m_out += '\n';

            for (auto* g : m->globals)
            {
                print_global(g);
                m_out += '\n';
            }

            for (std::size_t i = 0; i < m->functions.size(); ++i)
            {
                auto* f = m->functions[i];
                if (!m->globals.empty() || i != 0)
                    m_out += '\n';

                print_function(f);
            }
        }

        static constexpr std::string_view attr_name(IrFuncAttr attr)
        {
            switch (attr)
            {
                case IrFuncAttr::Inline:
                    return "inline";
                case IrFuncAttr::NoInline:
                    return "noinline";
                case IrFuncAttr::NoMangle:
                    return "nomangle";
                case IrFuncAttr::Section:
                    return "section";
                case IrFuncAttr::CallingConv:
                    return "calling_conv";
            }
            return "unknown";
        }
    };

} // namespace dcc::ir
