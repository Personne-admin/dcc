## Hint 1

Think of the four files as a pipeline: `words` makes the array, `stats`
measures it, `report` prints it and `main` owns it. Write and test one module
at a time; the stage list shows which one is current.

## Hint 2

`split` is the heart of the project: every other stage starts from an array
of words. Two variables carry it: where the current word began and whether you
are inside one. Write it first, then the simple loops of `stats`, then the Map.

## Hint 3

For the report, `main` writes `Array([]const char) found = {};`, `defer
found.deinit();`, then `words::split(&found, text)?;` and
`report::print_summary(&found)?;`. Inside `print_summary` the numbers come from
`words.len()` and the `stats` functions.
