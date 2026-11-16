## Hint 1

Build the stages bottom up and check each one before the next. Cell numbers are
the glue: every function takes or returns them, so get the `row` and `col`
conversions right once and reuse them.

## Hint 2

If a search is wrong, print the distance array of a tiny maze as rows and
compare it with a drawing on paper. If it hangs, a cell is being queued without
being marked.

## Hint 3

`find_path` does not need a second search. The distances already encode every
shortest route: from any cell with distance `d > 0`, at least one neighbour has
distance `d - 1`, and following those neighbours leads back to the start.
