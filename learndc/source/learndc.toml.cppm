export module learndc.toml;

import std;
import learndc.text;

export namespace learndc::toml
{
    struct Position
    {
        int line = 1;
        int column = 1;
        friend bool operator==(Position const&, Position const&) = default;
    };

    enum class Kind
    {
        String,
        Integer,
        Boolean,
        Array,
        Table,
        TableArray,
    };

    struct Entry;

    struct Value
    {
        Kind kind = Kind::Table;
        Position pos;
        std::string text;
        std::int64_t integer = 0;
        bool boolean = false;
        std::vector<Value> items;
        std::vector<Entry> entries;
        bool implicit = false;

        [[nodiscard]] bool is_string() const { return kind == Kind::String; }
        [[nodiscard]] bool is_integer() const { return kind == Kind::Integer; }
        [[nodiscard]] bool is_boolean() const { return kind == Kind::Boolean; }
        [[nodiscard]] bool is_array() const { return kind == Kind::Array; }
        [[nodiscard]] bool is_table() const { return kind == Kind::Table; }
        [[nodiscard]] bool is_table_array() const { return kind == Kind::TableArray; }

        [[nodiscard]] inline Value const* find(std::string_view key) const;
        [[nodiscard]] inline Value* find(std::string_view key);
        [[nodiscard]] inline Entry const* find_entry(std::string_view key) const;

        [[nodiscard]] std::string_view type_name() const
        {
            switch (kind)
            {
                case Kind::String:
                    return "string";
                case Kind::Integer:
                    return "integer";
                case Kind::Boolean:
                    return "boolean";
                case Kind::Array:
                    return "array";
                case Kind::Table:
                    return "table";
                case Kind::TableArray:
                    return "array of tables";
            }
            return "value";
        }
    };

    struct Entry
    {
        std::string key;
        Position pos;
        Value value;
    };

    inline Value const* Value::find(std::string_view key) const
    {
        for (auto const& e : entries)
            if (e.key == key)
                return &e.value;
        return nullptr;
    }

    inline Value* Value::find(std::string_view key)
    {
        for (auto& e : entries)
            if (e.key == key)
                return &e.value;
        return nullptr;
    }

    inline Entry const* Value::find_entry(std::string_view key) const
    {
        for (auto const& e : entries)
            if (e.key == key)
                return &e;
        return nullptr;
    }

    struct ParseError
    {
        Position pos;
        std::string message;

        [[nodiscard]] std::string format(std::string_view file) const { return std::format("{}:{}:{}: {}", file, pos.line, pos.column, message); }
    };

    namespace detail
    {
        [[nodiscard]] inline bool is_bare_char(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }

        class Parser
        {
        public:
            explicit Parser(std::string_view text) : m_text(text)
            {
                if (m_text.starts_with("\xEF\xBB\xBF"))
                    m_pos = m_line_start = 3;
            }

            [[nodiscard]] std::expected<Value, ParseError> run()
            {
                if (auto bad = check_utf8())
                    return std::unexpected(std::move(*bad));
                Value root;
                root.kind = Kind::Table;
                root.pos = {1, 1};
                Value* current = &root;
                while (true)
                {
                    if (auto r = skip_blank(); !r)
                        return std::unexpected(std::move(r.error()));
                    if (eof())
                        break;
                    if (peek() == '[')
                    {
                        auto table = parse_header(root);
                        if (!table)
                            return std::unexpected(std::move(table.error()));
                        current = *table;
                    }
                    else if (auto r = parse_key_value(*current); !r)
                        return std::unexpected(std::move(r.error()));
                }
                return root;
            }

        private:
            std::string_view m_text;
            std::size_t m_pos = 0;
            int m_line = 1;
            std::size_t m_line_start = 0;

            [[nodiscard]] bool eof() const { return m_pos >= m_text.size(); }
            [[nodiscard]] char peek(std::size_t ahead = 0) const { return m_pos + ahead < m_text.size() ? m_text[m_pos + ahead] : '\0'; }
            [[nodiscard]] bool has(std::size_t ahead) const { return m_pos + ahead < m_text.size(); }
            [[nodiscard]] Position here() const { return {m_line, static_cast<int>(m_pos - m_line_start) + 1}; }

            char get()
            {
                char const c = m_text[m_pos++];
                if (c == '\n')
                {
                    ++m_line;
                    m_line_start = m_pos;
                }
                return c;
            }

