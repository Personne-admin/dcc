# Why this works

A queue needs both ends. A singly linked list with only a head makes
`push` walk to the end; keeping a `tail` pointer makes both ends constant
time. The price is an invariant to maintain: `tail` is null exactly when
`head` is null, and otherwise `tail.next` is null. Every special case in the
code (pushing onto an empty queue, popping the last node) is the place where
that invariant could break, and each of them sets both pointers.

Ownership is the second invariant: every node is created by `push` and freed
by exactly one `pop` or `clear`. That is what the counting allocator checks.
`pop` copies `value` and `next` before freeing, because after
`destroy_item(node)` the node is not yours any more. `clear` is `pop` in a
loop, so there is one place that frees.

`push` fails cleanly because it does its only fallible step, the allocation,
first. When `create_item` returns an error nothing has been linked, `len` has
not changed and no node exists, so the caller sees the same queue as before
and the same count of allocations and frees. Doing the allocation last
(after bumping `len`, say) would leave a queue that claims an item it does not
have.

Taking the allocator as a type parameter is the library's convention. The
queue works with the heap, a fixed buffer, an arena or the test allocators in
the harness without a single change, and a test can observe every allocation.
That is how a leak becomes an assertion instead of a suspicion.
