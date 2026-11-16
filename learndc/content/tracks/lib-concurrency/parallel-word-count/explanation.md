# Why the slots make this safe

Every thread writes to exactly one `Job`, and no two threads share a job, so
there is no shared writable memory and no lock is needed. `main` only reads
the `words` fields after `count_in_threads` has joined every thread; a join
guarantees that everything the thread wrote is finished and visible. The
printing loop then walks the jobs in array order: the output reflects the
input order, not the order in which the threads happened to finish.

Joining in a second loop is what makes the work parallel. Spawning a thread
and joining it at once would run the jobs one after another.

One simplification to know about: if the fourth `spawn` failed, `?` would
return while three threads are still running and still pointing at `jobs`.
Production code joins the threads already started before it returns the
error.

`count_words` is a pure function. Keeping the real work in a function with no
threads in it is what made it testable on its own before any thread existed.
