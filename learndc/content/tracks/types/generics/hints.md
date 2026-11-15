## Hint 1

The compiler cannot pass `2.5` where an `i32` is expected. Look at how
`smaller` is declared: it does not name a concrete type at all.

## Hint 2

A generic function declares its type parameter in a first pair of
parentheses after the name, `name(T)`, then lists the ordinary parameters in
a second pair. `T` can then be used as the type of the parameters and of the
return value.

## Hint 3

In `src/pick.dc`, change the first function's header from
`public i32 larger(i32 a, i32 b)` to `public T larger(T)(T a, T b)`. The body
stays exactly as it is.
