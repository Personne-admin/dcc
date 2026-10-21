export module dcc.backend.x86.modasm;

import std;
import dcc.ir;
import dcc.sm;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;

export namespace dcc::backend::x86
{
    enum class ModuleAsmFormat : std::uint8_t
    {
        Elf,
        Coff,
    };

    struct ModuleAsmRelocation
    {
        std::uint64_t offset{};
        std::string symbol;
        Reloc::Kind kind{Reloc::Kind::Abs64};
        std::int64_t addend{};
    };

    struct ModuleAsmLine
    {
        std::uint64_t offset{};
        std::string text;
    };

    struct ModuleAsmSection
    {
        std::string name;
        bool alloc{};
        bool write{};
        bool exec{};
        bool nobits{};
        std::uint64_t alignment{1};
        std::vector<std::uint8_t> data;
        std::uint64_t size{};
        std::vector<ModuleAsmRelocation> relocations;
        std::vector<ModuleAsmLine> lines;
    };

    enum class ModuleAsmBinding : std::uint8_t
    {
        Local,
        Global,
        Weak,
    };

    enum class ModuleAsmSymbolType : std::uint8_t
    {
        None,
        Function,
        Object,
    };

    struct ModuleAsmSymbol
    {
        std::string name;
        std::size_t section{};
        std::uint64_t offset{};
        ModuleAsmBinding binding{ModuleAsmBinding::Local};
        ModuleAsmSymbolType type{ModuleAsmSymbolType::None};
    };

    struct ModuleAsmObject
    {
        std::vector<ModuleAsmSection> sections;
        std::vector<ModuleAsmSymbol> symbols;

        [[nodiscard]] bool empty() const noexcept { return sections.empty() && symbols.empty(); }

        [[nodiscard]] bool defines(std::string_view name) const noexcept
        {
            return std::ranges::any_of(symbols, [&](ModuleAsmSymbol const& s) { return s.name == name; });
        }
    };

    struct ModuleAsmError
    {
        sm::SourceRange range;
        std::string message;
    };

    [[nodiscard]] std::expected<ModuleAsmObject, ModuleAsmError> assemble_module_asm(std::span<ir::IrModuleAsm* const> blocks, EncodeMode mode,
                                                                                     ModuleAsmFormat format);

    [[nodiscard]] std::string module_asm_intel_text(ModuleAsmObject const& object, ModuleAsmFormat format, std::string_view prefix = "__dcc_");

    [[nodiscard]] std::vector<std::string> module_asm_external_references(ModuleAsmObject const& object);

} // namespace dcc::backend::x86

namespace dcc::backend::x86
{
    namespace
    {
        struct Token
        {
            enum class Kind : std::uint8_t
            {
                Ident,
                Number,
                String,
                Punct,
                End,
            };
            Kind kind{Kind::End};
            std::string text;
            std::int64_t value{};
            char punct{};
        };

        struct Value
        {
            std::int64_t constant{};
            std::string symbol;
            std::string minus;

            [[nodiscard]] bool is_constant() const noexcept { return symbol.empty() && minus.empty(); }
        };

        struct RegisterInfo
        {
            PhysReg reg{PhysReg::None};
            unsigned width{};
            bool high8{};
        };

