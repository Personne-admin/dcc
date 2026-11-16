# Why the arena is built this way

**Addresses, not offsets.** `align_up(base + used, align) - base` aligns the
address the caller will receive. Aligning `used` alone only works when the
buffer itself starts on a boundary, which a local array does not promise;
the checks start arenas at odd addresses to catch exactly this.

**Check, then change.** `alloc_bytes` does every test that can fail before
it touches `used`, `peak` or `allocations`. A refused request therefore leaves
the arena as it was and can be retried or ignored. The comparison
`size > memory.len - start` is written that way, instead of
`start + size > memory.len`, so that a huge `size` cannot wrap around and
look small.

**Padding is part of `used`.** The padding before an aligned block is lost
until `reset`, which is why the order of allocations matters for a tight
arena, and why the lecture on layout told you to put large alignments first.

**Typed helpers guard multiplication.** `count * sizeof(T)` can overflow
exactly like `start + size`. Dividing the largest `usize` by `sizeof(T)`
first turns "would this overflow?" into a comparison that cannot.

**One reset for a whole lifetime.** Everything in a frame is released
together, with no individual bookkeeping. The price is that blocks must not
be used after `reset`: in the frame loop `info` and `particles` are dead once
the frame ends. The `ArenaHandle` bonus shows that the same arena can stand
behind any container, because the contract only asks for `allocate`,
`resize_in_place` and `free`.
