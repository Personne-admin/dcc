export module dcc.lex.tokens;

import std;
import dcc.si;
import dcc.sm;

export namespace dcc::lex
{
    enum class TokenKind : std::uint16_t
    {
        Eof,
        Invalid,

        Identifier,
        IntLiteral,
        FloatLiteral,
        StringLiteral,
        U16StringLiteral,
        CharLiteral,
        U16CharLiteral,

        Kwu8,
        Kwi8,
        Kwu16,
        Kwi16,
        Kwu32,
        Kwi32,
        Kwu64,
        Kwi64,
        Kwf32,
        Kwf64,
        KwChar,
        KwVoid,
        KwBool,
        KwNullT,
        KwUsize,
        KwIsize,

        KwNull,
        KwTrue,
        KwFalse,

        KwStruct,
        KwEnum,
        KwUnion,
        KwUsing,

        KwConst,
        KwRestrict,
        KwVolatile,

        KwIf,
        KwElse,
        KwWhile,
        KwFor,
        KwDo,
        KwMatch,
        KwBreak,
        KwContinue,
        KwReturn,
        KwDefer,

        KwImport,
        KwModule,
        KwPublic,

        KwAs,
        KwIn,

        KwSizeof,
        KwAlignof,
        KwOffsetof,
        KwCompiles,

        KwStatic,
        KwExtern,

        KwAsm,

        LParen,
        RParen,
        LBracket,
        RBracket,
        LBrace,
        RBrace,

        Dot,
        DotDot,
        Ellipsis,

        Comma,

        Semicolon,
        Colon,
        ColonColon,

        Arrow,
        FatArrow,

        At,
        Hash,
        Tilde,
        Question,
        Dollar,

        Plus,
        Minus,
        Star,
        Slash,
        Percent,
        Increment,
        Decrement,

        Amp,
        Pipe,
        Caret,
        Bang,
        AmpAmp,
        PipePipe,

        Eq,
        EqEq,
        BangEq,
        Lt,
        Gt,
        LtEq,
        GtEq,

        LtLt,
        GtGt,

        PlusEq,
        MinusEq,
        StarEq,
        SlashEq,
        PercentEq,
        AmpEq,
        PipeEq,
        CaretEq,
        LtLtEq,
        GtGtEq,
    };

