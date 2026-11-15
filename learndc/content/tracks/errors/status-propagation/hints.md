## Hint 1

`?` can follow any call that returns `Result` or `Status`. On success a
`Result` yields its value, so `left = reserve(left, 30)?;` both stores the new
total and stops the function on failure. A failure returned from `plan` is
only seen by `main` if `main` also uses `?` on the call.

## Hint 2

Repeat the same two statements for each reservation: reserve with `?`, then
print the new total with `println(...)?`. Do not discard errors with
something like `unwrap_or`; that would keep going after a failure. In `main`,
call `plan()?;` and then return `Status::Ok`.

## Hint 3

In `plan`, after the existing `println`, add `left = reserve(left, 30)?;`
followed by `println("reserved 30, {} left", left)?;`. Do the same for 50 and
for 40. In `main`, put `plan()?;` before `return Status::Ok;`.