        [[nodiscard]] std::optional<RegisterInfo> lookup_register(std::string_view raw)
        {
            std::string name(raw);
            for (auto& c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            static constexpr std::array<std::string_view, 16> r64{"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                                                  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
            static constexpr std::array<std::string_view, 16> r32{"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                                                  "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
            static constexpr std::array<std::string_view, 16> r16{"ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
                                                                  "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
            static constexpr std::array<std::string_view, 16> r8{"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                                                                 "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
            static constexpr std::array<std::string_view, 4> high{"ah", "ch", "dh", "bh"};
            static constexpr std::array<std::string_view, 6> segments{"es", "cs", "ss", "ds", "fs", "gs"};
            for (std::size_t i = 0; i < 16; ++i)
            {
                auto const reg = static_cast<PhysReg>(i);
                if (name == r64[i])
                    return RegisterInfo{reg, 64};
                if (name == r32[i])
                    return RegisterInfo{reg, 32};
                if (name == r16[i])
                    return RegisterInfo{reg, 16};
                if (name == r8[i])
                    return RegisterInfo{reg, 8};
            }
            for (std::size_t i = 0; i < high.size(); ++i)
                if (name == high[i])
                    return RegisterInfo{static_cast<PhysReg>(i), 8, true};
            for (std::size_t i = 0; i < segments.size(); ++i)
                if (name == segments[i])
                    return RegisterInfo{static_cast<PhysReg>(static_cast<std::uint8_t>(PhysReg::ES) + i), 16};
            return std::nullopt;
        }

        [[nodiscard]] SegmentOverride segment_of(PhysReg reg) noexcept
        {
            switch (reg)
            {
                case PhysReg::ES:
                    return SegmentOverride::ES;
                case PhysReg::CS:
                    return SegmentOverride::CS;
                case PhysReg::SS:
                    return SegmentOverride::SS;
                case PhysReg::DS:
                    return SegmentOverride::DS;
                case PhysReg::FS:
                    return SegmentOverride::FS;
                case PhysReg::GS:
                    return SegmentOverride::GS;
                default:
                    return SegmentOverride::None;
            }
        }

        [[nodiscard]] bool ident_start(char c) noexcept
        {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@';
        }

        [[nodiscard]] bool ident_char(char c) noexcept
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@' || c == '$';
        }

        [[nodiscard]] std::expected<std::vector<Token>, std::string> tokenize(std::string_view line)
        {
            std::vector<Token> tokens;
            std::size_t i = 0;
            while (i < line.size())
            {
                char const c = line[i];
                if (c == ' ' || c == '\t' || c == '\r')
                {
                    ++i;
                    continue;
                }
                if (ident_start(c))
                {
                    auto const start = i++;
                    while (i < line.size() && ident_char(line[i]))
                        ++i;
                    tokens.push_back({Token::Kind::Ident, std::string(line.substr(start, i - start)), 0, 0});
                    continue;
                }
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    auto const start = i;
                    while (i < line.size() && std::isalnum(static_cast<unsigned char>(line[i])))
                        ++i;
                    std::string_view text = line.substr(start, i - start);
                    auto const last = text.back();
                    bool const all_digits_before = std::ranges::all_of(text.substr(0, text.size() - 1), [](char d) { return d >= '0' && d <= '9'; });
                    if (text.size() >= 2 && (last == 'f' || last == 'b') && all_digits_before && !(last == 'b' && text.size() > 2 && text[0] == '0'))
                    {
                        tokens.push_back({Token::Kind::Ident, std::string(text), 0, 0});
                        continue;
                    }
                    std::uint64_t value = 0;
                    unsigned base = 10;
                    std::string_view digits = text;
                    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                    {
                        base = 16;
                        digits = text.substr(2);
                    }
                    else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
                    {
                        base = 2;
                        digits = text.substr(2);
                    }
                    else if (text.size() > 1 && text[0] == '0')
                    {
                        base = 8;
                        digits = text.substr(1);
                    }
                    for (char d : digits)
                    {
                        unsigned digit = 0;
                        if (d >= '0' && d <= '9')
                            digit = static_cast<unsigned>(d - '0');
                        else if (d >= 'a' && d <= 'f')
                            digit = static_cast<unsigned>(d - 'a' + 10);
                        else if (d >= 'A' && d <= 'F')
                            digit = static_cast<unsigned>(d - 'A' + 10);
                        else
                            return std::unexpected(std::format("invalid number '{}'", text));
                        if (digit >= base)
                            return std::unexpected(std::format("invalid number '{}'", text));
                        value = value * base + digit;
                    }
                    tokens.push_back({Token::Kind::Number, std::string(text), static_cast<std::int64_t>(value), 0});
                    continue;
                }
                if (c == '"')
                {
                    std::string value;
                    ++i;
                    bool closed = false;
                    while (i < line.size())
                    {
                        char const ch = line[i++];
                        if (ch == '"')
                        {
                            closed = true;
                            break;
                        }
                        if (ch != '\\' || i >= line.size())
                        {
                            value += ch;
                            continue;
                        }
                        char const esc = line[i++];
                        switch (esc)
                        {
                            case 'n':
                                value += '\n';
                                break;
                            case 't':
                                value += '\t';
                                break;
                            case 'r':
                                value += '\r';
                                break;
                            case 'b':
                                value += '\b';
                                break;
                            case 'f':
                                value += '\f';
                                break;
                            case 'x': {
                                unsigned code = 0;
                                std::size_t count = 0;
                                while (i < line.size() && std::isxdigit(static_cast<unsigned char>(line[i])))
                                {
                                    char const h = line[i++];
                                    code =
                                        code * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : std::tolower(h) - 'a' + 10);
                                    ++count;
                                }
                                if (count == 0)
                                    return std::unexpected(std::string{"invalid \\x escape in string"});
                                value += static_cast<char>(code & 0xFF);
                                break;
                            }
                            default:
                                if (esc >= '0' && esc <= '7')
                                {
                                    unsigned code = static_cast<unsigned>(esc - '0');
                                    for (int k = 0; k < 2 && i < line.size() && line[i] >= '0' && line[i] <= '7'; ++k)
                                        code = code * 8 + static_cast<unsigned>(line[i++] - '0');
                                    value += static_cast<char>(code & 0xFF);
                                }
                                else
                                    value += esc;
                                break;
                        }
                    }
                    if (!closed)
                        return std::unexpected(std::string{"unterminated string"});
                    tokens.push_back({Token::Kind::String, std::move(value), 0, 0});
                    continue;
                }
                if (c == '\'')
                {
                    if (i + 1 >= line.size())
                        return std::unexpected(std::string{"unterminated character constant"});
                    char value = line[i + 1];
                    std::size_t next = i + 2;
                    if (value == '\\' && i + 2 < line.size())
                    {
                        char const esc = line[i + 2];
                        value = esc == 'n' ? '\n' : esc == 't' ? '\t' : esc == '0' ? '\0' : esc == 'r' ? '\r' : esc;
                        next = i + 3;
                    }
                    if (next < line.size() && line[next] == '\'')
                        ++next;
                    tokens.push_back({Token::Kind::Number, std::string(line.substr(i, next - i)), static_cast<unsigned char>(value), 0});
                    i = next;
                    continue;
                }
                if (c == '<' || c == '>')
                {
                    if (i + 1 < line.size() && line[i + 1] == c)
                    {
                        tokens.push_back({Token::Kind::Punct, std::string(2, c), 0, c});
                        i += 2;
                        continue;
                    }
                    return std::unexpected(std::format("unexpected character '{}'", c));
                }
                static constexpr std::string_view puncts = ",:[]()+-*/%$~&|^!=";
                if (puncts.find(c) == std::string_view::npos)
                    return std::unexpected(std::format("unexpected character '{}'", c));
                tokens.push_back({Token::Kind::Punct, std::string(1, c), 0, c});
                ++i;
            }
            tokens.push_back({});
            return tokens;
        }

        struct MemRef
        {
            PhysReg base{PhysReg::None};
            unsigned base_width{};
            PhysReg index{PhysReg::None};
            unsigned index_width{};
            std::uint8_t scale{1};
            Value disp;
            SegmentOverride segment{SegmentOverride::None};
            bool rip{};
            bool bare{};

            [[nodiscard]] bool has_registers() const noexcept { return base != PhysReg::None || index != PhysReg::None; }
        };

        struct Operand
        {
            enum class Kind : std::uint8_t
            {
                Reg,
                Imm,
                Mem,
            };
            Kind kind{Kind::Imm};
            RegisterInfo reg;
            Value value;
            MemRef mem;
            unsigned size{};
            bool indirect{};
        };

        enum class BranchHint : std::uint8_t
        {
            None,
            Short,
            Near
        };

        struct Statement
        {
            std::string mnemonic;
            std::vector<Operand> operands;
            unsigned suffix_width{};
            bool rep{};
            bool addr32{};
            BranchHint branch_hint{BranchHint::None};
        };

        struct SectionState
        {
            std::size_t index{};
        };

        struct LocalSymbol
        {
            std::size_t section{};
            std::uint64_t offset{};
        };

        struct DataFixup
        {
            std::size_t section{};
            std::uint64_t offset{};
            unsigned size{};
            Value value;
            std::size_t line{};
        };

        class Assembler
        {
        public:
            Assembler(EncodeMode mode, ModuleAsmFormat format) : m_mode(mode), m_format(format) {}

            [[nodiscard]] std::expected<ModuleAsmObject, ModuleAsmError> run(std::span<ir::IrModuleAsm* const> blocks)
            {
                for (auto const* block : blocks)
                {
                    if (!block)
                        continue;
                    m_range = block->range;
                    m_att = true;
                    std::size_t line_number = 0;
                    std::string_view text = block->template_str;
                    std::size_t pos = 0;
                    while (pos <= text.size())
                    {
                        auto const end = std::min(text.find('\n', pos), text.size());
                        ++line_number;
                        m_line = line_number;
                        if (!process_line(text.substr(pos, end - pos)))
                            return std::unexpected(ModuleAsmError{m_range, m_error});
                        pos = end + 1;
                    }
                }
                if (!finish())
                    return std::unexpected(ModuleAsmError{m_range, m_error});
                return std::move(m_object);
            }

        private:
            EncodeMode m_mode;
            ModuleAsmFormat m_format;
            ModuleAsmObject m_object;
            sm::SourceRange m_range{};
            std::size_t m_line{};
            bool m_att{true};
            std::string m_error;
            std::optional<std::size_t> m_current;
            std::optional<std::size_t> m_previous;
            std::vector<std::size_t> m_section_stack;
            struct ShortBranch
            {
                std::size_t section{};
                std::uint64_t offset{};
                std::string symbol;
                std::size_t line{};
            };

            std::unordered_map<std::string, LocalSymbol> m_defined;
            std::vector<ShortBranch> m_short_branches;
            std::vector<std::string> m_definition_order;
            std::unordered_set<std::string> m_globals;
            std::unordered_set<std::string> m_weak;
            std::unordered_map<std::string, ModuleAsmSymbolType> m_types;
            std::unordered_map<std::string, std::int64_t> m_constants;
            std::unordered_map<std::string, std::size_t> m_numeric_definitions;
            std::vector<DataFixup> m_difference_fixups;
            std::size_t m_dot_counter{};
            std::vector<Token> m_tokens;
            std::size_t m_pos{};

            bool fail(std::string message)
            {
                if (m_error.empty())
                    m_error = std::format("module asm line {}: {}", m_line, message);
                return false;
            }

            [[nodiscard]] bool is_64() const noexcept { return m_mode == EncodeMode::Long64; }

            [[nodiscard]] Token const& peek(std::size_t ahead = 0) const
            {
                auto const index = std::min(m_pos + ahead, m_tokens.size() - 1);
                return m_tokens[index];
            }

            [[nodiscard]] bool at_end() const { return peek().kind == Token::Kind::End; }

            [[nodiscard]] bool is_punct(char c, std::size_t ahead = 0) const { return peek(ahead).kind == Token::Kind::Punct && peek(ahead).punct == c; }

            bool accept_punct(char c)
            {
                if (!is_punct(c))
                    return false;
                ++m_pos;
                return true;
            }

            [[nodiscard]] static std::vector<std::string_view> split_statements(std::string_view line)
            {
                std::vector<std::string_view> parts;
                bool in_string = false;
                std::size_t start = 0;
                for (std::size_t i = 0; i < line.size(); ++i)
                {
                    char const c = line[i];
                    if (in_string)
                    {
                        if (c == '\\')
                            ++i;
                        else if (c == '"')
                            in_string = false;
                        continue;
                    }
                    if (c == '"')
                        in_string = true;
                    else if (c == '\'' && i + 2 < line.size())
                        i += line[i + 1] == '\\' ? 3 : 2;
                    else if (c == '#')
                    {
                        parts.push_back(line.substr(start, i - start));
                        return parts;
                    }
                    else if (c == ';')
                    {
                        parts.push_back(line.substr(start, i - start));
                        start = i + 1;
                    }
                }
                parts.push_back(line.substr(start));
                return parts;
            }

            bool process_line(std::string_view line)
            {
                for (auto statement : split_statements(line))
                    if (!process_statement(statement))
                        return false;
                return true;
            }

            [[nodiscard]] static std::string trim(std::string_view text)
            {
                auto const first = text.find_first_not_of(" \t\r");
                if (first == std::string_view::npos)
                    return {};
                auto const last = text.find_last_not_of(" \t\r");
                return std::string(text.substr(first, last - first + 1));
            }

            ModuleAsmSection& section() { return m_object.sections[*m_current]; }

            bool ensure_section()
            {
                if (!m_current)
                    return switch_section(".text", std::nullopt, std::nullopt);
                return true;
            }

            [[nodiscard]] std::uint64_t here() { return section().size; }

            bool process_statement(std::string_view raw)
            {
                auto text = trim(raw);
                if (text.empty())
                    return true;
                auto tokens = tokenize(text);
                if (!tokens)
                    return fail(tokens.error());
                m_tokens = std::move(*tokens);
                m_pos = 0;

                while (true)
                {
                    if (peek().kind == Token::Kind::Ident && is_punct(':', 1) && !lookup_register(peek().text))
                    {
                        auto name = peek().text;
                        m_pos += 2;
                        if (!define_label(name))
                            return false;
                        continue;
                    }
                    if (peek().kind == Token::Kind::Number && is_punct(':', 1))
                    {
                        auto const number = peek().text;
                        m_pos += 2;
                        auto& count = m_numeric_definitions[number];
                        auto const name = std::format(".Ldcc.num.{}.{}", number, count);
                        ++count;
                        if (!define_label(name))
                            return false;
                        continue;
                    }
                    break;
                }
                if (at_end())
                    return true;
                if (peek().kind != Token::Kind::Ident)
                    return fail(std::format("expected a directive or an instruction, found '{}'", peek().text));
                auto const word = peek().text;
                if (word.starts_with('.'))
                {
                    ++m_pos;
                    return directive(word, text);
                }
                return instruction(text);
            }

            bool define_label(std::string const& name)
            {
                if (!ensure_section())
                    return false;
                if (m_defined.contains(name))
                    return fail(std::format("symbol '{}' is already defined", name));
                m_defined[name] = LocalSymbol{*m_current, here()};
                m_definition_order.push_back(name);
                return true;
            }

            [[nodiscard]] std::string resolve_name(std::string const& name)
            {
                if (name.size() >= 2 && (name.back() == 'f' || name.back() == 'b') &&
                    std::ranges::all_of(std::string_view(name).substr(0, name.size() - 1), [](char c) { return c >= '0' && c <= '9'; }))
                {
                    auto const number = name.substr(0, name.size() - 1);
                    auto const count = m_numeric_definitions[number];
                    if (name.back() == 'b')
                    {
                        if (count == 0)
                        {
                            fail(std::format("no previous definition of local label '{}'", number));
                            return {};
                        }
                        return std::format(".Ldcc.num.{}.{}", number, count - 1);
                    }
                    return std::format(".Ldcc.num.{}.{}", number, count);
                }
                if (name == ".")
                {
                    auto label = std::format(".Ldcc.dot.{}", m_dot_counter++);
                    if (!define_label(label))
                        return {};
                    return label;
                }
                return name;
            }

            std::optional<Value> parse_primary()
            {
                if (accept_punct('('))
                {
                    auto inner = parse_expression();
                    if (!inner || !accept_punct(')'))
                    {
                        fail("expected ')'");
                        return std::nullopt;
                    }
                    return inner;
                }
                if (accept_punct('-'))
                {
                    auto inner = parse_primary();
                    if (!inner)
                        return std::nullopt;
                    if (!inner->is_constant())
                    {
                        fail("cannot negate a symbol");
                        return std::nullopt;
                    }
                    inner->constant = -inner->constant;
                    return inner;
                }
                if (accept_punct('+'))
                    return parse_primary();
                if (accept_punct('~'))
                {
                    auto inner = parse_primary();
                    if (!inner)
                        return std::nullopt;
                    if (!inner->is_constant())
                    {
                        fail("cannot complement a symbol");
                        return std::nullopt;
                    }
                    inner->constant = ~inner->constant;
                    return inner;
                }
                if (peek().kind == Token::Kind::Number)
                {
                    Value v;
                    v.constant = peek().value;
                    ++m_pos;
                    return v;
                }
                if (peek().kind == Token::Kind::Ident)
                {
                    auto name = peek().text;
                    ++m_pos;
                    if (auto it = m_constants.find(name); it != m_constants.end())
                        return Value{it->second, {}, {}};
                    auto resolved = resolve_name(name);
                    if (resolved.empty())
                        return std::nullopt;
                    Value v;
                    v.symbol = std::move(resolved);
                    return v;
                }
                fail(std::format("expected an expression, found '{}'", peek().kind == Token::Kind::End ? std::string{"end of statement"} : peek().text));
                return std::nullopt;
            }

            static std::optional<std::int64_t> apply(char op, std::int64_t a, std::int64_t b)
            {
                switch (op)
                {
                    case '*':
                        return a * b;
                    case '/':
                        if (b == 0)
                            return std::nullopt;
                        return a / b;
                    case '%':
                        if (b == 0)
                            return std::nullopt;
                        return a % b;
                    case '<':
                        return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) << (b & 63));
                    case '>':
                        return a >> (b & 63);
                    case '&':
                        return a & b;
                    case '|':
                        return a | b;
                    case '^':
                        return a ^ b;
                    default:
                        return std::nullopt;
                }
            }

            std::optional<Value> parse_term()
            {
                auto lhs = parse_primary();
                if (!lhs)
                    return std::nullopt;
                while (is_punct('*') || is_punct('/') || is_punct('%') || is_punct('<') || is_punct('>'))
                {
                    char const op = peek().punct;
                    ++m_pos;
                    auto rhs = parse_primary();
                    if (!rhs)
                        return std::nullopt;
                    if (!lhs->is_constant() || !rhs->is_constant())
                    {
                        fail("symbols can only be added or subtracted");
                        return std::nullopt;
                    }
                    auto result = apply(op, lhs->constant, rhs->constant);
                    if (!result)
                    {
                        fail("division by zero");
                        return std::nullopt;
                    }
                    lhs->constant = *result;
                }
                return lhs;
            }

            std::optional<Value> parse_sum()
            {
                auto lhs = parse_term();
                if (!lhs)
                    return std::nullopt;
                while (is_punct('+') || is_punct('-'))
                {
                    bool const minus = peek().punct == '-';
                    ++m_pos;
                    auto rhs = parse_term();
                    if (!rhs)
                        return std::nullopt;
                    if (!rhs->minus.empty())
                    {
                        fail("expression is too complex");
                        return std::nullopt;
                    }
                    if (minus)
                    {
                        lhs->constant -= rhs->constant;
                        if (!rhs->symbol.empty())
                        {
                            if (!lhs->minus.empty())
                            {
                                fail("expression is too complex");
                                return std::nullopt;
                            }
                            lhs->minus = rhs->symbol;
                        }
                    }
                    else
                    {
                        lhs->constant += rhs->constant;
                        if (!rhs->symbol.empty())
                        {
                            if (!lhs->symbol.empty())
                            {
                                fail("cannot add two symbols");
                                return std::nullopt;
                            }
                            lhs->symbol = rhs->symbol;
                        }
                    }
                }
                return lhs;
            }

            std::optional<Value> parse_expression()
            {
                auto lhs = parse_sum();
                if (!lhs)
                    return std::nullopt;
                while (is_punct('&') || is_punct('|') || is_punct('^'))
                {
                    char const op = peek().punct;
                    ++m_pos;
                    auto rhs = parse_sum();
                    if (!rhs)
                        return std::nullopt;
                    if (!lhs->is_constant() || !rhs->is_constant())
                    {
                        fail("symbols can only be added or subtracted");
                        return std::nullopt;
                    }
                    lhs->constant = *apply(op, lhs->constant, rhs->constant);
                }
                return lhs;
            }

            std::optional<std::int64_t> parse_constant(std::string_view what)
            {
                auto v = parse_expression();
                if (!v)
                    return std::nullopt;
                if (!v->symbol.empty() || !v->minus.empty())
                {
                    if (!v->symbol.empty() && !v->minus.empty())
                    {
                        auto a = m_defined.find(v->symbol);
                        auto b = m_defined.find(v->minus);
                        if (a != m_defined.end() && b != m_defined.end() && a->second.section == b->second.section)
                            return v->constant + static_cast<std::int64_t>(a->second.offset) - static_cast<std::int64_t>(b->second.offset);
                    }
                    fail(std::format("{} must be a constant", what));
                    return std::nullopt;
                }
                return v->constant;
            }

            bool expect_end()
            {
                if (at_end())
                    return true;
                return fail(std::format("unexpected '{}'", peek().text));
            }

            bool switch_section(std::string const& name, std::optional<std::string> flags, std::optional<std::string> type)
            {
                std::optional<std::size_t> found;
                for (std::size_t i = 0; i < m_object.sections.size(); ++i)
                    if (m_object.sections[i].name == name)
                        found = i;
                if (!found)
                {
                    ModuleAsmSection s;
                    s.name = name;
                    bool const text = name == ".text" || name.starts_with(".text.") || name.starts_with(".text$");
                    bool const data = name == ".data" || name.starts_with(".data.") || name.starts_with(".data$");
                    bool const bss = name == ".bss" || name.starts_with(".bss.") || name.starts_with(".bss$");
                    bool const rodata = name == ".rodata" || name.starts_with(".rodata.") || name == ".rdata" || name.starts_with(".rdata$");
                    if (text)
                        s.alloc = s.exec = true;
                    else if (data)
                        s.alloc = s.write = true;
                    else if (bss)
                        s.alloc = s.write = s.nobits = true;
                    else if (rodata)
                        s.alloc = true;
                    else if (m_format == ModuleAsmFormat::Coff)
                        s.alloc = s.write = true;
                    if (flags)
                    {
                        s.alloc = s.write = s.exec = s.nobits = false;
                        for (char f : *flags)
                        {
                            if (m_format == ModuleAsmFormat::Elf)
                            {
                                if (f == 'a')
                                    s.alloc = true;
                                else if (f == 'w')
                                    s.write = true;
                                else if (f == 'x')
                                    s.exec = true;
                                else
                                    return fail(std::format("section flag '{}' is not supported", f));
                            }
                            else
                            {
                                if (f == 'x')
                                    s.alloc = s.exec = true;
                                else if (f == 'w')
                                    s.alloc = s.write = true;
                                else if (f == 'd' || f == 'r')
                                    s.alloc = true;
                                else if (f == 'b')
                                    s.alloc = s.write = s.nobits = true;
                                else
                                    return fail(std::format("section flag '{}' is not supported", f));
                            }
                        }
                        if (m_format == ModuleAsmFormat::Elf && bss && !type)
                            s.nobits = true;
                    }
                    if (type)
                    {
                        if (*type == "@nobits" || *type == "%nobits")
                            s.nobits = true;
                        else if (*type == "@progbits" || *type == "%progbits")
                            s.nobits = false;
                        else
                            return fail(std::format("section type '{}' is not supported", *type));
                    }
                    m_object.sections.push_back(std::move(s));
                    found = m_object.sections.size() - 1;
                }
                else if (flags || type)
                {
                    auto const& s = m_object.sections[*found];
                    std::ignore = s;
                }
                m_previous = m_current;
                m_current = *found;
                return true;
            }

            bool directive(std::string const& name, std::string const& text)
            {
                if (name == ".intel_syntax")
                {
                    m_att = false;
                    if (peek().kind == Token::Kind::Ident && peek().text == "prefix")
                        return fail("'.intel_syntax prefix' is not supported; use 'noprefix'");
                    if (peek().kind == Token::Kind::Ident && peek().text == "noprefix")
                        ++m_pos;
                    return expect_end();
                }
                if (name == ".att_syntax")
                {
                    m_att = true;
                    if (peek().kind == Token::Kind::Ident && peek().text == "noprefix")
                        return fail("'.att_syntax noprefix' is not supported");
                    if (peek().kind == Token::Kind::Ident && peek().text == "prefix")
                        ++m_pos;
                    return expect_end();
                }
                if (name == ".code16" || name == ".code32" || name == ".code64")
                {
                    bool const ok = (name == ".code16" && m_mode == EncodeMode::Real16) || (name == ".code64" && m_mode == EncodeMode::Long64);
                    if (!ok)
                        return fail(std::format("'{}' does not match the target's code mode", name));
                    return expect_end();
                }
                if (name == ".text" || name == ".data" || name == ".bss")
                {
                    if (!expect_end())
                        return false;
                    return switch_section(name, std::nullopt, std::nullopt);
                }
                if (name == ".section" || name == ".pushsection")
                {
                    std::string section_name;
                    while (!at_end() && !is_punct(','))
                    {
                        if (peek().kind == Token::Kind::String)
                            section_name += peek().text;
                        else if (peek().kind == Token::Kind::Ident || peek().kind == Token::Kind::Number)
                            section_name += peek().text;
                        else if (peek().kind == Token::Kind::Punct && (peek().punct == '$' || peek().punct == '-'))
                            section_name += peek().punct;
                        else
                            return fail("expected a section name");
                        ++m_pos;
                    }
                    if (section_name.empty())
                        return fail("expected a section name");
                    std::optional<std::string> flags;
                    std::optional<std::string> type;
                    if (accept_punct(','))
                    {
                        if (peek().kind != Token::Kind::String)
                            return fail("expected section flags in quotes");
                        flags = peek().text;
                        ++m_pos;
                        if (accept_punct(','))
                        {
                            if (m_format == ModuleAsmFormat::Coff)
                                return fail("COFF sections take no section type");
                            if (accept_punct('%'))
                            {
                                if (peek().kind != Token::Kind::Ident)
                                    return fail("expected a section type");
                                type = "%" + peek().text;
                            }
                            else if (peek().kind == Token::Kind::Ident)
                                type = peek().text;
                            else
                                return fail("expected a section type");
                            ++m_pos;
                        }
                    }
                    if (!expect_end())
                        return false;
                    if (name == ".pushsection" && m_current)
                        m_section_stack.push_back(*m_current);
                    else if (name == ".pushsection")
                    {
                        if (!ensure_section())
                            return false;
                        m_section_stack.push_back(*m_current);
                    }
                    return switch_section(section_name, flags, type);
                }
                if (name == ".popsection")
                {
                    if (m_section_stack.empty())
                        return fail("'.popsection' without '.pushsection'");
                    m_previous = m_current;
                    m_current = m_section_stack.back();
                    m_section_stack.pop_back();
                    return expect_end();
                }
                if (name == ".previous")
                {
                    if (!m_previous)
                        return fail("'.previous' without a previous section");
                    std::swap(m_previous, m_current);
                    return expect_end();
                }
                if (name == ".globl" || name == ".global" || name == ".weak" || name == ".local")
                {
                    do
                    {
                        if (peek().kind != Token::Kind::Ident)
                            return fail(std::format("expected a symbol name after '{}'", name));
                        auto const symbol = peek().text;
                        ++m_pos;
                        if (name == ".weak")
                            m_weak.insert(symbol);
                        else if (name == ".local")
                        {
                            m_globals.erase(symbol);
                            m_weak.erase(symbol);
                        }
                        else
                            m_globals.insert(symbol);
                    } while (accept_punct(','));
                    return expect_end();
                }
                if (name == ".type")
                {
                    if (m_format != ModuleAsmFormat::Elf)
                        return fail("'.type' is only supported for ELF targets");
                    if (peek().kind != Token::Kind::Ident)
                        return fail("expected a symbol name after '.type'");
                    auto const symbol = peek().text;
                    ++m_pos;
                    if (!accept_punct(','))
                        return fail("expected ',' after the symbol name");
                    std::string kind;
                    if (accept_punct('%'))
                    {
                        if (peek().kind != Token::Kind::Ident)
                            return fail("expected a symbol type");
                        kind = peek().text;
                    }
                    else if (peek().kind == Token::Kind::Ident)
                        kind = peek().text.starts_with('@') ? peek().text.substr(1) : peek().text;
                    else
                        return fail("expected a symbol type");
                    ++m_pos;
                    if (kind == "function")
                        m_types[symbol] = ModuleAsmSymbolType::Function;
                    else if (kind == "object")
                        m_types[symbol] = ModuleAsmSymbolType::Object;
                    else if (kind == "notype")
                        m_types[symbol] = ModuleAsmSymbolType::None;
                    else
                        return fail(std::format("symbol type '{}' is not supported", kind));
                    return expect_end();
                }
                if (name == ".size" || name == ".file" || name == ".ident")
                    return true;
                if (name == ".set" || name == ".equ" || name == ".equiv")
                {
                    if (peek().kind != Token::Kind::Ident)
                        return fail(std::format("expected a name after '{}'", name));
                    auto const symbol = peek().text;
                    ++m_pos;
                    if (!accept_punct(','))
                        return fail("expected ','");
                    auto value = parse_constant(std::format("the value of '{}'", symbol));
                    if (!value)
                        return false;
                    if (name == ".equiv" && m_constants.contains(symbol))
                        return fail(std::format("'{}' is already defined", symbol));
                    m_constants[symbol] = *value;
                    return expect_end();
                }
                if (!ensure_section())
                    return false;
                if (name == ".align" || name == ".balign" || name == ".p2align")
                    return align_directive(name, text);
                if (name == ".byte" || name == ".word" || name == ".short" || name == ".value" || name == ".2byte" || name == ".long" || name == ".int" ||
                    name == ".4byte" || name == ".quad" || name == ".8byte")
                {
                    unsigned const size = name == ".byte"                                                                 ? 1
                                          : (name == ".word" || name == ".short" || name == ".value" || name == ".2byte") ? 2
                                          : (name == ".quad" || name == ".8byte")                                         ? 8
                                                                                                                          : 4;
                    return data_directive(size, text);
                }
                if (name == ".ascii" || name == ".asciz" || name == ".string")
                {
                    if (section().nobits)
                        return fail("cannot emit data into a section without contents");
                    record_line(text);
                    do
                    {
                        if (peek().kind != Token::Kind::String)
                            return fail(std::format("expected a string after '{}'", name));
                        auto const& value = peek().text;
                        for (char c : value)
                            emit_byte(static_cast<std::uint8_t>(c));
                        if (name != ".ascii")
                            emit_byte(0);
                        ++m_pos;
                    } while (accept_punct(','));
                    return expect_end();
                }
                if (name == ".zero" || name == ".skip" || name == ".space")
                {
                    auto count = parse_constant("the size");
                    if (!count)
                        return false;
                    std::int64_t fill = 0;
                    if (accept_punct(','))
                    {
                        auto value = parse_constant("the fill value");
                        if (!value)
                            return false;
                        fill = *value;
                    }
                    if (*count < 0)
                        return fail("negative size");
                    if (!expect_end())
                        return false;
                    record_line(text);
                    if (section().nobits)
                    {
                        if (fill != 0)
                            return fail("a section without contents can only be filled with zero");
                        section().size += static_cast<std::uint64_t>(*count);
                        return true;
                    }
                    for (std::int64_t i = 0; i < *count; ++i)
                        emit_byte(static_cast<std::uint8_t>(fill));
                    return true;
                }
                return fail(std::format("directive '{}' is not supported by the custom backend", name));
            }

            bool align_directive(std::string const& name, std::string const& text)
            {
                auto amount = parse_constant("the alignment");
                if (!amount)
                    return false;
                std::optional<std::int64_t> fill;
                if (accept_punct(','))
                {
                    if (!is_punct(','))
                    {
                        auto value = parse_constant("the fill value");
                        if (!value)
                            return false;
                        fill = *value;
                    }
                    if (accept_punct(','))
                    {
                        auto limit = parse_constant("the maximum skip");
                        if (!limit)
                            return false;
                        return fail("an alignment with a maximum skip is not supported");
                    }
                }
                if (!expect_end())
                    return false;
                std::uint64_t alignment = 0;
                if (name == ".p2align")
                {
                    if (*amount < 0 || *amount > 16)
                        return fail("alignment exponent out of range");
                    alignment = std::uint64_t{1} << *amount;
                }
                else
                {
                    if (*amount <= 0 || (*amount & (*amount - 1)) != 0)
                        return fail("alignment must be a power of two");
                    alignment = static_cast<std::uint64_t>(*amount);
                }
                auto& s = section();
                s.alignment = std::max(s.alignment, alignment);
                record_line(text);
                auto const target = (s.size + alignment - 1) / alignment * alignment;
                if (s.nobits)
                {
                    s.size = target;
                    return true;
                }
                std::uint8_t const pad = fill ? static_cast<std::uint8_t>(*fill) : s.exec ? std::uint8_t{0x90} : std::uint8_t{0};
                while (s.size < target)
                    emit_byte(pad);
                return true;
            }

            void emit_byte(std::uint8_t byte)
            {
                auto& s = section();
                s.data.push_back(byte);
                s.size = s.data.size();
            }

            void record_line(std::string const& text)
            {
                auto& s = section();
                s.lines.push_back({s.size, text});
            }

            [[nodiscard]] std::optional<Reloc::Kind> absolute_kind(unsigned size) const
            {
                switch (size)
                {
                    case 2:
                        return Reloc::Kind::Abs16;
                    case 4:
                        return Reloc::Kind::Abs32;
                    case 8:
                        return Reloc::Kind::Abs64;
                    default:
                        return std::nullopt;
                }
            }

            bool data_directive(unsigned size, std::string const& text)
            {
                if (section().nobits)
                    return fail("cannot emit data into a section without contents");
                record_line(text);
                do
                {
                    auto value = parse_expression();
                    if (!value)
                        return false;
                    auto const offset = here();
                    for (unsigned i = 0; i < size; ++i)
                        emit_byte(0);
                    if (!value->minus.empty())
                    {
                        if (value->symbol.empty())
                            return fail("cannot subtract a symbol from a constant");
                        m_difference_fixups.push_back({*m_current, offset, size, *value, m_line});
                        continue;
                    }
                    if (!value->symbol.empty())
                    {
                        auto kind = absolute_kind(size);
                        if (!kind)
                            return fail("a symbol address does not fit a byte");
                        section().relocations.push_back({offset, value->symbol, *kind, value->constant});
                        continue;
                    }
                    if (size < 8)
                    {
                        auto const bits = size * 8;
                        auto const lo = -(std::int64_t{1} << (bits - 1));
                        auto const hi = (std::int64_t{1} << bits) - 1;
                        if (value->constant < lo || value->constant > hi)
                            return fail(std::format("value {} does not fit {} bytes", value->constant, size));
                    }
                    for (unsigned i = 0; i < size; ++i)
                        section().data[offset + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value->constant) >> (8 * i));
                } while (accept_punct(','));
                return expect_end();
            }

