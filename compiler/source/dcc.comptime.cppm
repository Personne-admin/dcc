module;

#include <cassert>

export module dcc.comptime;

import std;
import dcc.types;

export namespace dcc::comptime
{
    struct Value;

    struct ValueAgg
    {
        std::vector<Value> elements;
    };

    struct ValuePtr
    {
        bool is_null{true};
        std::size_t allocation{};
        std::vector<std::uint32_t> path;

        [[nodiscard]] bool operator==(ValuePtr const&) const noexcept = default;
    };

    struct ValueSlice
    {
        std::vector<Value> elements;
        ValuePtr base;
        std::size_t length{};
    };

    enum class UnaryOp : std::uint8_t
    {
        Plus,
        Minus,
        Not,
        BitNot,
    };

    enum class BinaryOp : std::uint8_t
    {
        Add,
        Sub,
        Mul,
        Div,
        Rem,
        Eq,
        Ne,
        Lt,
        Le,
        Gt,
        Ge,
        BitAnd,
        BitOr,
        BitXor,
        Shl,
        Shr,
    };

    struct Value
    {
        using Storage = std::variant<std::monostate, std::int64_t, double, bool, std::uint32_t, std::string, ValueAgg, ValueSlice, ValuePtr>;

        enum class Kind : std::uint8_t
        {
            Null = 0,
            Int = 1,
            Float = 2,
            Bool = 3,
            Char = 4,
            String = 5,
            Aggregate = 6,
            Slice = 7,
            Pointer = 8,
        };

        types::TypePtr type{nullptr};
        Storage m_storage;

        [[nodiscard]] Kind kind() const noexcept { return static_cast<Kind>(m_storage.index()); }

        Value() = default;

        Value(Value const&) = default;
        Value& operator=(Value const&) = default;
        Value(Value&&) = default;
        Value& operator=(Value&&) = default;

        [[nodiscard]] static Value make_int(std::int64_t v, types::TypePtr t)
        {
            assert(t && t->kind == types::TypeKind::Int);
            Value val;
            val.type = t;
            val.m_storage.template emplace<std::int64_t>(v);
            return val;
        }

        [[nodiscard]] static Value make_float(double v, types::TypePtr t)
        {
            assert(t && t->kind == types::TypeKind::Float);
            Value val;
            val.type = t;
            val.m_storage.template emplace<double>(v);
            return val;
        }

        [[nodiscard]] static Value make_bool(bool v, types::TypePtr t)
        {
            assert(t && t->kind == types::TypeKind::Bool);
            Value val;
            val.type = t;
            val.m_storage.template emplace<bool>(v);
            return val;
        }

        [[nodiscard]] static Value make_char(std::uint32_t v, types::TypePtr t)
        {
            assert(t && t->kind == types::TypeKind::Char);
            Value val;
            val.type = t;
            val.m_storage.template emplace<std::uint32_t>(v);
            return val;
        }

        [[nodiscard]] static Value make_null(types::TypePtr t)
        {
            assert(t && (t->kind == types::TypeKind::NullT || t->kind == types::TypeKind::Pointer));
            Value val;
            val.type = t;
            val.m_storage.template emplace<std::monostate>();
            return val;
        }

        [[nodiscard]] static Value make_string(std::string s, types::TypePtr t)
        {
            assert(t);
            Value val;
            val.type = t;
            val.m_storage.template emplace<std::string>(std::move(s));
            return val;
        }

        [[nodiscard]] static Value make_aggregate(std::vector<Value> elems, types::TypePtr t)
        {
            assert(t);
            Value val;
            val.type = t;
            val.m_storage.template emplace<ValueAgg>(ValueAgg{std::move(elems)});
            return val;
        }

