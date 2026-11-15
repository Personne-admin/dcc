# Why the solution works

```dc
public bool is_working_day(i32 day) {
    return in_range(day, 1, 7) && !is_weekend(day);
}
```

- A comparison is an expression of type `bool`, so `return age >= 18;` needs
  no `if`. Writing `if age >= 18 { return true; } return false;` says the
  same thing in more words.
- `in_range` includes both ends: `>=` and `<=`. With `>` or `<` the values
  `low` and `high` themselves would be rejected, which the boundary checks
  catch.
- `is_working_day` is built from the two earlier functions. The first part
  rejects 0 and 8, the second removes 6 and 7. `!` flips the answer of
  `is_weekend`.
- The leap-year rule has three levels. `year % 100 != 0 || year % 400 == 0`
  is the exception to the exception, so it sits in parentheses and reads as
  one unit: "divisible by 4, and (not a century year, or a multiple of 400)".
  Here `&&` would bind first anyway, but code should not make a reader
  remember the precedence table.
