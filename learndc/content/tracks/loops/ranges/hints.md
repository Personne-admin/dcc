## Hint 1

Count by hand: the output for `n = 5` is 10, which is `1 + 2 + 3 + 4`. The
loop is missing the last number. Think about whether the end of a range is
included.

## Hint 2

`1..n` stops before `n`. DC has a second range form that includes its end,
written with `=`.

## Hint 3

In `src/sums.dc`, change `for i in 1..n {` to `for i in 1..=n {`.
