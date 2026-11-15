# Why the solution works

```dc
i32 width = 12;
i32 height = 5;
i32 area = 0;

area = width * height;
```

- `"12"` is text of type `[]const char`, so it cannot initialize an `i32`.
  The digit characters in quotes are not a number. Writing `12` creates an
  integer, which matches the declared type.
- Assigning a value to a variable never changes its type. `area` stays an
  `i32`, and `width * height` is an `i32` too, so the assignment is accepted.
- The compiler cannot know that a rectangle is 12 wide and 5 high. The swapped
  values compiled fine; only reading the output reveals them. Types prevent
  one class of mistakes, and checking results prevents the rest.
- `println` fills each `{}` with the next argument in order, so the report
  follows the variables you changed without editing the format strings.