        [[nodiscard]] static Value make_slice(std::vector<Value> elems, types::TypePtr t)
        {
            assert(t && t->kind == types::TypeKind::Slice);
            Value val;
            val.type = t;
            auto count = elems.size();
            val.m_storage.template emplace<ValueSlice>(ValueSlice{std::move(elems), ValuePtr{}, count});
            return val;
        }

        [[nodiscard]] static Value make_slice_ref(ValuePtr base, std::size_t length, types::TypePtr t)
        {
            assert(t && t->kind == types::TypeKind::Slice);
            assert(!base.is_null);
            Value val;
            val.type = t;
            val.m_storage.template emplace<ValueSlice>(ValueSlice{{}, std::move(base), length});
            return val;
        }

        [[nodiscard]] static Value make_pointer(types::TypePtr t)
        {
            assert(!t || t->kind == types::TypeKind::Pointer);
            Value val;
            val.type = t;
            val.m_storage.template emplace<ValuePtr>(ValuePtr{});
            return val;
        }

        [[nodiscard]] static Value make_pointer_to(ValuePtr p, types::TypePtr t)
        {
            assert(!t || t->kind == types::TypeKind::Pointer);
            Value val;
            val.type = t;
            val.m_storage.template emplace<ValuePtr>(std::move(p));
            return val;
        }

        [[nodiscard]] std::int64_t get_int() const
        {
            assert(kind() == Kind::Int);
            return std::get<std::int64_t>(m_storage);
        }

        [[nodiscard]] double get_float() const
        {
            assert(kind() == Kind::Float);
            return std::get<double>(m_storage);
        }

        [[nodiscard]] bool get_bool() const
        {
            assert(kind() == Kind::Bool);
            return std::get<bool>(m_storage);
        }

        [[nodiscard]] std::uint32_t get_char() const
        {
            assert(kind() == Kind::Char);
            return std::get<std::uint32_t>(m_storage);
        }

        [[nodiscard]] std::string const& get_string() const
        {
            assert(kind() == Kind::String);
            return std::get<std::string>(m_storage);
        }

        [[nodiscard]] bool is_null_ptr() const
        {
            assert(kind() == Kind::Pointer);
            return std::get<ValuePtr>(m_storage).is_null;
        }

        [[nodiscard]] ValuePtr const& get_pointer() const
        {
            assert(kind() == Kind::Pointer);
            return std::get<ValuePtr>(m_storage);
        }

        [[nodiscard]] ValuePtr const& slice_base() const
        {
            assert(kind() == Kind::Slice);
            return std::get<ValueSlice>(m_storage).base;
        }

        [[nodiscard]] std::size_t slice_length() const
        {
            assert(kind() == Kind::Slice);
            return std::get<ValueSlice>(m_storage).length;
        }

        [[nodiscard]] bool slice_is_ref() const
        {
            assert(kind() == Kind::Slice);
            return !std::get<ValueSlice>(m_storage).base.is_null;
        }

        void set_int(std::int64_t v)
        {
            assert(kind() == Kind::Int);
            std::get<std::int64_t>(m_storage) = v;
        }

        void set_float(double v)
        {
            assert(kind() == Kind::Float);
            std::get<double>(m_storage) = v;
        }

        void set_bool(bool v)
        {
            assert(kind() == Kind::Bool);
            std::get<bool>(m_storage) = v;
        }

        void set_char(std::uint32_t v)
        {
            assert(kind() == Kind::Char);
            std::get<std::uint32_t>(m_storage) = v;
        }

        [[nodiscard]] std::size_t size() const
        {
            assert(kind() == Kind::Aggregate || kind() == Kind::Slice);
            if (kind() == Kind::Aggregate)
                return std::get<ValueAgg>(m_storage).elements.size();

            return std::get<ValueSlice>(m_storage).length;
        }

        [[nodiscard]] Value& at(std::size_t i)
        {
            assert(kind() == Kind::Aggregate || (kind() == Kind::Slice && !slice_is_ref()));
            assert(i < size());
            if (kind() == Kind::Aggregate)
                return std::get<ValueAgg>(m_storage).elements[i];

            return std::get<ValueSlice>(m_storage).elements[i];
        }

