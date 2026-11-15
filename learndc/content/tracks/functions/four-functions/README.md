# Writing functions

A function gives a name to a piece of work. Its signature lists the return
type, the name and the typed parameters:

```dc
public i32 triple(i32 n) {
    return n * 3;
}
```

`return` hands a value back to the caller, which must match the return type.
`public` makes the function visible to other files. `main.dc` uses
`numbers.dc` through the scaffolding lines `import numbers;` and
`using numbers::*;`; the Modules chapter explains how they work, so leave
them as they are. A function can call any other function, including one
declared earlier in the same module.

Useful operators for this exercise: `*` multiplies, `/` divides integers
(dropping the fraction), `+` adds and `%` gives the remainder of a division.
Comparing with `==` produces a `bool`.

## Your task

`src/numbers.dc` declares four functions, but each one returns a placeholder.
Implement them:

| function                      | result                                     |
| ----------------------------- | ------------------------------------------ |
| `square(n)`                   | `n` times `n`                              |
| `sum_of_squares(a, b)`        | the square of `a` plus the square of `b`; reuse `square` |
| `to_fahrenheit(celsius)`      | `celsius * 9 / 5 + 32` as an integer       |
| `is_even(n)`                  | `true` when `n` is divisible by 2          |

`src/main.dc` already calls them and prints the results. Do not change its
output format.

## Acceptance criteria

- The project builds with `dpm build`.
- `learndc check` passes the program output and both function checks,
  including negative numbers such as `square(-3)` and `is_even(-4)`.