            [[nodiscard]] static std::string printable(char c)
            {
                auto const u = static_cast<unsigned char>(c);
                if (u < 0x20 || u >= 0x7F)
                    return std::format("\\x{:02x}", static_cast<unsigned>(u));
                return std::string(1, c);
            }

            [[nodiscard]] std::unexpected<ParseError> fail_at(Position pos, std::string message) const
            {
                return std::unexpected(ParseError{pos, std::move(message)});
            }

            [[nodiscard]] std::unexpected<ParseError> fail(std::string message) const { return fail_at(here(), std::move(message)); }

            [[nodiscard]] std::optional<ParseError> check_utf8() const
            {
                int line = 1;
                std::size_t line_start = 0;
                for (std::size_t i = 0; i < m_text.size();)
                {
                    auto const d = text::decode_utf8(m_text, i);
                    if (!d.valid)
                        return ParseError{{line, static_cast<int>(i - line_start) + 1}, "invalid UTF-8"};
                    if (d.cp == '\n')
                    {
                        ++line;
                        line_start = i + 1;
                    }
                    i += d.length;
                }
                return std::nullopt;
            }

            void skip_spaces()
            {
                while (!eof() && (peek() == ' ' || peek() == '\t'))
                    get();
            }

            [[nodiscard]] std::expected<void, ParseError> skip_comment()
            {
                if (!eof() && peek() == '#')
                    while (!eof() && peek() != '\n')
                    {
                        if (peek() == '\r' && peek(1) == '\n')
                            break;
                        auto const u = static_cast<unsigned char>(peek());
                        if ((u < 0x20 && u != '\t') || u == 0x7F)
                            return fail("control character in comment");
                        get();
                    }
                return {};
            }

            [[nodiscard]] std::expected<bool, ParseError> consume_newline()
            {
                if (peek() == '\n' && has(0))
                {
                    get();
                    return true;
                }
                if (peek() == '\r')
                {
                    if (peek(1) == '\n')
                    {
                        get();
                        get();
                        return true;
                    }
                    return fail("bare carriage return");
                }
                return false;
            }

            [[nodiscard]] std::expected<void, ParseError> skip_blank()
            {
                while (!eof())
                {
                    skip_spaces();
                    if (auto r = skip_comment(); !r)
                        return r;
                    if (eof())
                        break;
                    auto nl = consume_newline();
                    if (!nl)
                        return std::unexpected(std::move(nl.error()));
                    if (!*nl)
                        break;
                }
                return {};
            }

            [[nodiscard]] std::expected<void, ParseError> end_of_line()
            {
                skip_spaces();
                if (auto r = skip_comment(); !r)
                    return r;
                if (eof())
                    return {};
                auto nl = consume_newline();
                if (!nl)
                    return std::unexpected(std::move(nl.error()));
                if (!*nl)
                    return fail(std::format("unexpected '{}' (expected the end of the line)", printable(peek())));
                return {};
            }

            [[nodiscard]] std::expected<std::string, ParseError> parse_key()
            {
                if (eof())
                    return fail("expected a key");
                char const c = peek();
                if (c == '"')
                {
                    if (peek(1) == '"' && peek(2) == '"')
                        return fail("multi-line strings cannot be keys");
                    return parse_basic(false);
                }
                if (c == '\'')
                {
                    if (peek(1) == '\'' && peek(2) == '\'')
                        return fail("multi-line strings cannot be keys");
                    return parse_literal(false);
                }
                std::string key;
                while (!eof() && is_bare_char(peek()))
                    key += get();
                if (key.empty())
                    return fail(std::format("expected a key, found '{}'", printable(peek())));
                return key;
            }

            [[nodiscard]] std::expected<void, ParseError> parse_key_value(Value& table)
            {
                auto const key_pos = here();
                auto key = parse_key();
                if (!key)
                    return std::unexpected(std::move(key.error()));
                skip_spaces();
                if (peek() == '.' && !eof())
                    return fail("dotted keys are not supported");
                if (eof() || peek() != '=')
                    return fail("expected '=' after the key");
                get();
                skip_spaces();
                if (table.find(*key) != nullptr)
                    return fail_at(key_pos, std::format("duplicate key '{}'", *key));
                auto value = parse_value(0);
                if (!value)
                    return std::unexpected(std::move(value.error()));
                if (auto r = end_of_line(); !r)
                    return r;
                table.entries.push_back(Entry{std::move(*key), key_pos, std::move(*value)});
                return {};
            }