            std::optional<Operand> parse_intel_operand()
            {
                Operand op;
                if (peek().kind == Token::Kind::Ident)
                {
                    static constexpr std::array<std::pair<std::string_view, unsigned>, 5> sizes{
                        {{"byte", 8}, {"word", 16}, {"dword", 32}, {"qword", 64}, {"tbyte", 80}}};
                    std::string lowered = peek().text;
                    for (auto& c : lowered)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    for (auto const& [word, bits] : sizes)
                        if (lowered == word)
                        {
                            op.size = bits;
                            ++m_pos;
                            if (peek().kind == Token::Kind::Ident && (peek().text == "ptr" || peek().text == "PTR"))
                                ++m_pos;
                            break;
                        }
                }
                if (peek().kind == Token::Kind::Ident && (peek().text == "offset" || peek().text == "OFFSET"))
                {
                    ++m_pos;
                    auto value = parse_expression();
                    if (!value)
                        return std::nullopt;
                    op.kind = Operand::Kind::Imm;
                    op.value = std::move(*value);
                    return op;
                }
                auto segment = SegmentOverride::None;
                if (peek().kind == Token::Kind::Ident && is_punct(':', 1))
                {
                    auto reg = lookup_register(peek().text);
                    if (!reg || reg_class(reg->reg) != RegClass::Segment)
                    {
                        fail(std::format("'{}' is not a segment register", peek().text));
                        return std::nullopt;
                    }
                    segment = segment_of(reg->reg);
                    m_pos += 2;
                }
                if (accept_punct('['))
                {
                    op.kind = Operand::Kind::Mem;
                    if (!parse_intel_memory(op.mem))
                        return std::nullopt;
                    if (!accept_punct(']'))
                    {
                        fail("expected ']'");
                        return std::nullopt;
                    }
                    if (segment != SegmentOverride::None)
                    {
                        if (op.mem.segment != SegmentOverride::None)
                        {
                            fail("memory operand has more than one segment override");
                            return std::nullopt;
                        }
                        op.mem.segment = segment;
                    }
                    return op;
                }
                if (segment == SegmentOverride::None && peek().kind == Token::Kind::Ident && (is_punct(',', 1) || peek(1).kind == Token::Kind::End))
                {
                    if (auto reg = lookup_register(peek().text))
                    {
                        ++m_pos;
                        op.kind = Operand::Kind::Reg;
                        op.reg = *reg;
                        return op;
                    }
                }
                auto value = parse_expression();
                if (!value)
                    return std::nullopt;
                if (value->is_constant() && segment == SegmentOverride::None)
                {
                    op.kind = Operand::Kind::Imm;
                    op.value = std::move(*value);
                    return op;
                }
                op.kind = Operand::Kind::Mem;
                op.mem.disp = std::move(*value);
                op.mem.segment = segment;
                op.mem.bare = true;
                return op;
            }

