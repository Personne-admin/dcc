export module dcc.ir.mangle;

import std;
import dcc.types;
import dcc.comptime;
import dcc.ast;

export namespace dcc::ir::mangle
{
    struct NominalInfo
    {
        std::vector<std::string_view> module_path;
        std::string_view name;
    };

    struct TemplateArg
    {
        enum class Kind : std::uint8_t
        {
            Type,
            Value
        };

        Kind kind;
        dcc::types::TypePtr type{nullptr};
        dcc::comptime::Value const* value{nullptr};
    };

    using NominalResolver = std::function<std::optional<NominalInfo>(void const*)>;

    struct DemangledType
    {
        enum class Tag : std::uint8_t
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
            Struct,
            Union,
            Enum,
            Nominal,
            TemplateParam,
            Range,
            RangeInclusive,
            Error,
            Lambda,
        };

        Tag tag{};
        std::vector<std::string> module_path;
        std::string name;
        std::vector<DemangledType> template_args;
        std::uint8_t bits{8};
        bool is_signed{true};
        bool is_pointer_sized{false};
        std::string quals;
        std::shared_ptr<DemangledType> pointee;
        std::shared_ptr<DemangledType> element;
        std::uint64_t count{};
        std::shared_ptr<DemangledType> return_type_fp;
        std::uint32_t param_index{};

        DemangledType() = default;
    };

    struct DemangledValue
    {
        enum class Tag : std::uint8_t
        {
            Null,
            Int,
            Float,
            Bool,
            Char,
            String,
            Aggregate,
            Slice,
            Pointer
        };

        Tag tag{};
        DemangledType type;
        std::int64_t int_val{};
        double float_val{};
        bool bool_val{};
        std::uint32_t char_val{};
        std::string string_val;
        std::vector<DemangledValue> elements;
        bool is_null_ptr{true};
        std::size_t pointer_index{};

        DemangledValue() = default;
        DemangledValue(DemangledValue&&) = default;
    };

    struct DemangledTemplateArg
    {
        enum class Kind : std::uint8_t
        {
            Type,
            Value
        };

        Kind kind;
        DemangledType type;
        DemangledValue value;

        DemangledTemplateArg() = default;
        DemangledTemplateArg(DemangledTemplateArg&&) = default;
    };

    struct DemangledName
    {
        enum class Kind : std::uint8_t
        {
            Function,
            Global,
            Specialization,
            Type,
            Value,
            TypeSpec,
            Lambda
        };

        Kind kind{};
        std::vector<std::string> module_path;
        std::string name;
        std::vector<DemangledType> param_types;
        DemangledType return_type;
        std::string calling_conv;
        DemangledType global_type;
        std::vector<DemangledTemplateArg> template_args;
        DemangledType type_only;
        DemangledValue value_only;

        DemangledName() = default;
        DemangledName(DemangledName&&) = default;
    };

    std::string mangle_type(dcc::types::TypePtr type, NominalResolver resolver = {});
    std::string mangle_value(dcc::comptime::Value const& value, NominalResolver resolver = {});
    std::string mangle_function(std::span<std::string_view const> module_path, dcc::ast::FuncDecl const& decl, std::span<dcc::types::TypePtr const> param_types,
                                dcc::types::TypePtr return_type, std::span<TemplateArg const> template_args = {}, NominalResolver resolver = {});

    std::string mangle_lambda_function(std::span<std::string_view const> module_path, dcc::ast::FuncDecl const& decl,
                                       std::span<dcc::types::TypePtr const> param_types, dcc::types::TypePtr return_type,
                                       NominalResolver resolver = {});

    std::string mangle_global(std::span<std::string_view const> module_path, dcc::ast::VarDecl const& decl, dcc::types::TypePtr type,
                              std::span<TemplateArg const> template_args = {}, NominalResolver resolver = {});

    std::string mangle_specialization(std::span<std::string_view const> module_path, std::string_view name, std::span<dcc::types::TypePtr const> param_types,
                                      dcc::types::TypePtr return_type, std::span<TemplateArg const> template_args = {}, NominalResolver resolver = {});

    std::string mangle_type_specialization(std::span<std::string_view const> module_path, std::string_view type_name,
                                           std::span<TemplateArg const> template_args = {}, NominalResolver resolver = {});

    bool demangle(DemangledName& result, std::string_view s);

    std::optional<DemangledName> demangle(std::string_view s);

} // namespace dcc::ir::mangle

namespace dcc::ir::mangle
{
    namespace
    {
        std::string to_dec(std::uint64_t n)
        {
            if (n == 0)
                return "0";

            std::string s;
            while (n > 0)
            {
                s.push_back(static_cast<char>('0' + (n % 10)));
                n /= 10;
            }

            std::ranges::reverse(s);
            return s;
        }

