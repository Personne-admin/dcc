## Hint 1

A library manifest is the same struct with fewer fields: name and version are enough. Its modules live under src/<package name>/.

## Hint 2

libs/ports/project.dcon needs `name = "ports"` and `version`. position can be a chain of `if port == 1 { return 35; }` statements ending in `return 0;`; distance subtracts the two positions and flips a negative result.

## Hint 3

Add `dependencies = { { location = "path:libs/ports" } },` to the harbor manifest. distance: `i32 d = position(a) - position(b); if d < 0 { d = -d; } return d;`.
