## Hint 1

Work one stage at a time and run the checks after each. For every function,
ask first whether you know how many passes the loop needs: if yes use `for`
with a range, if not use `while` with the right condition.

## Hint 2

Trace a small input by hand with a table before you run anything: `digit_sum(123)`,
`is_prime(9)`, `collatz_steps(6)` and `gcd(12, 18)`. When a check fails, use the
`expected` and `got` lines to find the smallest failing input and trace that.

## Hint 3

`digit_sum` is a `while n > 0` loop adding `n % 10` and dividing by 10.
`is_prime` returns `false` early for `n < 2`, then tries every `d` in `2..n`
and returns `false` as soon as `n % d == 0`. `collatz_steps` is `while n != 1`
with an even/odd `if`. `gcd` is `while b != 0` swapping in the remainder.
In `main`, one `for i32 n in 1..=15` loop prints the rows.
