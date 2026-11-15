# Why the solution works

```dc
public []const char grade(i32 score) {
    if score >= 90 {
        return "A";
    } else if score >= 80 {
        return "B";
    } else if score >= 70 {
        return "C";
    }
    return "F";
}
```

- The chain is tested top to bottom. A score of 95 satisfies every condition,
  but the first one wins, so it returns `"A"`. Because each later test only
  runs when the earlier ones failed, `score >= 80` really means "from 80 to
  89". Testing the lowest bound first would give every passing score a `"C"`.
- `>=` includes the boundary. `grade(90)` is `"A"` and `grade(89)` is `"B"`;
  using `>` would move 90 to the lower grade.
- Branches that end in `return` need no `else` for the final case. Whatever
  is left after the chain (`return "F";`) is the "otherwise" answer, and
  the compiler checks that every path returns a value.
- `clamp` uses two independent early returns, because a value cannot be
  both below `low` and above `high` when `low <= high`. The last line returns
  the value unchanged, which is the case where it is already inside the range.
- Text results are compared in the checks with a library function, because
  `==` on two `[]const char` slices is rejected by the compiler.
