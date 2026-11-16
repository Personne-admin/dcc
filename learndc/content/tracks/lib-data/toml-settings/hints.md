## Hint 1

`text_at` already shows the pattern: `get_path` gives an Optional, return the
fallback when it is empty, and otherwise convert the value. `int_at` is the
same with `as_integer`. `port_at` is `int_at` plus a range test.

## Hint 2

For `print_routes`, `table.get("route")` is the array (an Optional). Turn it
into a slice with `as_array().some_or({})`, loop over it, and turn each
element into a table with `as_table().some_or({})`. Test
`fields.get("path").is_none()` and `continue` to skip a route with no path.

## Hint 3

```dc
for route in routes.as_array().some_or({}) {
    Table fields = route.as_table().some_or({});
    if fields.get("path").is_none() {
        continue;
    }
    println("route {} -> {}", text_at(&fields, "path", ""),
            text_at(&fields, "target", "(no target)"))?;
    *printed += 1;
}
```
