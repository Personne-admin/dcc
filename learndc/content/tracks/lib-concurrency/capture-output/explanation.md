# Why the write end is closed twice

A pipe is shared by whoever holds one of its ends. After `spawn` two processes
hold the write end: the child (on its standard output) and your program (the
handle you passed to `stdout`). The reader is told "no more data" only when
all holders have let go. The child lets go by exiting; your program has to let
go explicitly, immediately after the spawn, which is why `pipe.writer.close()`
comes before `read_all`.

The `defer` lines are the safety net for the other exits: a program that
cannot be started returns from `spawn` with an error, and the two deferred
closes release both ends; `close` ignores an end that is already closed, so
the explicit close and the deferred one do not clash. `command.deinit()` is
registered first and so runs last, after the child has been waited for.

Reading comes before waiting. A child that writes more than the pipe can
buffer blocks until someone reads; waiting for it first would wait for a
program that waits for us. The status is only meaningful afterwards:
`failing` shows that a program can print `partial` and still exit with 4,
and that the output of a failed program is still data.
