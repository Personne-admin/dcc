## Hint 1

One `println` can have several `{}`. Count the holes and give exactly that
many arguments after the format string, in the order the holes appear.

## Hint 2

The guest line has two holes: the name (text) and the table (a number). The
items line starts with a `print` that has no placeholder, followed by a
`println` with two number holes.

## Hint 3

```dc
println("Guest: {}, table {}", "Ada", 7)?;
print("Items: ")?;
println("{} x tea, {} x cake", 2, 1)?;
```

The total and the goodbye line follow the same pattern with one placeholder.
