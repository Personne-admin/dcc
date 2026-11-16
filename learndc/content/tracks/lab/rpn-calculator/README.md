# Project: RPN Calculator

Needs: the memory chapter (owning `Array`, `defer`, pointers to arrays), the
errors chapter (`Result`, `Status`, custom errors) and the text slices lesson.

*Reverse Polish notation* writes the operator after its operands:
`3 4 +` instead of `3 + 4`. It needs no parentheses and no precedence rules,
because a stack does the bookkeeping: a number is pushed, and an operator pops
two values, computes and pushes the answer.

```text
5 1 2 + 4 * + 3 -

token  stack after
5      5
1      5 1
2      5 1 2
+      5 3
4      5 3 4
*      5 12
+      17
3      17 3
-      14
```

You build the whole pipeline in `src/rpn.dc`: text, tokens, numbers, operators,
the evaluator and a report in `src/main.dc`. All values are `i64`.

```dc
public enum CalcError {
    @implicit_construction Alloc(AllocError),
    BadToken, StackUnderflow, DivideByZero, EmptyExpression, LeftOverValues,
}
```

## Stage 1: Think in postfix

Two questions. Do them on paper first.

## Stage 2: Parse a number

`parse_number(token)` returns `Optional(i64)`. A number is an optional leading
`-` and then 1 to 18 digits and nothing else (no `+`, no decimal point, no
trailing characters). `"-"` alone is not a number. Everything else is `None`.

## Stage 3: Cut text into tokens

`tokenize(out, text)` pushes one slice of `text` for every group of non-blank
characters onto the array `out`, in order. Only the space character separates.
It appends to `out` and never clears it, and the push can fail:
`Status(AllocError)`.

## Stage 4: Apply an operator

`apply(stack, token)` supports `+`, `-`, `*`, `/` and `%`. The two top values
are the operands, the lower one on the left: `7 3 -` is `7 - 3`. Division and
remainder truncate toward zero, like everywhere in DC. The refusals, checked
in this order and always leaving the stack untouched:

1. `BadToken` if the token is not an operator,
2. `StackUnderflow` if fewer than two values are on the stack,
3. `DivideByZero` for `/` or `%` with a right operand of 0.

## Stage 5: Evaluate an expression

`evaluate(text)` returns `Result(i64, CalcError)`. It creates its own token
array and stack (and releases both on every exit), pushes numbers, applies
anything that is not a number as an operator, and finally requires exactly one
value on the stack: otherwise `EmptyExpression` (no tokens at all) or
`LeftOverValues`. The first error wins.

## Stage 6: The calculator desk

`main` evaluates every expression in its list and prints the answer, or
`[expression]: message` for a refusal, then how many succeeded and failed.
Messages: `unknown token`, `stack underflow`, `divide by zero`,
`empty expression`, `more than one value left`, `out of memory`.

```text
3 4 + = 7
[10 0 /]: divide by zero
[]: empty expression
7 evaluated, 5 failed
```

## Bonus: stack words

Extend `apply` with the words `dup` (copy the top value), `neg` (change the
sign of the top value) and `swap` (exchange the top two). They underflow like
operators when the stack is too small. `6 dup *` is 36.
