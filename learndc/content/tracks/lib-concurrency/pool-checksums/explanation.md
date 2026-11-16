# Why the pool size does not change the result

Each task reads its own `text` and writes its own `checksum`: there is no
shared state, so the order in which workers pick up jobs is irrelevant. The
printing happens after `wait_all` on the main thread, in array order. Those
two rules (independent tasks, ordered collection) are what make parallel
programs reproducible.

`wait_all` is the join of the pool world. It returns when the queue is empty
and no worker is running a task, so every checksum has been written. Without
it, `main` would race the workers. `destroy` then shuts the workers down and
joins them; putting it in a `defer` means an early `?` return after a failed
`submit` still stops the threads before `jobs` disappears.

The context pointer is the one unsafe seam: `void*` carries no type, so the
cast in the task must match what was submitted. Keeping the job in an array
owned by the caller, outliving `wait_all`, is what keeps the pointers valid.
