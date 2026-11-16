# Why the simulation works

The flat array is the oldest trick for grids: one row after another, with
`row * WIDTH + col` turning two coordinates into one index. Everything else
is built from three small pure functions. `next_state` is only the rule,
`alive_at` hides the edge of the board, and `neighbours` loops over the eight
offsets around a cell. Because `alive_at` answers false outside the board, the
counting loop needs no special cases at the edges; the check lives in one
place.

`step` needs two arrays because the rules describe a simultaneous update:
every cell looks at the old board. Writing the result into the same array
would let half of the board see the new generation of the other half and the
glider would break up. Copying the whole array back with `a = b;` is cheap for
64 cells and keeps the loop in `main` simple.

`render` and `main` are separate on purpose: drawing and counting populations
are the only parts that print, so every rule above can be tested without
reading any output. The bonus uses the same `step` on private copies of the
board to answer a different question, which shows why small functions that do
one thing combine so well.
