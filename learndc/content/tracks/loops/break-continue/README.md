# Skipping and stopping

Two small keywords change how a loop runs:

- `continue` skips the rest of the **current pass** and jumps to the next one.
- `break` **leaves the loop** immediately; the program carries on after it.

```dc
for i32 n in 1..=10 {
    if n % 2 == 0 {
        continue;
    }
    if n > 7 {
        break;
    }
    println("{}", n)?;
}
```

This prints 1, 3, 5 and 7. For 2, 4, 6 and 8 the `continue` skips the
`println`. When `n` reaches 9 the second test leaves the loop (and 10 is never
visited).

```text
   +--> test the condition / take the next value -----> loop finished
   |                  |
   |                  v
   |              run the body
   |           continue |  | break
   +--------------------+  +--------------------------> after the loop
```

## Loops that run until told to stop

`while true { ... }` has a condition that is never false, so the only way out
is a `break` (or a `return`). Use it when the moment to stop is found in the
middle of the body, and make sure that moment can really happen:

```dc
i32 n = 1;
while true {
    n *= 2;
    if n > 100 {
        break;
    }
}
```

## Careful with continue in a while loop

`continue` skips the *rest* of the body, including an update at the bottom:

```dc
i32 i = 0;
while i < 10 {
    if i % 2 == 0 {
        continue;
    }
    i += 1;
}
```

Here `i` stays 0, `continue` runs every time and the loop never ends. Update
the variable before the `continue`, or use a `for` loop, which updates for you.

## Your task

`src/search.dc` has three functions that return 0. Implement them:

| function                    | result                                                        |
| --------------------------- | ------------------------------------------------------------- |
| `sum_skipping_threes(n)`    | the sum of `1..=n` without the multiples of 3; use `continue` |
| `terms_to_reach(target)`    | how many terms of `1 + 2 + 3 + ...` are needed until the sum is at least `target` (`target >= 1`); use `while true` and `break` |
| `smallest_divisor(n)`       | the smallest divisor of `n` that is greater than 1 (`n >= 2`); a prime is its own smallest divisor; loop over `2..=n` and use `break` |

For example `terms_to_reach(10)` is 4, because 1 + 2 + 3 + 4 is the first sum
that reaches 10. Remember that `for i32 i in 1..=n` gives an `i32` variable.

`src/main.dc` prints some results; leave it as it is.

## Acceptance criteria

- The project builds with `dpm build`.
- The program output matches and both checks pass.
