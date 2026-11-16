## Hint 1

Use a `while n != 1` loop; each round changes `n` once and counts one step.

## Hint 2

Test evenness with `n % 2 == 0`. For the peak keep a second variable that you raise whenever `n` exceeds it, starting with `n` itself.

## Hint 3

Both functions share the same loop; `steps` increments a counter, `peak` compares `n` with `highest` after every change.