        [[nodiscard]] Value const& at(std::size_t i) const
        {
            assert(kind() == Kind::Aggregate || (kind() == Kind::Slice && !slice_is_ref()));
            assert(i < size());
            if (kind() == Kind::Aggregate)
                return std::get<ValueAgg>(m_storage).elements[i];

            return std::get<ValueSlice>(m_storage).elements[i];
        }

        void push_back(Value v)
        {
            assert(kind() == Kind::Aggregate || (kind() == Kind::Slice && !slice_is_ref()));

            if (kind() == Kind::Aggregate)
                std::get<ValueAgg>(m_storage).elements.push_back(std::move(v));
            else
            {
                auto& s = std::get<ValueSlice>(m_storage);
                s.elements.push_back(std::move(v));
                s.length = s.elements.size();
            }
        }

        void pop_back()
        {
            assert(kind() == Kind::Aggregate || (kind() == Kind::Slice && !slice_is_ref()));
            assert(!empty());

            if (kind() == Kind::Aggregate)
                std::get<ValueAgg>(m_storage).elements.pop_back();
            else
            {
                auto& s = std::get<ValueSlice>(m_storage);
                s.elements.pop_back();
                s.length = s.elements.size();
            }
        }

        [[nodiscard]] bool empty() const
        {
            assert(kind() == Kind::Aggregate || kind() == Kind::Slice);
            return size() == 0;
        }

        [[nodiscard]] bool operator==(Value const& other) const noexcept
        {
            if (type != other.type || kind() != other.kind())
                return false;

            switch (kind())
            {
                case Kind::Null:
                    return true;
                case Kind::Int:
                    return get_int() == other.get_int();
                case Kind::Float:
                    return get_float() == other.get_float();
                case Kind::Bool:
                    return get_bool() == other.get_bool();
                case Kind::Char:
                    return get_char() == other.get_char();
                case Kind::String:
                    return get_string() == other.get_string();
                case Kind::Aggregate:
                    return std::get<ValueAgg>(m_storage).elements == std::get<ValueAgg>(other.m_storage).elements;
                case Kind::Slice: {
                    auto const& a = std::get<ValueSlice>(m_storage);
                    auto const& b = std::get<ValueSlice>(other.m_storage);
                    return a.base == b.base && a.length == b.length && a.elements == b.elements;
                }
                case Kind::Pointer:
                    return std::get<ValuePtr>(m_storage) == std::get<ValuePtr>(other.m_storage);
            }
            return false;
        }

        [[nodiscard]] bool operator!=(Value const& other) const noexcept { return !(*this == other); }

        [[nodiscard]] static std::size_t hash_pointer(ValuePtr const& p) noexcept
        {
            std::size_t h = std::hash<bool>{}(p.is_null);
            h ^= std::hash<std::size_t>{}(p.allocation);
            for (auto index : p.path)
                h ^= std::hash<std::uint32_t>{}(index) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }

        [[nodiscard]] std::size_t hash() const noexcept
        {
            std::size_t h = std::hash<types::TypePtr>{}(type);
            h ^= static_cast<std::size_t>(kind()) + 0x9e3779b9 + (h << 6) + (h >> 2);

            switch (kind())
            {
                case Kind::Null:
                    break;
                case Kind::Int:
                    h ^= std::hash<std::int64_t>{}(get_int());
                    break;
                case Kind::Float:
                    h ^= std::hash<double>{}(get_float());
                    break;
                case Kind::Bool:
                    h ^= std::hash<bool>{}(get_bool());
                    break;
                case Kind::Char:
                    h ^= std::hash<std::uint32_t>{}(get_char());
                    break;
                case Kind::String:
                    h ^= std::hash<std::string>{}(get_string());
                    break;
                case Kind::Aggregate:
                    for (auto const& e : std::get<ValueAgg>(m_storage).elements)
                        h ^= e.hash() + 0x9e3779b9 + (h << 6) + (h >> 2);
                    break;
                case Kind::Slice: {
                    auto const& s = std::get<ValueSlice>(m_storage);
                    h ^= std::hash<std::size_t>{}(s.length);
                    h ^= hash_pointer(s.base);
                    for (auto const& e : s.elements)
                        h ^= e.hash() + 0x9e3779b9 + (h << 6) + (h >> 2);
                    break;
                }
                case Kind::Pointer:
                    h ^= hash_pointer(std::get<ValuePtr>(m_storage));
                    break;
            }
            return h;
        }