        bool parse_dec(std::string_view sv, std::size_t& pos, std::uint64_t& val)
        {
            if (pos >= sv.size())
                return false;

            val = 0;
            std::size_t start = pos;
            while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9')
            {
                std::uint64_t d = static_cast<std::uint64_t>(sv[pos] - '0');
                if (val > (std::numeric_limits<std::uint64_t>::max() - d) / 10)
                    return false;

                val = val * 10 + d;
                ++pos;
            }

            return (pos > start);
        }

        bool is_safe_char(char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        }

        std::string escape(std::string_view s)
        {
            std::string out;
            out.reserve(s.size() * 2);
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                unsigned char c = static_cast<unsigned char>(s[i]);
                if (c == '$')
                    out += "$$";
                else if (is_safe_char(static_cast<char>(c)))
                    out.push_back(static_cast<char>(c));
                else
                {
                    out.push_back('$');
                    constexpr const char* hex = "0123456789ABCDEF";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                }
            }

            return out;
        }

        bool unescape(std::string_view sv, std::size_t& pos, std::size_t len, std::string& out)
        {
            std::size_t end = pos + len;
            if (end > sv.size())
                return false;

            out.clear();
            out.reserve(len);
            while (pos < end)
            {
                unsigned char c = static_cast<unsigned char>(sv[pos]);
                if (c == '$')
                {
                    if (pos + 1 >= end)
                        return false;

                    unsigned char nx = static_cast<unsigned char>(sv[pos + 1]);
                    if (nx == '$')
                    {
                        out.push_back('$');
                        pos += 2;
                    }
                    else
                    {
                        if (pos + 2 >= end)
                            return false;

                        auto hexv = [](unsigned char h) -> std::optional<unsigned char> {
                            if (h >= '0' && h <= '9')
                                return h - '0';
                            if (h >= 'A' && h <= 'F')
                                return h - 'A' + 10;
                            return std::nullopt;
                        };

                        auto hi = hexv(nx);
                        auto lo = hexv(static_cast<unsigned char>(sv[pos + 2]));
                        if (!hi || !lo)
                            return false;

                        out.push_back(static_cast<char>((*hi << 4) | *lo));
                        pos += 3;
                    }
                }
                else
                {
                    out.push_back(static_cast<char>(c));
                    ++pos;
                }
            }
            return true;
        }

        bool parse_ident(std::string_view sv, std::size_t& pos, std::string& out)
        {
            std::uint64_t len;
            if (!parse_dec(sv, pos, len))
                return false;

            if (pos >= sv.size() || sv[pos] != '.')
                return false;

            ++pos;
            return unescape(sv, pos, static_cast<std::size_t>(len), out);
        }

