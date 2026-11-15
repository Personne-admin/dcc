# Branching with if and else

A program often has to choose. `if` runs a block when its condition is
`true`; `else if` and `else` cover the other cases, and only the first
matching branch runs:

```dc
if temperature < 0 {
    return "freezing";
} else if temperature < 20 {
    return "mild";
}
return "warm";
```

The condition needs no parentheses, but the braces are required. The
comparison operators are `==`, `!=`, `<`, `<=`, `>` and `>=`. A function
that returns text can return a different string literal from each branch;
the type is `[]const char`.

Order matters. Branches are tested from top to bottom, so put the most
specific test first and be careful at the boundaries: `score >= 90` and
`score > 90` treat a score of exactly 90 differently.

## Your task

`src/classify.dc` has three functions that return placeholders. Implement:

- `describe(n)` returns `"negative"`, `"zero"` or `"positive"`.
- `grade(score)` returns `"A"` for 90 or more, `"B"` for 80 or more, `"C"` for
  70 or more and `"F"` otherwise.
- `clamp(value, low, high)` returns `low` if `value` is below it, `high` if
  `value` is above it, and `value` otherwise.

`src/main.dc` calls them and prints the results; leave it as it is.

## Acceptance criteria

- The project builds with `dpm build`.
- The program output matches, and both function checks pass, including the
  boundary scores 90, 89, 80, 79, 70 and 69.
