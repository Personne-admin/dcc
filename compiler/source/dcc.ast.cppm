export module dcc.ast;

import std;
import dcc.sm;
import dcc.lex.tokens;
import dcc.comptime;

namespace dcc::types
{
    struct Type;
}

export namespace dcc::ast
{
    struct TypeExpr;
    struct Expr;
    struct Stmt;
    struct Decl;
    struct FuncDecl;
    struct VarDecl;
    struct Pattern;
    struct EnumVariant;

    using Allocator = std::pmr::polymorphic_allocator<>;

    using TypePtr = TypeExpr*;
    using ExprPtr = Expr*;
    using StmtPtr = Stmt*;
    using DeclPtr = Decl*;
    using PatternPtr = Pattern*;

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

    [[nodiscard]] constexpr Qual operator&(Qual a, Qual b) noexcept
    {
        return static_cast<Qual>(std::to_underlying(a) & std::to_underlying(b));
    }

    constexpr Qual& operator|=(Qual& a, Qual b) noexcept
    {
        return a = a | b;
    }

    [[nodiscard]] constexpr bool has_qual(Qual set, Qual flag) noexcept
    {
        return (set & flag) != Qual::None;
    }

    struct PathSegment
    {
        std::string_view name;
        sm::SourceRange range;
    };

    struct Path
    {
        std::pmr::vector<PathSegment> segments;
        sm::SourceRange range;

        explicit Path(Allocator a) : segments(a) {}
        Path(sm::SourceRange r, Allocator a) : segments(a), range(r) {}

        [[nodiscard]] bool is_empty() const noexcept { return segments.empty(); }
        [[nodiscard]] bool is_simple() const noexcept { return segments.size() == 1; }
        [[nodiscard]] std::string_view simple_name() const noexcept { return segments.empty() ? std::string_view{} : segments.back().name; }
        [[nodiscard]] std::string_view tail_name() const noexcept { return simple_name(); }
    };

    struct Attribute
    {
        std::string_view name;
        sm::SourceRange range;
        std::pmr::vector<ExprPtr> args;

        explicit Attribute(Allocator a) : args(a) {}
    };

    enum class TypeKind : std::uint8_t
    {
        Primitive,
        Named,
        Pointer,
        Array,
        Slice,
        Fam,
        FuncPtr,
        Qualified,
        PackIndex,
    };

    enum class ExprKind : std::uint8_t
    {
        IntLiteral,
        FloatLiteral,
        StringLiteral,
        U16StringLiteral,
        CharLiteral,
        U16CharLiteral,
        BoolLiteral,
        NullLiteral,
        Ident,
        PathExpr,
        Unary,
        Postfix,
        Binary,
        Call,
        FieldAccess,
        Index,
        PackAccess,
        Cast,
        Block,
        If,
        Match,
        StructLiteral,
        Sizeof,
        Alignof,
        Offsetof,
        Compiles,
        Range,
        TypeAST,
        TemplateInst,
        SizeofPack,
        PackExpansion,
    };

    enum class StmtKind : std::uint8_t
    {
        Expr,
        DeclStmt,
        Return,
        Break,
        Continue,
        While,
        DoWhile,
        For,
        ForIn,
        Defer,
        StaticIf,
        StaticMatch,
        StaticFor,
        Ambiguous
    };

    enum class PatternKind : std::uint8_t
    {
        Literal,
        Binding,
        Wildcard,
        EnumDestructure,
        StructDestructure,
        Range,
        Or,
        Ref,
    };

    enum class DeclKind : std::uint8_t
    {
        Module,
        Import,
        Using,
        Struct,
        Union,
        Enum,
        Func,
        Var,
        StaticIfGroup,
    };

    enum class UsingKind : std::uint8_t
    {
        Alias,
        BareImport,
        Wildcard,
        List,
        Concept,
    };

    struct UsingItem
    {
        Path path;
        std::pmr::vector<UsingItem*> children;
        sm::SourceRange range;

        explicit UsingItem(Allocator a) : path(a), children(a) {}
    };

    enum class StorageClass : std::uint8_t
    {
        Unresolved,
        Local,
        ModuleGlobal,
        Static,
        Extern,
        Param,
    };

