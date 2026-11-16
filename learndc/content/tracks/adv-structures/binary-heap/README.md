# The binary heap

Many programs repeatedly ask "what is the smallest (or most urgent) item
now?" and keep adding new items: the next event of a simulation, the cheapest
unexplored node of a route search, the task with the nearest deadline. That
structure is a *priority queue*. The standard library has a sort and a
binary search but no priority queue, so you build one from an `Array`.

## Three obvious ways, and why they are slow

- Keep an unsorted array and scan for the minimum on every pop: pop costs n.
- Keep the array sorted and insert in place: each insert moves up to n items.
- Sort again before each pop: far worse.

A *binary heap* does both operations in about log2(n) steps.

## A tree stored in an array

A heap is a complete binary tree: every level is full except possibly the
last, which fills from the left. That shape needs no pointers. Store the
nodes level by level in an array; for the item at index `i`:

```text
parent of i:       (i - 1) / 2
left child of i:   2 * i + 1
right child of i:  2 * i + 2
```

The *heap property* says that every item is not greater than its children,
so the smallest item is always at index 0. Siblings and distant cousins are not
ordered, only each parent against its children. That is much less than sorted
order, and that is why it is cheap to maintain.

## Push: sift up

Add the new item at the end (the next free leaf), then move it up while it
is smaller than its parent:

```dc example=heap_ops
```

- `Array.push` appends; `heap_push` then swaps the new item with its parent
  until the parent is not greater. The loop ends at the root or at the first
  parent that is small enough, so it climbs at most log2(n) levels.
- The printed arrays are the tree in level order. After `after 1:` the array
  is `1 4 7 9`: 1 is the root, 4 and 7 its children, 9 under 4.

## Pop: sift down

The smallest item is the root. To remove it, move the last item to the root and
let it sink: swap it with the smaller child until neither child is smaller.
That restores the property after at most log2(n) levels, with two comparisons
per level (left against current, right against the best so far).

- `heap_pop` swaps the root with the last item, `pop`s that item off the
  array, and sifts the new root down. Items popped from the heap come out in
  ascending order, which is heap sort.
- `Optional::None` for an empty heap, like every other pop in the library.
- The names are `heap_push` and `heap_pop` on purpose. A module-level
  function called `push` would shadow `Array.push` inside the module, and
  `heap.push(value)` in the body would call itself forever.

## Costs and ties

| operation | cost |
|---|---|
| peek the smallest | 1 (index 0) |
| push | at most log2(n) swaps |
| pop | at most 2 * log2(n) comparisons |
| build by n pushes | at most n * log2(n) |

A heap is not stable: items that compare equal can come out in any order. When
order among equals matters, such as events at the same time, add a
tiebreaker to the comparison, like an arrival number that grows with every
push.

## Common mistakes

- Comparing against the wrong neighbour while sifting down. Choose the smaller
  of the two children first and compare with that one.
- Forgetting the bounds: `left < count` and `right < count` must use the
  size after the last item was removed.
- Holding `heap.items()` across a `push`: the array may move. Ask again.
- Using `(i + 1) / 2` or `i / 2` for the parent: with a root at index 0 it
  is `(i - 1) / 2`.
- Expecting `items()` of a heap to be sorted. Only index 0 is guaranteed.
