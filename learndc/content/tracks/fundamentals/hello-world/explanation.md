# Why the solution works

```dc
[]const char name = "Ada";
println("Hello, {}!", name)?;
println("Welcome to DC.")?;
return Status::Ok;
```

- `[]const char` is a slice of constant characters: a pointer and a length
  that borrow text without copying or owning it. A string literal already has
  this type, so no conversion is needed.
- The formatter replaces `{}` with the next argument, `name`, so the first
  line is `Hello, Ada!`. Changing the variable changes the greeting without
  touching the format string.
- `println` appends exactly one newline after each line, which is why the
  expected output ends with a newline after `Welcome to DC.`.
- Each `?` checks the status returned by `println`. On success execution
  continues; on a write failure `main` returns that error and the program
  exits with code 1. Reaching `return Status::Ok;` gives exit code 0.
