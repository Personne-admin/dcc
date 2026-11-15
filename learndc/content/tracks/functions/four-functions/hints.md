## Hint 1

A function body is a sequence of statements ending in `return value;`. The
value's type must match the return type written before the function name:
`i32` for the numbers and `bool` for `is_even`.

## Hint 2

`square` and `to_fahrenheit` are one expression each. `sum_of_squares` should
call `square` twice instead of repeating the multiplication. For `is_even`,
the remainder `n % 2` is 0 for every even number, and a comparison such as
`n % 2 == 0` already is a `bool`.

## Hint 3

In `src/numbers.dc`, replace the placeholders with:
`return n * n;`, `return square(a) + square(b);`,
`return celsius * 9 / 5 + 32;` and `return n % 2 == 0;`.