            bool add_address_register(MemRef& mem, RegisterInfo const& reg, std::int64_t scale)
            {
                if (reg_class(reg.reg) != RegClass::GPR || reg.width < 16 || reg.high8)
                    return fail("invalid register in a memory operand");
                if (scale != 1 || mem.base != PhysReg::None)
                {
                    if (mem.index != PhysReg::None)
                        return fail("memory operand has more than two registers");
                    if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
                        return fail("scale must be 1, 2, 4 or 8");
                    mem.index = reg.reg;
                    mem.index_width = reg.width;
                    mem.scale = static_cast<std::uint8_t>(scale);
                    return true;
                }
                mem.base = reg.reg;
                mem.base_width = reg.width;
                return true;
            }

            bool parse_intel_memory(MemRef& mem)
            {
                if (peek().kind == Token::Kind::Ident && is_punct(':', 1))
                {
                    auto reg = lookup_register(peek().text);
                    if (!reg || reg_class(reg->reg) != RegClass::Segment)
                        return fail(std::format("'{}' is not a segment register", peek().text));
                    mem.segment = segment_of(reg->reg);
                    m_pos += 2;
                }
                bool negative = false;
                bool first = true;
                while (!is_punct(']') && !at_end())
                {
                    if (!first)
                    {
                        if (accept_punct('+'))
                            negative = false;
                        else if (accept_punct('-'))
                            negative = true;
                        else
                            return fail("expected '+' or '-' in a memory operand");
                    }
                    else if (accept_punct('-'))
                        negative = true;
                    first = false;
                    if (peek().kind == Token::Kind::Ident && (peek().text == "rip" || peek().text == "RIP"))
                    {
                        if (negative || mem.rip || mem.has_registers())
                            return fail("invalid rip-relative memory operand");
                        mem.rip = true;
                        ++m_pos;
                        continue;
                    }
                    if (peek().kind == Token::Kind::Ident && lookup_register(peek().text))
                    {
                        auto reg = *lookup_register(peek().text);
                        ++m_pos;
                        std::int64_t scale = 1;
                        if (accept_punct('*'))
                        {
                            auto factor = parse_primary();
                            if (!factor || !factor->is_constant())
                                return fail("expected a constant scale");
                            scale = factor->constant;
                        }
                        if (negative)
                            return fail("a register cannot be subtracted in a memory operand");
                        if (!add_address_register(mem, reg, scale))
                            return false;
                        continue;
                    }
                    if (peek().kind == Token::Kind::Number && is_punct('*', 1) && peek(2).kind == Token::Kind::Ident && lookup_register(peek(2).text))
                    {
                        auto const scale = peek().value;
                        auto reg = *lookup_register(peek(2).text);
                        m_pos += 3;
                        if (negative)
                            return fail("a register cannot be subtracted in a memory operand");
                        if (!add_address_register(mem, reg, scale == 1 && mem.base == PhysReg::None ? 2 : scale))
                            return false;
                        if (scale == 1 && mem.index == reg.reg)
                            mem.scale = 1;
                        continue;
                    }
                    auto term = parse_term();
                    if (!term)
                        return false;
                    if (negative)
                    {
                        if (!term->symbol.empty())
                        {
                            if (!mem.disp.minus.empty())
                                return fail("memory displacement is too complex");
                            mem.disp.minus = term->symbol;
                        }
                        mem.disp.constant -= term->constant;
                    }
                    else
                    {
                        if (!term->symbol.empty())
                        {
                            if (!mem.disp.symbol.empty())
                                return fail("memory displacement is too complex");
                            mem.disp.symbol = term->symbol;
                        }
                        mem.disp.constant += term->constant;
                    }
                }
                return true;
            }

            std::optional<Operand> parse_att_operand()
            {
                Operand op;
                if (accept_punct('*'))
                    op.indirect = true;
                auto segment = SegmentOverride::None;
                if (is_punct('%') && peek(1).kind == Token::Kind::Ident && is_punct(':', 2))
                {
                    auto reg = lookup_register(peek(1).text);
                    if (!reg || reg_class(reg->reg) != RegClass::Segment)
                    {
                        fail(std::format("'{}' is not a segment register", peek(1).text));
                        return std::nullopt;
                    }
                    segment = segment_of(reg->reg);
                    m_pos += 3;
                }
                if (segment == SegmentOverride::None && accept_punct('%'))
                {
                    if (peek().kind != Token::Kind::Ident)
                    {
                        fail("expected a register after '%'");
                        return std::nullopt;
                    }
                    auto reg = lookup_register(peek().text);
                    if (!reg)
                    {
                        fail(std::format("unknown register '{}'", peek().text));
                        return std::nullopt;
                    }
                    ++m_pos;
                    op.kind = Operand::Kind::Reg;
                    op.reg = *reg;
                    return op;
                }
                if (segment == SegmentOverride::None && accept_punct('$'))
                {
                    auto value = parse_expression();
                    if (!value)
                        return std::nullopt;
                    op.kind = Operand::Kind::Imm;
                    op.value = std::move(*value);
                    return op;
                }
                op.kind = Operand::Kind::Mem;
                op.mem.segment = segment;
                bool const memory_parens = is_punct('(') && (is_punct('%', 1) || is_punct(',', 1));
                if (!memory_parens)
                {
                    auto value = parse_expression();
                    if (!value)
                        return std::nullopt;
                    op.mem.disp = std::move(*value);
                }
                if (!(is_punct('(') && (is_punct('%', 1) || is_punct(',', 1))))
                {
                    op.mem.bare = true;
                    return op;
                }
                ++m_pos;
                auto read_register = [&]() -> std::optional<RegisterInfo> {
                    if (!accept_punct('%') || peek().kind != Token::Kind::Ident)
                    {
                        fail("expected a register in the memory operand");
                        return std::nullopt;
                    }
                    auto const name = peek().text;
                    ++m_pos;
                    if (name == "rip")
                        return RegisterInfo{PhysReg::None, 64};
                    auto reg = lookup_register(name);
                    if (!reg)
                    {
                        fail(std::format("unknown register '{}'", name));
                        return std::nullopt;
                    }
                    return reg;
                };
                if (is_punct('%'))
                {
                    auto base = read_register();
                    if (!base)
                        return std::nullopt;
                    if (base->reg == PhysReg::None)
                        op.mem.rip = true;
                    else if (!add_address_register(op.mem, *base, 1))
                        return std::nullopt;
                }
                if (accept_punct(','))
                {
                    auto index = read_register();
                    if (!index || index->reg == PhysReg::None)
                    {
                        fail("invalid index register");
                        return std::nullopt;
                    }
                    std::int64_t scale = 1;
                    if (accept_punct(','))
                    {
                        auto factor = parse_constant("the scale");
                        if (!factor)
                            return std::nullopt;
                        scale = *factor;
                    }
                    if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
                    {
                        fail("scale must be 1, 2, 4 or 8");
                        return std::nullopt;
                    }
                    if (op.mem.index != PhysReg::None || reg_class(index->reg) != RegClass::GPR || index->width < 16)
                    {
                        fail("invalid index register");
                        return std::nullopt;
                    }
                    op.mem.index = index->reg;
                    op.mem.index_width = index->width;
                    op.mem.scale = static_cast<std::uint8_t>(scale);
                }
                if (!accept_punct(')'))
                {
                    fail("expected ')'");
                    return std::nullopt;
                }
                return op;
            }

            bool instruction(std::string const& text)
            {
                if (!ensure_section())
                    return false;
                if (!section().exec)
                    return fail(std::format("instruction in non-executable section '{}'", section().name));
                Statement st;
                while (true)
                {
                    if (peek().kind != Token::Kind::Ident)
                        return fail("expected an instruction");
                    std::string word = peek().text;
                    for (auto& c : word)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    ++m_pos;
                    if ((word == "rep" || word == "repe" || word == "repz") && peek().kind == Token::Kind::Ident)
                    {
                        st.rep = true;
                        continue;
                    }
                    if ((word == "addr32" || word == "a32") && peek().kind == Token::Kind::Ident)
                    {
                        st.addr32 = true;
                        continue;
                    }
                    if (word == "lock" || word == "repne" || word == "repnz" || word == "data16" || word == "data32" || word == "addr16")
                        return fail(std::format("prefix '{}' is not supported by the custom backend", word));
                    st.mnemonic = word;
                    break;
                }
                if (peek().kind == Token::Kind::Ident && (peek().text == "short" || peek().text == "near") && peek(1).kind == Token::Kind::Ident)
                {
                    st.branch_hint = peek().text == "short" ? BranchHint::Short : BranchHint::Near;
                    ++m_pos;
                }
                if (!at_end())
                {
                    do
                    {
                        auto op = m_att ? parse_att_operand() : parse_intel_operand();
                        if (!op)
                            return false;
                        st.operands.push_back(std::move(*op));
                    } while (accept_punct(','));
                    if (!expect_end())
                        return false;
                }
                if (m_att)
                {
                    std::ranges::reverse(st.operands);
                    apply_att_suffix(st);
                }
                record_line(text);
                return emit_instruction(st);
            }

            void apply_att_suffix(Statement& st)
            {
                static constexpr std::array<std::string_view, 33> suffixable{
                    "mov",  "add", "sub", "and", "or",  "xor",  "cmp", "test", "adc",  "sbb", "inc", "dec", "neg", "not",  "mul",   "div", "idiv",
                    "imul", "shl", "sal", "shr", "sar", "push", "pop", "lea",  "call", "jmp", "ret", "in",  "out", "xchg", "pushf", "popf"};
                if (st.mnemonic.size() < 2)
                    return;
                char const last = st.mnemonic.back();
                unsigned const width = last == 'b' ? 8 : last == 'w' ? 16 : last == 'l' ? 32 : last == 'q' ? 64 : 0;
                if (width == 0)
                    return;
                auto const stem = st.mnemonic.substr(0, st.mnemonic.size() - 1);
                if (std::ranges::find(suffixable, stem) == suffixable.end())
                    return;
                st.mnemonic = stem;
                st.suffix_width = width;
            }

            struct Selected
            {
                MInstr instr;
                std::optional<Value> immediate;
                unsigned immediate_bytes{};
            };

            [[nodiscard]] static MOp reg_op(RegisterInfo const& r) { return MOp::from_reg(VReg::phys(r.reg)); }

            std::optional<MMem> make_mem(MemRef const& ref, bool addr32)
            {
                MMem m{};
                m.segment = ref.segment;
                m.disp = 0;
                if (!ref.disp.minus.empty())
                {
                    fail("a memory displacement cannot subtract a symbol");
                    return std::nullopt;
                }
                if (ref.disp.constant < std::numeric_limits<std::int32_t>::min() || ref.disp.constant > std::numeric_limits<std::uint32_t>::max())
                {
                    fail("memory displacement out of range");
                    return std::nullopt;
                }
                m.disp = static_cast<std::int32_t>(ref.disp.constant);
                if (!ref.disp.symbol.empty())
                    m.symbol = intern(ref.disp.symbol);
                if (ref.base != PhysReg::None)
                    m.base = VReg::phys(ref.base);
                if (ref.index != PhysReg::None)
                    m.index = VReg::phys(ref.index);
                m.scale = ref.scale;
                if (is_64())
                {
                    if (ref.rip)
                    {
                        if (ref.disp.symbol.empty())
                        {
                            fail("rip-relative operands need a symbol");
                            return std::nullopt;
                        }
                        return m;
                    }
                    if (!ref.disp.symbol.empty())
                    {
                        fail("absolute symbol addresses are not supported in 64-bit module asm; use rip-relative addressing");
                        return std::nullopt;
                    }
                    if ((ref.base != PhysReg::None && ref.base_width != 64) || (ref.index != PhysReg::None && ref.index_width != 64))
                    {
                        fail("64-bit module asm addresses memory through 64-bit registers");
                        return std::nullopt;
                    }
                    if (!ref.has_registers())
                    {
                        if (ref.segment == SegmentOverride::None)
                        {
                            fail("absolute addresses need a segment override in 64-bit module asm");
                            return std::nullopt;
                        }
                        m.absolute = true;
                    }
                    return m;
                }
                if (ref.rip)
                {
                    fail("rip-relative addressing is only available in 64-bit code");
                    return std::nullopt;
                }
                unsigned width = 0;
                if (ref.base != PhysReg::None)
                    width = ref.base_width;
                if (ref.index != PhysReg::None)
                {
                    if (width != 0 && width != ref.index_width)
                    {
                        fail("memory operand mixes register widths");
                        return std::nullopt;
                    }
                    width = ref.index_width;
                }
                if (width == 64)
                {
                    fail("64-bit registers are not available in 16-bit code");
                    return std::nullopt;
                }
                if (width == 0)
                    width = addr32 ? 32 : 16;
                if (addr32 && width != 32)
                {
                    fail("'addr32' requires 32-bit address registers");
                    return std::nullopt;
                }
                m.address_bits = static_cast<std::uint8_t>(width);
                return m;
            }

            std::deque<std::string> m_strings;

            [[nodiscard]] std::string_view intern(std::string const& s)
            {
                m_strings.push_back(s);
                return m_strings.back();
            }