            [[nodiscard]] std::expected<Value*, ParseError> parse_header(Value& root)
            {
                auto const header_pos = here();
                get();
                bool const is_array = peek() == '[' && !eof();
                if (is_array)
                    get();
                std::vector<std::pair<std::string, Position>> path;
                while (true)
                {
                    skip_spaces();
                    auto const seg_pos = here();
                    auto seg = parse_key();
                    if (!seg)
                        return std::unexpected(std::move(seg.error()));
                    path.emplace_back(std::move(*seg), seg_pos);
                    skip_spaces();
                    if (!eof() && peek() == '.')
                    {
                        get();
                        continue;
                    }
                    break;
                }
                if (eof() || peek() != ']')
                    return fail("expected ']' to close the table header");
                get();
                if (is_array)
                {
                    if (eof() || peek() != ']')
                        return fail("expected ']]' to close the array-of-tables header");
                    get();
                }
                if (auto r = end_of_line(); !r)
                    return std::unexpected(std::move(r.error()));

                Value* table = &root;
                for (std::size_t i = 0; i + 1 < path.size(); ++i)
                {
                    auto& [name, pos] = path[i];
                    Value* child = table->find(name);
                    if (child == nullptr)
                    {
                        Value created;
                        created.kind = Kind::Table;
                        created.pos = pos;
                        created.implicit = true;
                        table->entries.push_back(Entry{name, pos, std::move(created)});
                        child = &table->entries.back().value;
                    }
                    if (child->kind == Kind::TableArray)
                        child = &child->items.back();
                    else if (child->kind != Kind::Table)
                        return fail_at(pos, std::format("key '{}' is not a table", name));
                    table = child;
                }
                auto& [name, pos] = path.back();
                Value* child = table->find(name);
                if (is_array)
                {
                    Value item;
                    item.kind = Kind::Table;
                    item.pos = header_pos;
                    if (child == nullptr)
                    {
                        Value array;
                        array.kind = Kind::TableArray;
                        array.pos = header_pos;
                        array.items.push_back(std::move(item));
                        table->entries.push_back(Entry{name, pos, std::move(array)});
                        return &table->entries.back().value.items.back();
                    }
                    if (child->kind != Kind::TableArray)
                        return fail_at(pos, std::format("'{}' is already defined and is not an array of tables", name));
                    child->items.push_back(std::move(item));
                    return &child->items.back();
                }
                if (child == nullptr)
                {
                    Value created;
                    created.kind = Kind::Table;
                    created.pos = header_pos;
                    table->entries.push_back(Entry{name, pos, std::move(created)});
                    return &table->entries.back().value;
                }
                if (child->kind != Kind::Table)
                    return fail_at(pos, std::format("'{}' is already defined and is not a table", name));
                if (!child->implicit)
                    return fail_at(pos, std::format("table '{}' is defined more than once", name));
                child->implicit = false;
                child->pos = header_pos;
                return child;
            }

            [[nodiscard]] std::expected<Value, ParseError> parse_value(int depth)
            {
                if (depth > 64)
                    return fail("arrays are nested too deeply");
                if (eof())
                    return fail("expected a value");
                auto const pos = here();
                char const c = peek();
                Value v;
                v.pos = pos;
                if (c == '"')
                {
                    auto s = (peek(1) == '"' && peek(2) == '"') ? parse_basic(true) : parse_basic(false);
                    if (!s)
                        return std::unexpected(std::move(s.error()));
                    v.kind = Kind::String;
                    v.text = std::move(*s);
                    return v;
                }
                if (c == '\'')
                {
                    auto s = (peek(1) == '\'' && peek(2) == '\'') ? parse_literal(true) : parse_literal(false);
                    if (!s)
                        return std::unexpected(std::move(s.error()));
                    v.kind = Kind::String;
                    v.text = std::move(*s);
                    return v;
                }
                if (c == '[')
                    return parse_array(depth);
                if (c == '{')
                    return fail("inline tables are not supported");
                if (c == '#' || c == '\n' || c == '\r')
                    return fail("expected a value");
                std::string token;
                while (!eof())
                {
                    char const t = peek();
                    if (is_bare_char(t) || t == '+' || t == '.' || t == ':')
                        token += get();
                    else
                        break;
                }
                if (token.empty())
                    return fail_at(pos, std::format("unexpected '{}' where a value was expected", printable(c)));
                if (token == "true" || token == "false")
                {
                    v.kind = Kind::Boolean;
                    v.boolean = token == "true";
                    return v;
                }
                return parse_integer(token, pos);
            }

