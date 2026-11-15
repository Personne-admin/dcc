# Why this loader works

Each step uses the smallest type that carries enough information.
`split_line` and `parse_flag` return `Optional`: when the line has no `=` or
the flag is neither `true` nor `false`, there is nothing more to say.
`parse_port` returns a `Result` because an empty text, a stray character and
a number that is too big are different problems, and the report prints a
different message for each.

`parse_port` checks the limit after every digit. A very long number is
rejected as `TooLarge` before the `u32` can wrap around, which is exactly the
kind of silent mistake the old loader made with a plain `return 0`.

`apply_line` is the only function that knows which key means what. It
returns a `Status(Problem)`: there is no value to hand back, it only works
or fails. Because it decides before it stores, a rejected line leaves the
old setting in place.

`load` adds the one thing a single line cannot know, its position. It wraps
the `Problem` in a `LoadError` together with the line number and stops at the
first failure, so the caller sees the earliest problem.

`report` and `describe` turn every variant into a sentence. The matches must
cover every variant, so adding a new `Problem` later makes the compiler point
at the report that needs a new message. The nested `match` on the
`NumberError` inside `BadPort` keeps the match on `Problem` flat and
exhaustive.
