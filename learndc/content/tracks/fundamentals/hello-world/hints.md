## Hint 1

`println` takes a format string first, then one argument for each `{}` in
it. An argument can be a number such as `5` or text in double quotes such as
`"Ada"`.

## Hint 2

You need two `println` calls, one per line of output, each followed by `?`.
Only the first one needs a placeholder.

## Hint 3

In `src/main.dc`, replace `println("Hello, world!")?;` with
`println("Hello, {}!", "Ada")?;` and add `println("Welcome to DC.")?;` on the
next line. Keep `return Status::Ok;` at the end.