            [[nodiscard]] std::expected<Value, ParseError> parse_integer(std::string const& token, Position pos)
            {
                std::string_view rest = token;
                bool negative = false;
                if (rest.starts_with('+') || rest.starts_with('-'))
                {
                    negative = rest.front() == '-';
                    rest.remove_prefix(1);
                }
                if (rest == "inf" || rest == "nan")
                    return fail_at(pos, "floating-point numbers are not supported");
                if (rest.empty() || rest.front() < '0' || rest.front() > '9')
                    return fail_at(pos, std::format("invalid value '{}'", token));
                if (rest.size() > 1 && rest.front() == '0' && (rest[1] == 'x' || rest[1] == 'o' || rest[1] == 'b'))
                    return fail_at(pos, "hexadecimal, octal and binary integers are not supported");
                if (rest.find(':') != std::string_view::npos || (rest.size() >= 8 && rest[4] == '-'))
                    return fail_at(pos, "dates and times are not supported");
                if (rest.find('.') != std::string_view::npos || rest.find('e') != std::string_view::npos || rest.find('E') != std::string_view::npos)
                    return fail_at(pos, "floating-point numbers are not supported");
                std::string digits;
                for (std::size_t i = 0; i < rest.size(); ++i)
                {
                    char const d = rest[i];
                    if (d == '_')
                    {
                        if (i == 0 || i + 1 == rest.size() || rest[i - 1] == '_' || rest[i + 1] < '0' || rest[i + 1] > '9')
                            return fail_at(pos, "underscores in integers must sit between digits");
                        continue;
                    }
                    if (d < '0' || d > '9')
                        return fail_at(pos, std::format("invalid integer '{}'", token));
                    digits += d;
                }
                if (digits.size() > 1 && digits.front() == '0')
                    return fail_at(pos, "leading zeros are not allowed in integers");
                std::uint64_t magnitude = 0;
                auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude);
                if (ec != std::errc{} || ptr != digits.data() + digits.size())
                    return fail_at(pos, "integer is out of range");
                constexpr std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                if (magnitude > limit + (negative ? 1u : 0u))
                    return fail_at(pos, "integer is out of range");
                Value v;
                v.kind = Kind::Integer;
                v.pos = pos;
                v.integer = negative ? static_cast<std::int64_t>(0 - magnitude) : static_cast<std::int64_t>(magnitude);
                return v;
            }

            [[nodiscard]] std::expected<Value, ParseError> parse_array(int depth)
            {
                Value v;
                v.kind = Kind::Array;
                v.pos = here();
                get();
                while (true)
                {
                    if (auto r = skip_blank(); !r)
                        return std::unexpected(std::move(r.error()));
                    if (eof())
                        return fail_at(v.pos, "unterminated array");
                    if (peek() == ']')
                    {
                        get();
                        return v;
                    }
                    auto item = parse_value(depth + 1);
                    if (!item)
                        return item;
                    v.items.push_back(std::move(*item));
                    if (auto r = skip_blank(); !r)
                        return std::unexpected(std::move(r.error()));
                    if (eof())
                        return fail_at(v.pos, "unterminated array");
                    if (peek() == ',')
                    {
                        get();
                        continue;
                    }
                    if (peek() == ']')
                    {
                        get();
                        return v;
                    }
                    return fail("expected ',' or ']' in array");
                }
            }

            [[nodiscard]] std::expected<std::string, ParseError> parse_literal(bool multi)
            {
                auto const start = here();
                get();
                if (multi)
                {
                    get();
                    get();
                    if (auto r = consume_newline(); !r)
                        return std::unexpected(std::move(r.error()));
                }
                std::string out;
                while (true)
                {
                    if (eof())
                        return fail_at(start, "unterminated string");
                    char const c = peek();
                    if (c == '\'')
                    {
                        std::size_t run = 0;
                        while (peek(run) == '\'' && has(run))
                            ++run;
                        if (!multi)
                        {
                            get();
                            return out;
                        }
                        if (run < 3)
                        {
                            for (std::size_t i = 0; i < run; ++i)
                                out += get();
                            continue;
                        }
                        if (run > 5)
                            return fail("too many quotes at the end of the string");
                        for (std::size_t i = 0; i < run - 3; ++i)
                            out += get();
                        get();
                        get();
                        get();
                        return out;
                    }
                    if (c == '\n' || c == '\r')
                    {
                        if (!multi)
                            return fail_at(start, "unterminated string (newline inside a single-line string)");
                        auto nl = consume_newline();
                        if (!nl)
                            return std::unexpected(std::move(nl.error()));
                        out += '\n';
                        continue;
                    }
                    auto const u = static_cast<unsigned char>(c);
                    if ((u < 0x20 && u != '\t') || u == 0x7F)
                        return fail("control character in string");
                    out += get();
                }
            }