        [[nodiscard]] std::optional<std::int64_t> const_to_int() const noexcept
        {
            switch (kind())
            {
                case Kind::Int:
                    return get_int();
                case Kind::Bool:
                    return get_bool() ? 1 : 0;
                case Kind::Char:
                    return static_cast<std::int64_t>(get_char());
                case Kind::Float: {
                    double v = get_float();
                    if (!std::isfinite(v))
                        return std::nullopt;
                    if (v < static_cast<double>(std::numeric_limits<std::int64_t>::min()) || v > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                        return std::nullopt;
                    return static_cast<std::int64_t>(v);
                }
                case Kind::Null:
                case Kind::String:
                case Kind::Aggregate:
                case Kind::Slice:
                case Kind::Pointer:
                    return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<double> const_to_float() const noexcept
        {
            switch (kind())
            {
                case Kind::Int:
                    return static_cast<double>(get_int());
                case Kind::Bool:
                    return get_bool() ? 1.0 : 0.0;
                case Kind::Char:
                    return static_cast<double>(get_char());
                case Kind::Float:
                    return get_float();
                case Kind::Null:
                case Kind::String:
                case Kind::Aggregate:
                case Kind::Slice:
                case Kind::Pointer:
                    return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<bool> const_to_bool() const noexcept
        {
            switch (kind())
            {
                case Kind::Int:
                    return get_int() != 0;
                case Kind::Bool:
                    return get_bool();
                case Kind::Char:
                    return get_char() != 0;
                case Kind::Float:
                    return get_float() != 0.0;
                case Kind::Null:
                case Kind::String:
                case Kind::Aggregate:
                case Kind::Slice:
                case Kind::Pointer:
                    return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::uint64_t> const_to_bits() const noexcept
        {
            if (auto v = const_to_int())
                return static_cast<std::uint64_t>(*v);

            return std::nullopt;
        }

        [[nodiscard]] static std::optional<Value> fold_int_binary(BinaryOp op, std::int64_t lhs, std::int64_t rhs, types::TypePtr out_type)
        {
            switch (op)
            {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Rem: {
                    std::int64_t value{};
                    bool valid = true;
                    switch (op)
                    {
                        case BinaryOp::Add:
                            valid = !__builtin_add_overflow(lhs, rhs, &value);
                            break;
                        case BinaryOp::Sub:
                            valid = !__builtin_sub_overflow(lhs, rhs, &value);
                            break;
                        case BinaryOp::Mul:
                            valid = !__builtin_mul_overflow(lhs, rhs, &value);
                            break;
                        case BinaryOp::Div:
                            valid = rhs != 0 && !(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1);
                            if (valid)
                                value = lhs / rhs;
                            break;
                        case BinaryOp::Rem:
                            valid = rhs != 0 && !(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1);
                            if (valid)
                                value = lhs % rhs;
                            break;
                        default:
                            valid = false;
                            break;
                    }
                    if (valid)
                        return make_int(value, out_type);
                    return std::nullopt;
                }

                case BinaryOp::BitAnd:
                case BinaryOp::BitOr:
                case BinaryOp::BitXor:
                case BinaryOp::Shl:
                case BinaryOp::Shr: {
                    auto lhs_bits = static_cast<std::uint64_t>(lhs);
                    auto rhs_bits = static_cast<std::uint64_t>(rhs);
                    std::uint64_t value{};
                    bool valid = true;
                    switch (op)
                    {
                        case BinaryOp::BitAnd:
                            value = lhs_bits & rhs_bits;
                            break;
                        case BinaryOp::BitOr:
                            value = lhs_bits | rhs_bits;
                            break;
                        case BinaryOp::BitXor:
                            value = lhs_bits ^ rhs_bits;
                            break;
                        case BinaryOp::Shl:
                            valid = rhs_bits < 64;
                            if (valid)
                                value = lhs_bits << rhs_bits;
                            break;
                        case BinaryOp::Shr:
                            valid = rhs_bits < 64;
                            if (valid)
                                value = lhs_bits >> rhs_bits;
                            break;
                        default:
                            valid = false;
                            break;
                    }
                    if (valid)
                        return make_int(static_cast<std::int64_t>(value), out_type);
                    return std::nullopt;
                }

                default:
                    return std::nullopt;
            }
        }

        [[nodiscard]] static std::optional<Value> fold_int_cmp(BinaryOp op, std::int64_t lhs, std::int64_t rhs, types::TypePtr out_type)
        {
            std::optional<bool> result;
            switch (op)
            {
                case BinaryOp::Eq:
                    result = lhs == rhs;
                    break;
                case BinaryOp::Ne:
                    result = lhs != rhs;
                    break;
                case BinaryOp::Lt:
                    result = lhs < rhs;
                    break;
                case BinaryOp::Le:
                    result = lhs <= rhs;
                    break;
                case BinaryOp::Gt:
                    result = lhs > rhs;
                    break;
                case BinaryOp::Ge:
                    result = lhs >= rhs;
                    break;
                default:
                    return std::nullopt;
            }
            if (result)
                return make_bool(*result, out_type);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<Value> fold_unary(UnaryOp op, types::TypePtr out_type) const
        {
            switch (op)
            {
                case UnaryOp::Plus:
                    if (kind() == Kind::Int)
                        return make_int(get_int(), out_type);
                    if (kind() == Kind::Float)
                        return make_float(get_float(), out_type);
                    break;
                case UnaryOp::Minus:
                    if (kind() == Kind::Int)
                    {
                        if (get_int() == std::numeric_limits<std::int64_t>::min())
                            return std::nullopt;
                        return make_int(-get_int(), out_type);
                    }
                    if (kind() == Kind::Float)
                        return make_float(-get_float(), out_type);
                    break;
                case UnaryOp::Not:
                    if (kind() == Kind::Bool)
                        return make_bool(!get_bool(), out_type);
                    break;
                case UnaryOp::BitNot:
                    if (kind() == Kind::Int)
                        return make_int(~get_int(), out_type);
                    break;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<Value> fold_binary(BinaryOp op, Value const& rhs, types::TypePtr out_type) const
        {
            auto const* ft = types::type_cast<types::FloatType>(out_type);
            auto const* it = types::type_cast<types::IntType>(out_type);

            switch (op)
            {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Rem:
                    if (ft && kind() == Kind::Float && rhs.kind() == Kind::Float)
                    {
                        if (op == BinaryOp::Rem)
                            return std::nullopt;

                        double value{};
                        switch (op)
                        {
                            case BinaryOp::Add:
                                value = get_float() + rhs.get_float();
                                break;
                            case BinaryOp::Sub:
                                value = get_float() - rhs.get_float();
                                break;
                            case BinaryOp::Mul:
                                value = get_float() * rhs.get_float();
                                break;
                            case BinaryOp::Div:
                                value = get_float() / rhs.get_float();
                                break;
                            default:
                                break;
                        }
                        return make_float(value, out_type);
                    }
                    if (it && kind() == Kind::Int && rhs.kind() == Kind::Int)
                        return fold_int_binary(op, get_int(), rhs.get_int(), out_type);
                    break;

                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Le:
                case BinaryOp::Gt:
                case BinaryOp::Ge: {
                    std::optional<bool> result;

                    if (kind() == Kind::Float && rhs.kind() == Kind::Float)
                    {
                        switch (op)
                        {
                            case BinaryOp::Eq:
                                result = get_float() == rhs.get_float();
                                break;
                            case BinaryOp::Ne:
                                result = get_float() != rhs.get_float();
                                break;
                            case BinaryOp::Lt:
                                result = get_float() < rhs.get_float();
                                break;
                            case BinaryOp::Le:
                                result = get_float() <= rhs.get_float();
                                break;
                            case BinaryOp::Gt:
                                result = get_float() > rhs.get_float();
                                break;
                            case BinaryOp::Ge:
                                result = get_float() >= rhs.get_float();
                                break;
                            default:
                                break;
                        }
                    }
                    else if ((kind() == Kind::Int || kind() == Kind::Char || kind() == Kind::Bool) &&
                             (rhs.kind() == Kind::Int || rhs.kind() == Kind::Char || rhs.kind() == Kind::Bool))
                    {
                        auto lv = const_to_int();
                        auto rv = rhs.const_to_int();
                        if (!lv || !rv)
                            return std::nullopt;

                        return fold_int_cmp(op, *lv, *rv, out_type);
                    }
                    else if (kind() == Kind::Null && rhs.kind() == Kind::Null)
                    {
                        if (op == BinaryOp::Eq)
                            result = true;
                        else if (op == BinaryOp::Ne)
                            result = false;
                    }

                    if (result)
                        return make_bool(*result, out_type);
                    break;
                }

                case BinaryOp::BitAnd:
                case BinaryOp::BitOr:
                case BinaryOp::BitXor:
                case BinaryOp::Shl:
                case BinaryOp::Shr:
                    if (it && kind() == Kind::Int && rhs.kind() == Kind::Int)
                        return fold_int_binary(op, get_int(), rhs.get_int(), out_type);
                    break;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<Value> fold_cast(types::TypePtr dst) const
        {
            if (!dst)
                return std::nullopt;

            if (dst->kind == types::TypeKind::Bool)
            {
                if (auto v = const_to_bool())
                    return make_bool(*v, dst);
                return std::nullopt;
            }

            if (types::type_cast<types::IntType>(dst))
            {
                if (auto v = const_to_int())
                    return make_int(*v, dst);
                return std::nullopt;
            }

            if (dst->kind == types::TypeKind::Char)
            {
                if (auto v = const_to_int())
                    return make_char(static_cast<std::uint32_t>(*v), dst);
                return std::nullopt;
            }

            if (types::type_cast<types::FloatType>(dst))
            {
                if (auto v = const_to_float())
                    return make_float(*v, dst);
                return std::nullopt;
            }

            if (types::type_cast<types::PointerType>(dst) || dst->kind == types::TypeKind::NullT)
            {
                if (kind() == Kind::Null)
                    return make_null(dst);
                if (kind() == Kind::Pointer && dst->kind == types::TypeKind::Pointer)
                    return make_pointer_to(get_pointer(), dst);
                return std::nullopt;
            }

            if (auto const* et = types::type_cast<types::EnumType>(dst))
            {
                if (!et->is_tagged)
                {
                    if (auto const* backing = types::type_cast<types::IntType>(et->backing))
                    {
                        if (auto v = const_to_int())
                            return make_int(*v, backing);
                    }
                }
                return std::nullopt;
            }

            return std::nullopt;
        }
    };

} // namespace dcc::comptime

template <> struct std::hash<dcc::comptime::Value>
{
    [[nodiscard]] std::size_t operator()(dcc::comptime::Value const& v) const noexcept { return v.hash(); }
};
