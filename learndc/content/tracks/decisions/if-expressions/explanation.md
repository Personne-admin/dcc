# Why the solution works

```dc
public i32 sign(i32 n) {
    return if n < 0 { -1 } else if n == 0 { 0 } else { 1 };
}
```

- The whole `if ... else ...` is one expression whose value is the value of
  the branch that was chosen, so it can follow `return` directly. No variable
  has to be declared first and set later in two places.
- Every branch ends in a value without a semicolon, and all the values have
  the same type (`i32` here, `[]const char` in `item_word`). That is what lets
  the compiler give the whole expression a type.
- `else if` chains work as in statements: the conditions are tested from the
  top and the first true one wins. The final `else` covers everything left,
  here the positive numbers.
- `max_of(5, 5)` returns `b`, because `a > b` is false for equal values. Both
  choices are correct there; checking equal inputs shows that no case falls
  through.
