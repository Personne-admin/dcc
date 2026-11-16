# Mastery: Expression Parser

Needs: the whole core curriculum. This challenge combines tagged enums, `Result`
with custom errors, owning arrays, recursion and checked arithmetic. There is
almost no scaffolding: you design the data structures.

Write `evaluate(text)` in `src/calc.dc` (you may add more modules) and the
report in `src/main.dc`. The error type is given:

```dc
public enum CalcError {
    @implicit_construction Alloc(AllocError),
    InvalidCharacter(usize),   // payload: position of the character
    UnexpectedToken(usize),    // position of the token
    UnexpectedEnd(usize),      // always text.len
    DivideByZero(usize),       // position of the / or %
    Overflow(usize),           // see below
}
```

Positions are 0-based character indexes into `text`.

## The language

All numbers are `i64`. The grammar, from loosest to tightest:

```text
expression = term { ("+" | "-") term }
term       = unary { ("*" | "/" | "%") unary }
unary      = "-" unary | primary
primary    = number | "(" expression ")"
```

- Binary operators are left associative: `7 - 3 - 2` is 2.
- A number is one or more decimal digits (leading zeros are fine, no sign,
  no decimal point). Spaces and tabs may appear between any two tokens, and
  before the first and after the last.
- `/` truncates toward zero and `%` keeps the sign of the left operand, as in DC.
- A literal larger than 9223372036854775807 is an `Overflow` at the position of
  its first digit. Writing the smallest value takes an expression:
  `0 - 9223372036854775807 - 1`.

## Errors, in this order

The text is processed in three phases and the first phase that finds a problem
decides the result, even if a later phase would have found one earlier in the text.

1. **Lexical.** The whole text is cut into tokens first. The first problem in
   text order is reported: `InvalidCharacter(p)` for any character that is not a
   digit, one of `+ - * / % ( )`, a space or a tab, or `Overflow(p)` for a literal
   that is too large.
2. **Syntax.** The tokens must match the grammar and be consumed completely.
   `UnexpectedToken(p)` points at the first token that cannot continue the
   expression (`2 3`, `* 3`, `()`, `1 )`, `(1 + )`). `UnexpectedEnd(text.len)` when
   the tokens run out while the expression is still incomplete (`""`, `2 +`,
   `(1`, `-`). The position counts trailing blanks: `"2 + "` ends at 4.
3. **Evaluation.** Operations run in the order the grammar nests them: for
   `a op b` the left operand is evaluated completely, then the right operand, then
   the operation. The first operation that cannot be performed is reported:
   - `DivideByZero(p)` for `/` or `%` with a zero right operand, `p` the position
     of the operator,
   - `Overflow(p)` when the exact result of `+`, `-`, `*` or `/` does not fit in
     an `i64` (this includes the smallest value divided by -1, and the smallest
     value times -1), `p` the position of the operator,
   - `Overflow(p)` for unary minus on the smallest value, `p` the position of that
     minus sign.
   - The smallest value `%` -1 is 0, not an error.

DC integer arithmetic wraps silently, so you must test for overflow *before* you
perform an operation. The checks are only comparisons: for example `a + b` overflows
when `b > 0 && a > MAX - b` or `b < 0 && a < MIN - b`. Think about how to check a
product without overflowing while checking it.

An allocation failure from a push is `CalcError::Alloc`: `push(...)?` converts it.

## Limits

Expressions of a thousand terms in a row (`1+1+...+1`), 200 nested groups and 501
unary minus signs in a row must work. Every temporary array must be released.

## The report

`src/main.dc` holds the list of expressions. Print one line for each, the text
in square brackets:

```text
[1 + 2 * 3] = 7
[2 +] error: unexpected end at 3
[4 $ 4] error: invalid character at 2
```

The messages are: `invalid character`, `unexpected token`, `unexpected end`,
`division by zero`, `overflow` and `out of memory`, followed by `at` and the position
(`out of memory` has no position).

## Acceptance criteria

- The project builds with `dpm build`.
- Every check passes, including the exact report in `main`.
