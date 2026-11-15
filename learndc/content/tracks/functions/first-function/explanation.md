# Why the solution works

```dc
public i32 seconds_in(i32 minutes) {
    return minutes * 60;
}
```

- The signature is a promise. `i32 minutes` promises the caller that one
  integer goes in, and the leading `i32` promises that one integer comes out.
  The body only has to keep that promise with a `return`.
- The parameter name is yours to choose. `minutes` explains what the number
  means, which is why `minutes * 60` needs no comment.
- `main` calls `seconds_in(3)` inside `println`. The call is an expression:
  it becomes 180 before `println` fills in the `{}`.
- The functions live in `basics.dc` and are `public`, so the checker can call
  them directly and test them with inputs `main` never uses, such as 0.
