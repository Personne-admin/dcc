## Hint 1

A comparison already is a `bool`, so most of these functions are a single
`return` of one expression. Combine two conditions with `&&` (both must hold)
or `||` (one is enough) and negate one with `!`.

## Hint 2

`in_range` needs both a lower and an upper comparison joined by `&&`.
`is_working_day` is "a valid day" and "not a weekend", so call `in_range`
and `is_weekend`. For the leap year, divisibility by 4 is required; the
exception (`% 100 != 0`) and its exception (`% 400 == 0`) are joined by `||`
and wrapped in parentheses.

## Hint 3

`age >= 18`; `n >= low && n <= high`; `day == 6 || day == 7`;
`in_range(day, 1, 7) && !is_weekend(day)`;
`year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)`.
