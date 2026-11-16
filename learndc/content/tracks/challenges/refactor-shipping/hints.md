## Hint 1

Write the module first and test it with the harness; the program keeps working with the old code until you switch `main` over.

## Hint 2

Each `if code == n` chain becomes one `match speed` with one arm per variant and no `_` arm, so the compiler checks that every speed is covered.

## Hint 3

In `main`, import `shipping`, call `show(Speed::Standard, 3)` and friends, and print the names with a `match` whose arms call `print`.
