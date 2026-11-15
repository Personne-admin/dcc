## Hint 1

Work one stage at a time and run the checks after each. For every step ask
the same two questions: does it produce a value, and does the caller need to
know why it failed? That decides between `Optional`, `Result` and `Status`.

## Hint 2

`split_line`, `parse_port` and `parse_flag` are small and independent.
`apply_line` combines them and `load` calls `apply_line` once per line, so a
bug in an early stage shows up again in the later ones.

## Hint 3

Write the loops with an index (`for usize i = 0; i < text.len; i++`) and use
slices such as `text[start..i]` for the pieces. In `apply_line`, when a
`match` arm needs to return a `Problem` that carries a value, put it in a
local variable first (`Problem problem = Problem::BadPort(why);`) and return
that variable.
