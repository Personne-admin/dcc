## Hint 1

Both loops visit the same elements. What if one loop updated both answers?

## Hint 2

Start both `low` and `high` with the first element, loop from index 1 once, read the element into a variable and compare it with both.

## Hint 3

Read each element into a local `v` with one `probe::read` call, then use `v` in two `if` statements inside the same loop.
