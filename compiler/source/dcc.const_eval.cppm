module;

export module dcc.const_eval;

import std;
import dcc.comptime;
import dcc.lex.tokens;
import dcc.types;

export namespace dcc::const_eval
{
    [[nodiscard]] std::optional<comptime::BinaryOp> token_to_arith_binop(lex::TokenKind op) noexcept
    {
        using BO = comptime::BinaryOp;
        switch (op)
        {
            case lex::TokenKind::Plus:
                return BO::Add;
            case lex::TokenKind::Minus:
                return BO::Sub;
            case lex::TokenKind::Star:
                return BO::Mul;
            case lex::TokenKind::Slash:
                return BO::Div;
            case lex::TokenKind::Percent:
                return BO::Rem;
            case lex::TokenKind::Amp:
                return BO::BitAnd;
            case lex::TokenKind::Pipe:
                return BO::BitOr;
            case lex::TokenKind::Caret:
                return BO::BitXor;
            case lex::TokenKind::LtLt:
                return BO::Shl;
            case lex::TokenKind::GtGt:
                return BO::Shr;
            default:
                return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<comptime::BinaryOp> token_to_cmp_binop(lex::TokenKind op) noexcept
    {
        using BO = comptime::BinaryOp;
        switch (op)
        {
            case lex::TokenKind::EqEq:
                return BO::Eq;
            case lex::TokenKind::BangEq:
                return BO::Ne;
            case lex::TokenKind::Lt:
                return BO::Lt;
            case lex::TokenKind::LtEq:
                return BO::Le;
            case lex::TokenKind::Gt:
                return BO::Gt;
            case lex::TokenKind::GtEq:
                return BO::Ge;
            default:
                return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<comptime::UnaryOp> token_to_unary_op(lex::TokenKind op) noexcept
    {
        using UO = comptime::UnaryOp;
        switch (op)
        {
            case lex::TokenKind::Plus:
                return UO::Plus;
            case lex::TokenKind::Minus:
                return UO::Minus;
            case lex::TokenKind::Bang:
                return UO::Not;
            case lex::TokenKind::Tilde:
                return UO::BitNot;
            default:
                return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<comptime::Value> fold_int_binary(lex::TokenKind op, std::int64_t lhs, std::int64_t rhs, types::TypePtr out_type)
    {
        auto bop = token_to_arith_binop(op);
        if (!bop)
            return std::nullopt;
        return comptime::Value::fold_int_binary(*bop, lhs, rhs, out_type);
    }

    [[nodiscard]] std::optional<comptime::Value> fold_int_cmp(lex::TokenKind op, std::int64_t lhs, std::int64_t rhs, types::TypePtr out_type)
    {
        auto bop = token_to_cmp_binop(op);
        if (!bop)
            return std::nullopt;
        return comptime::Value::fold_int_cmp(*bop, lhs, rhs, out_type);
    }

    [[nodiscard]] std::optional<comptime::Value> fold_unary(lex::TokenKind op, comptime::Value const& c, types::TypePtr out_type)
    {
        auto mapped = token_to_unary_op(op);
        if (!mapped)
            return std::nullopt;
        return c.fold_unary(*mapped, out_type);
    }

    [[nodiscard]] std::optional<comptime::Value> fold_cmp(lex::TokenKind op, comptime::Value const& lhs, comptime::Value const& rhs,
                                                                 types::TypePtr out_type)
    {
        auto bop = token_to_cmp_binop(op);
        if (!bop)
            return std::nullopt;

        if (lhs.kind() == comptime::Value::Kind::Float && rhs.kind() == comptime::Value::Kind::Float)
        {
            bool r{};
            switch (op)
            {
                case lex::TokenKind::EqEq:
                    r = lhs.get_float() == rhs.get_float();
                    break;
                case lex::TokenKind::BangEq:
                    r = lhs.get_float() != rhs.get_float();
                    break;
                case lex::TokenKind::Lt:
                    r = lhs.get_float() < rhs.get_float();
                    break;
                case lex::TokenKind::LtEq:
                    r = lhs.get_float() <= rhs.get_float();
                    break;
                case lex::TokenKind::Gt:
                    r = lhs.get_float() > rhs.get_float();
                    break;
                case lex::TokenKind::GtEq:
                    r = lhs.get_float() >= rhs.get_float();
                    break;
                default:
                    return std::nullopt;
            }
            return comptime::Value::make_bool(r, out_type);
        }

        if ((lhs.kind() == comptime::Value::Kind::Int || lhs.kind() == comptime::Value::Kind::Char || lhs.kind() == comptime::Value::Kind::Bool) &&
            (rhs.kind() == comptime::Value::Kind::Int || rhs.kind() == comptime::Value::Kind::Char || rhs.kind() == comptime::Value::Kind::Bool))
        {
            auto lv = lhs.const_to_int();
            auto rv = rhs.const_to_int();
            if (!lv || !rv)
                return std::nullopt;

            return fold_int_cmp(op, *lv, *rv, out_type);
        }

        if (lhs.kind() == comptime::Value::Kind::Null && rhs.kind() == comptime::Value::Kind::Null)
        {
            if (op == lex::TokenKind::EqEq)
                return comptime::Value::make_bool(true, out_type);
            if (op == lex::TokenKind::BangEq)
                return comptime::Value::make_bool(false, out_type);
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<comptime::Value> fold_binary(lex::TokenKind op, comptime::Value const& lhs, comptime::Value const& rhs,
                                                                    types::TypePtr out_type)
    {
        if (op == lex::TokenKind::Plus || op == lex::TokenKind::Minus || op == lex::TokenKind::Star || op == lex::TokenKind::Slash ||
            op == lex::TokenKind::Percent)
        {
            auto const* ft = types::type_cast<types::FloatType>(out_type);
            auto const* it = types::type_cast<types::IntType>(out_type);

            if (ft && lhs.kind() == comptime::Value::Kind::Float && rhs.kind() == comptime::Value::Kind::Float)
            {
                if (op == lex::TokenKind::Percent)
                    return std::nullopt;

                double value{};
                switch (op)
                {
                    case lex::TokenKind::Plus:
                        value = lhs.get_float() + rhs.get_float();
                        break;
                    case lex::TokenKind::Minus:
                        value = lhs.get_float() - rhs.get_float();
                        break;
                    case lex::TokenKind::Star:
                        value = lhs.get_float() * rhs.get_float();
                        break;
                    case lex::TokenKind::Slash:
                        value = lhs.get_float() / rhs.get_float();
                        break;
                    default:
                        break;
                }
                return comptime::Value::make_float(value, out_type);
            }

            if (it && lhs.kind() == comptime::Value::Kind::Int && rhs.kind() == comptime::Value::Kind::Int)
                return fold_int_binary(op, lhs.get_int(), rhs.get_int(), out_type);

            return std::nullopt;
        }

        if (op == lex::TokenKind::EqEq || op == lex::TokenKind::BangEq || op == lex::TokenKind::Lt || op == lex::TokenKind::LtEq || op == lex::TokenKind::Gt ||
            op == lex::TokenKind::GtEq)
        {
            return fold_cmp(op, lhs, rhs, out_type);
        }

        if (op == lex::TokenKind::Amp || op == lex::TokenKind::Pipe || op == lex::TokenKind::Caret || op == lex::TokenKind::LtLt || op == lex::TokenKind::GtGt)
        {
            auto const* it = types::type_cast<types::IntType>(out_type);
            if (it && lhs.kind() == comptime::Value::Kind::Int && rhs.kind() == comptime::Value::Kind::Int)
                return fold_int_binary(op, lhs.get_int(), rhs.get_int(), out_type);

            return std::nullopt;
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<comptime::Value> fold_cast(comptime::Value const& c, types::TypePtr dst)
    {
        return c.fold_cast(dst);
    }

    [[nodiscard]] bool values_equal(comptime::Value const& a, comptime::Value const& b) noexcept
    {
        return a == b;
    }

    [[nodiscard]] bool is_const_kind(comptime::Value const& c, comptime::Value::Kind k) noexcept
    {
        return c.kind() == k;
    }

    [[nodiscard]] std::optional<std::int64_t> const_to_int(comptime::Value const& c) noexcept
    {
        return c.const_to_int();
    }

    [[nodiscard]] std::optional<double> const_to_float(comptime::Value const& c) noexcept
    {
        return c.const_to_float();
    }

    [[nodiscard]] std::optional<bool> const_to_bool(comptime::Value const& c) noexcept
    {
        return c.const_to_bool();
    }

    [[nodiscard]] std::optional<std::uint64_t> const_to_bits(comptime::Value const& c) noexcept
    {
        return c.const_to_bits();
    }

    template <typename LookupFn>
    [[nodiscard]] comptime::Value const* lookup_identifier(std::string_view name, LookupFn&& lookup)
    {
        return lookup(name);
    }

} // namespace dcc::const_eval
