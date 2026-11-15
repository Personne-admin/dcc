## Hint 1

Work one stage at a time and run the checks after each: the objective list
shows which stage is current. Every stage has hints of its own, too.

## Hint 2

The stages build on each other. `apply` uses `forward`, `turn_left` and
`turn_right`; `apply_in_grid` uses `forward` and `inside` again and
`run_in_grid` uses `apply_in_grid`. Write each helper carefully once and
call it.

## Hint 3

In `apply_in_grid` move one cell at a time: `Robot next = forward(robot, 1);`,
look at `inside(grid, next.x, next.y)`, and either count a bump and `break`
or accept `robot = next;`. The counter is changed through the pointer with
`*bumps += 1;`.