            std::optional<unsigned> operand_width(Statement const& st, std::size_t mem_slot = 99)
            {
                unsigned width = st.suffix_width;
                for (std::size_t i = 0; i < st.operands.size(); ++i)
                {
                    auto const& op = st.operands[i];
                    unsigned w = 0;
                    if (op.kind == Operand::Kind::Reg && reg_class(op.reg.reg) == RegClass::GPR)
                        w = op.reg.width;
                    else if (op.kind == Operand::Kind::Mem && op.size != 0)
                        w = op.size;
                    if (w == 0 || i == mem_slot)
                        continue;
                    if (width != 0 && width != w)
                    {
                        fail("operand sizes do not match");
                        return std::nullopt;
                    }
                    width = w;
                }
                if (width == 0)
                {
                    fail(std::format("cannot determine the operand size of '{}'; add a size keyword or suffix", st.mnemonic));
                    return std::nullopt;
                }
                return width;
            }

            static std::optional<MOpc> pick(unsigned width, std::array<std::optional<MOpc>, 4> const& table)
            {
                switch (width)
                {
                    case 8:
                        return table[0];
                    case 16:
                        return table[1];
                    case 32:
                        return table[2];
                    case 64:
                        return table[3];
                    default:
                        return std::nullopt;
                }
            }

            bool check_register(RegisterInfo const& r)
            {
                if (r.high8)
                    return fail("registers ah, bh, ch and dh are not supported by the custom backend");
                if (!is_64() && (static_cast<std::uint8_t>(r.reg) > static_cast<std::uint8_t>(PhysReg::RDI) && reg_class(r.reg) == RegClass::GPR))
                    return fail("registers r8 to r15 are not available in 16-bit code");
                if (!is_64() && r.width == 64)
                    return fail("64-bit registers are not available in 16-bit code");
                return true;
            }

            bool immediate_fits(Value const& v, unsigned width)
            {
                if (!v.is_constant() || width >= 64)
                    return true;
                auto const lo = -(std::int64_t{1} << (width - 1));
                auto const hi = (std::int64_t{1} << width) - 1;
                if (v.constant < lo || v.constant > hi)
                    return fail(std::format("immediate {} does not fit {} bits", v.constant, width));
                return true;
            }

            bool set_immediate(Selected& sel, unsigned slot, Value const& v, unsigned width)
            {
                if (!v.minus.empty())
                    return fail("an immediate cannot subtract a symbol");
                if (v.symbol.empty())
                {
                    sel.instr.ops[slot] = MOp::from_imm(v.constant);
                    return true;
                }
                if (width == 8)
                    return fail("a symbol address does not fit an 8-bit immediate");
                sel.immediate = v;
                sel.immediate_bytes = width / 8;
                std::int64_t const placeholder = width == 16 ? 0x1234 : width == 32 ? 0x12345678 : 0x123456789ALL;
                sel.instr.ops[slot] = MOp::from_imm(placeholder);
                return true;
            }

