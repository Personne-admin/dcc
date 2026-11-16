## Hint 1

A test program is an ordinary file with `public i32 main()`. Declare `i32 failures = 0;` at module level, a small `check` function that prints ok or FAIL and counts, and `return failures;`.

## Hint 2

Test `max` with `{ -5, -2, -9 }` (expect -2) and `{ 3, 9, 4 }` (expect 9), `mean` with `{ 2, 4, 9 }` (expect 5). The negative slice fails: look at where `max` starts its search.

## Hint 3

`max` starts with `best = 0`, so a slice of negatives never beats it. Start from the first element instead: `i32 best = values[0];`, after returning 0 for an empty slice.
