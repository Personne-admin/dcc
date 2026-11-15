# Maybe a value: Optional

Some questions have no answer: what is `5 / 0`? Instead of inventing a magic
number, a function can return `Optional(T)`. It holds either
`Optional::Some(value)` or `Optional::None`:

```dc
Optional(i32) first_even(i32 a, i32 b) {
    if a % 2 == 0 { return Optional::Some(a); }
    if b % 2 == 0 { return Optional::Some(b); }
    return Optional::None;
}
```

`Optional::None` has no payload and is written without parentheses. The
caller handles both cases with `match`, which is an expression and must cover
every variant:

```dc
match first_even(3, 8) {
    Optional::Some(n) => println("even: {}", n)?,
    Optional::None => println("no even number")?,
}
```

A result of `0` is a real answer, so it is `Some(0)`, not `None`.

## Your task

1. In `src/safe_math.dc`, implement `checked_div(a, b)`. It returns `None`
   when `b` is zero and `Some(a / b)` otherwise.
2. In `src/main.dc`, make `show` print the outcome of the division with a
   `match` on `quotient`. The program must print:

```text
7 / 2 = 3
5 / 0 = undefined
-9 / 3 = -3
```

## Acceptance criteria

- The project builds with `dpm build`.
- `checked_div(0, 5)` is `Some(0)`; `checked_div(5, 0)` and `checked_div(0, 0)`
  are `None`.
- The program prints the three lines above and exits with code 0.

Run `learndc check` when you are ready, or `learndc run` to see the output.