            std::optional<Selected> select(Statement& st)
            {
                auto& ops = st.operands;
                Selected sel;
                auto& mi = sel.instr;
                auto const& m = st.mnemonic;
                for (auto const& op : ops)
                    if (op.kind == Operand::Kind::Reg && !check_register(op.reg))
                        return std::nullopt;
                auto memory = [&](Operand const& op) -> std::optional<MOp> {
                    auto mem = make_mem(op.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    return MOp::from_mem(*mem);
                };
                auto none = [&](MOpc opc) -> std::optional<Selected> {
                    if (!ops.empty())
                    {
                        fail(std::format("'{}' takes no operands", m));
                        return std::nullopt;
                    }
                    mi.opc = opc;
                    return sel;
                };
                if (st.rep && m != "movsb" && m != "stosb" && m != "movsw" && m != "stosw")
                {
                    fail(std::format("'rep' is not supported with '{}'", m));
                    return std::nullopt;
                }
                if (m == "movsb" || m == "stosb" || m == "movsw" || m == "stosw")
                {
                    if (!st.rep)
                    {
                        fail(std::format("'{}' is only supported with a 'rep' prefix", m));
                        return std::nullopt;
                    }
                    bool const word = m.back() == 'w';
                    bool const move = m.starts_with("movs");
                    if (is_64())
                    {
                        if (word || st.addr32)
                        {
                            fail(std::format("'rep {}' is not supported in 64-bit module asm", m));
                            return std::nullopt;
                        }
                        return none(move ? MOpc::REPMOVS : MOpc::REPSTOS);
                    }
                    if (st.addr32)
                        return none(move ? (word ? MOpc::REPMOVSW_A32 : MOpc::REPMOVS_A32) : (word ? MOpc::REPSTOSW_A32 : MOpc::REPSTOS_A32));
                    return none(move ? (word ? MOpc::REPMOVSW : MOpc::REPMOVS) : (word ? MOpc::REPSTOSW : MOpc::REPSTOS));
                }
                if (m == "nop")
                    return none(MOpc::NOP);
                if (m == "hlt")
                    return none(MOpc::HLT);
                if (m == "cli")
                    return none(MOpc::CLI);
                if (m == "sti")
                    return none(MOpc::STI);
                if (m == "cld")
                    return none(MOpc::CLD);
                if (m == "std")
                    return none(MOpc::STD);
                if (m == "ud2")
                    return none(MOpc::UD2);
                if (m == "cbw" || m == "cbtw")
                    return none(MOpc::CBW);
                if (m == "cwde" || m == "cwtl")
                    return none(MOpc::CWDE);
                if (m == "cwd" || m == "cwtd")
                    return none(MOpc::CWD);
                if (m == "cdq" || m == "cltd")
                    return none(MOpc::CDQ);
                if (m == "cqo" || m == "cqto")
                    return none(MOpc::CQO);
                if (m == "ret" || m == "retw" || m == "retq")
                    return none(MOpc::RET);
                if (m == "pushf" || m == "popf")
                {
                    unsigned const width = st.suffix_width ? st.suffix_width : is_64() ? 64 : 16;
                    if (is_64() ? width != 64 : width != 16)
                    {
                        fail(std::format("'{}' with this operand size is not supported", m));
                        return std::nullopt;
                    }
                    return none(m == "pushf" ? (is_64() ? MOpc::PUSHFQ : MOpc::PUSHF16) : (is_64() ? MOpc::POPFQ : MOpc::POPF16));
                }
                if (m == "int")
                {
                    if (ops.size() != 1 || ops[0].kind != Operand::Kind::Imm || !ops[0].value.is_constant())
                    {
                        fail("'int' expects an immediate vector");
                        return std::nullopt;
                    }
                    mi.opc = MOpc::INT;
                    mi.num_ops = 1;
                    mi.ops[0] = MOp::from_imm(ops[0].value.constant);
                    return sel;
                }
                if (m == "in" || m == "out")
                    return select_port(st, sel);
                if (m == "call" || m == "jmp" || jcc(m))
                    return select_branch(st, sel);
                if (m == "push" || m == "pop")
                    return select_stack(st, sel);
                if (m == "mov")
                    return select_mov(st, sel);
                if (m == "movzx" || m == "movsx" || m.starts_with("movz") || m.starts_with("movs"))
                    return select_extend(st, sel);
                if (m == "lea")
                {
                    if (ops.size() != 2 || ops[0].kind != Operand::Kind::Reg || ops[1].kind != Operand::Kind::Mem)
                    {
                        fail("'lea' expects a register and a memory operand");
                        return std::nullopt;
                    }
                    auto opc = pick(ops[0].reg.width, {std::nullopt, MOpc::LEA16rm, MOpc::LEA32rm, MOpc::LEA64rm});
                    auto mem = memory(ops[1]);
                    if (!opc || !mem)
                        return fail_none(st);
                    mi.opc = *opc;
                    mi.num_ops = 2;
                    mi.num_defs = 1;
                    mi.ops[0] = reg_op(ops[0].reg);
                    mi.ops[1] = *mem;
                    return sel;
                }
                if (m == "add" || m == "sub" || m == "and" || m == "or" || m == "xor" || m == "cmp" || m == "adc" || m == "sbb" || m == "test")
                    return select_alu(st, sel);
                if (m == "inc" || m == "dec" || m == "neg" || m == "not" || m == "mul" || m == "div" || m == "idiv")
                    return select_unary(st, sel);
                if (m == "imul")
                    return select_imul(st, sel);
                if (m == "shl" || m == "sal" || m == "shr" || m == "sar")
                    return select_shift(st, sel);
                if (m == "xchg")
                {
                    if (ops.size() != 2)
                    {
                        fail("'xchg' expects 2 operands");
                        return std::nullopt;
                    }
                    auto width = operand_width(st);
                    if (!width)
                        return std::nullopt;
                    if (ops[0].kind == Operand::Kind::Reg && ops[1].kind == Operand::Kind::Reg && *width == 64)
                    {
                        mi.opc = MOpc::XCHG64rr;
                        mi.num_ops = 2;
                        mi.num_defs = 1;
                        mi.ops[0] = reg_op(ops[0].reg);
                        mi.ops[1] = reg_op(ops[1].reg);
                        return sel;
                    }
                    auto const* mem_op = ops[0].kind == Operand::Kind::Mem ? &ops[0] : ops[1].kind == Operand::Kind::Mem ? &ops[1] : nullptr;
                    auto const* reg = ops[0].kind == Operand::Kind::Reg ? &ops[0] : ops[1].kind == Operand::Kind::Reg ? &ops[1] : nullptr;
                    auto opc = pick(*width, {MOpc::XCHG8mr, MOpc::XCHG16mr, MOpc::XCHG32mr, std::nullopt});
                    if (!mem_op || !reg || !opc)
                        return fail_none(st);
                    auto mem = memory(*mem_op);
                    if (!mem)
                        return std::nullopt;
                    mi.opc = *opc;
                    mi.num_ops = 2;
                    mi.ops[0] = *mem;
                    mi.ops[1] = reg_op(reg->reg);
                    return sel;
                }
                fail(std::format("instruction '{}' is not supported by the custom backend", m));
                return std::nullopt;
            }

            std::optional<Selected> fail_none(Statement const& st)
            {
                if (m_error.empty())
                    fail(std::format("this form of '{}' is not supported by the custom backend", st.mnemonic));
                return std::nullopt;
            }

            static std::optional<MOpc> jcc(std::string_view m)
            {
                static constexpr std::array<std::pair<std::string_view, MOpc>, 30> table{{
                    {"je", MOpc::JE},   {"jz", MOpc::JE},   {"jne", MOpc::JNE}, {"jnz", MOpc::JNE}, {"jl", MOpc::JL},   {"jnge", MOpc::JL},
                    {"jle", MOpc::JLE}, {"jng", MOpc::JLE}, {"jg", MOpc::JG},   {"jnle", MOpc::JG}, {"jge", MOpc::JGE}, {"jnl", MOpc::JGE},
                    {"jb", MOpc::JB},   {"jnae", MOpc::JB}, {"jc", MOpc::JB},   {"jbe", MOpc::JBE}, {"jna", MOpc::JBE}, {"ja", MOpc::JA},
                    {"jnbe", MOpc::JA}, {"jae", MOpc::JAE}, {"jnb", MOpc::JAE}, {"jnc", MOpc::JAE}, {"jp", MOpc::JP},   {"jpe", MOpc::JP},
                    {"jnp", MOpc::JNP}, {"jpo", MOpc::JNP}, {"js", MOpc::JS},   {"jns", MOpc::JNS}, {"jmp", MOpc::JMP}, {"call", MOpc::CALL},
                }};
                for (auto const& [name, opc] : table)
                    if (name == m && name != "jmp" && name != "call")
                        return opc;
                return std::nullopt;
            }

            [[nodiscard]] static std::optional<std::uint8_t> short_branch_opcode(MOpc opc)
            {
                switch (opc)
                {
                    case MOpc::JMP:
                        return std::uint8_t{0xEB};
                    case MOpc::JE:
                        return std::uint8_t{0x74};
                    case MOpc::JNE:
                        return std::uint8_t{0x75};
                    case MOpc::JL:
                        return std::uint8_t{0x7C};
                    case MOpc::JGE:
                        return std::uint8_t{0x7D};
                    case MOpc::JLE:
                        return std::uint8_t{0x7E};
                    case MOpc::JG:
                        return std::uint8_t{0x7F};
                    case MOpc::JB:
                        return std::uint8_t{0x72};
                    case MOpc::JAE:
                        return std::uint8_t{0x73};
                    case MOpc::JBE:
                        return std::uint8_t{0x76};
                    case MOpc::JA:
                        return std::uint8_t{0x77};
                    case MOpc::JP:
                        return std::uint8_t{0x7A};
                    case MOpc::JNP:
                        return std::uint8_t{0x7B};
                    case MOpc::JS:
                        return std::uint8_t{0x78};
                    case MOpc::JNS:
                        return std::uint8_t{0x79};
                    default:
                        return std::nullopt;
                }
            }

            std::optional<Selected> select_branch(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto const& m = st.mnemonic;
                if (st.operands.size() != 1)
                {
                    fail(std::format("'{}' expects one target", m));
                    return std::nullopt;
                }
                auto const& target = st.operands[0];
                bool const is_call = m == "call";
                bool const is_jmp = m == "jmp";
                bool const direct = !target.indirect && (target.kind == Operand::Kind::Mem && target.mem.bare && !target.mem.has_registers() &&
                                                         target.mem.segment == SegmentOverride::None && target.size == 0);
                bool const intel_direct = !m_att && target.kind == Operand::Kind::Imm && !target.value.symbol.empty();
                if (direct || intel_direct)
                {
                    auto const& value = direct ? target.mem.disp : target.value;
                    if (value.symbol.empty() || !value.minus.empty() || value.constant != 0)
                    {
                        fail(std::format("'{}' needs a symbol or label target", m));
                        return std::nullopt;
                    }
                    if (is_call)
                        mi.opc = is_64() ? MOpc::CALL : MOpc::CALL_rel16;
                    else if (is_jmp)
                        mi.opc = MOpc::JMP;
                    else
                        mi.opc = *jcc(m);
                    mi.num_ops = 1;
                    mi.ops[0] = MOp{};
                    mi.ops[0].kind = MOpKind::Symbol;
                    mi.ops[0].symbol = intern(value.symbol);
                    return sel;
                }
                if (!is_call && !is_jmp)
                {
                    fail(std::format("'{}' needs a label target", m));
                    return std::nullopt;
                }
                if (m_att && !target.indirect)
                {
                    fail(std::format("indirect '{}' needs '*' in AT&T syntax", m));
                    return std::nullopt;
                }
                if (target.kind == Operand::Kind::Reg)
                {
                    if (is_64() ? target.reg.width != 64 : target.reg.width != 16)
                    {
                        fail(std::format("indirect '{}' through this register is not supported", m));
                        return std::nullopt;
                    }
                    mi.opc = is_64() ? (is_call ? MOpc::CALL_r64 : MOpc::JMP_r64) : (is_call ? MOpc::CALL16r : MOpc::JMP16r);
                    mi.num_ops = 1;
                    mi.ops[0] = reg_op(target.reg);
                    return sel;
                }
                if (target.kind == Operand::Kind::Mem)
                {
                    auto mem = make_mem(target.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    mi.opc = is_64() ? (is_call ? MOpc::CALLm : MOpc::JMPm) : (is_call ? MOpc::CALL16m : MOpc::JMP16m);
                    mi.num_ops = 1;
                    mi.ops[0] = MOp::from_mem(*mem);
                    return sel;
                }
                fail(std::format("invalid target for '{}'", m));
                return std::nullopt;
            }

            std::optional<Selected> select_port(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto const& ops = st.operands;
                bool const in = st.mnemonic == "in";
                if (ops.size() != 2)
                {
                    fail(std::format("'{}' expects 2 operands", st.mnemonic));
                    return std::nullopt;
                }
                auto const& acc = in ? ops[0] : ops[1];
                auto const& port = in ? ops[1] : ops[0];
                if (acc.kind != Operand::Kind::Reg || acc.reg.reg != PhysReg::RAX || acc.reg.width == 64)
                {
                    fail(std::format("'{}' transfers through al, ax or eax", st.mnemonic));
                    return std::nullopt;
                }
                if (st.suffix_width && st.suffix_width != acc.reg.width)
                {
                    fail("operand sizes do not match");
                    return std::nullopt;
                }
                bool const dx = port.kind == Operand::Kind::Reg && port.reg.reg == PhysReg::RDX && port.reg.width == 16;
                bool const att_dx = port.kind == Operand::Kind::Mem && !port.mem.bare && port.mem.base == PhysReg::RDX && port.mem.base_width == 16 &&
                                    port.mem.index == PhysReg::None && port.mem.disp.is_constant() && port.mem.disp.constant == 0;
                bool const imm = port.kind == Operand::Kind::Imm && port.value.is_constant();
                if (!dx && !att_dx && !imm)
                {
                    fail(std::format("the port of '{}' is dx or an immediate", st.mnemonic));
                    return std::nullopt;
                }
                auto const w = acc.reg.width;
                mi.num_ops = 2;
                if (imm)
                {
                    mi.opc = *pick(w, {in ? MOpc::IN8ri : MOpc::OUT8ir, in ? MOpc::IN16ri : MOpc::OUT16ir, in ? MOpc::IN32ri : MOpc::OUT32ir, std::nullopt});
                    auto const port_op = MOp::from_imm(port.value.constant);
                    mi.ops[0] = in ? reg_op(acc.reg) : port_op;
                    mi.ops[1] = in ? port_op : reg_op(acc.reg);
                }
                else
                {
                    mi.opc = *pick(w, {in ? MOpc::IN8rr : MOpc::OUT8rr, in ? MOpc::IN16rr : MOpc::OUT16rr, in ? MOpc::IN32rr : MOpc::OUT32rr, std::nullopt});
                    auto const dx_op = MOp::from_reg(VReg::phys(PhysReg::RDX));
                    mi.ops[0] = in ? reg_op(acc.reg) : dx_op;
                    mi.ops[1] = in ? dx_op : reg_op(acc.reg);
                }
                mi.num_defs = in ? 1 : 0;
                return sel;
            }

            std::optional<Selected> select_stack(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto const& ops = st.operands;
                bool const push = st.mnemonic == "push";
                if (ops.size() != 1)
                {
                    fail(std::format("'{}' expects 1 operand", st.mnemonic));
                    return std::nullopt;
                }
                auto const& op = ops[0];
                mi.num_ops = 1;
                if (op.kind == Operand::Kind::Reg && reg_class(op.reg.reg) == RegClass::Segment)
                {
                    if (is_64())
                    {
                        fail("segment register pushes are not supported in 64-bit module asm");
                        return std::nullopt;
                    }
                    mi.opc = push ? MOpc::PUSHSEG : MOpc::POPSEG;
                    mi.ops[0] = reg_op(op.reg);
                    return sel;
                }
                if (op.kind == Operand::Kind::Reg)
                {
                    auto const w = op.reg.width;
                    std::optional<MOpc> opc =
                        is_64() ? (w == 64 ? std::optional{push ? MOpc::PUSH64r : MOpc::POP64r} : std::nullopt)
                                : pick(w, {std::nullopt, push ? MOpc::PUSH16r : MOpc::POP16r, push ? MOpc::PUSH32r : MOpc::POP32r, std::nullopt});
                    if (!opc || (st.suffix_width && st.suffix_width != w))
                        return fail_none(st);
                    mi.opc = *opc;
                    mi.num_defs = push ? 0 : 1;
                    mi.ops[0] = reg_op(op.reg);
                    return sel;
                }
                if (op.kind == Operand::Kind::Imm && push)
                {
                    unsigned const w = is_64() ? 32 : st.suffix_width ? st.suffix_width : op.size ? op.size : 16;
                    if (!is_64() && w != 16 && w != 32)
                        return fail_none(st);
                    mi.opc = is_64() ? MOpc::PUSH64i : (w == 32 ? MOpc::PUSH32i : MOpc::PUSH16i);
                    if (!immediate_fits(op.value, w) || !set_immediate(sel, 0, op.value, w))
                        return std::nullopt;
                    return sel;
                }
                if (op.kind == Operand::Kind::Mem && push && is_64())
                {
                    auto mem = make_mem(op.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    mi.opc = MOpc::PUSH64m;
                    mi.ops[0] = MOp::from_mem(*mem);
                    return sel;
                }
                return fail_none(st);
            }

            std::optional<Selected> select_mov(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto& ops = st.operands;
                if (ops.size() != 2)
                {
                    fail("'mov' expects 2 operands");
                    return std::nullopt;
                }
                auto& dst = ops[0];
                auto& src = ops[1];
                bool const dst_seg = dst.kind == Operand::Kind::Reg && reg_class(dst.reg.reg) == RegClass::Segment;
                bool const src_seg = src.kind == Operand::Kind::Reg && reg_class(src.reg.reg) == RegClass::Segment;
                mi.num_ops = 2;
                if (dst_seg || src_seg)
                {
                    if (is_64())
                    {
                        fail("segment register moves are not supported in 64-bit module asm");
                        return std::nullopt;
                    }
                    auto const& other = dst_seg ? src : dst;
                    if (other.kind == Operand::Kind::Reg && reg_class(other.reg.reg) == RegClass::GPR && other.reg.width == 16)
                    {
                        mi.opc = dst_seg ? MOpc::MOV16sr : MOpc::MOV16rs;
                        mi.num_defs = 1;
                        mi.ops[0] = reg_op(dst.reg);
                        mi.ops[1] = reg_op(src.reg);
                        return sel;
                    }
                    if (other.kind == Operand::Kind::Mem && (other.size == 0 || other.size == 16))
                    {
                        auto mem = make_mem(other.mem, st.addr32);
                        if (!mem)
                            return std::nullopt;
                        mi.opc = dst_seg ? MOpc::MOV16sm : MOpc::MOV16ms;
                        mi.num_defs = dst_seg ? 1 : 0;
                        mi.ops[0] = dst_seg ? reg_op(dst.reg) : MOp::from_mem(*mem);
                        mi.ops[1] = dst_seg ? MOp::from_mem(*mem) : reg_op(src.reg);
                        return sel;
                    }
                    return fail_none(st);
                }
                auto width = operand_width(st);
                if (!width)
                    return std::nullopt;
                auto const w = *width;
                if (dst.kind == Operand::Kind::Reg && src.kind == Operand::Kind::Reg)
                {
                    mi.opc = *pick(w, {MOpc::MOV8rr, MOpc::MOV16rr, MOpc::MOV32rr, MOpc::MOV64rr});
                    mi.num_defs = 1;
                    mi.ops[0] = reg_op(dst.reg);
                    mi.ops[1] = reg_op(src.reg);
                    return sel;
                }
                if (dst.kind == Operand::Kind::Reg && src.kind == Operand::Kind::Imm)
                {
                    mi.num_defs = 1;
                    mi.ops[0] = reg_op(dst.reg);
                    if (w == 64)
                    {
                        bool const wide = !src.value.symbol.empty() || src.value.constant < std::numeric_limits<std::int32_t>::min() ||
                                          src.value.constant > std::numeric_limits<std::int32_t>::max();
                        mi.opc = wide ? MOpc::MOV64ri : MOpc::MOV64ri32;
                    }
                    else
                        mi.opc = *pick(w, {MOpc::MOV8ri, MOpc::MOV16ri, MOpc::MOV32ri, std::nullopt});
                    if (!immediate_fits(src.value, w) || !set_immediate(sel, 1, src.value, w))
                        return std::nullopt;
                    return sel;
                }
                if (dst.kind == Operand::Kind::Reg && src.kind == Operand::Kind::Mem)
                {
                    auto mem = make_mem(src.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    mi.opc = *pick(w, {MOpc::MOV8rm, MOpc::MOV16rm, MOpc::MOV32rm, MOpc::MOV64rm});
                    mi.num_defs = 1;
                    mi.ops[0] = reg_op(dst.reg);
                    mi.ops[1] = MOp::from_mem(*mem);
                    return sel;
                }
                if (dst.kind == Operand::Kind::Mem && src.kind == Operand::Kind::Reg)
                {
                    auto mem = make_mem(dst.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    mi.opc = *pick(w, {MOpc::MOV8mr, MOpc::MOV16mr, MOpc::MOV32mr, MOpc::MOV64mr});
                    mi.ops[0] = MOp::from_mem(*mem);
                    mi.ops[1] = reg_op(src.reg);
                    return sel;
                }
                if (dst.kind == Operand::Kind::Mem && src.kind == Operand::Kind::Imm)
                {
                    auto mem = make_mem(dst.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    if (!mem->symbol.empty() && is_64())
                    {
                        fail("an immediate store to a rip-relative operand is not supported by the custom backend");
                        return std::nullopt;
                    }
                    mi.opc = *pick(w, {MOpc::MOV8mi, MOpc::MOV16mi, MOpc::MOV32mi, MOpc::MOV64mi32});
                    mi.ops[0] = MOp::from_mem(*mem);
                    if (!immediate_fits(src.value, w == 64 ? 32 : w) || !set_immediate(sel, 1, src.value, w == 64 ? 32 : w))
                        return std::nullopt;
                    return sel;
                }
                return fail_none(st);
            }

            std::optional<Selected> select_extend(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto& ops = st.operands;
                auto m = st.mnemonic;
                bool const zero = m.starts_with("movz");
                unsigned src_width = 0;
                unsigned dst_width = 0;
                if (m_att && m.size() == 6)
                {
                    auto width_of = [](char c) -> unsigned { return c == 'b' ? 8 : c == 'w' ? 16 : c == 'l' ? 32 : c == 'q' ? 64 : 0; };
                    src_width = width_of(m[4]);
                    dst_width = width_of(m[5]);
                }
                else if (m != "movzx" && m != "movsx")
                {
                    fail(std::format("instruction '{}' is not supported by the custom backend", m));
                    return std::nullopt;
                }
                if (ops.size() != 2 || ops[0].kind != Operand::Kind::Reg)
                {
                    fail(std::format("'{}' expects a register destination", m));
                    return std::nullopt;
                }
                if (dst_width == 0)
                    dst_width = ops[0].reg.width;
                if (src_width == 0)
                    src_width = ops[1].kind == Operand::Kind::Reg ? ops[1].reg.width : ops[1].size;
                if (src_width == 0 || dst_width != ops[0].reg.width)
                {
                    fail(std::format("cannot determine the operand sizes of '{}'", m));
                    return std::nullopt;
                }
                bool const mem = ops[1].kind == Operand::Kind::Mem;
                std::optional<MOpc> opc;
                auto const key = dst_width * 1000 + src_width;
                if (zero)
                {
                    switch (key)
                    {
                        case 16008:
                            opc = mem ? MOpc::MOVZX16rm8 : MOpc::MOVZX16_8rr;
                            break;
                        case 32008:
                            opc = mem ? MOpc::MOVZX32rm8 : MOpc::MOVZX32rr8;
                            break;
                        case 32016:
                            opc = mem ? MOpc::MOVZX32rm16 : MOpc::MOVZX32_16rr;
                            break;
                        case 64008:
                            opc = mem ? MOpc::MOVZX64rm8 : MOpc::MOVZX64rr8;
                            break;
                        case 64016:
                            opc = mem ? MOpc::MOVZX64rm16 : MOpc::MOVZX64rr16;
                            break;
                        default:
                            break;
                    }
                }
                else
                {
                    switch (key)
                    {
                        case 16008:
                            opc = mem ? MOpc::MOVSX16rm8 : MOpc::MOVSX16_8rr;
                            break;
                        case 32008:
                            opc = mem ? MOpc::MOVSX32rm8 : MOpc::MOVSX32_8rr;
                            break;
                        case 32016:
                            opc = mem ? MOpc::MOVSX32rm16 : MOpc::MOVSX32_16rr;
                            break;
                        case 64008:
                            opc = mem ? MOpc::MOVSX64rm8 : MOpc::MOVSX64rr8;
                            break;
                        case 64016:
                            opc = mem ? MOpc::MOVSX64rm16 : MOpc::MOVSX64rr16;
                            break;
                        case 64032:
                            opc = mem ? std::nullopt : std::optional{MOpc::MOVSX64_32rr};
                            break;
                        default:
                            break;
                    }
                }
                if (!opc)
                    return fail_none(st);
                mi.opc = *opc;
                mi.num_ops = 2;
                mi.num_defs = 1;
                mi.ops[0] = reg_op(ops[0].reg);
                if (mem)
                {
                    auto mm = make_mem(ops[1].mem, st.addr32);
                    if (!mm)
                        return std::nullopt;
                    mi.ops[1] = MOp::from_mem(*mm);
                }
                else
                    mi.ops[1] = reg_op(ops[1].reg);
                return sel;
            }

            std::optional<Selected> select_alu(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto& ops = st.operands;
                auto const& m = st.mnemonic;
                if (ops.size() != 2)
                {
                    fail(std::format("'{}' expects 2 operands", m));
                    return std::nullopt;
                }
                auto width = operand_width(st);
                if (!width)
                    return std::nullopt;
                auto const w = *width;
                using T = std::array<std::optional<MOpc>, 4>;
                T rr{};
                T ri{};
                T rm{};
                T mr{};
                T mri{};
                std::optional<MOpc> nil;
                if (m == "add")
                {
                    rr = {MOpc::ADD8rr, MOpc::ADD16rr, MOpc::ADD32rr, MOpc::ADD64rr};
                    ri = {MOpc::ADD8ri, MOpc::ADD16ri, MOpc::ADD32ri, MOpc::ADD64ri32};
                    rm = {nil, MOpc::ADD16rm, MOpc::ADD32rm, MOpc::ADD64rm};
                }
                else if (m == "sub")
                {
                    rr = {MOpc::SUB8rr, MOpc::SUB16rr, MOpc::SUB32rr, MOpc::SUB64rr};
                    ri = {MOpc::SUB8ri, MOpc::SUB16ri, MOpc::SUB32ri, MOpc::SUB64ri32};
                    rm = {nil, MOpc::SUB16rm, MOpc::SUB32rm, MOpc::SUB64rm};
                }
                else if (m == "and")
                {
                    rr = {MOpc::AND8rr, MOpc::AND16rr, MOpc::AND32rr, MOpc::AND64rr};
                    ri = {MOpc::AND8ri, MOpc::AND16ri, MOpc::AND32ri, MOpc::AND64ri32};
                    rm = {nil, MOpc::AND16rm, MOpc::AND32rm, MOpc::AND64rm};
                    mr = {nil, nil, nil, MOpc::AND64mr};
                }
                else if (m == "or")
                {
                    rr = {MOpc::OR8rr, MOpc::OR16rr, MOpc::OR32rr, MOpc::OR64rr};
                    ri = {MOpc::OR8ri, MOpc::OR16ri, MOpc::OR32ri, MOpc::OR64ri32};
                    rm = {nil, MOpc::OR16rm, MOpc::OR32rm, MOpc::OR64rm};
                    mr = {nil, nil, nil, MOpc::OR64mr};
                }
                else if (m == "xor")
                {
                    rr = {MOpc::XOR8rr, MOpc::XOR16rr, MOpc::XOR32rr, MOpc::XOR64rr};
                    ri = {MOpc::XOR8ri, MOpc::XOR16ri, MOpc::XOR32ri, MOpc::XOR64ri32};
                    rm = {nil, MOpc::XOR16rm, MOpc::XOR32rm, MOpc::XOR64rm};
                    mr = {nil, nil, nil, MOpc::XOR64mr};
                }
                else if (m == "cmp")
                {
                    rr = {MOpc::CMP8rr, MOpc::CMP16rr, MOpc::CMP32rr, MOpc::CMP64rr};
                    ri = {MOpc::CMP8ri, MOpc::CMP16ri, MOpc::CMP32ri, MOpc::CMP64ri32};
                    rm = {MOpc::CMP8rm, MOpc::CMP16rm, MOpc::CMP32rm, MOpc::CMP64rm};
                    mri = {MOpc::CMP8mi, MOpc::CMP16mi, MOpc::CMP32mi, nil};
                    mr = {MOpc::CMP8mr, MOpc::CMP16mr, MOpc::CMP32mr, nil};
                }
                else if (m == "test")
                {
                    rr = {MOpc::TEST8rr, MOpc::TEST16rr, MOpc::TEST32rr, MOpc::TEST64rr};
                    ri = {MOpc::TEST8ri, MOpc::TEST16ri, MOpc::TEST32ri, MOpc::TEST64ri};
                }
                else if (m == "adc")
                {
                    rr = {nil, MOpc::ADC16rr, MOpc::ADC32rr, nil};
                    ri = {nil, MOpc::ADC16ri, MOpc::ADC32ri, nil};
                    rm = {nil, MOpc::ADC16rm, MOpc::ADC32rm, nil};
                }
                else
                {
                    rr = {nil, MOpc::SBB16rr, MOpc::SBB32rr, nil};
                    ri = {nil, MOpc::SBB16ri, MOpc::SBB32ri, nil};
                }
                bool const compare = m == "cmp" || m == "test";
                auto const& dst = ops[0];
                auto const& src = ops[1];
                if (dst.kind == Operand::Kind::Reg && src.kind == Operand::Kind::Reg)
                {
                    auto opc = pick(w, rr);
                    if (!opc)
                        return fail_none(st);
                    mi.opc = *opc;
                    if (compare)
                    {
                        mi.num_ops = 2;
                        mi.ops[0] = reg_op(dst.reg);
                        mi.ops[1] = reg_op(src.reg);
                    }
                    else
                    {
                        mi.num_ops = 3;
                        mi.num_defs = 1;
                        mi.ops[0] = reg_op(dst.reg);
                        mi.ops[1] = reg_op(dst.reg);
                        mi.ops[2] = reg_op(src.reg);
                    }
                    return sel;
                }
                if (dst.kind == Operand::Kind::Reg && src.kind == Operand::Kind::Imm)
                {
                    auto opc = pick(w, ri);
                    if (!opc)
                        return fail_none(st);
                    mi.opc = *opc;
                    mi.num_ops = 2;
                    mi.num_defs = compare ? 0 : 1;
                    mi.ops[0] = reg_op(dst.reg);
                    auto const imm_width = w == 64 ? 32u : w;
                    if (w == 64 && !src.value.symbol.empty())
                    {
                        fail("a symbol address does not fit a sign-extended 32-bit immediate");
                        return std::nullopt;
                    }
                    if (!immediate_fits(src.value, imm_width) || !set_immediate(sel, 1, src.value, imm_width))
                        return std::nullopt;
                    return sel;
                }
                if (dst.kind == Operand::Kind::Reg && src.kind == Operand::Kind::Mem)
                {
                    auto opc = pick(w, rm);
                    auto mem = make_mem(src.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    if (!opc)
                        return fail_none(st);
                    mi.opc = *opc;
                    mi.num_ops = 3;
                    mi.num_defs = compare ? 0 : 1;
                    mi.ops[0] = reg_op(dst.reg);
                    mi.ops[1] = reg_op(dst.reg);
                    mi.ops[2] = MOp::from_mem(*mem);
                    return sel;
                }
                if (dst.kind == Operand::Kind::Mem && src.kind == Operand::Kind::Reg)
                {
                    auto opc = pick(w, mr);
                    auto mem = make_mem(dst.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    if (!opc)
                        return fail_none(st);
                    mi.opc = *opc;
                    mi.num_ops = 2;
                    mi.ops[0] = MOp::from_mem(*mem);
                    mi.ops[1] = reg_op(src.reg);
                    return sel;
                }
                if (dst.kind == Operand::Kind::Mem && src.kind == Operand::Kind::Imm)
                {
                    auto opc = pick(w, mri);
                    auto mem = make_mem(dst.mem, st.addr32);
                    if (!mem)
                        return std::nullopt;
                    if (!opc)
                        return fail_none(st);
                    mi.opc = *opc;
                    mi.num_ops = 2;
                    mi.num_defs = 0;
                    mi.ops[0] = MOp::from_mem(*mem);
                    auto const imm_width = w == 64 ? 32u : w;
                    if (!immediate_fits(src.value, imm_width) || !set_immediate(sel, 1, src.value, imm_width))
                        return std::nullopt;
                    return sel;
                }
                return fail_none(st);
            }

            std::optional<Selected> select_unary(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto& ops = st.operands;
                auto const& m = st.mnemonic;
                if (ops.size() != 1 || ops[0].kind != Operand::Kind::Reg)
                {
                    fail(std::format("'{}' expects 1 register operand", m));
                    return std::nullopt;
                }
                auto width = operand_width(st);
                if (!width)
                    return std::nullopt;
                using T = std::array<std::optional<MOpc>, 4>;
                std::optional<MOpc> nil;
                T table{};
                if (m == "inc")
                    table = {nil, MOpc::INC16r, MOpc::INC32r, MOpc::INC64r};
                else if (m == "dec")
                    table = {nil, MOpc::DEC16r, MOpc::DEC32r, MOpc::DEC64r};
                else if (m == "neg")
                    table = {MOpc::NEG8r, MOpc::NEG16r, MOpc::NEG32r, MOpc::NEG64r};
                else if (m == "not")
                    table = {MOpc::NOT8r, MOpc::NOT16r, MOpc::NOT32r, MOpc::NOT64r};
                else if (m == "mul")
                    table = {nil, MOpc::MUL16r, MOpc::MUL32r, MOpc::MUL64r};
                else if (m == "div")
                    table = {nil, MOpc::DIV16r, MOpc::DIV32r, MOpc::DIV64r};
                else
                    table = {nil, MOpc::IDIV16r, MOpc::IDIV32r, MOpc::IDIV64r};
                auto opc = pick(*width, table);
                if (!opc)
                    return fail_none(st);
                mi.opc = *opc;
                mi.num_ops = 1;
                bool const divide = m == "mul" || m == "div" || m == "idiv";
                mi.num_defs = divide ? 0 : 1;
                mi.ops[0] = reg_op(ops[0].reg);
                return sel;
            }

            std::optional<Selected> select_imul(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto& ops = st.operands;
                if (ops.size() < 2 || ops.size() > 3 || ops[0].kind != Operand::Kind::Reg || ops[1].kind != Operand::Kind::Reg)
                {
                    fail("'imul' supports register forms with 2 or 3 operands");
                    return std::nullopt;
                }
                if (m_att && ops.size() == 3)
                    std::swap(ops[1], ops[2]);
                auto width = operand_width(st);
                if (!width)
                    return std::nullopt;
                std::optional<MOpc> nil;
                mi.num_defs = 1;
                if (ops.size() == 3)
                {
                    if (ops[2].kind != Operand::Kind::Imm)
                    {
                        fail("the third operand of 'imul' must be an immediate");
                        return std::nullopt;
                    }
                    auto opc = pick(*width, {nil, MOpc::IMUL16rri, MOpc::IMUL32rri, MOpc::IMUL64rri32});
                    if (!opc)
                        return fail_none(st);
                    mi.opc = *opc;
                    mi.num_ops = 3;
                    mi.ops[0] = reg_op(ops[0].reg);
                    mi.ops[1] = reg_op(ops[1].reg);
                    auto const imm_width = *width == 64 ? 32u : *width;
                    if (!ops[2].value.symbol.empty())
                    {
                        fail("'imul' needs a constant immediate");
                        return std::nullopt;
                    }
                    if (!immediate_fits(ops[2].value, imm_width) || !set_immediate(sel, 2, ops[2].value, imm_width))
                        return std::nullopt;
                    return sel;
                }
                auto opc = pick(*width, {nil, MOpc::IMUL16rr, MOpc::IMUL32rr, MOpc::IMUL64rr});
                if (!opc)
                    return fail_none(st);
                mi.opc = *opc;
                mi.num_ops = 3;
                mi.ops[0] = reg_op(ops[0].reg);
                mi.ops[1] = reg_op(ops[0].reg);
                mi.ops[2] = reg_op(ops[1].reg);
                return sel;
            }

            std::optional<Selected> select_shift(Statement& st, Selected& sel)
            {
                auto& mi = sel.instr;
                auto& ops = st.operands;
                auto m = st.mnemonic == "sal" ? std::string{"shl"} : st.mnemonic;
                if (ops.size() == 1)
                {
                    Operand one;
                    one.kind = Operand::Kind::Imm;
                    one.value.constant = 1;
                    ops.push_back(one);
                }
                if (ops.size() != 2 || ops[0].kind != Operand::Kind::Reg)
                {
                    fail(std::format("'{}' expects a register and a count", st.mnemonic));
                    return std::nullopt;
                }
                auto width = operand_width(st, 1);
                if (!width)
                    return std::nullopt;
                std::optional<MOpc> nil;
                auto const& count = ops[1];
                mi.num_defs = 1;
                if (count.kind == Operand::Kind::Reg)
                {
                    if (count.reg.reg != PhysReg::RCX || count.reg.width != 8)
                    {
                        fail(std::format("the count of '{}' must be cl or an immediate", st.mnemonic));
                        return std::nullopt;
                    }
                    auto opc = m == "shl"   ? pick(*width, {nil, MOpc::SHL16rCL, MOpc::SHL32rCL, MOpc::SHL64rCL})
                               : m == "shr" ? pick(*width, {nil, MOpc::SHR16rCL, MOpc::SHR32rCL, MOpc::SHR64rCL})
                                            : pick(*width, {nil, MOpc::SAR16rCL, MOpc::SAR32rCL, MOpc::SAR64rCL});
                    if (!opc)
                        return fail_none(st);
                    mi.opc = *opc;
                    mi.num_ops = 2;
                    mi.ops[0] = reg_op(ops[0].reg);
                    mi.ops[1] = reg_op(count.reg);
                    return sel;
                }
                if (count.kind != Operand::Kind::Imm || !count.value.is_constant() || count.value.constant < 0 || count.value.constant > 255)
                {
                    fail(std::format("the count of '{}' must be cl or an immediate", st.mnemonic));
                    return std::nullopt;
                }
                auto opc = m == "shl"   ? pick(*width, {nil, MOpc::SHL16ri8, MOpc::SHL32ri8, MOpc::SHL64ri8})
                           : m == "shr" ? pick(*width, {nil, MOpc::SHR16ri8, MOpc::SHR32ri8, MOpc::SHR64ri8})
                                        : pick(*width, {nil, MOpc::SAR16ri8, MOpc::SAR32ri8, MOpc::SAR64ri8});
                if (!opc)
                    return fail_none(st);
                mi.opc = *opc;
                mi.num_ops = 3;
                mi.ops[0] = reg_op(ops[0].reg);
                mi.ops[1] = reg_op(ops[0].reg);
                mi.ops[2] = MOp::from_imm(count.value.constant);
                return sel;
            }

            bool emit_instruction(Statement& st)
            {
                auto selected = select(st);
                if (!selected)
                    return false;
                if (st.branch_hint != BranchHint::None)
                {
                    auto const short_opcode = short_branch_opcode(selected->instr.opc);
                    bool const call = selected->instr.opc == MOpc::CALL || selected->instr.opc == MOpc::CALL_rel16;
                    if (!short_opcode && !(st.branch_hint == BranchHint::Near && call))
                        return fail(std::format("'{}' is not a branch that accepts 'short' or 'near'", st.mnemonic));
                    if (st.branch_hint == BranchHint::Short)
                    {
                        if (selected->instr.num_ops != 1 || selected->instr.ops[0].kind != MOpKind::Symbol)
                            return fail(std::format("'{} short' needs a label target", st.mnemonic));
                        auto const offset = here();
                        emit_byte(*short_opcode);
                        emit_byte(0);
                        m_short_branches.push_back(ShortBranch{*m_current, offset, std::string{selected->instr.ops[0].symbol}, m_line});
                        return true;
                    }
                }
                auto encoded = encode_instruction(selected->instr, m_mode);
                std::optional<ModuleAsmRelocation> rip_field;
                if (is_64())
                {
                    for (std::uint8_t i = 0; i < selected->instr.num_ops; ++i)
                    {
                        auto const& op = selected->instr.ops[i];
                        if (op.kind != MOpKind::Mem || op.mem.symbol.empty())
                            continue;
                        bool const relocated = encoded && std::ranges::any_of(encoded->relocs, [&](Reloc const& r) { return r.symbol == op.mem.symbol; });
                        if (relocated)
                            break;
                        auto stripped = selected->instr;
                        stripped.ops[i].mem.symbol = {};
                        stripped.ops[i].mem.disp = 0;
                        encoded = encode_instruction(stripped, m_mode);
                        rip_field = ModuleAsmRelocation{encoded && encoded->bytes.size() >= 4 ? encoded->bytes.size() - 4 : 0, std::string(op.mem.symbol),
                                                        Reloc::Kind::Rel32, static_cast<std::int64_t>(op.mem.disp) - 4};
                        break;
                    }
                }
                if (!encoded && rip_field)
                    return fail(
                        std::format("'{}' with a rip-relative operand is not supported by the custom backend; load the address with 'lea' first", st.mnemonic));
                if (!encoded)
                    return fail(std::format("cannot encode '{}': {}", st.mnemonic, encoded.error()));
                auto const start = here();
                for (auto byte : encoded->bytes)
                    emit_byte(byte);
                for (auto const& r : encoded->relocs)
                    section().relocations.push_back({start + r.offset, std::string(r.symbol), r.kind, r.addend});
                if (rip_field)
                {
                    rip_field->offset += start;
                    section().relocations.push_back(std::move(*rip_field));
                }
                if (selected->immediate)
                {
                    auto const bytes = selected->immediate_bytes;
                    if (encoded->bytes.size() < bytes)
                        return fail("internal error: immediate field not found");
                    auto const field = start + encoded->bytes.size() - bytes;
                    for (unsigned i = 0; i < bytes; ++i)
                        section().data[field + i] = 0;
                    auto kind = absolute_kind(bytes);
                    if (!kind)
                        return fail("a symbol address does not fit this immediate");
                    section().relocations.push_back({field, selected->immediate->symbol, *kind, selected->immediate->constant});
                }
                return true;
            }

            [[nodiscard]] static bool is_assembler_local(std::string_view name) noexcept { return name.starts_with(".L"); }

            [[nodiscard]] static bool pc_relative(Reloc::Kind kind) noexcept
            {
                return kind == Reloc::Kind::Rel32 || kind == Reloc::Kind::Rel32_Call || kind == Reloc::Kind::Rel16;
            }

            bool finish()
            {
                for (auto const& fixup : m_difference_fixups)
                {
                    m_line = fixup.line;
                    auto a = m_defined.find(fixup.value.symbol);
                    auto b = m_defined.find(fixup.value.minus);
                    if (a == m_defined.end() || b == m_defined.end() || a->second.section != b->second.section)
                        return fail("a symbol difference needs two symbols defined in the same section");
                    auto const value = fixup.value.constant + static_cast<std::int64_t>(a->second.offset) - static_cast<std::int64_t>(b->second.offset);
                    auto& data = m_object.sections[fixup.section].data;
                    for (unsigned i = 0; i < fixup.size; ++i)
                        data[fixup.offset + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (8 * i));
                }
                for (auto const& branch : m_short_branches)
                {
                    m_line = branch.line;
                    auto it = m_defined.find(branch.symbol);
                    if (it == m_defined.end() || it->second.section != branch.section)
                        return fail(std::format("a short branch target '{}' must be defined in the same module asm section", branch.symbol));
                    auto const value = static_cast<std::int64_t>(it->second.offset) - static_cast<std::int64_t>(branch.offset) - 2;
                    if (value < -128 || value > 127)
                        return fail(std::format("short branch to '{}' is out of range", branch.symbol));
                    m_object.sections[branch.section].data[branch.offset + 1] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) & 0xFF);
                }
                m_line = 0;
                std::unordered_set<std::string> referenced;
                for (std::size_t si = 0; si < m_object.sections.size(); ++si)
                {
                    auto& s = m_object.sections[si];
                    std::vector<ModuleAsmRelocation> kept;
                    for (auto& r : s.relocations)
                    {
                        auto it = m_defined.find(r.symbol);
                        bool const binds_locally = it != m_defined.end() && !m_globals.contains(r.symbol) && !m_weak.contains(r.symbol);
                        if (binds_locally && it->second.section == si && pc_relative(r.kind))
                        {
                            auto const value = static_cast<std::int64_t>(it->second.offset) + r.addend - static_cast<std::int64_t>(r.offset);
                            auto const bytes = r.kind == Reloc::Kind::Rel16 ? 2u : 4u;
                            if (bytes == 4 && (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()))
                                return fail(std::format("branch to '{}' is out of range", r.symbol));
                            for (unsigned i = 0; i < bytes; ++i)
                                s.data[r.offset + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (8 * i));
                            continue;
                        }
                        if (it == m_defined.end() && is_assembler_local(r.symbol))
                            return fail(std::format("undefined local label '{}'", r.symbol));
                        referenced.insert(r.symbol);
                        kept.push_back(std::move(r));
                    }
                    s.relocations = std::move(kept);
                }
                for (auto const& name : m_definition_order)
                {
                    auto const& def = m_defined.at(name);
                    if (is_assembler_local(name) && !referenced.contains(name))
                        continue;
                    ModuleAsmSymbol sym;
                    sym.name = name;
                    sym.section = def.section;
                    sym.offset = def.offset;
                    sym.binding = m_weak.contains(name)      ? ModuleAsmBinding::Weak
                                  : m_globals.contains(name) ? ModuleAsmBinding::Global
                                                             : ModuleAsmBinding::Local;
                    if (auto t = m_types.find(name); t != m_types.end())
                        sym.type = t->second;
                    m_object.symbols.push_back(std::move(sym));
                }
                for (auto const& name : m_weak)
                    if (!m_defined.contains(name))
                        return fail(std::format("weak symbol '{}' must be defined in module asm", name));
                return true;
            }
        };

        [[nodiscard]] std::string reloc_directive(Reloc::Kind kind)
        {
            switch (kind)
            {
                case Reloc::Kind::Abs16:
                case Reloc::Kind::Rel16:
                    return "dw";
                case Reloc::Kind::Abs64:
                    return "dq";
                default:
                    return "dd";
            }
        }

        [[nodiscard]] std::uint64_t reloc_bytes(Reloc::Kind kind)
        {
            switch (kind)
            {
                case Reloc::Kind::Abs16:
                case Reloc::Kind::Rel16:
                    return 2;
                case Reloc::Kind::Abs64:
                    return 8;
                default:
                    return 4;
            }
        }

        [[nodiscard]] std::string nasm_name(std::string_view name, std::string_view prefix)
        {
            if (!name.starts_with('.'))
                return std::string(name);
            std::string out{prefix};
            out += "asm_";
            for (char c : name.substr(1))
                out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') ? c : '_';
            return out;
        }

        [[nodiscard]] std::string signed_term(std::int64_t value)
        {
            if (value == 0)
                return {};
            return value < 0 ? std::format(" - {}", -value) : std::format(" + {}", value);
        }

    } // namespace

    std::expected<ModuleAsmObject, ModuleAsmError> assemble_module_asm(std::span<ir::IrModuleAsm* const> blocks, EncodeMode mode, ModuleAsmFormat format)
    {
        Assembler assembler{mode, format};
        return assembler.run(blocks);
    }

    std::vector<std::string> module_asm_external_references(ModuleAsmObject const& object)
    {
        std::vector<std::string> names;
        for (auto const& s : object.sections)
            for (auto const& r : s.relocations)
                if (!object.defines(r.symbol))
                    names.push_back(r.symbol);
        std::ranges::sort(names);
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    std::string module_asm_intel_text(ModuleAsmObject const& object, ModuleAsmFormat format, std::string_view prefix)
    {
        std::string out;
        for (std::size_t si = 0; si < object.sections.size(); ++si)
        {
            auto const& s = object.sections[si];
            if (format == ModuleAsmFormat::Elf)
                out += std::format("\nsection {} {} {} {} {} align={}\n", s.name, s.nobits ? "nobits" : "progbits", s.alloc ? "alloc" : "noalloc",
                                   s.exec ? "exec" : "noexec", s.write ? "write" : "nowrite", s.alignment);
            else
                out += std::format("\nsection {} {} align={}\n", s.name, s.exec ? "code" : s.nobits ? "bss" : s.write ? "data" : "rdata", s.alignment);
            for (auto const& sym : object.symbols)
                if (sym.section == si && sym.binding != ModuleAsmBinding::Local)
                    out += std::format("{} {}\n", sym.binding == ModuleAsmBinding::Weak ? "weak" : "global", nasm_name(sym.name, prefix));
            std::set<std::uint64_t> events;
            for (auto const& sym : object.symbols)
                if (sym.section == si)
                    events.insert(sym.offset);
            for (auto const& line : s.lines)
                events.insert(line.offset);
            for (auto const& r : s.relocations)
            {
                events.insert(r.offset);
                events.insert(r.offset + reloc_bytes(r.kind));
            }
            events.insert(s.size);
            std::uint64_t pos = 0;
            std::size_t line_index = 0;
            auto emitted_bytes = [&](std::uint64_t until) {
                if (s.nobits)
                {
                    if (until > pos)
                        out += std::format("    resb {}\n", until - pos);
                    pos = until;
                    return;
                }
                while (pos < until)
                {
                    auto const chunk = std::min<std::uint64_t>(until - pos, 16);
                    out += "    db ";
                    for (std::uint64_t i = 0; i < chunk; ++i)
                        out += std::format("{}0x{:02X}", i ? ", " : "", s.data[pos + i]);
                    out += '\n';
                    pos += chunk;
                }
            };
            for (auto event : events)
            {
                if (event < pos)
                    continue;
                emitted_bytes(event);
                for (auto const& sym : object.symbols)
                    if (sym.section == si && sym.offset == event)
                        out += nasm_name(sym.name, prefix) + ":\n";
                while (line_index < s.lines.size() && s.lines[line_index].offset <= event)
                {
                    if (s.lines[line_index].offset == event)
                        out += std::format("    ; {}\n", s.lines[line_index].text);
                    ++line_index;
                }
                for (auto const& r : s.relocations)
                {
                    if (r.offset != event)
                        continue;
                    bool const pc = r.kind == Reloc::Kind::Rel32 || r.kind == Reloc::Kind::Rel32_Call || r.kind == Reloc::Kind::Rel16;
                    out += std::format("    {} {}{}{}\n", reloc_directive(r.kind), nasm_name(r.symbol, prefix), signed_term(r.addend), pc ? " - $" : "");
                    pos = r.offset + reloc_bytes(r.kind);
                }
            }
            emitted_bytes(s.size);
        }
        return out;
    }

} // namespace dcc::backend::x86
