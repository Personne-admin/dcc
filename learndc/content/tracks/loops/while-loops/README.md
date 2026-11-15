# Repeating with while

Some work has to be repeated an unknown number of times: "keep going until
the balance has doubled" or "until no digits are left". A `while` loop repeats
its body **as long as** its condition is true. The condition is tested before
every pass, including the first:

```dc
i32 count = 3;
while count > 0 {
    println("{}...", count)?;
    count -= 1;
}
```

This prints 3, 2 and 1, then the condition `count > 0` is false and the loop
ends. Almost every while loop has three parts:

1. **State before the loop**: `i32 count = 3;`
2. **A condition** that says whether to go on: `count > 0`
3. **An update inside the body** that moves the state towards the end:
   `count -= 1;`

If the update is missing or moves the wrong way, the condition stays true
forever: an *infinite loop*. The checker stops a program after a few seconds
and reports a timeout, so a check that runs out of time is the usual sign of
an endless loop.

To describe a loop, say what it is waiting for and write the opposite as the
condition. "Loop until the balance has doubled" becomes
`while balance < 2 * start`.

Useful operators: `+=` adds to a variable, `/=` divides it (integer division
drops the fraction) and `%` gives the remainder.

## Your task

`src/counting.dc` has three functions that return 0. Implement them with
`while` loops:

| function                      | result                                              |
| ----------------------------- | --------------------------------------------------- |
| `count_digits(n)`             | the number of digits of `n` (for `n >= 0`); `0` has one digit |
| `years_to_double(start, percent)` | the number of years until a balance that starts at `start` and grows by `percent` percent a year reaches twice `start` |
| `first_multiple_above(step, limit)` | the smallest multiple of `step` that is greater than `limit` |

Details:

- `count_digits`: dividing a number by 10 removes its last digit.
- `years_to_double`: each year the balance grows by `balance * percent / 100`
  (whole units; the fraction is dropped). Use inputs with `start >= 100` and
  `percent >= 1`. Count the years in a variable of your own.
- `first_multiple_above`: start at `step` and keep adding `step` while the
  multiple is not above `limit` (`step` is positive and `limit` is at least 0).

`src/main.dc` prints some results; leave it as it is.

## Acceptance criteria

- The project builds with `dpm build`.
- The program output matches and both function checks pass, including `0`,
  `10` and `99` for `count_digits`.
