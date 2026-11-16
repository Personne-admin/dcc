## Hint 1

`average(values)` gives the same answer in every round of the loop: it does not depend on `i`.

## Hint 2

Compute the average once before the loop and keep it in a local variable.

## Hint 3

Write `i32 mean = average(values);` above the loop and compare `probe::read(values, i) > mean`.
