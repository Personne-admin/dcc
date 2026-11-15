export module learndc.json;

import std;
import learndc.text;

export namespace learndc::json
{
    struct Member;

    struct Json
    {
        using Array = std::vector<Json>;
        using Object = std::vector<Member>;
        using Value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

        Value v;

        explicit inline Json(Value value);
        inline Json();
        inline Json(std::nullptr_t);
        inline Json(bool b);
        inline Json(int i);
        inline Json(std::int64_t i);
        inline Json(double d);
        inline Json(std::string s);
        inline Json(std::string_view s);
        inline Json(char const* s);
        inline Json(Json const& other);
        inline Json(Json&& other) noexcept;
        inline Json& operator=(Json const& other);
        inline Json& operator=(Json&& other) noexcept;
        inline ~Json();

        [[nodiscard]] static inline Json make_array();
        [[nodiscard]] static inline Json make_object();

        [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
        [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(v); }
        [[nodiscard]] bool is_int() const { return std::holds_alternative<std::int64_t>(v); }
        [[nodiscard]] bool is_double() const { return std::holds_alternative<double>(v); }
        [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(v); }
        [[nodiscard]] bool is_array() const { return std::holds_alternative<Array>(v); }
        [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(v); }

        [[nodiscard]] std::optional<bool> as_bool() const
        {
            if (auto const* p = std::get_if<bool>(&v))
                return *p;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::int64_t> as_int() const
        {
            if (auto const* p = std::get_if<std::int64_t>(&v))
                return *p;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<double> as_number() const
        {
            if (auto const* p = std::get_if<double>(&v))
                return *p;
            if (auto const* p = std::get_if<std::int64_t>(&v))
                return static_cast<double>(*p);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string_view> as_string() const
        {
            if (auto const* p = std::get_if<std::string>(&v))
                return std::string_view(*p);
            return std::nullopt;
        }

        [[nodiscard]] Array const* as_array() const { return std::get_if<Array>(&v); }
        [[nodiscard]] Array* as_array() { return std::get_if<Array>(&v); }
        [[nodiscard]] Object const* as_object() const { return std::get_if<Object>(&v); }
        [[nodiscard]] Object* as_object() { return std::get_if<Object>(&v); }

        [[nodiscard]] inline Json const* find(std::string_view key) const;
        [[nodiscard]] inline Json* find(std::string_view key);
        inline Json& set(std::string key, Json value);
        inline Json& push(Json value);
        [[nodiscard]] inline std::size_t size() const;
        [[nodiscard]] inline std::string dump(int indent = 2) const;
    };

    struct Member
    {
        std::string key;
        Json value;
    };

    inline Json::Json(Value value) : v(std::move(value)) {}
    inline Json::Json() : v(nullptr) {}
    inline Json::Json(std::nullptr_t) : v(nullptr) {}
    inline Json::Json(bool b) : v(b) {}
    inline Json::Json(int i) : v(static_cast<std::int64_t>(i)) {}
    inline Json::Json(std::int64_t i) : v(i) {}
    inline Json::Json(double d) : v(d) {}
    inline Json::Json(std::string s) : v(std::move(s)) {}
    inline Json::Json(std::string_view s) : v(std::string(s)) {}
    inline Json::Json(char const* s) : v(std::string(s)) {}
    inline Json::Json(Json const& other) = default;
    inline Json::Json(Json&& other) noexcept = default;
    inline Json& Json::operator=(Json const& other) = default;
    inline Json& Json::operator=(Json&& other) noexcept = default;
    inline Json::~Json() = default;
    inline Json Json::make_array() { return Json(Value(std::in_place_type<Array>)); }
    inline Json Json::make_object() { return Json(Value(std::in_place_type<Object>)); }

    inline Json const* Json::find(std::string_view key) const
    {
        if (auto const* o = as_object())
            for (auto const& m : *o)
                if (m.key == key)
                    return &m.value;
        return nullptr;
    }

    inline Json* Json::find(std::string_view key)
    {
        if (auto* o = as_object())
            for (auto& m : *o)
                if (m.key == key)
                    return &m.value;
        return nullptr;
    }

    inline Json& Json::set(std::string key, Json value)
    {
        if (!is_object())
            v = Object{};
        auto& o = *as_object();
        for (auto& m : o)
            if (m.key == key)
            {
                m.value = std::move(value);
                return m.value;
            }
        o.push_back(Member{std::move(key), std::move(value)});
        return o.back().value;
    }

    inline Json& Json::push(Json value)
    {
        if (!is_array())
            v = Array{};
        auto& a = *as_array();
        a.push_back(std::move(value));
        return a.back();
    }

    inline std::size_t Json::size() const
    {
        if (auto const* a = as_array())
            return a->size();
        if (auto const* o = as_object())
            return o->size();
        return 0;
    }

    [[nodiscard]] inline bool operator==(Member const& a, Member const& b);

    [[nodiscard]] inline bool operator==(Json const& a, Json const& b)
    {
        return a.v == b.v;
    }

    [[nodiscard]] inline bool operator==(Member const& a, Member const& b)
    {
        return a.key == b.key && a.value == b.value;
    }

    struct JsonError
    {
        int line = 1;
        int column = 1;
        std::string message;

        [[nodiscard]] std::string format() const { return std::format("{}:{}: {}", line, column, message); }
    };

    namespace detail
    {
        class Parser
        {
        public:
            explicit Parser(std::string_view text) : m_text(text)
            {
                if (m_text.starts_with("\xEF\xBB\xBF"))
                    m_pos = 3;
            }

            [[nodiscard]] std::expected<Json, JsonError> run()
            {
                skip_ws();
                auto value = parse_value(0);
                if (!value)
                    return std::unexpected(std::move(value.error()));
                skip_ws();
                if (m_pos < m_text.size())
                    return fail("unexpected content after the JSON value");
                return value;
            }

        private:
            std::string_view m_text;
            std::size_t m_pos = 0;

            [[nodiscard]] std::pair<int, int> position_at(std::size_t pos) const
            {
                int line = 1;
                std::size_t line_start = 0;
                for (std::size_t i = 0; i < pos && i < m_text.size(); ++i)
                    if (m_text[i] == '\n')
                    {
                        ++line;
                        line_start = i + 1;
                    }
                return {line, static_cast<int>(pos - line_start) + 1};
            }

            [[nodiscard]] std::unexpected<JsonError> fail_at(std::size_t pos, std::string message) const
            {
                auto const [line, column] = position_at(pos);
                return std::unexpected(JsonError{line, column, std::move(message)});
            }

            [[nodiscard]] std::unexpected<JsonError> fail(std::string message) const { return fail_at(m_pos, std::move(message)); }

            void skip_ws()
            {
                while (m_pos < m_text.size())
                {
                    char const c = m_text[m_pos];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        ++m_pos;
                    else
                        break;
                }
            }

            [[nodiscard]] std::expected<Json, JsonError> parse_value(int depth)
            {
                if (depth > 256)
                    return fail("nesting is too deep");
                if (m_pos >= m_text.size())
                    return fail("unexpected end of input");
                char const c = m_text[m_pos];
                if (c == '{')
                    return parse_object(depth);
                if (c == '[')
                    return parse_array(depth);
                if (c == '"')
                {
                    auto s = parse_string();
                    if (!s)
                        return std::unexpected(std::move(s.error()));
                    return Json(std::move(*s));
                }
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parse_number();
                if (consume_word("true"))
                    return Json(true);
                if (consume_word("false"))
                    return Json(false);
                if (consume_word("null"))
                    return Json(nullptr);
                return fail(std::format("unexpected character '{}'", printable(c)));
            }

            [[nodiscard]] static std::string printable(char c)
            {
                auto const u = static_cast<unsigned char>(c);
                if (u < 0x20 || u >= 0x7F)
                    return std::format("\\x{:02x}", static_cast<unsigned>(u));
                return std::string(1, c);
            }

            [[nodiscard]] bool consume_word(std::string_view word)
            {
                if (m_text.substr(m_pos).starts_with(word))
                {
                    auto const next = m_pos + word.size();
                    if (next < m_text.size())
                    {
                        char const n = m_text[next];
                        if ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') || (n >= '0' && n <= '9'))
                            return false;
                    }
                    m_pos = next;
                    return true;
                }
                return false;
            }

            [[nodiscard]] std::expected<Json, JsonError> parse_number()
            {
                auto const start = m_pos;
                if (m_text[m_pos] == '-')
                    ++m_pos;
                if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
                    return fail("expected a digit");
                if (m_text[m_pos] == '0')
                {
                    ++m_pos;
                    if (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                        return fail("leading zeros are not allowed");
                }
                else
                    while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                        ++m_pos;
                bool is_float = false;
                if (m_pos < m_text.size() && m_text[m_pos] == '.')
                {
                    is_float = true;
                    ++m_pos;
                    if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
                        return fail("expected a digit after the decimal point");
                    while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                        ++m_pos;
                }
                if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
                {
                    is_float = true;
                    ++m_pos;
                    if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
                        ++m_pos;
                    if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
                        return fail("expected a digit in the exponent");
                    while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                        ++m_pos;
                }
                auto const token = m_text.substr(start, m_pos - start);
                if (!is_float)
                {
                    std::int64_t value = 0;
                    auto const [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
                    if (ec == std::errc{} && ptr == token.data() + token.size())
                        return Json(value);
                }
                double value = 0;
                auto const [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
                if (ec != std::errc{} || ptr != token.data() + token.size())
                    return fail_at(start, "number is out of range");
                return Json(value);
            }

            [[nodiscard]] std::expected<unsigned, JsonError> parse_hex4()
            {
                if (m_pos + 4 > m_text.size())
                    return fail("incomplete \\u escape");
                unsigned value = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char const c = m_text[m_pos + static_cast<std::size_t>(i)];
                    unsigned digit = 0;
                    if (c >= '0' && c <= '9')
                        digit = static_cast<unsigned>(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        digit = static_cast<unsigned>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        digit = static_cast<unsigned>(c - 'A' + 10);
                    else
                        return fail_at(m_pos + static_cast<std::size_t>(i), "invalid hexadecimal digit in \\u escape");
                    value = value * 16 + digit;
                }
                m_pos += 4;
                return value;
            }

            [[nodiscard]] std::expected<std::string, JsonError> parse_string()
            {
                auto const start = m_pos;
                ++m_pos;
                std::string out;
                while (true)
                {
                    if (m_pos >= m_text.size())
                        return fail_at(start, "unterminated string");
                    auto const b = static_cast<unsigned char>(m_text[m_pos]);
                    if (b == '"')
                    {
                        ++m_pos;
                        return out;
                    }
                    if (b < 0x20)
                        return fail("control character in string");
                    if (b == '\\')
                    {
                        ++m_pos;
                        if (m_pos >= m_text.size())
                            return fail_at(start, "unterminated string");
                        char const e = m_text[m_pos];
                        ++m_pos;
                        switch (e)
                        {
                            case '"':
                                out += '"';
                                break;
                            case '\\':
                                out += '\\';
                                break;
                            case '/':
                                out += '/';
                                break;
                            case 'b':
                                out += '\b';
                                break;
                            case 'f':
                                out += '\f';
                                break;
                            case 'n':
                                out += '\n';
                                break;
                            case 'r':
                                out += '\r';
                                break;
                            case 't':
                                out += '\t';
                                break;
                            case 'u': {
                                auto const escape_pos = m_pos - 2;
                                auto hi = parse_hex4();
                                if (!hi)
                                    return std::unexpected(std::move(hi.error()));
                                char32_t cp = *hi;
                                if (cp >= 0xD800 && cp <= 0xDBFF)
                                {
                                    if (m_pos + 1 < m_text.size() && m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u')
                                    {
                                        m_pos += 2;
                                        auto lo = parse_hex4();
                                        if (!lo)
                                            return std::unexpected(std::move(lo.error()));
                                        if (*lo < 0xDC00 || *lo > 0xDFFF)
                                            return fail_at(escape_pos, "invalid surrogate pair");
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (*lo - 0xDC00);
                                    }
                                    else
                                        return fail_at(escape_pos, "lone high surrogate");
                                }
                                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                                    return fail_at(escape_pos, "lone low surrogate");
                                text::append_utf8(out, cp);
                                break;
                            }
                            default:
                                return fail_at(m_pos - 1, std::format("invalid escape '\\{}'", printable(e)));
                        }
                        continue;
                    }
                    if (b < 0x80)
                    {
                        out += static_cast<char>(b);
                        ++m_pos;
                        continue;
                    }
                    auto const d = text::decode_utf8(m_text, m_pos);
                    if (!d.valid)
                        return fail("invalid UTF-8 in string");
                    out.append(m_text.substr(m_pos, d.length));
                    m_pos += d.length;
                }
            }

            [[nodiscard]] std::expected<Json, JsonError> parse_array(int depth)
            {
                ++m_pos;
                Json result = Json::make_array();
                skip_ws();
                if (m_pos < m_text.size() && m_text[m_pos] == ']')
                {
                    ++m_pos;
                    return result;
                }
                while (true)
                {
                    skip_ws();
                    auto item = parse_value(depth + 1);
                    if (!item)
                        return item;
                    result.push(std::move(*item));
                    skip_ws();
                    if (m_pos >= m_text.size())
                        return fail("unexpected end of input in array");
                    if (m_text[m_pos] == ',')
                    {
                        ++m_pos;
                        skip_ws();
                        if (m_pos < m_text.size() && m_text[m_pos] == ']')
                            return fail("trailing comma in array");
                        continue;
                    }
                    if (m_text[m_pos] == ']')
                    {
                        ++m_pos;
                        return result;
                    }
                    return fail("expected ',' or ']' in array");
                }
            }

            [[nodiscard]] std::expected<Json, JsonError> parse_object(int depth)
            {
                ++m_pos;
                Json result = Json::make_object();
                skip_ws();
                if (m_pos < m_text.size() && m_text[m_pos] == '}')
                {
                    ++m_pos;
                    return result;
                }
                while (true)
                {
                    skip_ws();
                    if (m_pos >= m_text.size())
                        return fail("unexpected end of input in object");
                    if (m_text[m_pos] != '"')
                        return fail("expected a string key");
                    auto const key_pos = m_pos;
                    auto key = parse_string();
                    if (!key)
                        return std::unexpected(std::move(key.error()));
                    if (result.find(*key) != nullptr)
                        return fail_at(key_pos, std::format("duplicate key '{}'", *key));
                    skip_ws();
                    if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                        return fail("expected ':' after object key");
                    ++m_pos;
                    skip_ws();
                    auto value = parse_value(depth + 1);
                    if (!value)
                        return value;
                    result.as_object()->push_back(Member{std::move(*key), std::move(*value)});
                    skip_ws();
                    if (m_pos >= m_text.size())
                        return fail("unexpected end of input in object");
                    if (m_text[m_pos] == ',')
                    {
                        ++m_pos;
                        skip_ws();
                        if (m_pos < m_text.size() && m_text[m_pos] == '}')
                            return fail("trailing comma in object");
                        continue;
                    }
                    if (m_text[m_pos] == '}')
                    {
                        ++m_pos;
                        return result;
                    }
                    return fail("expected ',' or '}' in object");
                }
            }
        };

        inline void escape_into(std::string& out, std::string_view s)
        {
            out += '"';
            for (char const ch : s)
            {
                auto const u = static_cast<unsigned char>(ch);
                switch (ch)
                {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\b':
                        out += "\\b";
                        break;
                    case '\f':
                        out += "\\f";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (u < 0x20)
                            out += std::format("\\u{:04x}", static_cast<unsigned>(u));
                        else
                            out += ch;
                }
            }
            out += '"';
        }

        inline void dump_into(std::string& out, Json const& j, int indent, int level)
        {
            auto const newline = [&](int lvl) {
                if (indent > 0)
                {
                    out += '\n';
                    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(lvl), ' ');
                }
            };
            if (j.is_null())
                out += "null";
            else if (auto const b = j.as_bool())
                out += *b ? "true" : "false";
            else if (auto const i = j.as_int())
                out += std::format("{}", *i);
            else if (j.is_double())
            {
                double const d = *j.as_number();
                if (!std::isfinite(d))
                    out += "null";
                else
                {
                    auto text = std::format("{}", d);
                    if (text.find_first_of(".eE") == std::string::npos)
                        text += ".0";
                    out += text;
                }
            }
            else if (auto const s = j.as_string())
                escape_into(out, *s);
            else if (auto const* a = j.as_array())
            {
                if (a->empty())
                {
                    out += "[]";
                    return;
                }
                out += '[';
                bool first = true;
                for (auto const& item : *a)
                {
                    if (!first)
                        out += ',';
                    first = false;
                    newline(level + 1);
                    dump_into(out, item, indent, level + 1);
                }
                newline(level);
                out += ']';
            }
            else if (auto const* o = j.as_object())
            {
                if (o->empty())
                {
                    out += "{}";
                    return;
                }
                out += '{';
                bool first = true;
                for (auto const& m : *o)
                {
                    if (!first)
                        out += ',';
                    first = false;
                    newline(level + 1);
                    escape_into(out, m.key);
                    out += indent > 0 ? ": " : ":";
                    dump_into(out, m.value, indent, level + 1);
                }
                newline(level);
                out += '}';
            }
        }
    } // namespace detail

    [[nodiscard]] inline std::expected<Json, JsonError> parse(std::string_view text)
    {
        return detail::Parser(text).run();
    }

    inline std::string Json::dump(int indent) const
    {
        std::string out;
        detail::dump_into(out, *this, indent, 0);
        return out;
    }
} // namespace learndc::json
