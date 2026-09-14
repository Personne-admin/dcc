export module dcc.backend.inline_asm;

import std;
import dcc.ir;
import dcc.sm;
import dcc.target;
import dcc.backend.em64t.mir;

export namespace dcc::backend
{
    struct InlineAsmDiag
    {
        sm::SourceRange where;
        std::string message;
    };

    struct InlineAsmPlan
    {
        std::vector<std::string> registers;
        std::vector<std::string> clobbers;
        std::vector<em64t::MInstr> instructions;
        std::vector<std::string> literal_registers;
        bool has_memory_operands = false;
        std::string error;
    };

    [[nodiscard]] InlineAsmPlan prepare_inline_asm(ir::IrInlineAsmInst const& assembly, target::TargetConfig const& target);

    struct InlineAsmLlvm
    {
        std::string template_str;
        std::string constraints;
        std::vector<std::uint32_t> input_operand_indices;
        std::vector<std::uint32_t> output_address_indices;
        std::vector<std::string> clobbers;
        std::string error;
    };

    [[nodiscard]] InlineAsmLlvm prepare_llvm_asm(ir::IrInlineAsmInst const& assembly);

    [[nodiscard]] em64t::PhysReg inline_asm_family_phys(std::string_view family) noexcept;

} // namespace dcc::backend

namespace dcc::backend
{
    namespace
    {
        using namespace ir;

        [[nodiscard]] em64t::PhysReg family_to_phys(std::string_view family) noexcept
        {
            using em64t::PhysReg;
            if (family == "rax")
                return PhysReg::RAX;
            if (family == "rcx")
                return PhysReg::RCX;
            if (family == "rdx")
                return PhysReg::RDX;
            if (family == "rbx")
                return PhysReg::RBX;
            if (family == "rsp")
                return PhysReg::RSP;
            if (family == "rbp")
                return PhysReg::RBP;
            if (family == "rsi")
                return PhysReg::RSI;
            if (family == "rdi")
                return PhysReg::RDI;
            if (family == "r8")
                return PhysReg::R8;
            if (family == "r9")
                return PhysReg::R9;
            if (family == "r10")
                return PhysReg::R10;
            if (family == "r11")
                return PhysReg::R11;
            if (family == "r12")
                return PhysReg::R12;
            if (family == "r13")
                return PhysReg::R13;
            if (family == "r14")
                return PhysReg::R14;
            if (family == "r15")
                return PhysReg::R15;
            if (family.starts_with("xmm"))
            {
                unsigned index = 0;
                auto text = family.substr(3);
                for (char c : text)
                {
                    if (c < '0' || c > '9')
                        return PhysReg::None;

                    index = (index * 10) + static_cast<unsigned>(c - '0');
                }

                if (index < 16)
                    return static_cast<PhysReg>(static_cast<std::uint8_t>(PhysReg::XMM0) + index);
            }
            return PhysReg::None;
        }

        [[nodiscard]] bool is_high8(std::string_view name) noexcept
        {
            return name == "ah" || name == "bh" || name == "ch" || name == "dh";
        }

        struct ResolvedOperands
        {
            std::vector<std::string> registers;
            std::vector<std::string> clobbers;
            std::vector<std::string> literal_registers;
            bool has_memory_operands = false;
            std::string error;
        };

        [[nodiscard]] ResolvedOperands select_registers(IrInlineAsmInst const& assembly, target::TargetConfig const& target)
        {
            ResolvedOperands resolved;
            if (target.arch != target::Arch::X86_64)
            {
                resolved.error = "inline assembly is currently supported on x86_64";
                return resolved;
            }
            std::unordered_set<std::string_view> occupied;
            for (auto const& op : assembly.operands)
                if (!op.reg_name.empty())
                    occupied.insert(target::register_family(op.reg_name));

            for (auto clobber : assembly.clobbers)
            {
                if (clobber != "memory" && clobber != "cc")
                    occupied.insert(target::register_family(clobber));
                resolved.clobbers.emplace_back(clobber);
            }

            std::unordered_set<std::string_view> literal_families;
            for (auto const& part : assembly.template_parts)
            {
                if (part.operand != 0xFFFFFFFFU)
                    continue;

                auto begin = part.offset + part.length;
                auto end = begin;
                while (end < assembly.template_str.size() &&
                       (std::isalnum(static_cast<unsigned char>(assembly.template_str[end])) || assembly.template_str[end] == '_'))
                    ++end;

                auto name = std::string_view(assembly.template_str).substr(begin, end - begin);
                if (target::lookup_register(target.arch, name))
                    literal_families.insert(target::register_family(name));
            }

            for (auto family : literal_families)
                resolved.literal_registers.emplace_back(family);

            for (auto const& op : assembly.operands)
            {
                if (!op.type || op.placement_kind == IrAsmOperand::PlacementKind::RegPair)
                {
                    resolved.error = "malformed inline assembly IR operand";
                    return resolved;
                }

                bool memory = op.placement_kind == IrAsmOperand::PlacementKind::Mem;
                resolved.has_memory_operands |= memory;
                if (op.placement_kind == IrAsmOperand::PlacementKind::Imm || op.placement_kind == IrAsmOperand::PlacementKind::Sym ||
                    op.placement_kind == IrAsmOperand::PlacementKind::Flag)
                {
                    resolved.registers.emplace_back();
                    continue;
                }

                std::string_view selected = op.reg_name;
                if (selected.empty())
                {
                    auto width = memory ? 64U : static_cast<unsigned>(op.type->byte_size * 8);
                    bool floating = op.type->kind == IrTypeKind::Float && !memory;
                    for (auto const& reg : target::register_table(target.arch))
                    {
                        if (reg.reserved || occupied.contains(target::register_family(reg.name)))
                            continue;

                        if ((floating && reg.cls == target::PhysRegClass::XMM && reg.name.starts_with("xmm")) ||
                            (!floating && reg.cls == target::PhysRegClass::GPR && reg.width == width && reg.name != "ah" && reg.name != "bh" &&
                             reg.name != "ch" && reg.name != "dh"))
                        {
                            if (literal_families.contains(target::register_family(reg.name)))
                                continue;

                            selected = reg.name;
                            break;
                        }
                    }
                    if (selected.empty())
                    {
                        resolved.error = "too many inline assembly operands for the available registers";
                        return resolved;
                    }
                }

                occupied.insert(target::register_family(selected));
                resolved.registers.emplace_back(selected);
            }
            return resolved;
        }

        [[nodiscard]] std::optional<std::string> render_operand(IrInlineAsmInst const& assembly, ResolvedOperands const& resolved, std::size_t index,
                                                                std::uint32_t part_offset, std::string& error)
        {
            if (index >= assembly.operands.size())
            {
                error = "inline assembly IR operand reference out of range";
                return std::nullopt;
            }

            auto const& op = assembly.operands[index];
            bool intel = assembly.dialect == IrAsmDialect::Intel;
            if (op.placement_kind == IrAsmOperand::PlacementKind::Imm)
            {
                std::string number;
                if (auto* constant = ir_cast<IrIntConstant>(op.value))
                {
                    auto* type = static_cast<IrIntType const*>(constant->type);
                    auto bits = type->bits;
                    auto value = static_cast<std::uint64_t>(constant->value);
                    if (bits < 64)
                        value &= (bits == 64 ? ~0ULL : ((1ULL << bits) - 1));

                    if (type->is_signed && bits < 64 && bits > 0 && (value & (1ULL << (bits - 1))))
                        value |= ~((1ULL << bits) - 1);

                    number = type->is_signed ? std::to_string(static_cast<std::int64_t>(value)) : std::to_string(value);
                }
                else if (auto* bool_constant = ir_cast<IrBoolConstant>(op.value))
                    number = bool_constant->value ? "1" : "0";
                else
                    error = "inline assembly immediate is not an integer constant";

                if (!error.empty())
                    return std::nullopt;

                return (intel ? "" : "$") + number;
            }
            auto name = resolved.registers[index];
            if (op.placement_kind == IrAsmOperand::PlacementKind::Mem)
            {
                std::size_t cursor = part_offset;
                std::string_view view(assembly.template_str);
                while (cursor > 0 && (view[cursor - 1] == ' ' || view[cursor - 1] == '\t'))
                    --cursor;
                bool composed = cursor > 0 && ((intel && view[cursor - 1] == '[') || (!intel && view[cursor - 1] == '('));
                if (composed)
                    return (intel ? "" : "%") + name;
                unsigned pointee_bits = 0;
                if (auto* pointer = ir_type_cast<IrPointerType>(op.type))
                    if (pointer->pointee)
                        pointee_bits = static_cast<unsigned>(pointer->pointee->byte_size * 8);

                if (!intel)
                    return "(%" + name + ")";
                std::string prefix;
                if (pointee_bits == 8)
                    prefix = "byte ";
                else if (pointee_bits == 16)
                    prefix = "word ";
                else if (pointee_bits == 32)
                    prefix = "dword ";
                else if (pointee_bits == 64)
                    prefix = "qword ";
                return prefix + "[" + name + "]";
            }

            return (intel ? "" : "%") + name;
        }

        [[nodiscard]] std::optional<std::vector<std::pair<std::string, std::uint32_t>>>
        substitute_template(IrInlineAsmInst const& assembly, ResolvedOperands const& resolved, std::string& error)
        {
            std::string text;
            std::size_t cursor = 0;
            auto parts = assembly.template_parts;
            std::ranges::sort(parts, {}, &IrAsmTemplatePart::offset);
            for (auto const& part : parts)
            {
                if (part.offset < cursor || part.offset + part.length > assembly.template_str.size())
                {
                    error = "malformed inline assembly IR template";
                    return std::nullopt;
                }

                text += std::string_view(assembly.template_str).substr(cursor, part.offset - cursor);
                if (part.operand == 0xFFFFFFFFU)
                {
                    if (assembly.dialect == IrAsmDialect::Att)
                        text += '%';
                    else
                    {
                        auto start = part.offset + part.length;
                        auto end = start;
                        while (end < assembly.template_str.size() && std::isalnum(static_cast<unsigned char>(assembly.template_str[end])))
                            ++end;
                        if (!target::lookup_register(target::Arch::X86_64, std::string_view(assembly.template_str).substr(start, end - start)))
                            text += '%';
                    }
                }
                else
                {
                    auto rendered = render_operand(assembly, resolved, part.operand, part.offset, error);
                    if (!rendered)
                        return std::nullopt;
                    text += *rendered;
                }
                cursor = part.offset + part.length;
            }
            text += std::string_view(assembly.template_str).substr(cursor);

            std::vector<std::pair<std::string, std::uint32_t>> lines;
            std::uint32_t line_number = 1;
            std::string current;
            for (char c : text)
            {
                if (c == '\n' || c == ';')
                {
                    lines.emplace_back(current, line_number);
                    if (c == '\n')
                        ++line_number;
                    current.clear();
                }
                else
                    current += c;
            }
            lines.emplace_back(current, line_number);
            return lines;
        }

