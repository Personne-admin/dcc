# Why the solution works

```dc
i32 total = total_price(250, 3);
```

- The error `no matching call for total_price` means the compiler found a
  function with that name but none that accepts the arguments you wrote.
- The `~~~` note printed the one candidate, `total_price(i32, i32)`, with the
  reason `argument count mismatch: expected 2, got 1`. Comparing that
  signature with the call shows the quantity is missing.
- The frame's `^^^` marks the whole call, so the fix belongs inside that
  expression, not in the function. Editing `total_price` to take one
  parameter would also compile, but would break the meaning of the program:
  a total needs both a price and a quantity.
- Calls are checked against signatures at compile time. A wrong number or
  type of arguments never reaches run time, so a diagnostic like this is a
  free test of every call site.
