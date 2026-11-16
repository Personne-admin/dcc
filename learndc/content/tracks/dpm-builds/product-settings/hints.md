## Hint 1

Product entries are structs: besides `name` and `entry` they take `settings`, the same struct the project uses.

## Hint 2

`optimize` takes an `Optimize` value (`O0`, `O1`, `O2` or `Size`). Write it inside the `settings = { ... }` of the `crunch` entry.

## Hint 3

`{ name = "crunch", entry = "src/crunch.dc", settings = { optimize = Optimize::O2 } },`