        struct AsmToken
        {
            enum class Kind
            {
                Ident,
                Integer,
                Percent,
                Dollar,
                Comma,
                Colon,
                LBracket,
                RBracket,
                LParen,
                RParen,
                Plus,
                Minus,
                Star,
                End,
            };
            Kind kind = Kind::End;
            std::string_view text;
            std::int64_t integer = 0;
        };

        [[nodiscard]] std::vector<AsmToken> tokenize_asm_line(std::string_view line, std::string& error)
        {
            std::vector<AsmToken> tokens;
            std::size_t i = 0;
            auto fail = [&](std::string message) {
                error = std::move(message);
                return std::vector<AsmToken>{};
            };

            while (i < line.size())
            {
                char c = line[i];
                if (c == ' ' || c == '\t' || c == '\r')
                {
                    ++i;
                    continue;
                }
                if (c == '#')
                    break;

                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.')
                {
                    std::size_t start = i++;
                    while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_' || line[i] == '.'))
                        ++i;

                    tokens.push_back({AsmToken::Kind::Ident, line.substr(start, i - start), 0});
                    continue;
                }
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    std::size_t start = i;
                    std::int64_t value = 0;
                    unsigned base = 10;
                    if (c == '0' && i + 1 < line.size() && (line[i + 1] == 'x' || line[i + 1] == 'X'))
                    {
                        base = 16;
                        i += 2;
                        std::size_t digits = i;
                        while (i < line.size() && std::isxdigit(static_cast<unsigned char>(line[i])))
                        {
                            value = (value * 16) + (std::isdigit(static_cast<unsigned char>(line[i])) ? line[i] - '0' : std::tolower(line[i]) - 'a' + 10);
                            ++i;
                        }
                        if (i == digits)
                            return fail("invalid integer literal in inline assembly");
                    }
                    else if (c == '0' && i + 1 < line.size() && (line[i + 1] == 'b' || line[i + 1] == 'B'))
                    {
                        base = 2;
                        i += 2;
                        std::size_t digits = i;
                        while (i < line.size() && (line[i] == '0' || line[i] == '1'))
                        {
                            value = (value * 2) + (line[i] - '0');
                            ++i;
                        }
                        if (i == digits)
                            return fail("invalid integer literal in inline assembly");
                    }
                    else if (c == '0' && i + 1 < line.size() && (line[i + 1] == 'o' || line[i + 1] == 'O'))
                    {
                        base = 8;
                        i += 2;
                        std::size_t digits = i;
                        while (i < line.size() && line[i] >= '0' && line[i] <= '7')
                        {
                            value = (value * 8) + (line[i] - '0');
                            ++i;
                        }
                        if (i == digits)
                            return fail("invalid integer literal in inline assembly");
                    }
                    else
                    {
                        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])))
                        {
                            value = (value * 10) + (line[i] - '0');
                            ++i;
                        }
                    }
                    if (i < line.size() && (std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '_'))
                        return fail("invalid integer suffix in inline assembly");

                    std::ignore = base;
                    tokens.push_back({AsmToken::Kind::Integer, line.substr(start, i - start), value});
                    continue;
                }
                AsmToken::Kind kind = AsmToken::Kind::End;
                switch (c)
                {
                    case '%':
                        kind = AsmToken::Kind::Percent;
                        break;
                    case '$':
                        kind = AsmToken::Kind::Dollar;
                        break;
                    case ',':
                        kind = AsmToken::Kind::Comma;
                        break;
                    case ':':
                        kind = AsmToken::Kind::Colon;
                        break;
                    case '[':
                        kind = AsmToken::Kind::LBracket;
                        break;
                    case ']':
                        kind = AsmToken::Kind::RBracket;
                        break;
                    case '(':
                        kind = AsmToken::Kind::LParen;
                        break;
                    case ')':
                        kind = AsmToken::Kind::RParen;
                        break;
                    case '+':
                        kind = AsmToken::Kind::Plus;
                        break;
                    case '-':
                        kind = AsmToken::Kind::Minus;
                        break;
                    case '*':
                        kind = AsmToken::Kind::Star;
                        break;
                    default:
                        return fail(std::format("unexpected character '{}' in inline assembly", c));
                }
                tokens.push_back({kind, line.substr(i, 1), 0});
                ++i;
            }
            tokens.push_back({AsmToken::Kind::End, {}, 0});
            return tokens;
        }

        struct ParsedOperand
        {
            enum class Kind
            {
                Reg,
                Imm,
                Mem,
            };
            Kind kind = Kind::Reg;
            em64t::PhysReg reg = em64t::PhysReg::None;
            unsigned width = 0;
            std::int64_t imm = 0;
            em64t::MMem mem{};
        };

        struct AsmParser
        {
            std::vector<AsmToken> tokens;
            std::size_t pos = 0;
            bool att = false;
            std::string error;

            [[nodiscard]] AsmToken const& peek() const { return tokens[pos]; }

            [[nodiscard]] bool at_end() const { return peek().kind == AsmToken::Kind::End; }

            [[nodiscard]] bool accept(AsmToken::Kind kind)
            {
                if (peek().kind == kind)
                {
                    ++pos;
                    return true;
                }

                return false;
            }

            [[nodiscard]] std::optional<std::string_view> expect_ident(std::string_view what)
            {
                if (peek().kind != AsmToken::Kind::Ident)
                {
                    error = std::format("expected {} in inline assembly", what);
                    return std::nullopt;
                }

                return tokens[pos++].text;
            }

            [[nodiscard]] std::optional<em64t::PhysReg> resolve_register(std::string_view name)
            {
                auto const* entry = target::lookup_register(target::Arch::X86_64, name);
                if (!entry)
                    return std::nullopt;

                if (is_high8(name))
                {
                    error = "8-bit high registers 'ah', 'bh', 'ch', and 'dh' are not supported in native inline assembly";
                    return std::nullopt;
                }

                auto phys = family_to_phys(target::register_family(name));
                if (phys == em64t::PhysReg::None)
                {
                    error = std::format("register '{}' cannot be used in native inline assembly", name);
                    return std::nullopt;
                }
                return phys;
            }

            [[nodiscard]] std::optional<unsigned> register_width(std::string_view name)
            {
                auto const* entry = target::lookup_register(target::Arch::X86_64, name);
                if (!entry)
                    return std::nullopt;

                return entry->width;
            }

            [[nodiscard]] std::optional<ParsedOperand> parse_intel_operand()
            {
                unsigned size_prefix = 0;
                if (peek().kind == AsmToken::Kind::Ident)
                {
                    std::string_view word = peek().text;
                    unsigned width = 0;
                    if (word == "byte")
                        width = 8;
                    else if (word == "word")
                        width = 16;
                    else if (word == "dword")
                        width = 32;
                    else if (word == "qword")
                        width = 64;

                    if (width != 0 && pos + 1 < tokens.size() && tokens[pos + 1].kind == AsmToken::Kind::LBracket)
                    {
                        size_prefix = width;
                        ++pos;
                    }
                }
                if (accept(AsmToken::Kind::LBracket))
                {
                    auto mem = parse_intel_memory();
                    if (!mem)
                        return std::nullopt;

                    if (!accept(AsmToken::Kind::RBracket))
                    {
                        error = "expected ']' to close the memory operand in inline assembly";
                        return std::nullopt;
                    }
                    if (size_prefix != 0)
                    {
                        if (mem->width != 0 && mem->width != size_prefix)
                        {
                            error = "memory size prefix does not match the memory operand width in inline assembly";
                            return std::nullopt;
                        }
                        mem->width = size_prefix;
                    }
                    return mem;
                }
                if (peek().kind == AsmToken::Kind::Ident)
                {
                    auto name = tokens[pos++].text;
                    auto phys = resolve_register(name);
                    if (!phys)
                    {
                        if (error.empty())
                            error = std::format("unknown inline assembly register '{}'", name);

                        return std::nullopt;
                    }
                    auto width = register_width(name);
                    ParsedOperand op;
                    op.kind = ParsedOperand::Kind::Reg;
                    op.reg = *phys;
                    op.width = width.value_or(64);
                    return op;
                }
                return parse_immediate();
            }

            [[nodiscard]] std::optional<ParsedOperand> parse_immediate()
            {
                bool negative = false;
                if (accept(AsmToken::Kind::Minus))
                    negative = true;
                else
                    std::ignore = accept(AsmToken::Kind::Plus);

                if (peek().kind != AsmToken::Kind::Integer)
                {
                    error = "expected an operand in inline assembly";
                    return std::nullopt;
                }

                ParsedOperand op;
                op.kind = ParsedOperand::Kind::Imm;
                op.imm = negative ? -tokens[pos].integer : tokens[pos].integer;
                ++pos;
                return op;
            }

            [[nodiscard]] std::optional<ParsedOperand> parse_intel_memory()
            {
                em64t::VReg base;
                em64t::VReg index;
                std::uint8_t scale = 1;
                std::int32_t disp = 0;
                bool have_base_or_index = false;
                bool have_disp = false;

                if (peek().kind == AsmToken::Kind::Integer || ((peek().kind == AsmToken::Kind::Minus || peek().kind == AsmToken::Kind::Plus) &&
                                                               pos + 1 < tokens.size() && tokens[pos + 1].kind == AsmToken::Kind::Integer))
                {
                    bool negative = accept(AsmToken::Kind::Minus);
                    std::ignore = accept(AsmToken::Kind::Plus);
                    std::int64_t value = tokens[pos].integer;
                    ++pos;
                    if (negative)
                        value = -value;
                    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                    {
                        error = "memory displacement does not fit a 32-bit signed value in inline assembly";
                        return std::nullopt;
                    }

                    disp = static_cast<std::int32_t>(value);
                    have_disp = true;
                    if (!accept(AsmToken::Kind::Plus) && !accept(AsmToken::Kind::Minus))
                    {
                        error = "absolute addresses are not supported in native inline assembly; pass the address as an operand";
                        return std::nullopt;
                    }

                    if (tokens[pos - 1].kind == AsmToken::Kind::Minus)
                    {
                        error = "expected a register after '-' in the memory operand";
                        return std::nullopt;
                    }
                }

                auto parse_reg = [&]() -> std::optional<em64t::VReg> {
                    if (peek().kind != AsmToken::Kind::Ident)
                    {
                        error = "expected a register in the memory operand";
                        return std::nullopt;
                    }

                    auto name = tokens[pos++].text;
                    auto phys = resolve_register(name);
                    if (!phys)
                    {
                        if (error.empty())
                            error = std::format("unknown inline assembly register '{}'", name);
                        return std::nullopt;
                    }

                    auto width = register_width(name);
                    if (!width || (*width != 32 && *width != 64))
                    {
                        error = std::format("address registers must be 32- or 64-bit, not '{}'", name);
                        return std::nullopt;
                    }

                    return em64t::VReg::phys(*phys);
                };

                auto first = parse_reg();
                if (!first)
                    return std::nullopt;

                base = *first;
                have_base_or_index = true;

                if (peek().kind == AsmToken::Kind::Star)
                {
                    error = "invalid memory operand; expected '+', '-', or ']' after the base register";
                    return std::nullopt;
                }

                if (accept(AsmToken::Kind::Plus))
                {
                    if (peek().kind == AsmToken::Kind::Ident)
                    {
                        auto idx = parse_reg();
                        if (!idx)
                            return std::nullopt;

                        index = *idx;
                        if (accept(AsmToken::Kind::Star))
                        {
                            if (peek().kind != AsmToken::Kind::Integer)
                            {
                                error = "expected a scale of 1, 2, 4, or 8 in the memory operand";
                                return std::nullopt;
                            }

                            auto value = tokens[pos++].integer;
                            if (value != 1 && value != 2 && value != 4 && value != 8)
                            {
                                error = "invalid scale in memory operand; expected 1, 2, 4, or 8";
                                return std::nullopt;
                            }

                            scale = static_cast<std::uint8_t>(value);
                        }
                        if (accept(AsmToken::Kind::Plus) || accept(AsmToken::Kind::Minus))
                        {
                            bool negative = tokens[pos - 1].kind == AsmToken::Kind::Minus;
                            if (peek().kind != AsmToken::Kind::Integer)
                            {
                                error = "expected a displacement in the memory operand";
                                return std::nullopt;
                            }
                            std::int64_t value = tokens[pos++].integer;
                            if (negative)
                                value = -value;

                            if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                            {
                                error = "memory displacement does not fit a 32-bit signed value in inline assembly";
                                return std::nullopt;
                            }

                            disp = static_cast<std::int32_t>(value);
                            have_disp = true;
                        }
                    }
                    else if (peek().kind == AsmToken::Kind::Integer)
                    {
                        std::int64_t value = tokens[pos++].integer;
                        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                        {
                            error = "memory displacement does not fit a 32-bit signed value in inline assembly";
                            return std::nullopt;
                        }

                        disp = static_cast<std::int32_t>(value);
                        have_disp = true;
                    }
                    else
                    {
                        error = "expected a register or displacement in the memory operand";
                        return std::nullopt;
                    }
                }
                else if (accept(AsmToken::Kind::Minus))
                {
                    if (peek().kind != AsmToken::Kind::Integer)
                    {
                        error = "expected a displacement after '-' in the memory operand";
                        return std::nullopt;
                    }
                    std::int64_t value = -tokens[pos++].integer;
                    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                    {
                        error = "memory displacement does not fit a 32-bit signed value in inline assembly";
                        return std::nullopt;
                    }
                    disp = static_cast<std::int32_t>(value);
                    have_disp = true;
                }

                if (!have_base_or_index && !have_disp)
                {
                    error = "empty memory operand in inline assembly";
                    return std::nullopt;
                }
                ParsedOperand op;
                op.kind = ParsedOperand::Kind::Mem;
                op.mem = em64t::MMem{base, index, scale, disp};
                return op;
            }

            [[nodiscard]] std::optional<ParsedOperand> parse_att_operand()
            {
                if (accept(AsmToken::Kind::Percent))
                {
                    auto name = expect_ident("a register after '%'");
                    if (!name)
                        return std::nullopt;

                    auto phys = resolve_register(*name);
                    if (!phys)
                    {
                        if (error.empty())
                            error = std::format("unknown inline assembly register '{}'", *name);
                        return std::nullopt;
                    }

                    auto width = register_width(*name);
                    ParsedOperand op;
                    op.kind = ParsedOperand::Kind::Reg;
                    op.reg = *phys;
                    op.width = width.value_or(64);
                    return op;
                }
                if (accept(AsmToken::Kind::Dollar))
                {
                    bool negative = accept(AsmToken::Kind::Minus);
                    if (!negative)
                        std::ignore = accept(AsmToken::Kind::Plus);
                    if (peek().kind != AsmToken::Kind::Integer)
                    {
                        error = "expected an integer after '$' in inline assembly";
                        return std::nullopt;
                    }

                    ParsedOperand op;
                    op.kind = ParsedOperand::Kind::Imm;
                    op.imm = negative ? -tokens[pos].integer : tokens[pos].integer;
                    ++pos;
                    return op;
                }
                if (peek().kind == AsmToken::Kind::Integer || peek().kind == AsmToken::Kind::Minus || peek().kind == AsmToken::Kind::LParen)
                {
                    std::int32_t disp = 0;
                    if (peek().kind != AsmToken::Kind::LParen)
                    {
                        bool negative = accept(AsmToken::Kind::Minus);
                        if (!negative)
                            std::ignore = accept(AsmToken::Kind::Plus);
                        if (peek().kind != AsmToken::Kind::Integer)
                        {
                            error = "expected a displacement in the memory operand";
                            return std::nullopt;
                        }

                        std::int64_t value = tokens[pos++].integer;
                        if (negative)
                            value = -value;

                        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                        {
                            error = "memory displacement does not fit a 32-bit signed value in inline assembly";
                            return std::nullopt;
                        }

                        disp = static_cast<std::int32_t>(value);
                    }
                    if (!accept(AsmToken::Kind::LParen))
                    {
                        ParsedOperand op;
                        op.kind = ParsedOperand::Kind::Imm;
                        op.imm = disp;
                        return op;
                    }
                    auto parse_base_or_index = [&](bool required) -> std::optional<em64t::VReg> {
                        if (!accept(AsmToken::Kind::Percent))
                        {
                            if (required)
                            {
                                error = "expected '%' before the register in the memory operand";
                                return std::nullopt;
                            }
                            return em64t::VReg{};
                        }

                        auto name = expect_ident("a register in the memory operand");
                        if (!name)
                            return std::nullopt;

                        auto phys = resolve_register(*name);
                        if (!phys)
                        {
                            if (error.empty())
                                error = std::format("unknown inline assembly register '{}'", *name);
                            return std::nullopt;
                        }

                        auto width = register_width(*name);
                        if (!width || (*width != 32 && *width != 64))
                        {
                            error = std::format("address registers must be 32- or 64-bit, not '{}'", *name);
                            return std::nullopt;
                        }
                        return em64t::VReg::phys(*phys);
                    };
                    auto base = parse_base_or_index(true);
                    if (!base)
                        return std::nullopt;

                    em64t::VReg index;
                    std::uint8_t scale = 1;
                    if (accept(AsmToken::Kind::Comma))
                    {
                        if (peek().kind == AsmToken::Kind::RParen)
                        {
                            error = "expected an index register after ',' in the memory operand";
                            return std::nullopt;
                        }

                        auto idx = parse_base_or_index(true);
                        if (!idx)
                            return std::nullopt;

                        index = *idx;
                        if (accept(AsmToken::Kind::Comma))
                        {
                            if (peek().kind != AsmToken::Kind::Integer)
                            {
                                error = "expected a scale of 1, 2, 4, or 8 in the memory operand";
                                return std::nullopt;
                            }
                            auto value = tokens[pos++].integer;
                            if (value != 1 && value != 2 && value != 4 && value != 8)
                            {
                                error = "invalid scale in memory operand; expected 1, 2, 4, or 8";
                                return std::nullopt;
                            }
                            scale = static_cast<std::uint8_t>(value);
                        }
                    }
                    if (!accept(AsmToken::Kind::RParen))
                    {
                        error = "expected ')' to close the memory operand in inline assembly";
                        return std::nullopt;
                    }

                    ParsedOperand op;
                    op.kind = ParsedOperand::Kind::Mem;
                    op.mem = em64t::MMem{*base, index, scale, disp};
                    return op;
                }
                if (peek().kind == AsmToken::Kind::Ident)
                {
                    error = "expected '%' before the register in AT&T inline assembly; write Intel syntax with @[intel] "
                            "or prefix registers with '%'";
                    return std::nullopt;
                }

                error = "expected an operand in inline assembly";
                return std::nullopt;
            }

            [[nodiscard]] std::optional<ParsedOperand> parse_operand()
            {
                if (att)
                    return parse_att_operand();
                return parse_intel_operand();
            }
        };

        [[nodiscard]] std::string to_lower_asm(std::string_view text)
        {
            std::string lowered(text);
            for (char& c : lowered)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lowered;
        }

        struct ResolvedMnemonic
        {
            std::string canonical;
            unsigned width = 0;
        };

        [[nodiscard]] std::optional<ResolvedMnemonic> resolve_mnemonic(std::string_view raw, bool att, std::string& error)
        {
            auto lowered = to_lower_asm(raw);
            if (!att)
                return ResolvedMnemonic{lowered, 0};
            if (lowered == "movzx" || lowered == "movsx")
            {
                ResolvedMnemonic named;
                named.canonical = lowered;
                return named;
            }
            if (lowered == "movq")
                return ResolvedMnemonic{"movq", 64};
            if (lowered.starts_with("movz") || lowered.starts_with("movs"))
            {
                bool zero = lowered[3] == 'z';
                std::string pair = lowered.substr(4);
                unsigned src_width = 0;
                unsigned dst_width = 0;
                auto width_of = [](char c) -> unsigned {
                    if (c == 'b')
                        return 8;
                    if (c == 'w')
                        return 16;
                    if (c == 'l')
                        return 32;
                    if (c == 'q')
                        return 64;
                    return 0;
                };

                if (pair.size() == 2)
                {
                    src_width = width_of(pair[0]);
                    dst_width = width_of(pair[1]);
                }

                if (src_width == 0 || dst_width == 0)
                {
                    error = std::format("invalid mnemonic suffix in '{}'; expected 'movzbl', 'movzbq', 'movslq', and similar", raw);
                    return std::nullopt;
                }

                if (((src_width != 8 && src_width != 16) || (dst_width != 32 && dst_width != 64)) && !(src_width == 32 && dst_width == 64 && !zero))
                {
                    error = std::format("unsupported width combination in '{}'", raw);
                    return std::nullopt;
                }

                return ResolvedMnemonic{zero ? "movzx" : "movsx", dst_width * 256 + src_width};
            }
            unsigned suffix_width = 0;
            std::string_view stem = lowered;
            if (lowered.size() > 1)
            {
                char last = lowered.back();
                unsigned width = (last == 'b') ? 8 : (last == 'w') ? 16 : (last == 'l') ? 32 : (last == 'q') ? 64 : 0;
                if (width != 0)
                {
                    static constexpr std::string_view suffixed[] = {"mov",  "add", "sub",  "and", "or",  "xor",  "cmp",   "test", "shl",
                                                                    "shr",  "sar", "inc",  "dec", "neg", "not",  "imul",  "push", "pop",
                                                                    "xchg", "lea", "call", "jmp", "ret", "movq", "callq", "jmpq"};
                    auto candidate = lowered.substr(0, lowered.size() - 1);
                    for (auto known : suffixed)
                    {
                        if (candidate == known)
                        {
                            stem = candidate;
                            suffix_width = width;
                            break;
                        }
                    }
                }
            }
            return ResolvedMnemonic{std::string(stem), suffix_width};
        }

        [[nodiscard]] bool fits_i(std::int64_t value, unsigned bits)
        {
            if (bits >= 64)
                return true;

            std::int64_t lo = -(1LL << (bits - 1));
            std::int64_t hi = (1LL << (bits - 1)) - 1;
            return value >= lo && value <= hi;
        }

        [[nodiscard]] bool fits_i_or_u(std::int64_t value, unsigned bits)
        {
            if (bits >= 64)
                return true;

            std::int64_t lo = -(1LL << (bits - 1));
            std::uint64_t hi = (bits == 64 ? ~0ULL : ((1ULL << bits) - 1));
            return value >= lo && static_cast<std::uint64_t>(value) <= hi;
        }

        [[nodiscard]] std::optional<em64t::MInstr> build_native_instruction(std::string_view mnemonic, unsigned width,
                                                                            std::vector<ParsedOperand> const& operands, std::string& error)
        {
            using em64t::MOpc;
            using Kind = ParsedOperand::Kind;
            auto fail = [&](std::string message) -> std::optional<em64t::MInstr> {
                error = std::move(message);
                return std::nullopt;
            };
            auto kinds = [&] {
                std::string result;
                for (auto const& op : operands)
                    result += op.kind == Kind::Reg ? 'r' : op.kind == Kind::Imm ? 'i' : 'm';
                return result;
            };
            auto reg_op = [](ParsedOperand const& op) { return em64t::MOp::from_reg(em64t::VReg::phys(op.reg)); };
            auto imm_op = [](ParsedOperand const& op) { return em64t::MOp::from_imm(op.imm); };
            auto mem_op = [](ParsedOperand const& op) { return em64t::MOp::from_mem(op.mem); };

            std::string owned_mnemonic;
            if (mnemonic == "movq")
            {
                bool xmm = false;
                for (auto const& op : operands)
                    if (op.kind == Kind::Reg && op.width == 128)
                        xmm = true;

                if (!xmm)
                {
                    owned_mnemonic = "mov";
                    mnemonic = owned_mnemonic;
                }
            }

            auto infer_width = [&]() -> unsigned {
                if (width != 0)
                    return width;

                for (auto const& op : operands)
                    if (op.kind == Kind::Reg)
                        return op.width;

                return 0;
            };

            if (mnemonic == "nop")
            {
                if (!operands.empty())
                    return fail("'nop' expects 0 operands");

                em64t::MInstr mi;
                mi.opc = MOpc::NOP;
                return mi;
            }
            if (mnemonic == "xchg")
            {
                if (operands.size() != 2 || operands[0].kind != Kind::Reg || operands[1].kind != Kind::Reg)
                    return fail("'xchg' expects 2 register operands");

                unsigned w = width != 0 ? width : operands[0].width;
                if (w != 64 || operands[0].width != 64 || operands[1].width != 64)
                    return fail("'xchg' supports 64-bit registers in native inline assembly");

                em64t::MInstr mi;
                mi.opc = MOpc::XCHG64rr;
                mi.num_ops = 2;
                mi.num_defs = 1;
                mi.ops[0] = reg_op(operands[0]);
                mi.ops[1] = reg_op(operands[1]);
                return mi;
            }
            if (mnemonic == "movq" && operands.size() == 2 &&
                ((operands[0].kind == Kind::Reg && operands[0].width == 128) || (operands[1].kind == Kind::Reg && operands[1].width == 128)))
            {
                if (operands[0].kind != Kind::Reg || operands[1].kind != Kind::Reg)
                    return fail("'movq' expects 2 register operands");

                em64t::MInstr mi;
                mi.num_ops = 2;
                mi.num_defs = 1;
                if (operands[0].width == 128)
                {
                    mi.opc = MOpc::MOVQ64rr;
                    mi.ops[0] = reg_op(operands[0]);
                    mi.ops[1] = reg_op(operands[1]);
                }
                else
                {
                    mi.opc = MOpc::MOVQ64rr_rev;
                    mi.ops[0] = reg_op(operands[0]);
                    mi.ops[1] = reg_op(operands[1]);
                }
                return mi;
            }
            if ((mnemonic == "movsd" || mnemonic == "movss") && operands.size() == 2)
            {
                if (operands[0].kind != Kind::Reg || operands[1].kind != Kind::Reg || operands[0].width != 128 || operands[1].width != 128)
                    return fail(std::format("'{}' expects 2 XMM register operands in native inline assembly", mnemonic));

                em64t::MInstr mi;
                mi.opc = mnemonic == "movsd" ? MOpc::MOVSDrr : MOpc::MOVSSrr;
                mi.num_ops = 2;
                mi.num_defs = 1;
                mi.ops[0] = reg_op(operands[0]);
                mi.ops[1] = reg_op(operands[1]);
                return mi;
            }
            if (mnemonic == "mov" || mnemonic == "movzx" || mnemonic == "movsx")
            {
                bool extending = mnemonic != "mov";
                unsigned src_width = 0;
                unsigned effective = width;
                if (extending && width >= 256)
                {
                    src_width = width % 256;
                    effective = width / 256;
                }

                if (operands.size() != 2)
                    return fail(std::format("'{}' expects 2 operands", mnemonic));

                auto const& dst = operands[0];
                auto const& src = operands[1];
                if (!extending && dst.kind == Kind::Mem)
                {
                    unsigned mem_w = dst.width;
                    if (mem_w == 0)
                    {
                        if (src.kind == Kind::Reg)
                            mem_w = src.width;
                        else if (width != 0 && width < 256)
                            mem_w = width;
                    }
                    if (mem_w != 8 && mem_w != 16 && mem_w != 32 && mem_w != 64)
                        return fail("ambiguous memory operand width for 'mov'; write 'byte', 'word', 'dword', or 'qword' before the "
                                    "memory operand, or use a suffixed AT&T mnemonic");
                    if (src.kind == Kind::Reg && src.width != mem_w)
                        return fail(std::format("cannot use {}-bit register as a {}-bit operand", src.width, mem_w));
                    if (src.kind == Kind::Imm)
                    {
                        if (mem_w == 64)
                        {
                            if (!fits_i(src.imm, 32))
                                return fail("immediate does not fit instruction encoding; move it into a register first");
                        }
                        else if (!fits_i_or_u(src.imm, mem_w))
                            return fail("immediate does not fit instruction encoding");
                    }
                    if (src.kind != Kind::Reg && src.kind != Kind::Imm)
                        return fail("'mov' between two memory operands is not supported; use a register");
                    em64t::MInstr store;
                    store.num_ops = 2;
                    store.num_defs = 0;
                    store.ops[0] = mem_op(dst);
                    store.ops[1] = src.kind == Kind::Reg ? reg_op(src) : imm_op(src);
                    if (mem_w == 8)
                        store.opc = src.kind == Kind::Reg ? MOpc::MOV8mr : MOpc::MOV8mi;
                    else if (mem_w == 16)
                        store.opc = src.kind == Kind::Reg ? MOpc::MOV16mr : MOpc::MOV16mi;
                    else if (mem_w == 32)
                        store.opc = src.kind == Kind::Reg ? MOpc::MOV32mr : MOpc::MOV32mi;
                    else
                        store.opc = src.kind == Kind::Reg ? MOpc::MOV64mr : MOpc::MOV64mi32;
                    return store;
                }
                if (dst.kind != Kind::Reg)
                    return fail(std::format("the destination of '{}' must be a register", mnemonic));

                unsigned dst_width = effective != 0 ? effective : dst.width;
                if (effective != 0 && dst.width != 128 && dst_width != dst.width)
                    return fail(std::format("cannot use {}-bit register '{}' as a {}-bit operand", dst.width, "register", dst_width));

                if (dst.width == 128)
                    return fail(std::format("'{}' between XMM registers uses 'movq', 'movsd', or 'movss'", mnemonic));

                unsigned src_w = src.kind == Kind::Reg ? src.width : (src.kind == Kind::Mem ? src.width : 0);
                if (extending)
                {
                    if (src_w == 0)
                        src_w = src_width;
                    else if (src_width != 0 && src_w != src_width)
                        return fail(std::format("'{}' source width does not match the mnemonic suffix", mnemonic));
                    if (src.kind == Kind::Imm)
                        return fail(std::format("'{}' cannot extend an immediate; move it into a register first", mnemonic));
                    if (src.kind == Kind::Mem && src_w == 0)
                        return fail(std::format("ambiguous memory operand width for '{}'; write 'byte', 'word', 'dword', or 'qword' before the "
                                                "memory operand, or use a suffixed AT&T mnemonic",
                                                mnemonic));
                    MOpc opc = MOpc::NOP;
                    bool mem = src.kind == Kind::Mem;
                    if (mnemonic == "movzx")
                    {
                        if (dst_width == 64 && src_w == 8)
                            opc = mem ? MOpc::MOVZX64rm8 : MOpc::MOVZX64rr8;
                        else if (dst_width == 64 && src_w == 16)
                            opc = mem ? MOpc::MOVZX64rm16 : MOpc::MOVZX64rr16;
                        else if (dst_width == 32 && src_w == 8)
                            opc = mem ? MOpc::MOVZX32rm8 : MOpc::MOVZX32rr8;
                        else if (dst_width == 32 && src_w == 16)
                        {
                            if (mem)
                                return fail("'movzx' from 16-bit memory to a 32-bit register is not supported in native inline assembly");
                            opc = MOpc::MOVZX32_16rr;
                        }
                        else if (dst_width == 64 && src_w == 32)
                            return fail("'movzx' from 32 bits to 64 bits is a plain 32-bit 'mov' (which zero-extends)");
                        else
                            return fail(std::format("unsupported operand combination for 'movzx' ({}-bit destination, {}-bit source)", dst_width, src_w));
                    }
                    else
                    {
                        if (dst_width == 64 && src_w == 8)
                        {
                            if (mem)
                                return fail("'movsx' from 8-bit memory to a 64-bit register is not supported in native inline assembly");
                            opc = MOpc::MOVSX64rr8;
                        }
                        else if (dst_width == 64 && src_w == 16)
                        {
                            if (mem)
                                return fail("'movsx' from 16-bit memory to a 64-bit register is not supported in native inline assembly");
                            opc = MOpc::MOVSX64rr16;
                        }
                        else if (dst_width == 64 && src_w == 32)
                        {
                            if (mem)
                                return fail("'movsx' from 32-bit memory is not supported in native inline assembly; load it first");
                            opc = MOpc::MOVSX64_32rr;
                        }
                        else if (dst_width == 32 && src_w == 8)
                        {
                            if (mem)
                                return fail("'movsx' from 8-bit memory to a 32-bit register is not supported in native inline assembly");
                            opc = MOpc::MOVSX32_8rr;
                        }
                        else if (dst_width == 32 && src_w == 16)
                        {
                            if (mem)
                                return fail("'movsx' from 16-bit memory to a 32-bit register is not supported in native inline assembly");
                            opc = MOpc::MOVSX32_16rr;
                        }
                        else
                            return fail(std::format("unsupported operand combination for 'movsx' ({}-bit destination, {}-bit source)", dst_width, src_w));
                    }
                    em64t::MInstr mi;
                    mi.opc = opc;
                    mi.num_ops = 2;
                    mi.num_defs = 1;
                    mi.ops[0] = reg_op(dst);
                    mi.ops[1] = src.kind == Kind::Mem ? mem_op(src) : reg_op(src);
                    return mi;
                }
                unsigned w = infer_width();
                if (src.kind == Kind::Reg && src.width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", src.width, w));
                if (dst.width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", dst.width, w));
                if (src.kind != Kind::Reg && src.kind != Kind::Imm && src.kind != Kind::Mem)
                    return fail("'mov' expects a register, immediate, or memory source");
                if (src.kind == Kind::Mem && dst.kind == Kind::Mem)
                    return fail("'mov' between two memory operands is not supported; use a register");
                if (src.kind == Kind::Imm && w != 8 && w != 16 && w != 32 && w != 64)
                    return fail("'mov' supports 8-, 16-, 32-, and 64-bit operands");

                MOpc opc = MOpc::NOP;
                auto pattern = kinds();
                if (w == 8)
                {
                    if (pattern == "rr")
                        opc = MOpc::MOV8rr;
                    else if (pattern == "ri")
                    {
                        if (!fits_i_or_u(src.imm, 8))
                            return fail("immediate does not fit instruction encoding");
                        opc = MOpc::MOV8ri;
                    }
                    else if (pattern == "rm")
                        opc = MOpc::MOV8rm;
                    else if (pattern == "mr")
                        opc = MOpc::MOV8mr;
                    else if (pattern == "mi")
                    {
                        if (!fits_i_or_u(src.imm, 8))
                            return fail("immediate does not fit instruction encoding");
                        opc = MOpc::MOV8mi;
                    }
                    else
                        return fail("'mov' expects register, immediate, or memory operands");
                }
                else if (w == 16)
                {
                    if (pattern == "rr")
                        opc = MOpc::MOV16rr;
                    else if (pattern == "ri")
                    {
                        if (!fits_i_or_u(src.imm, 16))
                            return fail("immediate does not fit instruction encoding");
                        opc = MOpc::MOV16ri;
                    }
                    else if (pattern == "rm")
                        opc = MOpc::MOV16rm;
                    else if (pattern == "mr")
                        opc = MOpc::MOV16mr;
                    else if (pattern == "mi")
                    {
                        if (!fits_i_or_u(src.imm, 16))
                            return fail("immediate does not fit instruction encoding");
                        opc = MOpc::MOV16mi;
                    }
                    else
                        return fail("'mov' expects register, immediate, or memory operands");
                }
                else if (w == 32)
                {
                    if (pattern == "rr")
                        opc = MOpc::MOV32rr;
                    else if (pattern == "ri")
                    {
                        if (!fits_i_or_u(src.imm, 32))
                            return fail("immediate does not fit instruction encoding");
                        opc = MOpc::MOV32ri;
                    }
                    else if (pattern == "rm")
                        opc = MOpc::MOV32rm;
                    else if (pattern == "mr")
                        opc = MOpc::MOV32mr;
                    else if (pattern == "mi")
                    {
                        if (!fits_i_or_u(src.imm, 32))
                            return fail("immediate does not fit instruction encoding");
                        opc = MOpc::MOV32mi;
                    }
                    else
                        return fail("'mov' expects register, immediate, or memory operands");
                }
                else if (w == 64)
                {
                    if (pattern == "rr")
                        opc = MOpc::MOV64rr;
                    else if (pattern == "ri")
                        opc = fits_i(src.imm, 32) ? MOpc::MOV64ri32 : MOpc::MOV64ri;
                    else if (pattern == "rm")
                        opc = MOpc::MOV64rm;
                    else if (pattern == "mr")
                        opc = MOpc::MOV64mr;
                    else if (pattern == "mi")
                    {
                        if (!fits_i(src.imm, 32))
                            return fail("immediate does not fit instruction encoding; move it into a register first");
                        opc = MOpc::MOV64mi32;
                    }
                    else
                        return fail("'mov' expects register, immediate, or memory operands");
                }
                else
                    return fail("'mov' supports 8-, 16-, 32-, and 64-bit operands");
                em64t::MInstr mi;
                mi.opc = opc;
                mi.num_ops = 2;
                mi.num_defs = dst.kind == Kind::Reg ? 1 : 0;
                mi.ops[0] = dst.kind == Kind::Reg ? reg_op(dst) : mem_op(dst);
                mi.ops[1] = src.kind == Kind::Reg ? reg_op(src) : src.kind == Kind::Imm ? imm_op(src) : mem_op(src);
                return mi;
            }
            if ((mnemonic == "and" || mnemonic == "or" || mnemonic == "xor") && operands.size() == 2 && operands[0].kind == Kind::Mem &&
                operands[1].kind == Kind::Reg)
            {
                unsigned w = width != 0 ? width : operands[1].width;
                if (w != 64 || operands[1].width != 64)
                    return fail(std::format("'{}' with a memory destination supports only 64-bit operands in native inline assembly", mnemonic));
                if (operands[0].width != 0 && operands[0].width != 64)
                    return fail(std::format("memory operand width ({} bits) does not match the 64-bit '{}'", operands[0].width, mnemonic));
                em64t::MInstr mi;
                mi.num_ops = 2;
                mi.num_defs = 0;
                mi.ops[0] = mem_op(operands[0]);
                mi.ops[1] = reg_op(operands[1]);
                mi.opc = mnemonic == "and" ? MOpc::AND64mr : mnemonic == "or" ? MOpc::OR64mr : MOpc::XOR64mr;
                return mi;
            }
            if (mnemonic == "add" || mnemonic == "sub" || mnemonic == "and" || mnemonic == "or" || mnemonic == "xor")
            {
                if (operands.size() != 2)
                    return fail(std::format("'{}' expects 2 operands", mnemonic));

                auto const& dst = operands[0];
                auto const& src = operands[1];
                if (dst.kind != Kind::Reg)
                    return fail(std::format("the destination of '{}' must be a register", mnemonic));
                unsigned w = width != 0 ? width : dst.width;
                if (w != 32 && w != 64)
                    return fail(std::format("'{}' supports 32- and 64-bit operands in native inline assembly", mnemonic));
                if (dst.width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", dst.width, w));
                if (src.kind == Kind::Reg && src.width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", src.width, w));
                if (src.kind == Kind::Mem && src.width != 0 && src.width != w)
                    return fail(std::format("memory operand width ({} bits) does not match the {}-bit '{}'", src.width, w, mnemonic));

                bool is64 = w == 64;
                em64t::MInstr mi;
                mi.num_defs = 1;
                if (src.kind == Kind::Reg)
                {
                    mi.num_ops = 3;
                    mi.ops[0] = reg_op(dst);
                    mi.ops[1] = reg_op(dst);
                    mi.ops[2] = reg_op(src);
                    if (mnemonic == "add")
                        mi.opc = is64 ? MOpc::ADD64rr : MOpc::ADD32rr;
                    else if (mnemonic == "sub")
                        mi.opc = is64 ? MOpc::SUB64rr : MOpc::SUB32rr;
                    else if (mnemonic == "and")
                        mi.opc = is64 ? MOpc::AND64rr : MOpc::AND32rr;
                    else if (mnemonic == "or")
                        mi.opc = is64 ? MOpc::OR64rr : MOpc::OR32rr;
                    else
                        mi.opc = is64 ? MOpc::XOR64rr : MOpc::XOR32rr;
                    return mi;
                }
                if (src.kind == Kind::Imm)
                {
                    if (!fits_i(src.imm, 32))
                        return fail("immediate does not fit instruction encoding; move it into a register first");

                    mi.num_ops = 2;
                    mi.ops[0] = reg_op(dst);
                    mi.ops[1] = imm_op(src);
                    if (mnemonic == "add")
                        mi.opc = is64 ? MOpc::ADD64ri32 : MOpc::ADD32ri;
                    else if (mnemonic == "sub")
                        mi.opc = is64 ? MOpc::SUB64ri32 : MOpc::SUB32ri;
                    else if (mnemonic == "and")
                        mi.opc = is64 ? MOpc::AND64ri32 : MOpc::AND32ri;
                    else if (mnemonic == "or")
                        mi.opc = is64 ? MOpc::OR64ri32 : MOpc::OR32ri;
                    else
                        mi.opc = is64 ? MOpc::XOR64ri32 : MOpc::XOR32ri;
                    return mi;
                }
                if (src.kind == Kind::Mem)
                {
                    mi.num_ops = 3;
                    mi.ops[0] = reg_op(dst);
                    mi.ops[1] = reg_op(dst);
                    mi.ops[2] = mem_op(src);
                    if (mnemonic == "add")
                        mi.opc = is64 ? MOpc::ADD64rm : MOpc::ADD32rm;
                    else if (mnemonic == "sub")
                        mi.opc = is64 ? MOpc::SUB64rm : MOpc::SUB32rm;
                    else if (!is64)
                        return fail(std::format("'{}' with a memory operand supports only 64-bit operands in native inline assembly", mnemonic));
                    else if (mnemonic == "and")
                        mi.opc = MOpc::AND64rm;
                    else if (mnemonic == "or")
                        mi.opc = MOpc::OR64rm;
                    else
                        mi.opc = MOpc::XOR64rm;
                    return mi;
                }
                return fail(std::format("'{}' expects a register, immediate, or memory source", mnemonic));
            }
            if (mnemonic == "cmp")
            {
                if (operands.size() != 2)
                    return fail("'cmp' expects 2 operands");

                auto const& lhs = operands[0];
                auto const& rhs = operands[1];
                if (lhs.kind != Kind::Reg)
                    return fail("the first operand of 'cmp' must be a register");
                unsigned w = width != 0 ? width : lhs.width;
                if (w != 8 && w != 32 && w != 64)
                    return fail("'cmp' supports 8-, 32-, and 64-bit operands in native inline assembly");
                if (lhs.width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", lhs.width, w));
                if (rhs.kind == Kind::Reg && rhs.width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", rhs.width, w));
                if (rhs.kind == Kind::Mem && rhs.width != 0 && rhs.width != w)
                    return fail(std::format("memory operand width ({} bits) does not match the {}-bit 'cmp'", rhs.width, w));
                em64t::MInstr mi;
                mi.num_defs = 0;
                if (rhs.kind == Kind::Reg)
                {
                    mi.num_ops = 2;
                    mi.ops[0] = reg_op(lhs);
                    mi.ops[1] = reg_op(rhs);
                    mi.opc = w == 64 ? MOpc::CMP64rr : w == 32 ? MOpc::CMP32rr : MOpc::CMP8rr;
                    return mi;
                }
                if (rhs.kind == Kind::Imm)
                {
                    if (w == 64)
                    {
                        if (!fits_i(rhs.imm, 32))
                            return fail("immediate does not fit instruction encoding; move it into a register first");
                        mi.num_ops = 2;
                        mi.ops[0] = reg_op(lhs);
                        mi.ops[1] = imm_op(rhs);
                        mi.opc = MOpc::CMP64ri32;
                        return mi;
                    }
                    if (w == 32)
                    {
                        if (!fits_i_or_u(rhs.imm, 32))
                            return fail("immediate does not fit instruction encoding");
                        mi.num_ops = 2;
                        mi.ops[0] = reg_op(lhs);
                        mi.ops[1] = imm_op(rhs);
                        mi.opc = MOpc::CMP32ri;
                        return mi;
                    }
                    if (!fits_i_or_u(rhs.imm, 8))
                        return fail("immediate does not fit instruction encoding");
                    mi.num_ops = 2;
                    mi.ops[0] = reg_op(lhs);
                    mi.ops[1] = imm_op(rhs);
                    mi.opc = MOpc::CMP8ri;
                    return mi;
                }
                if (rhs.kind == Kind::Mem)
                {
                    if (w == 8)
                        return fail("'cmp' with an 8-bit memory operand is not supported in native inline assembly; load it first");
                    mi.num_ops = 3;
                    mi.ops[0] = reg_op(lhs);
                    mi.ops[1] = reg_op(lhs);
                    mi.ops[2] = mem_op(rhs);
                    mi.opc = w == 64 ? MOpc::CMP64rm : MOpc::CMP32rm;
                    return mi;
                }
                return fail("'cmp' expects a register, immediate, or memory second operand");
            }
            if (mnemonic == "test")
            {
                if (operands.size() != 2)
                    return fail("'test' expects 2 operands");

                auto const& lhs = operands[0];
                auto const& rhs = operands[1];
                if (lhs.kind != Kind::Reg)
                    return fail("the first operand of 'test' must be a register");
                unsigned w = width != 0 ? width : lhs.width;
                if (w != 8 && w != 32 && w != 64)
                    return fail("'test' supports 8-, 32-, and 64-bit operands in native inline assembly");
                if (lhs.width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", lhs.width, w));

                em64t::MInstr mi;
                mi.num_defs = 0;
                if (rhs.kind == Kind::Reg)
                {
                    if (rhs.width != w)
                        return fail(std::format("cannot use {}-bit register as a {}-bit operand", rhs.width, w));
                    mi.num_ops = 2;
                    mi.ops[0] = reg_op(lhs);
                    mi.ops[1] = reg_op(rhs);
                    mi.opc = w == 64 ? MOpc::TEST64rr : w == 32 ? MOpc::TEST32rr : MOpc::TEST8rr;
                    return mi;
                }
                if (rhs.kind == Kind::Imm)
                {
                    if (w != 64)
                        return fail("'test' with an immediate supports only 64-bit operands in native inline assembly");
                    if (!fits_i(rhs.imm, 32))
                        return fail("immediate does not fit instruction encoding; move it into a register first");
                    mi.num_ops = 2;
                    mi.ops[0] = reg_op(lhs);
                    mi.ops[1] = imm_op(rhs);
                    mi.opc = MOpc::TEST64ri;
                    return mi;
                }
                return fail("'test' expects a register or immediate second operand");
            }
            if (mnemonic == "lea")
            {
                if (operands.size() != 2 || operands[0].kind != Kind::Reg || operands[1].kind != Kind::Mem)
                    return fail("'lea' expects a register and a memory operand");

                unsigned w = width != 0 ? width : operands[0].width;
                if (w != 64 || operands[0].width != 64)
                    return fail("'lea' supports 64-bit destination registers in native inline assembly");

                em64t::MInstr mi;
                mi.opc = MOpc::LEA64rm;
                mi.num_ops = 2;
                mi.num_defs = 1;
                mi.ops[0] = reg_op(operands[0]);
                mi.ops[1] = mem_op(operands[1]);
                return mi;
            }
            if (mnemonic == "shl" || mnemonic == "shr" || mnemonic == "sar")
            {
                if (operands.size() != 2 || operands[0].kind != Kind::Reg)
                    return fail(std::format("'{}' expects a register and a shift count", mnemonic));

                unsigned w = width != 0 ? width : operands[0].width;
                if (w != 32 && w != 64)
                    return fail(std::format("'{}' supports 32- and 64-bit operands in native inline assembly", mnemonic));
                if (operands[0].width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", operands[0].width, w));
                bool is64 = w == 64;
                auto const& count = operands[1];
                auto pick = [&](MOpc cl, MOpc ri) { return count.kind == Kind::Reg ? cl : ri; };
                MOpc opc = MOpc::NOP;
                if (mnemonic == "shl")
                    opc = pick(is64 ? MOpc::SHL64rCL : MOpc::SHL32rCL, is64 ? MOpc::SHL64ri8 : MOpc::SHL32ri8);
                else if (mnemonic == "shr")
                    opc = pick(is64 ? MOpc::SHR64rCL : MOpc::SHR32rCL, is64 ? MOpc::SHR64ri8 : MOpc::SHR32ri8);
                else
                    opc = pick(is64 ? MOpc::SAR64rCL : MOpc::SAR32rCL, is64 ? MOpc::SAR64ri8 : MOpc::SAR32ri8);
                em64t::MInstr mi;
                mi.opc = opc;
                mi.num_defs = 1;
                if (count.kind == Kind::Reg)
                {
                    if (count.reg != em64t::PhysReg::RCX || count.width != 8)
                        return fail(std::format("the shift count register must be 'cl' for '{}'", mnemonic));
                    mi.num_ops = 2;
                    mi.ops[0] = reg_op(operands[0]);
                    mi.ops[1] = reg_op(count);
                    return mi;
                }
                if (count.kind == Kind::Imm)
                {
                    if (count.imm < 0 || count.imm > 255)
                        return fail("immediate does not fit instruction encoding");
                    mi.num_ops = 3;
                    mi.ops[0] = reg_op(operands[0]);
                    mi.ops[1] = reg_op(operands[0]);
                    mi.ops[2] = imm_op(count);
                    return mi;
                }
                return fail(std::format("the shift count for '{}' must be 'cl' or an immediate", mnemonic));
            }
            if (mnemonic == "imul")
            {
                if (operands.size() != 2 && operands.size() != 3)
                    return fail("'imul' expects 2 or 3 operands");
                auto const& dst = operands[0];
                if (dst.kind != Kind::Reg)
                    return fail("the destination of 'imul' must be a register");
                unsigned w = width != 0 ? width : dst.width;
                if (w != 32 && w != 64)
                    return fail("'imul' supports 32- and 64-bit operands in native inline assembly");
                if (dst.width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", dst.width, w));
                bool is64 = w == 64;
                em64t::MInstr mi;
                mi.num_defs = 1;
                if (operands.size() == 3)
                {
                    auto const& src = operands[1];
                    auto const& imm = operands[2];
                    if (src.kind != Kind::Reg || src.width != w || imm.kind != Kind::Imm)
                        return fail("'imul' with 3 operands expects a register and an immediate");
                    if (!fits_i(imm.imm, 32))
                        return fail("immediate does not fit instruction encoding; move it into a register first");
                    mi.num_ops = 3;
                    mi.ops[0] = reg_op(dst);
                    mi.ops[1] = reg_op(src);
                    mi.ops[2] = imm_op(imm);
                    mi.opc = is64 ? MOpc::IMUL64rri32 : MOpc::IMUL32rri;
                    return mi;
                }
                auto const& src = operands[1];
                if (src.kind == Kind::Reg)
                {
                    if (src.width != w)
                        return fail(std::format("cannot use {}-bit register as a {}-bit operand", src.width, w));
                    mi.num_ops = 3;
                    mi.ops[0] = reg_op(dst);
                    mi.ops[1] = reg_op(dst);
                    mi.ops[2] = reg_op(src);
                    mi.opc = is64 ? MOpc::IMUL64rr : MOpc::IMUL32rr;
                    return mi;
                }
                if (src.kind == Kind::Mem)
                {
                    if (src.width != 0 && src.width != w)
                        return fail(std::format("memory operand width ({} bits) does not match the {}-bit 'imul'", src.width, w));
                    if (!is64)
                        return fail("'imul' with a memory operand supports only 64-bit operands in native inline assembly");
                    mi.num_ops = 3;
                    mi.ops[0] = reg_op(dst);
                    mi.ops[1] = reg_op(dst);
                    mi.ops[2] = mem_op(src);
                    mi.opc = MOpc::IMUL64rm;
                    return mi;
                }
                return fail("'imul' expects a register or memory source");
            }
            if (mnemonic == "inc" || mnemonic == "dec" || mnemonic == "neg" || mnemonic == "not")
            {
                if (operands.size() != 1 || operands[0].kind != Kind::Reg)
                    return fail(std::format("'{}' expects 1 register operand", mnemonic));
                unsigned w = width != 0 ? width : operands[0].width;
                if ((mnemonic == "inc" || mnemonic == "dec"))
                {
                    if (w != 64 || operands[0].width != 64)
                        return fail(std::format("'{}' supports 64-bit registers in native inline assembly; use 'add'/'sub' 1 for other widths", mnemonic));
                }
                else if (w != 32 && w != 64)
                    return fail(std::format("'{}' supports 32- and 64-bit operands in native inline assembly", mnemonic));
                if (operands[0].width != w)
                    return fail(std::format("cannot use {}-bit register as a {}-bit operand", operands[0].width, w));
                em64t::MInstr mi;
                mi.num_ops = 1;
                mi.num_defs = 1;
                mi.ops[0] = reg_op(operands[0]);
                bool is64 = w == 64;
                if (mnemonic == "inc")
                    mi.opc = MOpc::INC64r;
                else if (mnemonic == "dec")
                    mi.opc = MOpc::DEC64r;
                else if (mnemonic == "neg")
                    mi.opc = is64 ? MOpc::NEG64r : MOpc::NEG32r;
                else
                    mi.opc = is64 ? MOpc::NOT64r : MOpc::NOT32r;
                return mi;
            }
            if (mnemonic == "push" || mnemonic == "pop")
            {
                if (mnemonic == "pop")
                {
                    if (operands.size() != 1 || operands[0].kind != Kind::Reg)
                        return fail("'pop' expects 1 register operand");
                    unsigned w = width != 0 ? width : operands[0].width;
                    if (w != 64 || operands[0].width != 64)
                        return fail("'pop' supports 64-bit registers in native inline assembly");
                    em64t::MInstr mi;
                    mi.opc = MOpc::POP64r;
                    mi.num_ops = 1;
                    mi.num_defs = 1;
                    mi.ops[0] = reg_op(operands[0]);
                    mi.implicit_uses |= (1ULL << static_cast<std::uint8_t>(em64t::PhysReg::RSP));
                    mi.implicit_defs |= (1ULL << static_cast<std::uint8_t>(em64t::PhysReg::RSP));
                    return mi;
                }
                if (operands.size() != 1)
                    return fail("'push' expects 1 operand");
                auto const& src = operands[0];
                em64t::MInstr mi;
                mi.num_ops = 1;
                mi.num_defs = 0;
                mi.implicit_uses |= (1ULL << static_cast<std::uint8_t>(em64t::PhysReg::RSP));
                mi.implicit_defs |= (1ULL << static_cast<std::uint8_t>(em64t::PhysReg::RSP));
                if (src.kind == Kind::Reg)
                {
                    unsigned w = width != 0 ? width : src.width;
                    if (w != 64 || src.width != 64)
                        return fail("'push' supports 64-bit registers in native inline assembly");
                    mi.opc = MOpc::PUSH64r;
                    mi.ops[0] = reg_op(src);
                    return mi;
                }
                if (src.kind == Kind::Imm)
                {
                    if (!fits_i(src.imm, 32))
                        return fail("immediate does not fit instruction encoding; move it into a register first");
                    mi.opc = MOpc::PUSH64i;
                    mi.ops[0] = imm_op(src);
                    return mi;
                }
                if (src.kind == Kind::Mem)
                {
                    mi.opc = MOpc::PUSH64m;
                    mi.ops[0] = mem_op(src);
                    return mi;
                }
                return fail("'push' expects a register, immediate, or memory operand");
            }
            if (mnemonic == "movq" || mnemonic == "movsd" || mnemonic == "movss")
                return fail(std::format("'{}' expects XMM operands; use 64-bit general registers with 'movq', or XMM-to-XMM 'movsd'/'movss'", mnemonic));
            static constexpr std::string_view unsupported[] = {"call",  "callq", "jmp",  "jmpq",  "ret",  "int",   "int3",  "syscall", "sysenter", "rdtsc",
                                                               "rdpmc", "cpuid", "hlt",  "cli",   "sti",  "loop",  "cbw",   "cwde",    "cdq",      "cqo",
                                                               "xlat",  "lahf",  "sahf", "pushf", "popf", "leave", "enter", "out",     "in"};
            for (auto name : unsupported)
                if (mnemonic == name)
                    return fail(std::format("'{}' is not supported in native inline assembly", mnemonic));
            return fail(std::format("unknown inline assembly instruction '{}'", mnemonic));
        }

        [[nodiscard]] std::optional<std::vector<em64t::MInstr>> parse_native_line(std::string_view line, bool att, std::string& error)
        {
            std::vector<em64t::MInstr> instructions;
            std::size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string_view::npos)
                return instructions;
            if (line[first] == '.')
            {
                error = "assembler directives are not allowed in inline assembly";
                return std::nullopt;
            }
            auto tokens = tokenize_asm_line(line, error);
            if (!error.empty())
                return std::nullopt;
            AsmParser parser{std::move(tokens), 0, att, {}};
            if (parser.at_end())
                return instructions;
            auto raw_mnemonic = parser.expect_ident("an instruction mnemonic");
            if (!raw_mnemonic)
            {
                error = parser.error;
                return std::nullopt;
            }
            auto lowered_mnemonic = to_lower_asm(*raw_mnemonic);
            static constexpr std::string_view prefixes[] = {"lock",     "rep",      "repe",   "repne",  "repz", "repnz",
                                                            "xacquire", "xrelease", "data16", "addr32", "rex64"};
            for (auto prefix : prefixes)
                if (lowered_mnemonic == prefix)
                {
                    error = std::format("the '{}' prefix is not supported in native inline assembly", *raw_mnemonic);
                    return std::nullopt;
                }
            auto resolved = resolve_mnemonic(*raw_mnemonic, att, error);
            if (!resolved)
                return std::nullopt;
            std::vector<ParsedOperand> operands;
            if (!parser.at_end())
            {
                while (true)
                {
                    auto operand = parser.parse_operand();
                    if (!operand)
                    {
                        error = parser.error;
                        return std::nullopt;
                    }
                    operands.push_back(*operand);
                    if (!parser.accept(AsmToken::Kind::Comma))
                        break;
                    if (parser.at_end())
                    {
                        error = "expected an operand after ',' in inline assembly";
                        return std::nullopt;
                    }
                }
            }
            if (!parser.at_end())
            {
                error = "unexpected trailing tokens in inline assembly";
                return std::nullopt;
            }

            if (att && operands.size() >= 2)
                std::ranges::reverse(operands);
            unsigned width = resolved->width;
            unsigned src_width = 0;
            if (width >= 256)
            {
                src_width = width % 256;
                width /= 256;
            }
            auto instruction = build_native_instruction(resolved->canonical, width, operands, error);
            if (!instruction)
                return std::nullopt;

            if (src_width != 0 && operands.size() == 2)
            {
                auto const& src = operands[1];
                unsigned actual = src.kind == ParsedOperand::Kind::Reg ? src.width : src.width;
                if (actual != 0 && actual != src_width)
                {
                    error = std::format("'{}' source width ({} bits) does not match the mnemonic suffix ({} bits)", resolved->canonical, actual, src_width);
                    return std::nullopt;
                }
            }

            for (auto const& op : operands)
            {
                auto touch = [&](em64t::PhysReg reg) {
                    if (reg != em64t::PhysReg::None)
                    {
                        instruction->implicit_uses |= (1ULL << static_cast<std::uint8_t>(reg));
                        instruction->implicit_defs |= (1ULL << static_cast<std::uint8_t>(reg));
                    }
                };

                if (op.kind == ParsedOperand::Kind::Reg)
                    touch(op.reg);
                else if (op.kind == ParsedOperand::Kind::Mem)
                {
                    if (op.mem.base.is_physical())
                        touch(op.mem.base.phys_reg());
                    if (op.mem.index.is_valid() && op.mem.index.is_physical())
                        touch(op.mem.index.phys_reg());
                }
            }
            instructions.push_back(*instruction);
            return instructions;
        }

    } // namespace

    em64t::PhysReg inline_asm_family_phys(std::string_view family) noexcept
    {
        return family_to_phys(family);
    }

    InlineAsmPlan prepare_inline_asm(IrInlineAsmInst const& assembly, target::TargetConfig const& target)
    {
        InlineAsmPlan plan;
        for (auto const& op : assembly.operands)
        {
            if (op.placement_kind == IrAsmOperand::PlacementKind::Sym)
            {
                plan.error = "symbolic operands are not supported on the native backend";
                return plan;
            }
            if (op.placement_kind == IrAsmOperand::PlacementKind::Flag)
            {
                plan.error = "flag outputs are not supported on the native backend";
                return plan;
            }
        }
        for (auto const& part : assembly.template_parts)
        {
            if (part.modifier != 0)
            {
                plan.error = "operand modifiers are not supported on the native backend";
                return plan;
            }
            if (part.operand == 0xFFFFFFFEU)
            {
                plan.error = "unique stamps are not supported on the native backend";
                return plan;
            }
        }
        auto resolved = select_registers(assembly, target);
        if (!resolved.error.empty())
        {
            plan.error = resolved.error;
            return plan;
        }

        auto lines = substitute_template(assembly, resolved, plan.error);
        if (!lines)
            return plan;

        bool att = assembly.dialect != IrAsmDialect::Intel;
        for (auto const& [text, number] : *lines)
        {
            auto parsed = parse_native_line(text, att, plan.error);
            if (!parsed)
            {
                plan.error = std::format("line {}: {}", number, plan.error);
                return plan;
            }
            for (auto& instruction : *parsed)
                plan.instructions.push_back(instruction);
        }

        if (plan.instructions.empty())
        {
            em64t::MInstr nop;
            nop.opc = em64t::MOpc::NOP;
            plan.instructions.push_back(nop);
        }

        plan.registers = std::move(resolved.registers);
        plan.clobbers = std::move(resolved.clobbers);
        plan.literal_registers = std::move(resolved.literal_registers);
        plan.has_memory_operands = resolved.has_memory_operands;
        return plan;
    }

    InlineAsmLlvm prepare_llvm_asm(IrInlineAsmInst const& assembly)
    {
        InlineAsmLlvm result;
        auto fail = [&](std::string message) {
            result.error = std::move(message);
            return result;
        };

        std::vector<std::int64_t> out_number(assembly.operands.size(), -1);
        std::vector<std::int64_t> in_position(assembly.operands.size(), -1);
        std::vector<std::string> output_constraints;
        std::vector<std::string> input_constraints;
        auto reg_class_for_type = [](IrType const* type) -> char { return (type && type->kind == IrTypeKind::Float) ? 'x' : 'r'; };
        auto flag_code_for = [](std::string_view cond) -> std::string_view {
            if (cond == "zero" || cond == "equal")
                return "z";
            if (cond == "not_zero" || cond == "not_equal")
                return "nz";
            if (cond == "carry" || cond == "below")
                return "c";
            if (cond == "not_carry" || cond == "above_equal")
                return "nc";
            if (cond == "above")
                return "a";
            if (cond == "below_equal")
                return "be";
            if (cond == "sign")
                return "s";
            if (cond == "not_sign")
                return "ns";
            if (cond == "overflow")
                return "o";
            if (cond == "not_overflow")
                return "no";
            if (cond == "parity_even")
                return "p";
            if (cond == "parity_odd")
                return "np";
            if (cond == "less")
                return "l";
            if (cond == "less_equal")
                return "le";
            if (cond == "greater")
                return "g";
            if (cond == "greater_equal")
                return "ge";
            return {};
        };
        for (std::size_t i = 0; i < assembly.operands.size(); ++i)
        {
            auto const& op = assembly.operands[i];
            if (!op.type)
                return fail("malformed inline assembly IR operand");

            if (op.placement_kind == IrAsmOperand::PlacementKind::RegPair)
                return fail("register pairs must be split during lowering before backend emission");

            bool is_output = op.direction != IrAsmOperand::Direction::In;
            bool is_input = op.direction != IrAsmOperand::Direction::Out;
            bool tied = op.direction == IrAsmOperand::Direction::InOut && op.placement_kind == IrAsmOperand::PlacementKind::Reg;
            std::string out_constraint;
            if (is_output)
            {
                out_number[i] = static_cast<std::int64_t>(output_constraints.size());
                switch (op.placement_kind)
                {
                    case IrAsmOperand::PlacementKind::Reg:
                        out_constraint = op.direction == IrAsmOperand::Direction::InOut ? "=" : "=&";
                        if (!op.reg_name.empty())
                            out_constraint += "{" + std::string(op.reg_name) + "}";
                        else
                            out_constraint += reg_class_for_type(op.value ? op.value->type : op.type);
                        break;
                    case IrAsmOperand::PlacementKind::Mem:
                        out_constraint = "=*m";
                        result.output_address_indices.push_back(static_cast<std::uint32_t>(i));
                        break;
                    case IrAsmOperand::PlacementKind::Imm:
                        return fail("immediate operands must be inputs");
                    case IrAsmOperand::PlacementKind::Sym:
                        return fail("symbolic operands must be inputs");
                    case IrAsmOperand::PlacementKind::Flag: {
                        auto code = flag_code_for(op.flag_cond);
                        if (code.empty())
                            return fail("unknown asm flag condition");
                        out_constraint = "=@cc" + std::string(code);
                        break;
                    }
                    case IrAsmOperand::PlacementKind::RegPair:
                        return fail("register pairs must be split during lowering before backend emission");
                }
                output_constraints.push_back(out_constraint);
            }
            if (is_input)
            {
                in_position[i] = static_cast<std::int64_t>(result.input_operand_indices.size());
                result.input_operand_indices.push_back(static_cast<std::uint32_t>(i));
                std::string in_constraint;
                switch (op.placement_kind)
                {
                    case IrAsmOperand::PlacementKind::Reg:
                        if (tied)
                            in_constraint = std::to_string(out_number[i]);
                        else if (!op.reg_name.empty())
                            in_constraint = "{" + std::string(op.reg_name) + "}";
                        else
                            in_constraint += reg_class_for_type(op.value ? op.value->type : op.type);
                        break;
                    case IrAsmOperand::PlacementKind::Mem:
                        in_constraint = "*m";
                        break;
                    case IrAsmOperand::PlacementKind::Imm: {
                        if ((!ir_cast<IrIntConstant>(op.value) && !ir_cast<IrBoolConstant>(op.value)) || !op.value)
                            return fail("inline assembly immediate is not an integer constant");
                        in_constraint = "i";
                        break;
                    }
                    case IrAsmOperand::PlacementKind::Sym: {
                        if (!op.value || op.value->kind != IrNodeKind::GlobalRef)
                            return fail("inline assembly symbolic operand has no symbol");
                        in_constraint = "s";
                        break;
                    }
                    case IrAsmOperand::PlacementKind::Flag:
                        return fail("flag operands must be outputs");
                    case IrAsmOperand::PlacementKind::RegPair:
                        return fail("register pairs must be split during lowering before backend emission");
                }
                input_constraints.push_back(in_constraint);
            }
        }

        std::int64_t output_count = static_cast<std::int64_t>(output_constraints.size());
        std::string rewritten;
        std::size_t cursor = 0;
        auto append_gap = [&](std::string_view gap) {
            for (std::size_t k = 0; k < gap.size(); ++k)
            {
                if (gap[k] == '$' && k + 1 < gap.size() &&
                    (std::isdigit(static_cast<unsigned char>(gap[k + 1])) ||
                     (gap[k + 1] == '-' && k + 2 < gap.size() && std::isdigit(static_cast<unsigned char>(gap[k + 2])))))
                    continue;
                rewritten += gap[k];
            }
        };
        auto parts = assembly.template_parts;
        std::ranges::sort(parts, {}, &IrAsmTemplatePart::offset);
        for (auto const& part : parts)
        {
            if (part.offset < cursor || part.offset + part.length > assembly.template_str.size())
                return fail("malformed inline assembly IR template");

            append_gap(std::string_view(assembly.template_str).substr(cursor, part.offset - cursor));
            if (part.operand == 0xFFFFFFFFU)
            {
                if (assembly.dialect == IrAsmDialect::Intel)
                {
                    auto start = part.offset + part.length;
                    auto end = start;
                    while (end < assembly.template_str.size() && std::isalnum(static_cast<unsigned char>(assembly.template_str[end])))
                        ++end;
                    auto name = std::string_view(assembly.template_str).substr(start, end - start);
                    if (!target::lookup_register(target::Arch::X86_64, name) && !target::lookup_register(target::Arch::X86, name))
                        rewritten += '%';
                }
                else
                    rewritten += '%';
            }
            else
            {
                if (part.operand == 0xFFFFFFFEU)
                {
                    rewritten += "${:uid}";
                }
                else
                {
                if (part.operand >= assembly.operands.size())
                    return fail("inline assembly IR operand reference out of range");

                std::int64_t number = -1;
                if (out_number[part.operand] >= 0)
                    number = out_number[part.operand];
                else if (in_position[part.operand] >= 0)
                    number = output_count + in_position[part.operand];
                else
                    return fail("inline assembly operand is neither an input nor an output");
                if (part.modifier == 'c')
                    rewritten += "${" + std::to_string(number) + ":c}";
                else if (part.modifier == 'P')
                    rewritten += "${" + std::to_string(number) + ":P}";
                else if (assembly.operands[part.operand].placement_kind == IrAsmOperand::PlacementKind::Sym)
                    rewritten += "${" + std::to_string(number) + ":c}";
                else
                    rewritten += "$" + std::to_string(number);
                }
            }
            cursor = part.offset + part.length;
        }
        append_gap(std::string_view(assembly.template_str).substr(cursor));
        result.template_str = std::move(rewritten);
        for (auto const& entry : output_constraints)
        {
            if (!result.constraints.empty())
                result.constraints += ",";
            result.constraints += entry;
        }

        for (auto const& entry : input_constraints)
        {
            if (!result.constraints.empty())
                result.constraints += ",";
            result.constraints += entry;
        }

        for (auto clobber : assembly.clobbers)
            result.clobbers.emplace_back(clobber);
        return result;
    }

} // namespace dcc::backend
