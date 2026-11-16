## Hint 1

Both messages come from the same file in libs/units. The first compares the `module` line with the path of the file; the second is about who may use a function.

## Hint 2

The file is libs/units/src/units/convert.dc, so its module is units::convert. A function that another module calls must carry the keyword that exports it.

## Hint 3

Change the first line to `module units::convert;` and write `public i32 tons_to_kg(i32 tons)`.
