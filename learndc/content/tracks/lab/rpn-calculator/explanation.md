# Why this calculator works

Postfix notation moves all the difficulty of precedence into the order of the
tokens, which is why the evaluator is so short. The program only ever does
one of two things per token: push a number or let an operator consume the top
two values. The `Array(i64)` is the stack; `push`, `pop` and the last element
`items()[len - 1]` are everything it needs.

Each stage has a single responsibility and a precise contract. `parse_number`
hand-parses text, `tokenize` is the word-splitting loop with an array instead of
a counter, and `apply` performs one step on a stack it borrows through a
pointer. Because `apply` checks the token, the depth and the divisor before it
pops, a refusal leaves the stack exactly as it was, so the error can be
reported with the state intact.

`evaluate` is the only function that owns memory, and it releases it with two
`defer` lines placed right after the arrays are created. Every early `return`
and every `?` therefore frees both arrays. The `@implicit_construction`
variant lets `push(...)?` and `tokenize(...)?` report running out of memory
through `CalcError` with no extra code.

Finally the typed error replaces the magic values (0, -1) a calculator without
errors would have to invent: "10 0 /" and "0" are different answers, and
`main` has to decide in a `match` what each failure should say.
