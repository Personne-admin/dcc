## Hint 1

`if condition { first } else { second }` is itself a value. Put it after
`return`, or on the right of `=` in a declaration. There is no semicolon after
the value inside the braces.

## Hint 2

`max_of` compares `a > b` and picks `a` or `b`. `abs_of` picks `-n` when `n <
0`, else `n`. `sign` needs one `else if` in the middle for the zero case.
`item_word` compares the count with 1 and picks one of two text literals.

## Hint 3

`return if a > b { a } else { b };`, `return if n < 0 { -n } else { n };`,
`return if n < 0 { -1 } else if n == 0 { 0 } else { 1 };` and
`return if count == 1 { "item" } else { "items" };`.
