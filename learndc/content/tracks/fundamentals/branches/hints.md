## Hint 1

`if`, `else if` and `else` form one chain: the first condition that is true
wins and the rest are skipped. Each branch can `return` a value, which ends
the function immediately.

## Hint 2

For `grade`, test the highest score first: `score >= 90`, then `score >= 80`,
then `score >= 70`, and fall through to `"F"`. For `clamp`, handle the two
out-of-range cases with early returns and return `value` at the end.

## Hint 3

In `src/classify.dc`, `describe` is `if n < 0 { return "negative"; } else if
n == 0 { return "zero"; }` followed by `return "positive";`. `clamp` is
`if value < low { return low; }`, `if value > high { return high; }` and
`return value;`.