    [[nodiscard]] std::string_view to_string(TokenKind kind) noexcept
    {
        switch (kind)
        {
            case TokenKind::Eof:
                return "eof";
            case TokenKind::Invalid:
                return "invalid";
            case TokenKind::Identifier:
                return "identifier";
            case TokenKind::IntLiteral:
                return "integer literal";
            case TokenKind::FloatLiteral:
                return "float literal";
            case TokenKind::StringLiteral:
                return "string literal";
            case TokenKind::U16StringLiteral:
                return "utf-16 string literal";
            case TokenKind::CharLiteral:
                return "char literal";
            case TokenKind::U16CharLiteral:
                return "utf-16 char literal";

            case TokenKind::Kwu8:
                return "u8";
            case TokenKind::Kwi8:
                return "i8";
            case TokenKind::Kwu16:
                return "u16";
            case TokenKind::Kwi16:
                return "i16";
            case TokenKind::Kwu32:
                return "u32";
            case TokenKind::Kwi32:
                return "i32";
            case TokenKind::Kwu64:
                return "u64";
            case TokenKind::Kwi64:
                return "i64";
            case TokenKind::Kwf32:
                return "f32";
            case TokenKind::Kwf64:
                return "f64";
            case TokenKind::KwChar:
                return "char";
            case TokenKind::KwVoid:
                return "void";
            case TokenKind::KwBool:
                return "bool";
            case TokenKind::KwNullT:
                return "null_t";
            case TokenKind::KwUsize:
                return "usize";
            case TokenKind::KwIsize:
                return "isize";

            case TokenKind::KwNull:
                return "null";
            case TokenKind::KwTrue:
                return "true";
            case TokenKind::KwFalse:
                return "false";

            case TokenKind::KwStruct:
                return "struct";
            case TokenKind::KwEnum:
                return "enum";
            case TokenKind::KwUnion:
                return "union";
            case TokenKind::KwUsing:
                return "using";
            case TokenKind::KwConst:
                return "const";
            case TokenKind::KwRestrict:
                return "restrict";
            case TokenKind::KwVolatile:
                return "volatile";

            case TokenKind::KwIf:
                return "if";
            case TokenKind::KwElse:
                return "else";
            case TokenKind::KwWhile:
                return "while";
            case TokenKind::KwFor:
                return "for";
            case TokenKind::KwDo:
                return "do";
            case TokenKind::KwMatch:
                return "match";
            case TokenKind::KwBreak:
                return "break";
            case TokenKind::KwContinue:
                return "continue";
            case TokenKind::KwReturn:
                return "return";
            case TokenKind::KwDefer:
                return "defer";

            case TokenKind::KwImport:
                return "import";
            case TokenKind::KwModule:
                return "module";
            case TokenKind::KwPublic:
                return "public";

            case TokenKind::KwAs:
                return "as";
            case TokenKind::KwIn:
                return "in";
            case TokenKind::KwSizeof:
                return "sizeof";
            case TokenKind::KwAlignof:
                return "alignof";
            case TokenKind::KwOffsetof:
                return "offsetof";
            case TokenKind::KwCompiles:
                return "compiles";
            case TokenKind::KwStatic:
                return "static";
            case TokenKind::KwExtern:
                return "extern";

            case TokenKind::KwAsm:
                return "asm";

            case TokenKind::LParen:
                return "(";
            case TokenKind::RParen:
                return ")";
            case TokenKind::LBracket:
                return "[";
            case TokenKind::RBracket:
                return "]";
            case TokenKind::LBrace:
                return "{";
            case TokenKind::RBrace:
                return "}";

            case TokenKind::Dot:
                return ".";
            case TokenKind::DotDot:
                return "..";
            case TokenKind::Ellipsis:
                return "...";
            case TokenKind::Comma:
                return ",";
            case TokenKind::Semicolon:
                return ";";
            case TokenKind::Colon:
                return ":";
            case TokenKind::ColonColon:
                return "::";
            case TokenKind::Arrow:
                return "->";
            case TokenKind::FatArrow:
                return "=>";
            case TokenKind::At:
                return "@";
            case TokenKind::Hash:
                return "#";
            case TokenKind::Tilde:
                return "~";
            case TokenKind::Question:
                return "?";
            case TokenKind::Dollar:
                return "$";

            case TokenKind::Plus:
                return "+";
            case TokenKind::Minus:
                return "-";
            case TokenKind::Star:
                return "*";
            case TokenKind::Slash:
                return "/";
            case TokenKind::Percent:
                return "%";
            case TokenKind::Increment:
                return "++";
            case TokenKind::Decrement:
                return "--";

            case TokenKind::Amp:
                return "&";
            case TokenKind::Pipe:
                return "|";
            case TokenKind::Caret:
                return "^";
            case TokenKind::Bang:
                return "!";
            case TokenKind::AmpAmp:
                return "&&";
            case TokenKind::PipePipe:
                return "||";

            case TokenKind::Eq:
                return "=";
            case TokenKind::EqEq:
                return "==";
            case TokenKind::BangEq:
                return "!=";
            case TokenKind::Lt:
                return "<";
            case TokenKind::Gt:
                return ">";
            case TokenKind::LtEq:
                return "<=";
            case TokenKind::GtEq:
                return ">=";

            case TokenKind::LtLt:
                return "<<";
            case TokenKind::GtGt:
                return ">>";

            case TokenKind::PlusEq:
                return "+=";
            case TokenKind::MinusEq:
                return "-=";
            case TokenKind::StarEq:
                return "*=";
            case TokenKind::SlashEq:
                return "/=";
            case TokenKind::PercentEq:
                return "%=";
            case TokenKind::AmpEq:
                return "&=";
            case TokenKind::PipeEq:
                return "|=";
            case TokenKind::CaretEq:
                return "^=";
            case TokenKind::LtLtEq:
                return "<<=";
            case TokenKind::GtGtEq:
                return ">>=";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr bool is_keyword(TokenKind k) noexcept
    {
        return k >= TokenKind::Kwu8 && k <= TokenKind::KwAsm;
    }

    [[nodiscard]] constexpr bool is_punctuation(TokenKind k) noexcept
    {
        return k >= TokenKind::LParen && k <= TokenKind::GtGtEq;
    }

    [[nodiscard]] constexpr bool is_assignment(TokenKind k) noexcept
    {
        return k == TokenKind::Eq || (k >= TokenKind::PlusEq && k <= TokenKind::GtGtEq);
    }

    [[nodiscard]] constexpr bool is_literal(TokenKind k) noexcept
    {
        return k >= TokenKind::IntLiteral && k <= TokenKind::U16CharLiteral;
    }

    using TokenValue = std::variant<std::monostate, std::intmax_t, double, std::string, std::uint32_t, std::u16string>;

    struct Token
    {
        TokenKind kind{TokenKind::Eof};
        sm::SourceRange range{};

        std::string_view interned{};
        std::optional<TokenValue> value;

        [[nodiscard]] constexpr bool is(TokenKind k) const noexcept { return kind == k; }
        [[nodiscard]] constexpr bool is_not(TokenKind k) const noexcept { return kind != k; }

        template <typename... Kinds> [[nodiscard]] constexpr bool is_one_of(Kinds... kinds) const noexcept { return (is(kinds) || ...); }

        [[nodiscard]] constexpr bool eof() const noexcept { return kind == TokenKind::Eof; }
        [[nodiscard]] constexpr bool valid() const noexcept { return kind != TokenKind::Invalid; }

        [[nodiscard]] std::string_view error_message() const noexcept
        {
            if (kind == TokenKind::Invalid && value)
                if (auto* msg = std::get_if<std::string>(&*value))
                    return *msg;

            return {};
        }

        [[nodiscard]] constexpr sm::Location begin_loc() const noexcept { return range.begin; }
        [[nodiscard]] constexpr sm::Location end_loc() const noexcept { return range.end; }
    };

    [[nodiscard]] TokenKind classify_identifier(std::string_view text) noexcept
    {
        struct KeywordEntry
        {
            std::string_view spelling;
            TokenKind kind;
        };

        static constexpr auto keyword_table = std::to_array<KeywordEntry>({
            {"alignof", TokenKind::KwAlignof},
            {"as", TokenKind::KwAs},
            {"asm", TokenKind::KwAsm},
            {"bool", TokenKind::KwBool},
            {"break", TokenKind::KwBreak},
            {"char", TokenKind::KwChar},
            {"compiles", TokenKind::KwCompiles},
            {"const", TokenKind::KwConst},
            {"continue", TokenKind::KwContinue},
            {"defer", TokenKind::KwDefer},
            {"do", TokenKind::KwDo},
            {"else", TokenKind::KwElse},
            {"enum", TokenKind::KwEnum},
            {"extern", TokenKind::KwExtern},
            {"f32", TokenKind::Kwf32},
            {"f64", TokenKind::Kwf64},
            {"false", TokenKind::KwFalse},
            {"for", TokenKind::KwFor},
            {"i16", TokenKind::Kwi16},
            {"i32", TokenKind::Kwi32},
            {"i64", TokenKind::Kwi64},
            {"i8", TokenKind::Kwi8},
            {"if", TokenKind::KwIf},
            {"import", TokenKind::KwImport},
            {"in", TokenKind::KwIn},
            {"isize", TokenKind::KwIsize},
            {"match", TokenKind::KwMatch},
            {"module", TokenKind::KwModule},
            {"null", TokenKind::KwNull},
            {"null_t", TokenKind::KwNullT},
            {"offsetof", TokenKind::KwOffsetof},
            {"public", TokenKind::KwPublic},
            {"restrict", TokenKind::KwRestrict},
            {"return", TokenKind::KwReturn},
            {"sizeof", TokenKind::KwSizeof},
            {"static", TokenKind::KwStatic},
            {"struct", TokenKind::KwStruct},
            {"true", TokenKind::KwTrue},
            {"u16", TokenKind::Kwu16},
            {"u32", TokenKind::Kwu32},
            {"u64", TokenKind::Kwu64},
            {"u8", TokenKind::Kwu8},
            {"union", TokenKind::KwUnion},
            {"using", TokenKind::KwUsing},
            {"usize", TokenKind::KwUsize},
            {"void", TokenKind::KwVoid},
            {"volatile", TokenKind::KwVolatile},
            {"while", TokenKind::KwWhile},
        });

        auto it = std::ranges::lower_bound(keyword_table, text, {}, &KeywordEntry::spelling);
        if (it != keyword_table.end() && it->spelling == text)
            return it->kind;

        return TokenKind::Identifier;
    }

} // namespace dcc::lex
