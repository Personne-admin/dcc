# Why this works

The heap keeps one invariant: no item comes before its parent. `heap_push`
can break it only at the new leaf, and only against its parent, so sifting
that leaf up along its single path to the root repairs it: each swap moves
the item above a parent that comes after it, and the parent that moved down
is not before the grandparent chain it still sits under. The path has
log2(n) nodes, so a push costs at most that many comparisons.

`heap_pop` removes the root, which leaves a hole. Filling it with the last
item keeps the tree complete, but the new root may come after its children.
Sifting down repairs the invariant on one path: at each level the item is
swapped with the child that comes first, so the child that moves up is before
its sibling and before the item, and the invariant holds above the item.
Choosing the smaller child is what makes that true; swapping with the larger
child would leave the smaller child under a parent that comes after it.

Implicit trees are the reason this is fast and small. There are no nodes to
allocate, no pointers to chase, and the `Array` supplies growth and
cleanup. The comparator is a parameter, so the same code is a min-heap of
numbers, a max-heap with the reversed comparator, or the scheduler's deadline
queue with a tiebreaker. Because the tiebreaker is part of the comparator,
the order of equal deadlines is deterministic even though a heap is not
stable.

Note the names: the helpers are `heap_push` and `heap_pop`. A module function
named `push` would take precedence over the library's `Array.push` inside that
module, and the call `heap.push(value)` would call itself without end.
