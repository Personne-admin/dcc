# The Robot Courier

Robot B-7 drives on a grid. Columns are numbered by `x` from 0, rows by `y`
from 0, `x` grows to the East and `y` grows to the North. `src/courier.dc`
holds the types and one stub per function; `src/main.dc` holds tonight's
route.

```dc
public enum Facing { North, East, South, West }

public struct Robot {
    i32 x;
    i32 y;
    Facing facing;
}

public enum Command {
    Forward(i32),
    Left,
    Right,
}

public struct Grid {
    i32 width;
    i32 height;
}
```

Each stage below is checked on its own, so you can clear them in order and
watch them light up. All functions take the `Robot` by value and return the
changed copy unless the stage says otherwise.

## Stage 1: Read the old firmware

A question about the old `turn_right`. Answer it before you write your own.

## Stage 2: Turn left and right

- `turn_left(facing)` returns the facing after a quarter turn to the left:
  North becomes West, West becomes South, South becomes East, East becomes
  North.
- `turn_right(facing)` is the opposite turn.

Write each as a `match` with one arm per variant and no `_` arm.

## Stage 3: Move forward

`forward(robot, distance)` returns the robot moved `distance` cells in the
direction it faces. North adds to `y`, East adds to `x`, South subtracts from
`y` and West subtracts from `x`. The facing does not change. There are no
walls yet.

## Stage 4: Run a program

- `apply(robot, command)` executes one command: `Forward(n)` moves, `Left`
  and `Right` turn in place.
- `run(robot, program)` executes every command of the slice in order and
  returns the final robot. An empty program returns the robot unchanged.

## Stage 5: Stay inside the grid

- `inside(grid, x, y)` is true for the cells with `0 <= x < width` and
  `0 <= y < height`.
- `apply_in_grid(robot, grid, command, bumps)` works like `apply`, but a
  `Forward(n)` moves one cell at a time and stops in front of a wall. When
  the move is cut short, add 1 to `*bumps`: one bump per cut-short command.
  Arriving exactly at the last cell is no bump. Turning never bumps.
- `run_in_grid(robot, grid, program, bumps)` does the same for a whole
  program, passing the same pointer along.

`bumps` is an `i32*`: the caller owns the counter and the function adds to
it.

## Stage 6: Print the route report

Complete `main` in `src/main.dc`: run every command of the program with
`apply_in_grid`, number the steps from 1 and print one line after each
command, then a summary line. The helper `print_facing` is provided in
`courier.dc`; it prints the facing name without a newline.

```text
route on a 5 x 4 grid
step 1: (0, 2) facing North
step 2: (0, 2) facing East
step 3: (4, 2) facing East
step 4: (4, 2) facing North
step 5: (4, 3) facing North
step 6: (4, 3) facing West
step 7: (0, 3) facing West
step 8: (0, 3) facing South
step 9: (0, 0) facing South
finished at (0, 0) after 9 commands with 2 bumps
```

## Bonus: back home

`back_home(start, grid, program)` returns `true` when running the program in
the grid ends on the cell the robot started from, whatever its facing. The
bonus is not needed to defeat the boss.
