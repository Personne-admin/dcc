## Hint 1

`Optional(i32)` is an enum with two variants, `Optional::Some(i32)` and
`Optional::None`. A function picks one with `return`, and a `match` reads
whichever one it received.

## Hint 2

In `checked_div`, test the divisor first: when `b == 0` return the empty
variant, otherwise return the quotient wrapped in the value variant. In
`show`, write one `match` arm per variant, and print inside each arm. The
payload of `Some` is bound to a name you choose in the pattern.

## Hint 3

In `src/safe_math.dc`, write `if b == 0 { return Optional::None; }` and then
`return Optional::Some(a / b);`. In `src/main.dc`, replace the `println`
with `match quotient { Optional::Some(value) => println("{} / {} = {}", a, b,
value)?, Optional::None => println("{} / {} = undefined", a, b)?, }`.
