## Hint 1

`symbol` and `apply` both depend on which variant of `Op` they get, so they
are `match` expressions over `op`. `evaluate` repeats the same action for
every element of a list, so it needs a loop and a variable that remembers
the running value.

## Hint 2

Write `symbol` first: one arm per variant, returning a string literal. In
`apply`, three arms are single arithmetic expressions. The `Div` arm needs
a choice between `acc` and `acc / operand`; an `if operand == 0 { ... } else
{ ... }` expression can be used as the arm's value. In `evaluate`, update the
running value with the result of `apply` on every iteration.

## Hint 3

In `src/calc.dc`: `symbol` and `apply` become `return match op { Op::Add =>
..., Op::Sub => ..., Op::Mul => ..., Op::Div => ..., };`. For `Div` use
`if operand == 0 { acc } else { acc / operand }`. `evaluate` declares
`i32 acc = 0;`, loops with `for step in steps { acc = apply(step.op, acc,
step.operand); }` and returns `acc`.
