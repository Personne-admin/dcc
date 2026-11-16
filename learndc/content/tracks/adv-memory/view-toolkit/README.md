# A toolkit of views

Functions that work on "some numbers in memory" often take a pointer and a
count instead of a slice, so they can be used on any block: an array on the
stack, a part of a bigger buffer, memory from an allocator. Fill in the four
functions in `src/views.dc`. `src/main.dc` is finished; it calls them on one
array and prints a report.

```dc
public i32 sum(const i32* p, usize n)
public const i32* find_last(const i32* p, usize n, i32 target)
public []i32 window(i32* base, usize start, usize len)
public void reverse(i32* p, usize n)
```

1. `sum` adds the `n` elements starting at `p`. Walk with a pointer until it
   reaches the end address `p + n`.
2. `find_last` returns a pointer to the last element equal to `target`, or
   `null` when there is none. Walking from the back avoids looking at the
   whole block when the match is near the end.
3. `window` returns a slice of `len` elements that starts `start` elements
   after `base`. It copies nothing: writes through the slice change the
   original block.
4. `reverse` reverses the `n` elements in place. Keep one pointer at each end
   and swap, moving toward the middle until they meet. It must also cope with
   `n` of 0 and 1.

The program must print:

```text
sum: 28
last 3 at index 3
4 not found
window: 9 3 7
after write: 5 3 0 3 7 1
reversed: 1 7 3 0 3 5
```

A second check calls your functions directly with edge cases: empty blocks,
a match at index 0, ranges that start in the middle, an odd and an even
count.
