## Hint 1

The error message names the problem: slices cannot be compared with `==`.
The fix is not a different operator but a library function that looks at the
characters.

## Hint 2

Add `import std::slice;` after the `module names;` line. Then every
`a == b` between two texts becomes `std::slice::eq(a, b)`. The `if` and
`return` statements around them stay as they are.

## Hint 3

```dc
public bool is_admin([]const char name) {
    return std::slice::eq(name, "admin");
}
```

and in the two loops write `if std::slice::eq(name, who) { ... }`.
