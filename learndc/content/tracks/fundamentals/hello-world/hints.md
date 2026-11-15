## Hint 1

A variable declaration in DC puts the type first: `i32 count = 3;`. The type
of borrowed text such as a string literal is `[]const char`.

## Hint 2

Declare the variable before the first `println`, then pass it as the argument
that fills the `{}` placeholder. Each line of output is its own `println`
call followed by `?`.

## Hint 3

In `src/main.dc`, replace the single `println("Hello, world!")?;` line with
three lines: a `[]const char name = "Ada";` declaration, a `println` whose
format string is `"Hello, {}!"` with `name` as its argument, and a second
`println` for `Welcome to DC.`. Keep `return Status::Ok;` at the end.