    struct ExprSema
    {
        types::Type const* resolved_type{};
        Decl const* resolved_decl{};
        FuncDecl const* resolved_specialization{};
        Decl const* ufcs_callee{};
        bool is_type_instantiation{};
        enum class ConstructionKind : std::uint8_t
        {
            None,
            Struct,
            Array,
            Slice,
            Enum,
        };

        ConstructionKind construction_kind{ConstructionKind::None};

        EnumVariant const* constructed_variant{};

        dcc::comptime::Value const* const_value{};

        bool is_lvalue : 1 {};
        bool is_constant : 1 {};
        bool is_diverging : 1 {};
        bool implicit_deref : 1 {};
        bool implicit_addr_of : 1 {};
    };

    struct TypeSema
    {
        types::Type const* canonical{};
        Decl const* resolved_decl{};
        std::uint64_t byte_size{};
        std::uint32_t byte_align{};
        bool is_complete : 1 {};
        bool is_zero_sized : 1 {};
        bool layout_is_default : 1 {true};
    };

    struct DeclSema
    {
        std::string_view mangled_name;

        std::uint64_t byte_size{};
        std::uint32_t byte_align{};

        StorageClass storage{StorageClass::Unresolved};
        std::uint32_t frame_offset{};
        std::uint32_t param_index{};

        bool is_inline : 1 {};
        bool is_intrinsic : 1 {};
        bool is_nominal : 1 {};
        bool is_nomangle : 1 {};
        bool is_noinline : 1 {};
        bool is_dll_import : 1 {};
        bool is_dll_export : 1 {};

        bool layout_resolved : 1 {};
        bool is_diverging : 1 {};

        bool exported : 1 {};
        bool spilled : 1 {};

        std::string_view section;
        std::string_view calling_conv;
        std::uint32_t alignment{};
    };

    struct TypeExpr
    {
        TypeKind kind;
        sm::SourceRange range;
        TypeSema sema;

    protected:
        TypeExpr(TypeKind k, sm::SourceRange r) : kind(k), range(r) {}
    };

    struct Expr
    {
        ExprKind kind;
        sm::SourceRange range;
        ExprSema sema;

    protected:
        Expr(ExprKind k, sm::SourceRange r) : kind(k), range(r) {}
    };

    struct Stmt
    {
        StmtKind kind;
        sm::SourceRange range;

    protected:
        Stmt(StmtKind k, sm::SourceRange r) : kind(k), range(r) {}
    };

    struct Pattern
    {
        PatternKind kind;
        sm::SourceRange range;

        types::Type const* matched_type{};

    protected:
        Pattern(PatternKind k, sm::SourceRange r) : kind(k), range(r) {}
    };

    struct Decl
    {
        DeclKind kind;
        sm::SourceRange range;
        bool is_public{false};
        bool is_extern{false};
        std::pmr::vector<Attribute> attrs;
        DeclSema sema;

    protected:
        Decl(DeclKind k, sm::SourceRange r, Allocator a) : kind(k), range(r), attrs(a) {}
    };

    struct TemplateParam
    {
        std::string_view name;
        sm::SourceRange range;
        TypePtr value_type{};
        bool is_pack : 1 {};
    };

    struct TemplateArg
    {
        enum class ResolvedAs : std::uint8_t
        {
            Unresolved,
            Type,
            Value,
        };

        sm::SourceRange range;
        TypePtr type{};
        ExprPtr expr{};
        ResolvedAs resolved_as{ResolvedAs::Unresolved};
    };

    struct FuncParam
    {
        std::string_view name;
        sm::SourceRange range;
        TypePtr type{};
        DeclSema sema;
        VarDecl const* synthetic_decl{};
        bool is_pack : 1 {};
    };

    struct Block
    {
        sm::SourceRange range;
        std::pmr::vector<StmtPtr> stmts;
        ExprPtr tail{};
        std::pmr::vector<sm::SourceRange> exit_defers;

        explicit Block(Allocator a) : stmts(a), exit_defers(a) {}
        Block(sm::SourceRange r, Allocator a) : range(r), stmts(a), exit_defers(a) {}
    };

    struct MatchArm
    {
        PatternPtr pattern{};
        TypePtr type_pattern{};
        sm::SourceRange range;
        ExprPtr guard{};
        ExprPtr body{};
        bool is_wildcard{false};
    };

