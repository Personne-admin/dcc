## Hint 1

Start with `dpm test` and read the first FAIL line: it names the check, the value you got and the value that was wanted. A function a test calls must exist before the test can compile.

## Hint 2

In `total_tons`, look at the line inside the loop: it assigns to `sum` instead of adding to it. For `lightest_tons`, start with the first ship's tons as the best so far and replace it whenever a ship is lighter.

## Hint 3

`sum += ship.tons;` fixes the total. `lightest_tons`: `i32 best = FLEET[0].tons;`, then `for ship in FLEET { if ship.tons < best { best = ship.tons; } }`, then `return best;`.
