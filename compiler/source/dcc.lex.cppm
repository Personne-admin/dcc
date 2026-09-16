export module dcc.lex;

export import dcc.lex.tokens;

import std;
import dcc.si;
import dcc.sm;
import dcc.utf8;

export namespace dcc::lex
{
    class Lexer
    {
    public:
        explicit Lexer(sm::SourceFile const& file, si::string_interner& interner) noexcept : m_src{file.text()}, m_fid{file.id()}, m_interner{interner} {}

        Token next()
        {
            if (auto err = skip_trivia())
                return *err;

            if (at_end())
                return eof();

            auto const start = m_pos;
            char c = peek();

            if (c == 'u' && peek_at(1) == '"')
                return lex_u16_string(start);
            else if (c == 'u' && peek_at(1) == '\'')
                return lex_u16_char(start);
            else if (is_ident_start(c))
                return lex_identifier(start);
            else if (is_digit(c))
                return lex_number(start);
            else if (c == '"')
                return lex_string(start);
            else if (c == '\'')
                return lex_char(start);

            return lex_punctuation(start);
        }

    private:
        std::string_view m_src;
        sm::FileId m_fid;
        si::string_interner& m_interner;
        std::uint32_t m_pos{};

        static constexpr bool is_ident_start(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
        static constexpr bool is_ident_cont(char c) noexcept { return is_ident_start(c) || (c >= '0' && c <= '9'); }

        static constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
        static constexpr bool is_hex_digit(char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
        static constexpr bool is_bin_digit(char c) noexcept { return c == '0' || c == '1'; }
        static constexpr bool is_oct_digit(char c) noexcept { return c >= '0' && c <= '7'; }
        static constexpr bool is_whitespace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

        static constexpr int hex_val(char c) noexcept
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            else if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        static void encode_utf8(char32_t cp, std::string& out)
        {
            if (cp < 0x80)
                out.push_back(static_cast<char>(cp));
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        [[nodiscard]] bool at_end() const noexcept { return m_pos >= m_src.size(); }
        [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : m_src[m_pos]; }

        [[nodiscard]] char peek_at(std::uint32_t offset) const noexcept
        {
            auto i = m_pos + offset;
            return i < m_src.size() ? m_src[i] : '\0';
        }

        char advance() noexcept { return m_src[m_pos++]; }

        bool match(char expected) noexcept
        {
            if (peek() == expected)
            {
                advance();
                return true;
            }

            return false;
        }

        [[nodiscard]] sm::SourceRange make_range(std::uint32_t begin, std::uint32_t end) const noexcept { return {{m_fid, begin}, {m_fid, end}}; }

        [[nodiscard]] Token make_token(TokenKind kind, std::uint32_t start) const noexcept
        {
            return Token{.kind = kind, .range = make_range(start, m_pos), .interned = {}, .value = {}};
        }

        [[nodiscard]] Token make_error(std::uint32_t start, std::string msg) const
        {
            return Token{.kind = TokenKind::Invalid, .range = make_range(start, m_pos), .interned = {}, .value = std::move(msg)};
        }

        [[nodiscard]] Token eof() const noexcept
        {
            auto p = static_cast<std::uint32_t>(m_src.size());

            return Token{.kind = TokenKind::Eof, .range = make_range(p, p), .interned = {}, .value = {}};
        }

        std::optional<Token> skip_trivia()
        {
            while (!at_end())
            {
                if (is_whitespace(peek()))
                {
                    advance();
                    continue;
                }

                if (peek() == '/' && peek_at(1) == '/')
                {
                    advance();
                    advance();
                    while (!at_end() && peek() != '\n')
                        advance();

                    continue;
                }

                if (peek() == '/' && peek_at(1) == '*')
                {
                    auto const start = m_pos;
                    advance();
                    advance();
                    int depth = 1;

                    while (!at_end() && depth > 0)
                    {
                        if (peek() == '/' && peek_at(1) == '*')
                        {
                            advance();
                            advance();
                            ++depth;
                            continue;
                        }

                        if (peek() == '*' && peek_at(1) == '/')
                        {
                            advance();
                            advance();
                            --depth;
                            continue;
                        }

                        advance();
                    }

                    if (depth > 0)
                        return make_error(start, "unterminated block comment");

                    continue;
                }

                break;
            }

            return std::nullopt;
        }

        Token lex_identifier(std::uint32_t start)
        {
            while (!at_end() && is_ident_cont(peek()))
                advance();

            auto raw = m_src.substr(start, m_pos - start);
            auto kind = classify_identifier(raw);

            auto tok = make_token(kind, start);
            tok.interned = m_interner.intern(raw);
            return tok;
        }

        Token lex_number(std::uint32_t start)
        {
            if (peek() == '0')
            {
                advance();
                if (match('x') || match('X'))
                    return lex_radix_int<16>(start, "0x", is_hex_digit);
                if (match('b') || match('B'))
                    return lex_radix_int<2>(start, "0b", is_bin_digit);
                if (match('o') || match('O'))
                    return lex_radix_int<8>(start, "0o", is_oct_digit);
            }

            return lex_decimal(start);
        }

        template <int Radix> Token lex_radix_int(std::uint32_t start, char const* prefix, bool (*is_valid)(char))
        {
            if (!is_valid(peek()))
                return make_error(start, std::format("expected digit after '{}'", prefix));

            std::uint64_t val = 0;
            bool overflow = false;

            while (is_valid(peek()) || peek() == '_')
            {
                if (peek() == '_')
                {
                    advance();
                    continue;
                }

                auto prev = val;

                if constexpr (Radix == 16)
                    val = (val << 4) | static_cast<std::uint64_t>(hex_val(peek()));
                else if constexpr (Radix == 8)
                    val = (val << 3) | static_cast<std::uint64_t>(peek() - '0');
                else
                    val = (val << 1) | static_cast<std::uint64_t>(peek() - '0');

                if (val < prev)
                    overflow = true;

                advance();
            }

            if (overflow)
                return make_error(start, std::format("{} literal overflows 64-bit integer", prefix));

            auto tok = make_token(TokenKind::IntLiteral, start);
            tok.interned = m_interner.intern(m_src.substr(start, m_pos - start));
            tok.value = static_cast<std::int64_t>(val);
            return tok;
        }

        Token lex_decimal(std::uint32_t start)
        {
            while (is_digit(peek()) || peek() == '_')
                advance();

            bool is_float = false;

            if (peek() == '.' && is_digit(peek_at(1)))
            {
                is_float = true;
                advance();
                while (is_digit(peek()) || peek() == '_')
                    advance();
            }

            if (peek() == 'e' || peek() == 'E')
            {
                is_float = true;
                advance();
                if (peek() == '+' || peek() == '-')
                    advance();

                if (!is_digit(peek()))
                    return make_error(start, "expected digit in exponent");

                while (is_digit(peek()) || peek() == '_')
                    advance();
            }

            auto raw = m_src.substr(start, m_pos - start);
            auto interned = m_interner.intern(raw);

            std::string clean;
            clean.reserve(raw.size());
            for (char ch : raw)
                if (ch != '_')
                    clean.push_back(ch);

            if (is_float)
            {
                double val{};
                auto [ptr, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), val);

                if (ec != std::errc{})
                    return make_error(start, "invalid float literal");

                auto tok = make_token(TokenKind::FloatLiteral, start);
                tok.interned = interned;
                tok.value = val;
                return tok;
            }

            std::uint64_t val{};
            auto [ptr, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), val);

            if (ec == std::errc::result_out_of_range)
                return make_error(start, "integer literal overflows u64");

            if (ec != std::errc{})
                return make_error(start, "invalid integer literal");

            auto tok = make_token(TokenKind::IntLiteral, start);
            tok.interned = interned;
            tok.value = static_cast<std::int64_t>(val);
            return tok;
        }

        Token lex_string(std::uint32_t start)
        {
            advance();

            std::string val;

            while (!at_end() && peek() != '"')
            {
                if (peek() == '\n')
                    return make_error(start, "unterminated string literal");

                if (peek() == '\\')
                {
                    advance();
                    if (auto err = lex_escape(val); !err.empty())
                        return make_error(start, std::string{err});

                    continue;
                }

                val.push_back(advance());
            }

            if (at_end())
                return make_error(start, "unterminated string literal");

            advance();

            auto tok = make_token(TokenKind::StringLiteral, start);
            tok.interned = m_interner.intern(m_src.substr(start, m_pos - start));
            tok.value = std::move(val);
            return tok;
        }

        static void encode_utf16(char32_t cp, std::u16string& out)
        {
            if (cp < 0x10000)
                out.push_back(static_cast<char16_t>(cp));
            else
            {
                cp -= 0x10000;
                out.push_back(static_cast<char16_t>(0xD800 | (cp >> 10)));
                out.push_back(static_cast<char16_t>(0xDC00 | (cp & 0x3FF)));
            }
        }

        Token lex_u16_string(std::uint32_t start)
        {
            advance();
            advance();

            std::u16string val;

            while (!at_end() && peek() != '"')
            {
                if (peek() == '\n')
                    return make_error(start, "unterminated utf-16 string literal");

                if (peek() == '\\')
                {
                    advance();
                    if (auto err = lex_u16_escape(val); !err.empty())
                        return make_error(start, std::string{err});

                    continue;
                }

                auto res = utf8::decode_one(m_src.substr(m_pos));
                if (!res)
                    return make_error(start, "invalid UTF-8 in utf-16 string literal");

                encode_utf16(res->codepoint, val);
                m_pos += static_cast<std::uint32_t>(res->bytes_consumed);
            }

            if (at_end())
                return make_error(start, "unterminated utf-16 string literal");

            advance();

            auto tok = make_token(TokenKind::U16StringLiteral, start);
            tok.interned = m_interner.intern(m_src.substr(start, m_pos - start));
            tok.value = std::move(val);
            return tok;
        }

        Token lex_char(std::uint32_t start)
        {
            advance();

            if (at_end() || peek() == '\'')
                return make_error(start, "empty character literal");

            char32_t cp{};

            if (peek() == '\\')
            {
                advance();
                std::string buf;
                if (auto err = lex_escape(buf); !err.empty())
                    return make_error(start, std::string{err});

                auto res = utf8::decode_one(std::string_view{buf});
                if (!res)
                    return make_error(start, "invalid escape in character literal");

                cp = res->codepoint;
            }
            else
            {
                auto res = utf8::decode_one(m_src.substr(m_pos));
                if (!res)
                    return make_error(start, "invalid UTF-8 in character literal");

                cp = res->codepoint;
                m_pos += static_cast<std::uint32_t>(res->bytes_consumed);
            }

            if (at_end() || peek() != '\'')
                return make_error(start, "unterminated character literal");

            advance();

            auto tok = make_token(TokenKind::CharLiteral, start);
            tok.interned = m_interner.intern(m_src.substr(start, m_pos - start));
            tok.value = static_cast<std::uint32_t>(cp);
            return tok;
        }

        Token lex_u16_char(std::uint32_t start)
        {
            advance();
            advance();

            if (at_end() || peek() == '\'')
                return make_error(start, "empty utf-16 character literal");

            std::u16string val;

            if (peek() == '\\')
            {
                advance();
                if (auto err = lex_u16_escape(val); !err.empty())
                    return make_error(start, std::string{err});
            }
            else
            {
                auto res = utf8::decode_one(m_src.substr(m_pos));
                if (!res)
                    return make_error(start, "invalid UTF-8 in utf-16 character literal");

                encode_utf16(res->codepoint, val);
                m_pos += static_cast<std::uint32_t>(res->bytes_consumed);
            }

            if (val.empty())
                return make_error(start, "empty utf-16 character literal");

            if (val.size() > 1)
                return make_error(start, "utf-16 character literal must contain a single code unit");

            if (val[0] >= 0xD800 && val[0] <= 0xDFFF)
                return make_error(start, "utf-16 character literal cannot be a surrogate");

            if (!at_end() && peek() != '\'')
            {
                bool found_closing = false;
                for (auto scan = m_pos; scan < m_src.size() && m_src[scan] != '\n'; ++scan)
                {
                    if (m_src[scan] == '\'')
                    {
                        found_closing = true;
                        break;
                    }
                }

                if (found_closing)
                    return make_error(start, "utf-16 character literal must contain a single code unit");
                else
                    return make_error(start, "unterminated utf-16 character literal");
            }

            if (at_end() || peek() != '\'')
                return make_error(start, "unterminated utf-16 character literal");

            advance();

            auto tok = make_token(TokenKind::U16CharLiteral, start);
            tok.interned = m_interner.intern(m_src.substr(start, m_pos - start));
            tok.value = static_cast<std::uint32_t>(val[0]);
            return tok;
        }

        std::string_view lex_escape(std::string& out)
        {
            if (at_end())
                return "unexpected end of file in escape sequence";

            char c = advance();

            switch (c)
            {
                case 'n':
                    out.push_back('\n');
                    return {};
                case 't':
                    out.push_back('\t');
                    return {};
                case 'r':
                    out.push_back('\r');
                    return {};
                case '0':
                    out.push_back('\0');
                    return {};
                case '\\':
                    out.push_back('\\');
                    return {};
                case '\'':
                    out.push_back('\'');
                    return {};
                case '"':
                    out.push_back('"');
                    return {};

                case 'x': {
                    if (!is_hex_digit(peek()) || !is_hex_digit(peek_at(1)))
                        return "expected two hex digits after '\\x'";

                    auto hi = hex_val(advance());
                    auto lo = hex_val(advance());
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    return {};
                }

                case 'u': {
                    if (!match('{'))
                        return "expected '{' after '\\u'";

                    std::uint32_t codepoint = 0;
                    int digits = 0;

                    while (!at_end() && peek() != '}')
                    {
                        if (!is_hex_digit(peek()))
                            return "invalid hex digit in unicode escape";

                        codepoint = (codepoint << 4) | static_cast<std::uint32_t>(hex_val(advance()));

                        if (++digits > 6)
                            return "unicode escape exceeds 6 digits";
                    }

                    if (!match('}'))
                        return "expected '}' in unicode escape";
                    if (digits == 0)
                        return "empty unicode escape";
                    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                        return "invalid unicode codepoint";

                    encode_utf8(static_cast<char32_t>(codepoint), out);
                    return {};
                }

                default:
                    return "unknown escape sequence";
            }
        }

        std::string_view lex_u16_escape(std::u16string& out)
        {
            if (at_end())
                return "unexpected end of file in escape sequence";

            char c = advance();

            switch (c)
            {
                case 'n':
                    out.push_back(u'\n');
                    return {};
                case 't':
                    out.push_back(u'\t');
                    return {};
                case 'r':
                    out.push_back(u'\r');
                    return {};
                case '0':
                    out.push_back(u'\0');
                    return {};
                case '\\':
                    out.push_back(u'\\');
                    return {};
                case '\'':
                    out.push_back(u'\'');
                    return {};
                case '"':
                    out.push_back(u'"');
                    return {};

                case 'x': {
                    if (!is_hex_digit(peek()) || !is_hex_digit(peek_at(1)))
                        return "expected two hex digits after '\\x'";

                    auto hi = hex_val(advance());
                    auto lo = hex_val(advance());
                    out.push_back(static_cast<char16_t>((hi << 4) | lo));
                    return {};
                }

                case 'u': {
                    if (match('{'))
                    {
                        std::uint32_t codepoint = 0;
                        int digits = 0;

                        while (!at_end() && peek() != '}')
                        {
                            if (!is_hex_digit(peek()))
                                return "invalid hex digit in unicode escape";

                            codepoint = (codepoint << 4) | static_cast<std::uint32_t>(hex_val(advance()));

                            if (++digits > 6)
                                return "unicode escape exceeds 6 digits";
                        }

                        if (!match('}'))
                            return "expected '}' in unicode escape";
                        if (digits == 0)
                            return "empty unicode escape";
                        if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                            return "invalid unicode codepoint";

                        encode_utf16(static_cast<char32_t>(codepoint), out);
                        return {};
                    }
                    else
                    {
                        std::uint32_t codepoint = 0;
                        for (int i = 0; i < 4; ++i)
                        {
                            if (at_end() || !is_hex_digit(peek()))
                                return "expected exactly 4 hex digits after '\\u'";

                            codepoint = (codepoint << 4) | static_cast<std::uint32_t>(hex_val(advance()));
                        }
                        if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
                            return "invalid unicode codepoint (surrogate) in \\u escape";

                        encode_utf16(static_cast<char32_t>(codepoint), out);
                        return {};
                    }
                }

                case 'U': {
                    std::uint32_t codepoint = 0;
                    for (int i = 0; i < 8; ++i)
                    {
                        if (at_end() || !is_hex_digit(peek()))
                            return "expected exactly 8 hex digits after '\\U'";

                        codepoint = (codepoint << 4) | static_cast<std::uint32_t>(hex_val(advance()));
                    }
                    if (codepoint > 0x10FFFF)
                        return "invalid unicode codepoint in \\U escape";
                    if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
                        return "invalid unicode codepoint (surrogate) in \\U escape";

                    encode_utf16(static_cast<char32_t>(codepoint), out);
                    return {};
                }

                default:
                    return "unknown escape sequence";
            }
        }

        Token lex_punctuation(std::uint32_t start)
        {
            char c = advance();

            switch (c)
            {
                case '(':
                    return make_token(TokenKind::LParen, start);
                case ')':
                    return make_token(TokenKind::RParen, start);
                case '[':
                    return make_token(TokenKind::LBracket, start);
                case ']':
                    return make_token(TokenKind::RBracket, start);
                case '{':
                    return make_token(TokenKind::LBrace, start);
                case '}':
                    return make_token(TokenKind::RBrace, start);
                case ',':
                    return make_token(TokenKind::Comma, start);
                case ';':
                    return make_token(TokenKind::Semicolon, start);
                case '~':
                    return make_token(TokenKind::Tilde, start);
                case '?':
                    return make_token(TokenKind::Question, start);
                case '$':
                    return make_token(TokenKind::Dollar, start);
                case '@':
                    return make_token(TokenKind::At, start);
                case '#':
                    return make_token(TokenKind::Hash, start);

                case '.':
                    if (peek() == '.')
                    {
                        advance();
                        if (match('.'))
                            return make_token(TokenKind::Ellipsis, start);
                        return make_token(TokenKind::DotDot, start);
                    }
                    return make_token(TokenKind::Dot, start);

                case ':':
                    if (match(':'))
                        return make_token(TokenKind::ColonColon, start);
                    return make_token(TokenKind::Colon, start);

                case '+':
                    if (match('+'))
                        return make_token(TokenKind::Increment, start);
                    if (match('='))
                        return make_token(TokenKind::PlusEq, start);
                    return make_token(TokenKind::Plus, start);

                case '-':
                    if (match('-'))
                        return make_token(TokenKind::Decrement, start);
                    if (match('='))
                        return make_token(TokenKind::MinusEq, start);
                    if (match('>'))
                        return make_token(TokenKind::Arrow, start);
                    return make_token(TokenKind::Minus, start);

                case '*':
                    if (match('='))
                        return make_token(TokenKind::StarEq, start);
                    return make_token(TokenKind::Star, start);

                case '/':
                    if (match('='))
                        return make_token(TokenKind::SlashEq, start);
                    return make_token(TokenKind::Slash, start);

                case '%':
                    if (match('='))
                        return make_token(TokenKind::PercentEq, start);
                    return make_token(TokenKind::Percent, start);

                case '&':
                    if (match('&'))
                        return make_token(TokenKind::AmpAmp, start);
                    if (match('='))
                        return make_token(TokenKind::AmpEq, start);
                    return make_token(TokenKind::Amp, start);

                case '|':
                    if (match('|'))
                        return make_token(TokenKind::PipePipe, start);
                    if (match('='))
                        return make_token(TokenKind::PipeEq, start);
                    return make_token(TokenKind::Pipe, start);

                case '^':
                    if (match('='))
                        return make_token(TokenKind::CaretEq, start);
                    return make_token(TokenKind::Caret, start);

                case '!':
                    if (match('='))
                        return make_token(TokenKind::BangEq, start);
                    return make_token(TokenKind::Bang, start);

                case '=':
                    if (match('='))
                        return make_token(TokenKind::EqEq, start);
                    if (match('>'))
                        return make_token(TokenKind::FatArrow, start);
                    return make_token(TokenKind::Eq, start);

                case '<':
                    if (peek() == '<')
                    {
                        advance();
                        if (match('='))
                            return make_token(TokenKind::LtLtEq, start);
                        return make_token(TokenKind::LtLt, start);
                    }

                    if (match('='))
                        return make_token(TokenKind::LtEq, start);
                    return make_token(TokenKind::Lt, start);

                case '>':
                    if (peek() == '>')
                    {
                        advance();
                        if (match('='))
                            return make_token(TokenKind::GtGtEq, start);
                        return make_token(TokenKind::GtGt, start);
                    }

                    if (match('='))
                        return make_token(TokenKind::GtEq, start);
                    return make_token(TokenKind::Gt, start);

                default:
                    return make_error(start, std::format("unexpected byte 0x{:02X}", static_cast<unsigned char>(c)));
            }
        }
    };

} // namespace dcc::lex