            [[nodiscard]] std::expected<char32_t, ParseError> parse_unicode_escape(std::size_t digits, Position at)
            {
                char32_t cp = 0;
                for (std::size_t i = 0; i < digits; ++i)
                {
                    if (eof())
                        return fail("incomplete unicode escape");
                    char const c = peek();
                    unsigned d = 0;
                    if (c >= '0' && c <= '9')
                        d = static_cast<unsigned>(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        d = static_cast<unsigned>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        d = static_cast<unsigned>(c - 'A' + 10);
                    else
                        return fail("invalid hexadecimal digit in unicode escape");
                    get();
                    cp = (cp << 4) | d;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return fail_at(at, "unicode escape is not a valid scalar value");
                return cp;
            }

            [[nodiscard]] std::expected<std::string, ParseError> parse_basic(bool multi)
            {
                auto const start = here();
                get();
                if (multi)
                {
                    get();
                    get();
                    if (auto r = consume_newline(); !r)
                        return std::unexpected(std::move(r.error()));
                }
                std::string out;
                while (true)
                {
                    if (eof())
                        return fail_at(start, "unterminated string");
                    char const c = peek();
                    if (c == '"')
                    {
                        std::size_t run = 0;
                        while (peek(run) == '"' && has(run))
                            ++run;
                        if (!multi)
                        {
                            get();
                            return out;
                        }
                        if (run < 3)
                        {
                            for (std::size_t i = 0; i < run; ++i)
                                out += get();
                            continue;
                        }
                        if (run > 5)
                            return fail("too many quotes at the end of the string");
                        for (std::size_t i = 0; i < run - 3; ++i)
                            out += get();
                        get();
                        get();
                        get();
                        return out;
                    }
                    if (c == '\\')
                    {
                        auto const esc_pos = here();
                        get();
                        if (eof())
                            return fail_at(start, "unterminated string");
                        char const e = peek();
                        if (multi && (e == ' ' || e == '\t' || e == '\n' || e == '\r'))
                        {
                            std::size_t probe = 0;
                            while (peek(probe) == ' ' || peek(probe) == '\t')
                                ++probe;
                            if (peek(probe) != '\n' && !(peek(probe) == '\r' && peek(probe + 1) == '\n'))
                                return fail_at(esc_pos, "invalid escape sequence");
                            while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
                                get();
                            continue;
                        }
                        get();
                        switch (e)
                        {
                            case 'b':
                                out += '\b';
                                break;
                            case 't':
                                out += '\t';
                                break;
                            case 'n':
                                out += '\n';
                                break;
                            case 'f':
                                out += '\f';
                                break;
                            case 'r':
                                out += '\r';
                                break;
                            case '"':
                                out += '"';
                                break;
                            case '\\':
                                out += '\\';
                                break;
                            case 'u':
                            case 'U': {
                                auto cp = parse_unicode_escape(e == 'u' ? 4 : 8, esc_pos);
                                if (!cp)
                                    return std::unexpected(std::move(cp.error()));
                                text::append_utf8(out, *cp);
                                break;
                            }
                            default:
                                return fail_at(esc_pos, std::format("invalid escape sequence '\\{}'", printable(e)));
                        }
                        continue;
                    }
                    if (c == '\n' || c == '\r')
                    {
                        if (!multi)
                            return fail_at(start, "unterminated string (newline inside a single-line string)");
                        auto nl = consume_newline();
                        if (!nl)
                            return std::unexpected(std::move(nl.error()));
                        out += '\n';
                        continue;
                    }
                    auto const u = static_cast<unsigned char>(c);
                    if ((u < 0x20 && u != '\t') || u == 0x7F)
                        return fail("control character in string");
                    out += get();
                }
            }
        };
    } // namespace detail

    [[nodiscard]] inline std::expected<Value, ParseError> parse(std::string_view text)
    {
        return detail::Parser(text).run();
    }
} // namespace learndc::toml
