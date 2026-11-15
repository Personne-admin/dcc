# Why this courier works

Each piece of data has the type that fits it. A `Facing` is exactly one of
four things, so it is an enum and every `match` on it lists four arms with no
wildcard: add a fifth facing later and the compiler points at every turn and
every move that needs a decision. A `Robot` is several values that belong
together, so it is a struct. A `Command` is one of three things and one of
them carries a number, so it is a tagged enum, and the arm
`Command::Forward(distance)` binds that number.

Functions that take the robot by value work on a copy and return it. That
keeps `forward`, `apply` and `run` easy to test: the harness can check that
the robot you passed in is unchanged. The one place that needs the caller's
variable is the bump counter. `apply_in_grid` receives `i32* bumps`, adds to
`*bumps` and every caller decides where the number lives: `main` keeps one
for the whole route, `back_home` a throwaway one.

Moving one cell at a time looks slower than computing the final cell, but
it makes the wall rule trivial: "stop in front of the first cell outside".
The same loop yields the right position and the right bump for every
distance, including 0, and it needs no special case for negative or huge
distances.

`main` has little to do because the types and functions carry the logic: it
loops over the slice, applies each command and prints. `print_facing` prints
inside the match arms instead of returning text, which keeps the printing
code next to the variants it names.
