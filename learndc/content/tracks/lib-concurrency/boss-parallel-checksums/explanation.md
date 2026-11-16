# Why the report never changes

Three decisions make a parallel program reproducible.

**The work is a pure function.** `summarize` takes text and returns numbers;
it knows nothing about threads. Anything tested on one thread is also correct
on eight, because the tasks do not share anything they write: each task owns
one `Job` slot.

**The shared parts are tiny and locked.** The only shared writable data is the
progress counter, and both functions that touch it hold the mutex with a
`defer` unlock beside it. The check hammers it from four threads, 20000 times:
an unlocked version loses updates every run. Locking only what is shared keeps
the pool fast, because the expensive work (reading the text) runs outside any
lock.

**Order is imposed after the fact.** The tasks finish in any order; the
report does not care, because `main` prints from the jobs array, in document
order, after `wait_all` guarantees that all results exist. The tie rule in
`busiest` is part of the same discipline: with equal word counts the first
document wins, so the answer does not depend on which task finished first.

The task pointers are the one thing to treat carefully. The pool holds the
addresses of `Task` values until each runs; resizing the array once before
taking addresses keeps them stable, while pushing as you go could move them
under the pool's feet. That is the same rule as for any borrowed pointer into
an `Array`.

In the bonus, a child program joins the picture: `wc -l` is a second opinion
that shares no code with yours. Its two pipes follow the rule from the capture
lesson: each pipe end belongs to exactly one side, and the other side must
close its copy or the reader never sees the end of the data.
