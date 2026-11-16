# How an Array grows

You have pushed into arrays since the memory chapter. This lecture looks
at what `push` does when the array is full, how to avoid repeated work, and
which methods take elements out again.

## Length and capacity

An `Array(T)` has two sizes:

- `len()` is the number of elements stored.
- `capacity()` is the number of elements the current heap block can hold.

`items()` only covers the first `len()` elements. The rest of the block is
spare room that `push` fills without asking the allocator again.

## The growth rule

An empty array owns no block at all (capacity 0). Watch the capacity change
while twenty values are pushed:

```dc example=capacity_steps
```

- The first push allocates room for **8** elements.
- Whenever a push finds the block full, the capacity **doubles** and every
  element is copied into the new block.
- Only the pushes that find the block full allocate; the others are plain
  stores. Growing costs a copy, but because the size doubles, it happens
  rarely.

That copy is also the reason a slice or pointer into the array must not
survive a push: the elements may have moved.

## Reserve and trim

If you know how many elements are coming, ask for the room once:

```dc example=reserve_once
```

- `reserve(n)?` makes sure the capacity is at least `n`. It allocates, so it
  can fail, hence the `?`. It never shrinks the array and never changes
  `len()`.
- `clear()` forgets the elements but keeps the block: the capacity stays at
  100, ready to be refilled.
- `shrink_to_fit()?` gives the spare room back by moving the elements into a
  block of exactly `len()` elements (and releasing the block entirely when
  the array is empty). It allocates, so it can fail too.

Reserving is an optimisation, never a requirement: push works on any array.

## Taking elements out

```dc example=edit_in_place
```

| Method               | Order of the rest | Cost                 | Returns       |
| -------------------- | ----------------- | -------------------- | ------------- |
| `pop()`              | unchanged         | cheap                | last, or None |
| `remove(i)`          | kept              | shifts later items   | element, or None |
| `swap_remove(i)`     | changed           | cheap                | element, or None |
| `insert(i, v)?`      | kept              | shifts later items   | status (can allocate) |
| `clear()`            | n/a               | cheap                | nothing       |

An index that is out of range gives `None` instead of crashing, so you can
always ask `is_none()`. `insert` may need to grow the array, so like `push`
it returns a status.

## Common mistakes

- Holding `items()` (or a pointer to an element) across `push`, `insert`,
  `reserve` or `shrink_to_fit`: all of them may move the elements.
- Treating `capacity()` as the length. Indexing past `len()` reads
  elements that were never stored.
- Using `swap_remove` while the order matters (a sorted array, a queue).
- Calling `remove` in a loop over `items()`: the loop's slice and the array
  disagree afterwards.
- Forgetting that `clear()` keeps the memory: the array still needs
  `deinit()`.
