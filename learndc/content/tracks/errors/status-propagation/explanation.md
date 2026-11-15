# Why the solution works

```dc
left = reserve(left, 30)?;
println("reserved 30, {} left", left)?;
```

- `reserve(...)` returns `Result(usize, Error)`. On `Ok`, `?` produces the
  `usize` and the assignment stores it. On `Err`, `?` makes `plan` return
  that same error immediately, so the statements after it never run.
- The third reservation asks for 40 units with only 20 left, so `reserve`
  returns `Err`. `plan` leaves before the third `println`, which is why that
  line never appears.

```dc
public Status(Error) main() {
    plan()?;
    return Status::Ok;
}
```

- `plan()` returns `Status(Error)`; `?` on it forwards the failure out of
  `main`. Without the `?`, the failed status would be dropped, `main` would
  return `Status::Ok` and the program would wrongly exit with code 0.
- The hosted startup turns `Status::Ok` into exit code 0 and any error into
  exit code 1. It does not print the error, so a program that wants a message
  prints one before it fails.
- `?` only needs the callee's error type to match the caller's, or to convert
  into it. Here everything uses `Error`, so the error travels unchanged.
