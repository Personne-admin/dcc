## Hint 1

A deferred statement runs when the block it is written in ends. The inner
`{ ... }` block ends before `party running` is printed.

## Hint 2

Deferred statements of the same block run in reverse order. Since the music
was switched on after the lights, its `defer` runs first.

## Hint 3

The lines to add, each directly below its partner:
`defer println("lights off");`, `defer println("music off");` and, inside the
braces, `defer println("plates cleared");`.