    struct FieldDecl
    {
        std::string_view name;
        sm::SourceRange range;
        sm::SourceRange name_range;
        TypePtr type{};

        std::uint32_t byte_offset{};
        std::uint32_t index{};
    };

    struct EnumVariant
    {
        std::string_view name;
        sm::SourceRange range;
        std::pmr::vector<Attribute> attrs;
        ExprPtr explicit_value{};
        std::pmr::vector<TypePtr> payload;

        std::int64_t discriminant{};
        bool discriminant_is_negative{};

        explicit EnumVariant(Allocator a) : attrs(a), payload(a) {}
    };

    struct StructLiteralField
    {
        std::string_view name;
        sm::SourceRange name_range;
        sm::SourceRange range;
        ExprPtr value{};

        std::uint32_t resolved_field_index{};
    };

    struct StructPatternField
    {
        std::string_view field_name;
        sm::SourceRange range;
        PatternPtr pattern{};

        std::uint32_t resolved_field_index{};
    };

    struct CompilesParam
    {
        std::string_view name;
        sm::SourceRange range;
        TypePtr type{};
        bool is_pack : 1 {};
    };

    struct PrimitiveType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::Primitive;
        lex::TokenKind which;
        PrimitiveType(sm::SourceRange r, lex::TokenKind w) : TypeExpr(Kind, r), which(w) {}
    };

    struct NamedType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::Named;
        Path path;
        std::pmr::vector<TemplateArg> template_args;
        NamedType(sm::SourceRange r, Path p, Allocator a) : TypeExpr(Kind, r), path(std::move(p)), template_args(a) {}
    };

    struct PointerType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::Pointer;
        TypePtr pointee;
        PointerType(sm::SourceRange r, TypePtr pt) : TypeExpr(Kind, r), pointee(pt) {}
    };

    struct ArrayType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::Array;
        TypePtr element;
        ExprPtr size;
        ArrayType(sm::SourceRange r, TypePtr el, ExprPtr sz) : TypeExpr(Kind, r), element(el), size(sz) {}
    };

    struct SliceType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::Slice;
        TypePtr element;
        SliceType(sm::SourceRange r, TypePtr el) : TypeExpr(Kind, r), element(el) {}
    };

    struct FamType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::Fam;
        TypePtr element;
        FamType(sm::SourceRange r, TypePtr el) : TypeExpr(Kind, r), element(el) {}
    };

    struct FuncPtrType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::FuncPtr;
        TypePtr return_type;
        std::pmr::vector<TypePtr> params;
        FuncPtrType(sm::SourceRange r, TypePtr ret, Allocator a) : TypeExpr(Kind, r), return_type(ret), params(a) {}
    };

    struct QualifiedType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::Qualified;
        Qual quals;
        TypePtr inner;
        QualifiedType(sm::SourceRange r, Qual q, TypePtr t) : TypeExpr(Kind, r), quals(q), inner(t) {}
    };

    struct PackIndexType : TypeExpr
    {
        static constexpr auto Kind = TypeKind::PackIndex;
        TypePtr base;
        ExprPtr index;
        PackIndexType(sm::SourceRange r, TypePtr b, ExprPtr i) : TypeExpr(Kind, r), base(b), index(i) {}
    };

    struct IntLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::IntLiteral;
        std::int64_t value;
        std::string_view spelling;
        IntLiteralExpr(sm::SourceRange r, std::int64_t v, std::string_view sp) : Expr(Kind, r), value(v), spelling(sp) {}
    };

    struct FloatLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::FloatLiteral;
        double value;
        std::string_view spelling;
        FloatLiteralExpr(sm::SourceRange r, double v, std::string_view sp) : Expr(Kind, r), value(v), spelling(sp) {}
    };

    struct StringLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::StringLiteral;
        std::pmr::string value;
        std::string_view spelling;
        StringLiteralExpr(sm::SourceRange r, std::string_view v, std::string_view sp, Allocator a) : Expr(Kind, r), value(v, a), spelling(sp) {}
    };

    struct U16StringLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::U16StringLiteral;
        std::pmr::u16string value;
        std::string_view spelling;
        U16StringLiteralExpr(sm::SourceRange r, std::u16string_view v, std::string_view sp, Allocator a) : Expr(Kind, r), value(v, a), spelling(sp) {}
    };

    struct CharLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::CharLiteral;
        std::uint32_t codepoint;
        CharLiteralExpr(sm::SourceRange r, std::uint32_t cp) : Expr(Kind, r), codepoint(cp) {}
    };

    struct U16CharLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::U16CharLiteral;
        std::uint32_t value;
        U16CharLiteralExpr(sm::SourceRange r, std::uint32_t v) : Expr(Kind, r), value(v) {}
    };

    struct BoolLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::BoolLiteral;
        bool value;
        BoolLiteralExpr(sm::SourceRange r, bool v) : Expr(Kind, r), value(v) {}
    };

    struct NullLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::NullLiteral;
        explicit NullLiteralExpr(sm::SourceRange r) : Expr(Kind, r) {}
    };

    struct IdentExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Ident;
        std::string_view name;
        IdentExpr(sm::SourceRange r, std::string_view n) : Expr(Kind, r), name(n) {}
    };

    struct PathExpr : Expr
    {
        static constexpr auto Kind = ExprKind::PathExpr;
        Path path;
        std::pmr::vector<TemplateArg> explicit_enum_args;
        PathExpr(sm::SourceRange r, Path p, Allocator a) : Expr(Kind, r), path(std::move(p)), explicit_enum_args(a) {}
    };

    struct UnaryExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Unary;
        lex::TokenKind op;
        ExprPtr operand;
        UnaryExpr(sm::SourceRange r, lex::TokenKind o, ExprPtr operand) : Expr(Kind, r), op(o), operand(operand) {}
    };

    struct PostfixExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Postfix;
        ExprPtr operand;
        lex::TokenKind op;
        PostfixExpr(sm::SourceRange r, ExprPtr operand, lex::TokenKind o) : Expr(Kind, r), operand(operand), op(o) {}
    };

    struct BinaryExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Binary;
        ExprPtr lhs;
        lex::TokenKind op;
        ExprPtr rhs;
        BinaryExpr(sm::SourceRange r, ExprPtr l, lex::TokenKind o, ExprPtr rr) : Expr(Kind, r), lhs(l), op(o), rhs(rr) {}
    };

    struct CallExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Call;
        ExprPtr callee;
        std::pmr::vector<ExprPtr> args;
        CallExpr(sm::SourceRange r, ExprPtr callee, Allocator a) : Expr(Kind, r), callee(callee), args(a) {}
    };

    struct FieldAccessExpr : Expr
    {
        static constexpr auto Kind = ExprKind::FieldAccess;
        ExprPtr object;
        std::string_view field;
        sm::SourceRange field_range;
        FieldAccessExpr(sm::SourceRange r, ExprPtr obj, std::string_view f, sm::SourceRange fr) : Expr(Kind, r), object(obj), field(f), field_range(fr) {}
    };

    struct IndexExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Index;
        ExprPtr object;
        ExprPtr index;
        IndexExpr(sm::SourceRange r, ExprPtr obj, ExprPtr idx) : Expr(Kind, r), object(obj), index(idx) {}
    };

    struct PackAccessExpr : Expr
    {
        static constexpr auto Kind = ExprKind::PackAccess;
        ExprPtr object;
        ExprPtr index;
        PackAccessExpr(sm::SourceRange r, ExprPtr obj, ExprPtr idx) : Expr(Kind, r), object(obj), index(idx) {}
    };

    struct CastExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Cast;
        ExprPtr operand;
        TypePtr target;
        CastExpr(sm::SourceRange r, ExprPtr operand, TypePtr target) : Expr(Kind, r), operand(operand), target(target) {}
    };

    struct BlockExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Block;
        Block body;
        BlockExpr(sm::SourceRange r, Block b) : Expr(Kind, r), body(std::move(b)) {}
    };

    struct IfExpr : Expr
    {
        static constexpr auto Kind = ExprKind::If;
        ExprPtr condition;
        Block then_block;
        ExprPtr else_branch{};
        IfExpr(sm::SourceRange r, ExprPtr cond, Block then_blk) : Expr(Kind, r), condition(cond), then_block(std::move(then_blk)) {}
    };

    struct MatchExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Match;
        ExprPtr operand;
        std::pmr::vector<MatchArm> arms;
        MatchExpr(sm::SourceRange r, ExprPtr operand, Allocator a) : Expr(Kind, r), operand(operand), arms(a) {}
    };

    struct StructLiteralExpr : Expr
    {
        static constexpr auto Kind = ExprKind::StructLiteral;
        TypePtr type{};
        std::pmr::vector<StructLiteralField> fields;
        explicit StructLiteralExpr(sm::SourceRange r, Allocator a) : Expr(Kind, r), fields(a) {}
    };

    struct SizeofExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Sizeof;
        TypePtr target;
        SizeofExpr(sm::SourceRange r, TypePtr t) : Expr(Kind, r), target(t) {}
    };

    struct SizeofPackExpr : Expr
    {
        static constexpr auto Kind = ExprKind::SizeofPack;
        std::string_view pack_name;
        sm::SourceRange name_range;
        SizeofPackExpr(sm::SourceRange r, std::string_view n, sm::SourceRange nr) : Expr(Kind, r), pack_name(n), name_range(nr) {}
    };

    struct PackExpansionExpr : Expr
    {
        static constexpr auto Kind = ExprKind::PackExpansion;
        ExprPtr operand;
        PackExpansionExpr(sm::SourceRange r, ExprPtr op) : Expr(Kind, r), operand(op) {}
    };

    struct AlignofExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Alignof;
        TypePtr target;
        AlignofExpr(sm::SourceRange r, TypePtr t) : Expr(Kind, r), target(t) {}
    };

    struct OffsetofExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Offsetof;
        TypePtr target;
        std::string_view field;
        OffsetofExpr(sm::SourceRange r, TypePtr t, std::string_view f) : Expr(Kind, r), target(t), field(f) {}
    };

    struct CompilesExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Compiles;
        std::pmr::vector<CompilesParam> params;
        Block body;

        bool value{};
        bool resolved{};

        CompilesExpr(sm::SourceRange r, Allocator a) : Expr(Kind, r), params(a), body(a) {}
    };

    struct TypeASTExpr : Expr
    {
        static constexpr auto Kind = ExprKind::TypeAST;
        TypeExpr* type_node;

        TypeASTExpr(sm::SourceRange r, TypeExpr* t) : Expr(Kind, r), type_node(t) {}
    };

    struct TemplateInstExpr : Expr
    {
        static constexpr auto Kind = ExprKind::TemplateInst;
        ExprPtr callee;
        std::pmr::vector<TemplateArg> template_args;
        TemplateInstExpr(sm::SourceRange r, ExprPtr callee, Allocator a) : Expr(Kind, r), callee(callee), template_args(a) {}
    };

    struct RangeExpr : Expr
    {
        static constexpr auto Kind = ExprKind::Range;
        ExprPtr start;
        ExprPtr end;
        bool inclusive;
        RangeExpr(sm::SourceRange r, ExprPtr s, ExprPtr e, bool incl = false) : Expr(Kind, r), start(s), end(e), inclusive(incl) {}
    };

    struct LiteralPattern : Pattern
    {
        static constexpr auto Kind = PatternKind::Literal;
        ExprPtr value;
        LiteralPattern(sm::SourceRange r, ExprPtr v) : Pattern(Kind, r), value(v) {}
    };

    struct BindingPattern : Pattern
    {
        static constexpr auto Kind = PatternKind::Binding;
        std::string_view name;
        bool by_reference{false};
        BindingPattern(sm::SourceRange r, std::string_view n) : Pattern(Kind, r), name(n) {}
    };

    struct RefPattern : Pattern
    {
        static constexpr auto Kind = PatternKind::Ref;
        PatternPtr inner;
        RefPattern(sm::SourceRange r, PatternPtr i) : Pattern(Kind, r), inner(i) {}
    };

    struct WildcardPattern : Pattern
    {
        static constexpr auto Kind = PatternKind::Wildcard;
        explicit WildcardPattern(sm::SourceRange r) : Pattern(Kind, r) {}
    };

    struct EnumDestructurePattern : Pattern
    {
        static constexpr auto Kind = PatternKind::EnumDestructure;
        Path variant_path;
        std::pmr::vector<PatternPtr> payload;

        EnumVariant const* resolved_variant{};
        bool has_parens{false};

        EnumDestructurePattern(sm::SourceRange r, Path vp, Allocator a) : Pattern(Kind, r), variant_path(std::move(vp)), payload(a) {}
    };

    struct StructDestructurePattern : Pattern
    {
        static constexpr auto Kind = PatternKind::StructDestructure;
        Path type_path;
        std::pmr::vector<StructPatternField> fields;
        bool has_rest{};
        StructDestructurePattern(sm::SourceRange r, Path tp, Allocator a) : Pattern(Kind, r), type_path(std::move(tp)), fields(a) {}
    };

    struct RangePattern : Pattern
    {
        static constexpr auto Kind = PatternKind::Range;
        ExprPtr start;
        ExprPtr end;
        bool inclusive;
        RangePattern(sm::SourceRange r, ExprPtr s, ExprPtr e, bool incl = false) : Pattern(Kind, r), start(s), end(e), inclusive(incl) {}
    };

    struct OrPattern : Pattern
    {
        static constexpr auto Kind = PatternKind::Or;
        std::pmr::vector<PatternPtr> alternatives;
        OrPattern(sm::SourceRange r, Allocator a) : Pattern(Kind, r), alternatives(a) {}
    };

    struct ExprStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::Expr;
        ExprPtr expr;
        ExprStmt(sm::SourceRange r, ExprPtr e) : Stmt(Kind, r), expr(e) {}
    };

    struct DeclStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::DeclStmt;
        DeclPtr decl;
        DeclStmt(sm::SourceRange r, DeclPtr d) : Stmt(Kind, r), decl(d) {}
    };

    struct ReturnStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::Return;
        ExprPtr value{};
        std::pmr::vector<sm::SourceRange> exit_defers;
        ReturnStmt(sm::SourceRange r, Allocator a) : Stmt(Kind, r), exit_defers(a) {}
    };

    struct BreakStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::Break;
        std::pmr::vector<sm::SourceRange> exit_defers;
        BreakStmt(sm::SourceRange r, Allocator a) : Stmt(Kind, r), exit_defers(a) {}
    };

    struct ContinueStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::Continue;
        std::pmr::vector<sm::SourceRange> exit_defers;
        ContinueStmt(sm::SourceRange r, Allocator a) : Stmt(Kind, r), exit_defers(a) {}
    };

    struct WhileStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::While;
        ExprPtr condition;
        Block body;
        WhileStmt(sm::SourceRange r, ExprPtr cond, Block b) : Stmt(Kind, r), condition(cond), body(std::move(b)) {}
    };

    struct DoWhileStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::DoWhile;
        Block body;
        ExprPtr condition;
        DoWhileStmt(sm::SourceRange r, Block b, ExprPtr cond) : Stmt(Kind, r), body(std::move(b)), condition(cond) {}
    };

    struct ForStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::For;
        StmtPtr init{};
        ExprPtr cond{};
        ExprPtr update{};
        Block body;
        ForStmt(sm::SourceRange r, Block b) : Stmt(Kind, r), body(std::move(b)) {}
    };

    struct ForInStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::ForIn;
        TypePtr item_type{};
        std::string_view item_name;
        sm::SourceRange name_range;
        ExprPtr iterable{};
        Block body;

        types::Type const* resolved_item_type{};
        bool by_reference{false};

        ForInStmt(sm::SourceRange r, Block b) : Stmt(Kind, r), body(std::move(b)) {}
    };

    struct DeferStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::Defer;
        StmtPtr body;
        DeferStmt(sm::SourceRange r, StmtPtr b) : Stmt(Kind, r), body(b) {}
    };

    struct StaticIfStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::StaticIf;
        ExprPtr condition;
        Block then_block;
        StmtPtr else_branch{};

        std::int8_t taken_branch{-1};
        bool is_type_if{false};

        StaticIfStmt(sm::SourceRange r, ExprPtr cond, Block then_blk) : Stmt(Kind, r), condition(cond), then_block(std::move(then_blk)) {}
    };

    struct StaticMatchStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::StaticMatch;
        ExprPtr operand;
        std::pmr::vector<MatchArm> arms;

        std::int32_t taken_arm{-1};
        bool is_type_match{false};

        StaticMatchStmt(sm::SourceRange r, ExprPtr op, Allocator a) : Stmt(Kind, r), operand(op), arms(a) {}
    };

    struct StaticForStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::StaticFor;
        std::string_view item_name;
        sm::SourceRange name_range;
        ExprPtr pack_expr{};
        Block body;

        bool is_range_for : 1 {};
        bool is_type_for : 1 {};
        types::Type const* resolved_pack_type{};

        StaticForStmt(sm::SourceRange r, Block b) : Stmt(Kind, r), body(std::move(b)) {}
    };

    struct AmbiguousStmt : Stmt
    {
        static constexpr auto Kind = StmtKind::Ambiguous;

        enum class Resolution : std::uint8_t
        {
            Unresolved,
            AsDecl,
            AsExpr,
        };

        DeclPtr as_decl{};
        ExprPtr as_expr{};
        Resolution resolution{Resolution::Unresolved};

        AmbiguousStmt(sm::SourceRange r, DeclPtr d, ExprPtr e) : Stmt(Kind, r), as_decl(d), as_expr(e) {}
    };

    struct ModuleDecl : Decl
    {
        static constexpr auto Kind = DeclKind::Module;
        Path module_path;
        sm::SourceRange name_range;
        ModuleDecl(sm::SourceRange r, Path p, sm::SourceRange nr, Allocator a) : Decl(Kind, r, a), module_path(std::move(p)), name_range(nr) {}
    };

    struct ImportDecl : Decl
    {
        static constexpr auto Kind = DeclKind::Import;
        Path module_path;
        sm::SourceRange name_range;

        Decl const* resolved_module{};

        ImportDecl(sm::SourceRange r, Path p, sm::SourceRange nr, Allocator a) : Decl(Kind, r, a), module_path(std::move(p)), name_range(nr) {}
    };

    struct UsingDecl : Decl
    {
        static constexpr auto Kind = DeclKind::Using;

        UsingKind using_kind{};
        bool is_spill{};

        Path alias_path;
        sm::SourceRange name_range;
        std::pmr::vector<TemplateParam> template_params;

        TypePtr target_type{};
        Path target_path;
        std::pmr::vector<Path> target_list;
        std::pmr::vector<UsingItem*> target_items;
        ExprPtr target_expr{};

        struct ResolvedBinding
        {
            std::string_view name;
            Decl const* decl;
        };
        std::pmr::vector<ResolvedBinding> resolved_bindings;

        UsingDecl(sm::SourceRange r, Allocator a)
            : Decl(Kind, r, a), alias_path(a), template_params(a), target_path(a), target_list(a), target_items(a), resolved_bindings(a)
        {
        }
    };

    struct StructDecl : Decl
    {
        static constexpr auto Kind = DeclKind::Struct;
        std::string_view name;
        sm::SourceRange name_range;
        std::pmr::vector<TemplateParam> template_params;
        std::pmr::vector<FieldDecl> fields;
        ExprPtr constraint{};
        StructDecl(sm::SourceRange r, std::string_view n, sm::SourceRange nr, Allocator a)
            : Decl(Kind, r, a), name(n), name_range(nr), template_params(a), fields(a)
        {
        }
    };

    struct UnionDecl : Decl
    {
        static constexpr auto Kind = DeclKind::Union;
        std::string_view name;
        sm::SourceRange name_range;
        std::pmr::vector<FieldDecl> fields;
        UnionDecl(sm::SourceRange r, std::string_view n, sm::SourceRange nr, Allocator a) : Decl(Kind, r, a), name(n), name_range(nr), fields(a) {}
    };

    struct EnumDecl : Decl
    {
        static constexpr auto Kind = DeclKind::Enum;
        std::string_view name;
        sm::SourceRange name_range;
        std::pmr::vector<TemplateParam> template_params;
        TypePtr backing_type{};
        std::pmr::vector<EnumVariant> variants;
        ExprPtr constraint{};
        bool is_tagged{};
        EnumDecl(sm::SourceRange r, std::string_view n, sm::SourceRange nr, Allocator a)
            : Decl(Kind, r, a), name(n), name_range(nr), template_params(a), variants(a)
        {
        }
    };

    struct FuncDecl : Decl
    {
        static constexpr auto Kind = DeclKind::Func;
        TypePtr return_type{};
        std::string_view name;
        sm::SourceRange name_range;
        std::pmr::vector<TemplateParam> template_params;
        std::pmr::vector<FuncParam> params;
        ExprPtr constraint{};
        std::optional<Block> body;
        FuncDecl(sm::SourceRange r, std::string_view n, sm::SourceRange nr, Allocator a)
            : Decl(Kind, r, a), name(n), name_range(nr), template_params(a), params(a)
        {
        }
    };

    struct VarDecl : Decl
    {
        static constexpr auto Kind = DeclKind::Var;
        TypePtr type{};
        std::string_view name;
        sm::SourceRange name_range;
        ExprPtr init{};
        VarDecl(sm::SourceRange r, std::string_view n, sm::SourceRange nr, Allocator a) : Decl(Kind, r, a), name(n), name_range(nr) {}
    };

    struct StaticIfGroup : Decl
    {
        static constexpr auto Kind = DeclKind::StaticIfGroup;
        ExprPtr condition{};
        std::pmr::vector<DeclPtr> then_decls;
        StaticIfGroup* else_group{};
        std::int8_t taken_branch{-1};

        StaticIfGroup(sm::SourceRange r, ExprPtr cond, Allocator a) : Decl(Kind, r, a), condition(cond), then_decls(a) {}
    };

    struct TranslationUnit
    {
        sm::SourceRange range;
        DeclPtr module_decl{};
        std::pmr::vector<DeclPtr> imports;
        std::pmr::vector<DeclPtr> decls;

        explicit TranslationUnit(Allocator a) : imports(a), decls(a) {}
    };

    class AstContext
    {
    public:
        explicit AstContext(std::size_t initial_buffer = 64 * 1024) : m_buffer(initial_buffer) {}

        AstContext(const AstContext&) = delete;
        AstContext& operator=(const AstContext&) = delete;
        AstContext(AstContext&&) = delete;
        AstContext& operator=(AstContext&&) = delete;
        ~AstContext() = default;

        [[nodiscard]] std::pmr::memory_resource* resource() noexcept { return &m_buffer; }
        [[nodiscard]] Allocator allocator() noexcept { return Allocator{&m_buffer}; }

        template <typename T, typename... Args> [[nodiscard]] T* make(Args&&... args)
        {
            void* p = m_buffer.allocate(sizeof(T), alignof(T));
            if constexpr (std::is_constructible_v<T, Args..., Allocator>)
                return ::new (p) T(std::forward<Args>(args)..., allocator());
            else
                return ::new (p) T(std::forward<Args>(args)...);
        }

        [[nodiscard]] void* allocate_raw(std::size_t bytes, std::size_t align) { return m_buffer.allocate(bytes, align); }

        [[nodiscard]] comptime::Value const* own_value(comptime::Value v)
        {
            m_owned_values.push_back(std::make_unique<comptime::Value>(std::move(v)));
            return m_owned_values.back().get();
        }

    private:
        std::pmr::monotonic_buffer_resource m_buffer;
        std::vector<std::unique_ptr<comptime::Value>> m_owned_values;
    };

    template <typename To, typename From> [[nodiscard]] To* node_cast(From* node) noexcept
    {
        return (node && node->kind == To::Kind) ? static_cast<To*>(node) : nullptr;
    }

    template <typename To, typename From> [[nodiscard]] To const* node_cast(From const* node) noexcept
    {
        return (node && node->kind == To::Kind) ? static_cast<To const*>(node) : nullptr;
    }

    [[nodiscard]] constexpr bool is_primitive_type(lex::TokenKind k) noexcept
    {
        return k >= lex::TokenKind::Kwu8 && k <= lex::TokenKind::KwIsize;
    }

    [[nodiscard]] constexpr bool is_qualifier(lex::TokenKind k) noexcept
    {
        return k == lex::TokenKind::KwConst || k == lex::TokenKind::KwVolatile || k == lex::TokenKind::KwRestrict;
    }

    [[nodiscard]] constexpr bool is_type_start(lex::TokenKind k) noexcept
    {
        return is_primitive_type(k) || is_qualifier(k) || k == lex::TokenKind::Identifier || k == lex::TokenKind::LBracket;
    }

} // namespace dcc::ast
