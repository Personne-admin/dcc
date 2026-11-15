## Hint 1

Start with the first `error:` line and its `-->` position. Read what the
message says is wrong, then look at the line it points to and at the line
just above it.

## Hint 2

There are two mistakes: one name is misspelled, and one statement is missing
its ending. The second error points at the line *after* the broken statement.

## Hint 3

Change `printn` to `println`. Then add a `;` after the `?` at the end of the
second `println` line, so it reads `println("Every error has a line number")?;`.
