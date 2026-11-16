# Why this queue works

`open_queue` reserves once, so the 20 pushes and inserts that follow never
allocate and the capacity stays at 20. That is why `spare` can report 20 at
the end: `clear` and `remove` give length back, never capacity.

`serve` is `remove(0)`: it returns the front ticket as an `Optional(u32)` and
shifts the others left, so the order is kept. The `None` for an empty queue is
the normal "nobody is waiting" case, not an error. `cancel` is `remove` too,
guarded by `index_of`; `swap_remove` would be faster but would let the last
ticket jump the queue, which a queue must never do.

Every helper takes `Array(u32)*`: the caller owns the memory and the one
`defer queue.deinit();` in `main` releases it once, however the program
ends.
