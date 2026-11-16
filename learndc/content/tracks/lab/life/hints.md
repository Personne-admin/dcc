## Hint 1

Take the functions in order; each uses the one before. The harness checks them
separately, so a later function can be written while an earlier one is still
wrong, but the results are only meaningful once the earlier ones pass.

## Hint 2

Draw a 3 by 3 corner of the board on paper and count by hand what a cell with
0, 1, 2 and 3 neighbours should do. Most wrong generations come from counting
the cell itself or from reading outside the board.

## Hint 3

If `step` gives strange boards, check that you write to `next` and only read
`current`. If a glider is wrong after four generations but right after one,
the bug is in `alive_at` at the edges or in `neighbours`.
