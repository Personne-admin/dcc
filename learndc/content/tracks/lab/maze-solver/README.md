# Boss Fight: The Maze Solver

Needs: the memory chapter (owning `Array`, `defer`, pointers) and the errors
chapter (`Optional`, `Result`, `Status`, custom errors).

A maze is a list of text rows. `#` is a wall, `.` is floor, `S` is the start and
`E` the exit:

```text
#########
#S..#...#
#.#.#.#.#
#.#...#E#
#.#####.#
#.......#
#########
```

A cell is named by one number, `row * width + col`, where `width` is the
length of the first row. In the maze above, `S` is cell 10 and `E` is cell 34.
You can step to the cell above, to the right, below and to the left, but not
through a wall and not off the edge. Nothing wraps around.

The route is found with a *breadth-first search*: look at all cells 1 step from
the start, then all cells 2 steps away, and so on. The first time the search
reaches the exit it has used the fewest possible steps. Two arrays do all the
work: a *queue* of cells to visit and a *distance* array that also remembers
which cells were already seen.

```dc
public enum MazeError {
    @implicit_construction Alloc(AllocError),
    NoStart, NoExit, NoPath,
}
```

`src/maze.dc` declares every function; each returns a placeholder. `rows` is
always `[]const []const char`. Each stage below is checked on its own.

## Stage 1: Read the old solver

A question about the apprentice's search. Read it before writing yours.

## Stage 2: Find a cell

`locate(rows, target)` returns the cell number of the first occurrence of
`target`, scanning the rows from top to bottom and each row from left to right,
or `None`.

## Stage 3: Look around

`open_neighbours(rows, cell, out)` pushes onto `out` every neighbour that is
inside the maze and not a wall, in the order up, right, down, left. It appends
and does not clear `out`.

## Stage 4: Breadth first

`distances(rows, start, dist)` resets `dist` to one entry per cell, all `-1`,
sets `dist[start]` to 0 and runs the search. Afterwards `dist[c]` is the number
of steps from `start` to cell `c`, and stays `-1` for walls and cells that
cannot be reached. A queue is an `Array(usize)` and a `head` counter: push
at the end, read at `head`, and add 1 to `head`.

A cell is marked the moment it is queued, never later.

## Stage 5: How far is the exit?

`shortest(rows)` returns `Result(i32, MazeError)`: the number of steps from `S`
to `E`. Errors, in this order: `NoStart`, `NoExit`, `NoPath` (the exit exists
but cannot be reached). Run `distances` from the start and read the exit's entry.

## Stage 6: Walk it back

`find_path(rows, path)` clears `path` and fills it with the cell numbers of a
shortest route from `S` to `E`, both included, in travel order. It reports the
same three errors as `shortest`. You need no extra bookkeeping: standing on a
cell with distance `d`, one of its neighbours has distance `d - 1`. Starting
at the exit, repeatedly move to such a neighbour, then reverse the result.

When several neighbours qualify, take the first in the order up, right, down,
left. This makes the route unique, and it is the route the castle drawing
below shows.

## Stage 7: Draw the route

`draw(rows, path)` prints the maze with `*` on every route cell that is
floor (`.`); walls, `S` and `E` stay as they are. `main` calls `explore` on
four mazes; implement `explore` to print `maze N: K steps` and the drawing, or
`maze N: no path` / `no exit` / `no start` / `out of memory`:

```text
maze 1: 12 steps
#########
#S**#***#
#.#*#*#*#
#.#***#E#
#.#####.#
#.......#
#########
maze 2: no path
maze 3: no exit
maze 4: no start
```

## Bonus: how deep is the maze?

`farthest(rows)` returns the largest finite distance from `S`, or 0 when the
maze has no start.
