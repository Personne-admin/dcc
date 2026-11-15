# Why the solution works

```dc
println("Hello, {}!", "Ada")?;
println("Welcome to DC.")?;
return Status::Ok;
```

- The formatter replaces `{}` with the next argument, `"Ada"`, so the first
  line is `Hello, Ada!`. The next exercise stores values like this name in
  variables, so the same format string can greet anyone.
- `println` appends exactly one newline after each line, which is why the
  expected output ends with a newline after `Welcome to DC.`.
- Each `?` checks the status returned by `println`. On success execution
  continues; on a write failure `main` returns that error and the program
  exits with code 1. Reaching `return Status::Ok;` gives exit code 0.
