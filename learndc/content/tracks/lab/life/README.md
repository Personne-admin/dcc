# Project: Game of Life

Needs: loops (including nested loops and `continue`), decisions and the
collections chapter (fixed arrays and slices).

In 1970 John Conway invented a game with no players: a board of cells, each
alive or dead, and one rule applied to every cell at once to produce the next
generation. Surprisingly rich things happen. In this project you build the
simulation and watch a *glider* walk across the board.

The board is 8 by 8, stored in a flat array of 64 `bool` values, row after
row. Cell (row, col) lives at index `row * WIDTH + col`. `src/life.dc`
declares `WIDTH`, `HEIGHT` and `CELLS` and every function you need.

## The rules

For each cell count its live neighbours, the up to eight cells around it:

- a live cell with 2 or 3 live neighbours stays alive, otherwise it dies,
- a dead cell with exactly 3 live neighbours comes to life.

The edge of the board is a wall: cells outside it count as dead. Nothing
wraps around.

## Stage 1: The rules of life

`next_state(alive, neighbours)` returns the state of one cell in the next
generation.

## Stage 2: Read a cell safely

`alive_at(cells, row, col)` returns whether that cell is alive. Row and
column are `i32`, so `-1` can be asked for; everything outside the board is
dead. Check the position before you index the array: a wrong index stops the
program.

## Stage 3: Count the neighbours

`neighbours(cells, row, col)` counts the live cells around a position. The
cell itself is never one of its own neighbours.

## Stage 4: One generation

`step(current, next)` fills `next` with the following generation. `current`
must not change: if you updated one array in place, a cell changed early would
be counted by its neighbours as if it had always been like that.

## Stage 5: Draw the board

`render(cells)` prints eight lines, `#` for a live cell and `.` for a dead one.

```text
.#......
..#.....
###.....
........
```

## Stage 6: Watch a glider

`main` already builds the glider and a second, empty board. For generation 0
to 4 print `generation N, population P`, draw the board, step and copy the new
generation back with `a = b;`. The first two generations of the output:

```text
generation 0, population 5
.#......
..#.....
###.....
........
........
........
........
........
generation 1, population 5
........
#.#.....
.##.....
.#......
```

After four steps the glider has moved one cell down and one to the right.

## Bonus: when does it settle?

`settle(start, limit)` steps a copy of `start` until a generation is equal to
the one after it and returns the number of steps needed (0 for a block, which
never changes), or `limit` when that does not happen in time.