        bool parse_path(std::string_view sv, std::size_t& pos, std::vector<std::string>& out)
        {
            std::uint64_t count;
            if (!parse_dec(sv, pos, count))
                return false;

            if (pos >= sv.size() || sv[pos] != '.')
                return false;

            ++pos;
            out.clear();
            out.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t i = 0; i < count; ++i)
            {
                std::string seg;
                if (!parse_ident(sv, pos, seg))
                    return false;

                out.push_back(std::move(seg));
            }
            return true;
        }

        bool parse_quals(std::string_view sv, std::size_t& pos, std::string& q)
        {
            q.clear();
            while (pos < sv.size())
            {
                char c = sv[pos];
                if (c == 'C' || c == 'V' || c == 'R')
                {
                    q.push_back(c);
                    ++pos;
                }
                else if (c == 'q')
                {
                    ++pos;
                    return true;
                }
                else
                    return false;
            }
            return false;
        }

        bool parse_sint(std::string_view sv, std::size_t& pos, std::int64_t& val)
        {
            bool neg = false;
            if (pos < sv.size() && sv[pos] == 'm')
            {
                neg = true;
                ++pos;
            }
            std::uint64_t uv;
            if (!parse_dec(sv, pos, uv))
                return false;

            val = static_cast<std::int64_t>(uv);
            if (neg)
                val = -val;

            return true;
        }

        std::string encode_cc(std::string_view cc)
        {
            if (cc.empty())
                return {};

            auto eq = [](std::string_view a, std::string_view b) {
                return std::ranges::equal(
                    a, b, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
            };

            if (eq(cc, "cdecl") || cc.empty())
                return {};
            if (eq(cc, "stdcall"))
                return "c1";
            if (eq(cc, "fastcall"))
                return "c2";
            if (eq(cc, "vectorcall"))
                return "c3";
            if (eq(cc, "systemv") || eq(cc, "sysv"))
                return "c4";
            if (eq(cc, "win64"))
                return "c5";

            return {};
        }

        std::string cc_name(int n)
        {
            switch (n)
            {
                case 1:
                    return "stdcall";
                case 2:
                    return "fastcall";
                case 3:
                    return "vectorcall";
                case 4:
                    return "systemv";
                case 5:
                    return "win64";
                default:
                    return "";
            }
        }

        void encode_type(std::string& out, dcc::types::TypePtr type, NominalResolver const& resolver);
        void encode_value(std::string& out, dcc::comptime::Value const& value, NominalResolver const& resolver);
        void encode_seg(std::string& out, std::string_view name);
        void encode_path(std::string& out, std::span<std::string_view const> path, NominalResolver const& resolver);
        void encode_quals(std::string& out, dcc::types::Qual q);
        bool demangle_type_into(DemangledType& dt, std::string_view sv, std::size_t& pos);
        bool demangle_value_into(DemangledValue& dv, std::string_view sv, std::size_t& pos);
        bool demangle_template_arg_into(DemangledTemplateArg& dta, std::string_view sv, std::size_t& pos);

        void encode_seg(std::string& out, std::string_view name)
        {
            auto esc = escape(name);
            out += to_dec(esc.size());
            out += '.';
            out += esc;
        }

        void encode_path(std::string& out, std::span<std::string_view const> path, NominalResolver const&)
        {
            out += to_dec(path.size());
            out += '.';
            for (auto& seg : path)
                encode_seg(out, seg);
        }

        void encode_quals(std::string& out, dcc::types::Qual q)
        {
            if (dcc::types::has_qual(q, dcc::types::Qual::Const))
                out += 'C';
            if (dcc::types::has_qual(q, dcc::types::Qual::Volatile))
                out += 'V';
            if (dcc::types::has_qual(q, dcc::types::Qual::Restrict))
                out += 'R';

            out += 'q';
        }

        void encode_type(std::string& out, dcc::types::TypePtr type, NominalResolver const& resolver)
        {
            if (!type)
            {
                out += 'E';
                return;
            }
            switch (type->kind)
            {
                case dcc::types::TypeKind::Void:
                    out += 'v';
                    return;
                case dcc::types::TypeKind::Bool:
                    out += 'b';
                    return;
                case dcc::types::TypeKind::Int: {
                    auto* it = static_cast<dcc::types::IntType const*>(type);
                    if (it->is_pointer_sized)
                    {
                        out += 'I';
                        out += (it->is_signed ? 's' : 'u');
                    }
                    else
                    {
                        out += 'i';
                        out += to_dec(it->bits);
                        out += (it->is_signed ? 's' : 'u');
                    }
                    return;
                }
                case dcc::types::TypeKind::Float: {
                    auto* ft = static_cast<dcc::types::FloatType const*>(type);
                    out += 'f';
                    out += to_dec(ft->bits);
                    out += '.';
                    return;
                }
                case dcc::types::TypeKind::Char:
                    out += 'c';
                    return;
                case dcc::types::TypeKind::NullT:
                    out += 'n';
                    return;
                case dcc::types::TypeKind::Lambda: {
                    auto* lt = static_cast<dcc::types::LambdaType const*>(type);
                    out += 'L';
                    if (lt->expr)
                    {
                        auto* lexpr = static_cast<dcc::ast::LambdaExpr const*>(lt->expr);
                        if (lexpr->range.valid())
                        {
                            encode_seg(out, std::to_string(static_cast<std::uint32_t>(lexpr->range.begin.fileId)));
                            encode_seg(out, std::to_string(lexpr->range.begin.offset));
                        }
                    }
                    return;
                }
                case dcc::types::TypeKind::Pointer: {
                    auto* pt = static_cast<dcc::types::PointerType const*>(type);
                    out += 'P';
                    encode_quals(out, pt->pointee_quals);
                    encode_type(out, pt->pointee, resolver);
                    return;
                }
                case dcc::types::TypeKind::Array: {
                    auto* at = static_cast<dcc::types::ArrayType const*>(type);
                    out += 'A';
                    out += to_dec(at->count);
                    encode_type(out, at->element, resolver);
                    return;
                }
                case dcc::types::TypeKind::RuntimeArray: {
                    auto* rt = static_cast<dcc::types::RuntimeArrayType const*>(type);
                    out += 'a';
                    encode_type(out, rt->element, resolver);
                    return;
                }
                case dcc::types::TypeKind::Slice: {
                    auto* st = static_cast<dcc::types::SliceType const*>(type);
                    out += 'S';
                    encode_quals(out, st->element_quals);
                    encode_type(out, st->element, resolver);
                    return;
                }
                case dcc::types::TypeKind::Fam: {
                    auto* ft = static_cast<dcc::types::FamType const*>(type);
                    out += 'F';
                    encode_type(out, ft->element, resolver);
                    return;
                }
                case dcc::types::TypeKind::FuncPtr: {
                    auto* fpt = static_cast<dcc::types::FuncPtrType const*>(type);
                    out += 'p';
                    encode_type(out, fpt->return_type, resolver);
                    out += to_dec(fpt->params.size());
                    for (auto* p : fpt->params)
                        encode_type(out, p, resolver);

                    return;
                }
                case dcc::types::TypeKind::Struct:
                case dcc::types::TypeKind::Union:
                case dcc::types::TypeKind::Enum: {
                    auto* ut = static_cast<dcc::types::UserType const*>(type);
                    out += 'D';
                    std::vector<std::string_view> mp;
                    std::string_view dn;
                    if (resolver)
                    {
                        auto info = resolver(static_cast<void const*>(ut->decl));
                        if (info)
                        {
                            mp = info->module_path;
                            dn = info->name;
                        }
                    }

                    encode_path(out, mp, resolver);
                    encode_seg(out, dn);
                    out += to_dec(ut->template_args.size());
                    out += '.';
                    for (auto* ta : ut->template_args)
                        encode_type(out, ta, resolver);

                    return;
                }
                case dcc::types::TypeKind::TemplateParam: {
                    auto* tpt = static_cast<dcc::types::TemplateParamType const*>(type);
                    out += 'Z';
                    encode_seg(out, tpt->name);
                    out += to_dec(tpt->index);
                    return;
                }
                case dcc::types::TypeKind::Range: {
                    auto* rt = static_cast<dcc::types::RangeType const*>(type);
                    out += 'r';
                    encode_type(out, rt->element, resolver);
                    return;
                }
                case dcc::types::TypeKind::RangeInclusive: {
                    auto* rit = static_cast<dcc::types::RangeInclusiveType const*>(type);
                    out += 'R';
                    encode_type(out, rit->element, resolver);
                    return;
                }
                case dcc::types::TypeKind::Nominal: {
                    auto* nt = static_cast<dcc::types::NominalType const*>(type);
                    out += 'N';
                    encode_type(out, nt->underlying, resolver);
                    std::vector<std::string_view> mp;
                    std::string_view dn;
                    if (resolver)
                    {
                        auto info = resolver(const_cast<void*>(nt->decl));
                        if (info)
                        {
                            mp = info->module_path;
                            dn = info->name;
                        }
                    }
                    encode_path(out, mp, resolver);
                    encode_seg(out, dn);
                    return;
                }
                case dcc::types::TypeKind::TypePack:
                case dcc::types::TypeKind::Error:
                    out += 'E';
                    return;
            }
        }

        void encode_value(std::string& out, dcc::comptime::Value const& value, NominalResolver const& resolver)
        {
            if (!value.type)
            {
                out += 'N';
                out += 'E';
                return;
            }

            switch (value.kind())
            {
                case dcc::comptime::Value::Kind::Null:
                    out += 'N';
                    encode_type(out, value.type, resolver);
                    return;
                case dcc::comptime::Value::Kind::Int: {
                    auto v = value.get_int();
                    out += 'I';
                    encode_type(out, value.type, resolver);
                    if (v < 0)
                    {
                        out += 'm';
                        out += to_dec(static_cast<std::uint64_t>(-(v + 1)) + 1);
                    }
                    else
                        out += to_dec(static_cast<std::uint64_t>(v));
                    return;
                }
                case dcc::comptime::Value::Kind::Float: {
                    double d = value.get_float();
                    out += 'F';
                    encode_type(out, value.type, resolver);
                    if (auto* ft = dcc::types::type_cast<dcc::types::FloatType>(value.type))
                    {
                        if (ft->bits == 32)
                            out += std::format("{:08X}", std::bit_cast<std::uint32_t>(static_cast<float>(d)));
                        else
                            out += std::format("{:016X}", std::bit_cast<std::uint64_t>(d));
                    }
                    else
                        out += std::format("{:016X}", std::bit_cast<std::uint64_t>(d));
                    return;
                }
                case dcc::comptime::Value::Kind::Bool:
                    out += 'B';
                    encode_type(out, value.type, resolver);
                    out += (value.get_bool() ? '1' : '0');
                    return;
                case dcc::comptime::Value::Kind::Char:
                    out += 'C';
                    encode_type(out, value.type, resolver);
                    out += to_dec(value.get_char());
                    return;
                case dcc::comptime::Value::Kind::String:
                    out += 'S';
                    encode_type(out, value.type, resolver);
                    encode_seg(out, value.get_string());
                    return;
                case dcc::comptime::Value::Kind::Aggregate:
                    out += 'A';
                    encode_type(out, value.type, resolver);
                    out += to_dec(value.size());
                    for (std::size_t i = 0; i < value.size(); ++i)
                        encode_value(out, value.at(i), resolver);

                    return;
                case dcc::comptime::Value::Kind::Slice:
                    out += 's';
                    encode_type(out, value.type, resolver);
                    out += to_dec(value.size());
                    for (std::size_t i = 0; i < value.size(); ++i)
                        encode_value(out, value.at(i), resolver);

                    return;
                case dcc::comptime::Value::Kind::Pointer:
                    out += 'P';
                    encode_type(out, value.type, resolver);
                    if (value.is_null_ptr())
                        out += '0';
                    else
                    {
                        out += '1';
                        out += to_dec(value.pointer_index());
                    }
                    return;
            }
        }

        void encode_template_args(std::string& out, std::span<TemplateArg const> args, NominalResolver const& resolver)
        {
            if (args.empty())
                return;

            out += 'X';
            out += to_dec(args.size());
            for (auto const& a : args)
            {
                if (a.kind == TemplateArg::Kind::Type)
                {
                    out += 't';
                    encode_type(out, a.type, resolver);
                }
                else
                {
                    out += 'v';
                    if (a.value)
                        encode_value(out, *a.value, resolver);
                    else
                    {
                        out += 'N';
                        out += 'E';
                    }
                }
            }
        }

        bool demangle_type_into(DemangledType& dt, std::string_view sv, std::size_t& pos)
        {
            if (pos >= sv.size())
                return false;

            char c = sv[pos++];
            switch (c)
            {
                case 'v':
                    dt.tag = DemangledType::Tag::Void;
                    return true;
                case 'b':
                    dt.tag = DemangledType::Tag::Bool;
                    return true;
                case 'i': {
                    dt.tag = DemangledType::Tag::Int;
                    std::uint64_t bits;
                    if (!parse_dec(sv, pos, bits))
                        return false;

                    dt.bits = static_cast<std::uint8_t>(bits);
                    dt.is_pointer_sized = false;
                    if (pos >= sv.size())
                        return false;

                    char sgn = sv[pos++];
                    if (sgn == 's')
                        dt.is_signed = true;
                    else if (sgn == 'u')
                        dt.is_signed = false;
                    else
                        return false;

                    return true;
                }
                case 'I': {
                    dt.tag = DemangledType::Tag::Int;
                    dt.is_pointer_sized = true;
                    if (pos >= sv.size())
                        return false;

                    char sgn = sv[pos++];
                    if (sgn == 's')
                        dt.is_signed = true;
                    else if (sgn == 'u')
                        dt.is_signed = false;
                    else
                        return false;

                    return true;
                }
                case 'f': {
                    dt.tag = DemangledType::Tag::Float;
                    std::uint64_t bits;
                    if (!parse_dec(sv, pos, bits))
                        return false;

                    if (pos >= sv.size() || sv[pos] != '.')
                        return false;

                    ++pos;
                    dt.bits = static_cast<std::uint8_t>(bits);
                    return true;
                }
                case 'c':
                    dt.tag = DemangledType::Tag::Char;
                    return true;
                case 'n':
                    dt.tag = DemangledType::Tag::NullT;
                    return true;
                case 'P': {
                    dt.tag = DemangledType::Tag::Pointer;
                    std::string q;
                    if (!parse_quals(sv, pos, q))
                        return false;

                    dt.quals = std::move(q);
                    auto pt = std::make_shared<DemangledType>();
                    if (!demangle_type_into(*pt, sv, pos))
                        return false;

                    dt.pointee = std::move(pt);
                    return true;
                }
                case 'A': {
                    dt.tag = DemangledType::Tag::Array;
                    std::uint64_t cnt;
                    if (!parse_dec(sv, pos, cnt))
                        return false;

                    dt.count = cnt;
                    auto el = std::make_shared<DemangledType>();
                    if (!demangle_type_into(*el, sv, pos))
                        return false;

                    dt.element = std::move(el);
                    return true;
                }
                case 'S': {
                    dt.tag = DemangledType::Tag::Slice;
                    std::string q;
                    if (!parse_quals(sv, pos, q))
                        return false;

                    dt.quals = std::move(q);
                    auto el = std::make_shared<DemangledType>();
                    if (!demangle_type_into(*el, sv, pos))
                        return false;

                    dt.element = std::move(el);
                    return true;
                }
                case 'F': {
                    dt.tag = DemangledType::Tag::Fam;
                    auto el = std::make_shared<DemangledType>();
                    if (!demangle_type_into(*el, sv, pos))
                        return false;

                    dt.element = std::move(el);
                    return true;
                }
                case 'p': {
                    dt.tag = DemangledType::Tag::FuncPtr;
                    auto ret = std::make_shared<DemangledType>();
                    if (!demangle_type_into(*ret, sv, pos))
                        return false;

                    dt.return_type_fp = std::move(ret);
                    std::uint64_t cnt;
                    if (!parse_dec(sv, pos, cnt))
                        return false;

                    for (std::uint64_t i = 0; i < cnt; ++i)
                    {
                        DemangledType p;
                        if (!demangle_type_into(p, sv, pos))
                            return false;

                        dt.template_args.push_back(std::move(p));
                    }

                    return true;
                }
                case 'D': {
                    std::vector<std::string> mp;
                    if (!parse_path(sv, pos, mp))
                        return false;

                    dt.module_path = std::move(mp);
                    if (!parse_ident(sv, pos, dt.name))
                        return false;

                    std::uint64_t tc = 0;
                    if (!parse_dec(sv, pos, tc))
                        return false;

                    if (pos >= sv.size() || sv[pos] != '.')
                        return false;

                    ++pos;
                    for (std::uint64_t i = 0; i < tc; ++i)
                    {
                        DemangledType ta;
                        if (!demangle_type_into(ta, sv, pos))
                            return false;
                        dt.template_args.push_back(std::move(ta));
                    }

                    dt.tag = DemangledType::Tag::Struct;
                    return true;
                }
                case 'N': {
                    auto underlying = std::make_shared<DemangledType>();
                    if (!demangle_type_into(*underlying, sv, pos))
                        return false;

                    std::vector<std::string> mp;
                    if (!parse_path(sv, pos, mp))
                        return false;

                    dt.module_path = std::move(mp);
                    if (!parse_ident(sv, pos, dt.name))
                        return false;

                    dt.tag = DemangledType::Tag::Nominal;
                    return true;
                }
                case 'Z': {
                    dt.tag = DemangledType::Tag::TemplateParam;
                    if (!parse_ident(sv, pos, dt.name))
                        return false;

                    std::uint64_t idx;
                    if (!parse_dec(sv, pos, idx))
                        return false;

                    dt.param_index = static_cast<std::uint32_t>(idx);
                    return true;
                }
                case 'r': {
                    dt.tag = DemangledType::Tag::Range;
                    auto el = std::make_shared<DemangledType>();
                    if (!demangle_type_into(*el, sv, pos))
                        return false;

                    dt.element = std::move(el);
                    return true;
                }
                case 'R': {
                    dt.tag = DemangledType::Tag::RangeInclusive;
                    auto el = std::make_shared<DemangledType>();
                    if (!demangle_type_into(*el, sv, pos))
                        return false;

                    dt.element = std::move(el);
                    return true;
                }
                case 'E':
                    dt.tag = DemangledType::Tag::Error;
                    return true;
                case 'L': {
                    dt.tag = DemangledType::Tag::Lambda;
                    std::string fid;
                    if (!parse_ident(sv, pos, fid))
                        return false;
                    dt.name = std::move(fid);
                    std::string off;
                    if (!parse_ident(sv, pos, off))
                        return false;
                    if (!dt.name.empty())
                        dt.name += ".";
                    dt.name += off;
                    return true;
                }
                default:
                    return false;
            }
        }

        bool demangle_value_into(DemangledValue& dv, std::string_view sv, std::size_t& pos)
        {
            if (pos >= sv.size())
                return false;

            char c = sv[pos++];
            switch (c)
            {
                case 'N': {
                    dv.tag = DemangledValue::Tag::Null;
                    return demangle_type_into(dv.type, sv, pos);
                }
                case 'I': {
                    dv.tag = DemangledValue::Tag::Int;
                    if (!demangle_type_into(dv.type, sv, pos))
                        return false;

                    return parse_sint(sv, pos, dv.int_val);
                }
                case 'F': {
                    dv.tag = DemangledValue::Tag::Float;
                    if (!demangle_type_into(dv.type, sv, pos))
                        return false;

                    std::size_t hd = (dv.type.bits == 32) ? 8 : 16;
                    if (pos + hd > sv.size())
                        return false;

                    auto hex = sv.substr(pos, hd);
                    pos += hd;
                    std::uint64_t bits = 0;
                    for (std::size_t i = 0; i < hex.size(); ++i)
                    {
                        char h = hex[i];
                        bits <<= 4;

                        if (h >= '0' && h <= '9')
                            bits |= static_cast<std::uint64_t>(h - '0');
                        else if (h >= 'A' && h <= 'F')
                            bits |= static_cast<std::uint64_t>(h - 'A' + 10);
                        else
                            return false;
                    }

                    if (dv.type.bits == 32)
                        dv.float_val = static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
                    else
                        dv.float_val = std::bit_cast<double>(bits);

                    return true;
                }
                case 'B': {
                    dv.tag = DemangledValue::Tag::Bool;
                    if (!demangle_type_into(dv.type, sv, pos))
                        return false;

                    if (pos >= sv.size())
                        return false;

                    dv.bool_val = (sv[pos++] == '1');
                    return true;
                }
                case 'C': {
                    dv.tag = DemangledValue::Tag::Char;
                    if (!demangle_type_into(dv.type, sv, pos))
                        return false;

                    std::uint64_t v;
                    if (!parse_dec(sv, pos, v) || v > std::numeric_limits<std::uint32_t>::max())
                        return false;

                    dv.char_val = static_cast<std::uint32_t>(v);
                    return true;
                }
                case 'S': {
                    dv.tag = DemangledValue::Tag::String;
                    if (!demangle_type_into(dv.type, sv, pos))
                        return false;

                    return parse_ident(sv, pos, dv.string_val);
                }
                case 'A': {
                    dv.tag = DemangledValue::Tag::Aggregate;
                    if (!demangle_type_into(dv.type, sv, pos))
                        return false;

                    std::uint64_t cnt;
                    if (!parse_dec(sv, pos, cnt))
                        return false;

                    dv.elements.reserve(static_cast<std::size_t>(cnt));
                    for (std::uint64_t i = 0; i < cnt; ++i)
                    {
                        DemangledValue e;
                        if (!demangle_value_into(e, sv, pos))
                            return false;

                        dv.elements.push_back(std::move(e));
                    }

                    return true;
                }
                case 's': {
                    dv.tag = DemangledValue::Tag::Slice;
                    if (!demangle_type_into(dv.type, sv, pos))
                        return false;

                    std::uint64_t cnt;
                    if (!parse_dec(sv, pos, cnt))
                        return false;

                    dv.elements.reserve(static_cast<std::size_t>(cnt));
                    for (std::uint64_t i = 0; i < cnt; ++i)
                    {
                        DemangledValue e;
                        if (!demangle_value_into(e, sv, pos))
                            return false;

                        dv.elements.push_back(std::move(e));
                    }

                    return true;
                }
                case 'P': {
                    dv.tag = DemangledValue::Tag::Pointer;
                    if (!demangle_type_into(dv.type, sv, pos))
                        return false;

                    if (pos >= sv.size())
                        return false;

                    char n = sv[pos++];
                    if (n == '0')
                    {
                        dv.is_null_ptr = true;
                        dv.pointer_index = 0;
                        return true;
                    }

                    if (n == '1')
                    {
                        dv.is_null_ptr = false;
                        std::uint64_t idx;
                        if (!parse_dec(sv, pos, idx))
                            return false;

                        dv.pointer_index = static_cast<std::size_t>(idx);
                        return true;
                    }

                    return false;
                }
                default:
                    return false;
            }
        }

        bool demangle_template_arg_into(DemangledTemplateArg& dta, std::string_view sv, std::size_t& pos)
        {
            if (pos >= sv.size())
                return false;

            char c = sv[pos++];
            if (c == 't')
            {
                dta.kind = DemangledTemplateArg::Kind::Type;
                return demangle_type_into(dta.type, sv, pos);
            }

            if (c == 'v')
            {
                dta.kind = DemangledTemplateArg::Kind::Value;
                return demangle_value_into(dta.value, sv, pos);
            }
            return false;
        }

    } // anonymous namespace

    std::string mangle_type(dcc::types::TypePtr type, NominalResolver resolver)
    {
        std::string out = "_DC0T";
        encode_type(out, type, resolver);
        return out;
    }

    std::string mangle_value(dcc::comptime::Value const& value, NominalResolver resolver)
    {
        std::string out = "_DC0V";
        encode_value(out, value, resolver);
        return out;
    }

    std::string mangle_function(std::span<std::string_view const> module_path, dcc::ast::FuncDecl const& decl, std::span<dcc::types::TypePtr const> param_types,
                                dcc::types::TypePtr return_type, std::span<TemplateArg const> template_args, NominalResolver resolver)
    {
        if (decl.sema.is_nomangle)
            return std::string{decl.name};

        if (decl.synthesized_lambda || decl.lambda_source)
            return mangle_lambda_function(module_path, decl, param_types, return_type, resolver);

        std::string out = "_DC0F";
        encode_path(out, module_path, resolver);
        encode_seg(out, decl.name);
        out += to_dec(param_types.size());
        for (auto* pt : param_types)
            encode_type(out, pt, resolver);

        encode_type(out, return_type, resolver);
        auto cc = encode_cc(decl.sema.calling_conv);
        if (!cc.empty())
            out += cc;

        encode_template_args(out, template_args, resolver);
        return out;
    }

    std::string mangle_lambda_function(std::span<std::string_view const> module_path, dcc::ast::FuncDecl const& decl,
                                        std::span<dcc::types::TypePtr const> param_types, dcc::types::TypePtr return_type, NominalResolver resolver)
    {
        std::string out = "_DC0L";
        encode_path(out, module_path, resolver);
        if (decl.lambda_source && decl.lambda_source->range.valid())
        {
            encode_seg(out, std::to_string(static_cast<std::uint32_t>(decl.lambda_source->range.begin.fileId)));
            encode_seg(out, std::to_string(decl.lambda_source->range.begin.offset));
        }
        else
        {
            encode_seg(out, "0");
            encode_seg(out, "0");
        }
        out += to_dec(param_types.size());
        for (auto* pt : param_types)
            encode_type(out, pt, resolver);

        encode_type(out, return_type, resolver);
        auto cc = encode_cc(decl.sema.calling_conv);
        if (!cc.empty())
            out += cc;
        return out;
    }

    std::string mangle_global(std::span<std::string_view const> module_path, dcc::ast::VarDecl const& decl, dcc::types::TypePtr type,
                              std::span<TemplateArg const> template_args, NominalResolver resolver)
    {
        if (decl.sema.is_nomangle)
            return std::string{decl.name};

        std::string out = "_DC0G";
        encode_path(out, module_path, resolver);
        encode_seg(out, decl.name);
        encode_type(out, type, resolver);
        encode_template_args(out, template_args, resolver);
        return out;
    }

    std::string mangle_specialization(std::span<std::string_view const> module_path, std::string_view name, std::span<dcc::types::TypePtr const> param_types,
                                      dcc::types::TypePtr return_type, std::span<TemplateArg const> template_args, NominalResolver resolver)
    {
        std::string out = "_DC0S";
        encode_path(out, module_path, resolver);
        encode_seg(out, name);
        out += to_dec(param_types.size());
        for (auto* pt : param_types)
            encode_type(out, pt, resolver);

        encode_type(out, return_type, resolver);
        encode_template_args(out, template_args, resolver);
        return out;
    }

    std::string mangle_type_specialization(std::span<std::string_view const> module_path, std::string_view type_name,
                                           std::span<TemplateArg const> template_args, NominalResolver resolver)
    {
        std::string out = "_DC0Y";
        encode_path(out, module_path, resolver);
        encode_seg(out, type_name);
        encode_template_args(out, template_args, resolver);
        return out;
    }

    bool demangle(DemangledName& result, std::string_view s)
    {
        std::size_t pos = 0;
        if (s.size() < pos + 4 || s.substr(pos, 4) != "_DC0")
            return false;

        pos += 4;
        if (pos >= s.size())
            return false;

        char kind = s[pos++];

        auto parse_func_like = [&](DemangledName::Kind k) -> bool {
            result.kind = k;
            {
                std::vector<std::string> path;
                if (!parse_path(s, pos, path))
                    return false;

                result.module_path = std::move(path);
            }
            if (!parse_ident(s, pos, result.name))
                return false;

            std::uint64_t pc;
            if (!parse_dec(s, pos, pc))
                return false;

            for (std::uint64_t i = 0; i < pc; ++i)
            {
                DemangledType pt;
                if (!demangle_type_into(pt, s, pos))
                    return false;

                result.param_types.push_back(std::move(pt));
            }

            if (!demangle_type_into(result.return_type, s, pos))
                return false;

            if (pos < s.size() && s[pos] == 'c')
            {
                ++pos;
                std::uint64_t cc;
                if (!parse_dec(s, pos, cc))
                    return false;

                result.calling_conv = cc_name(static_cast<int>(cc));
            }

            if (pos < s.size() && s[pos] == 'X')
            {
                ++pos;
                std::uint64_t tc = 0;
                if (!parse_dec(s, pos, tc))
                    return false;

                for (std::uint64_t i = 0; i < tc; ++i)
                {
                    DemangledTemplateArg ta;
                    if (!demangle_template_arg_into(ta, s, pos))
                        return false;

                    result.template_args.push_back(std::move(ta));
                }
            }

            return (pos == s.size());
        };

        if (kind == 'F')
            return parse_func_like(DemangledName::Kind::Function);
        else if (kind == 'S')
            return parse_func_like(DemangledName::Kind::Specialization);
        else if (kind == 'L')
            return parse_func_like(DemangledName::Kind::Lambda);
        else if (kind == 'G')
        {
            result.kind = DemangledName::Kind::Global;
            {
                std::vector<std::string> path;
                if (!parse_path(s, pos, path))
                    return false;

                result.module_path = std::move(path);
            }
            if (!parse_ident(s, pos, result.name))
                return false;

            if (!demangle_type_into(result.global_type, s, pos))
                return false;

            if (pos < s.size() && s[pos] == 'X')
            {
                ++pos;
                std::uint64_t tc = 0;
                if (!parse_dec(s, pos, tc))
                    return false;

                for (std::uint64_t i = 0; i < tc; ++i)
                {
                    DemangledTemplateArg ta;
                    if (!demangle_template_arg_into(ta, s, pos))
                        return false;

                    result.template_args.push_back(std::move(ta));
                }
            }

            return (pos == s.size());
        }
        else if (kind == 'T')
        {
            result.kind = DemangledName::Kind::Type;
            if (!demangle_type_into(result.type_only, s, pos))
                return false;

            return (pos == s.size());
        }
        else if (kind == 'V')
        {
            result.kind = DemangledName::Kind::Value;
            if (!demangle_value_into(result.value_only, s, pos))
                return false;

            return (pos == s.size());
        }
        else if (kind == 'Y')
        {
            result.kind = DemangledName::Kind::TypeSpec;
            {
                std::vector<std::string> path;
                if (!parse_path(s, pos, path))
                    return false;

                result.module_path = std::move(path);
            }
            if (!parse_ident(s, pos, result.name))
                return false;

            if (pos < s.size() && s[pos] == 'X')
            {
                ++pos;
                std::uint64_t tc = 0;
                if (!parse_dec(s, pos, tc))
                    return false;

                for (std::uint64_t i = 0; i < tc; ++i)
                {
                    DemangledTemplateArg ta;
                    if (!demangle_template_arg_into(ta, s, pos))
                        return false;

                    result.template_args.push_back(std::move(ta));
                }
            }

            return (pos == s.size());
        }

        return false;
    }

    std::optional<DemangledName> demangle(std::string_view s)
    {
        DemangledName result;
        if (demangle(result, s))
            return std::move(result);

        return std::nullopt;
    }

} // namespace dcc::ir::mangle
