# Why the solution works

`printn` is not a name the compiler knows, so it reported an unknown name and
pointed at `printn`. Names must match exactly, letter for letter; there is no
guessing.

The second statement was missing its `;`. The compiler could only tell when
it reached `return` on the next line and found a new statement starting
before the old one had ended, so the error pointed at `return`, not at the
line that needed the fix. Remember this: a `^` at the start of a line that
looks correct usually means the line above it is unfinished.

Fixing one error and building again is the routine you will use for every
error from now on.
