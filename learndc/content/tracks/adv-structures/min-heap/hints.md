## Hint 1

Everything works on `heap.items()`, a slice of the array in level order.
Push: append, then repeatedly swap with the parent while the parent comes
after the item. Pop: swap the first and last item, drop the last, then
repeatedly swap the root item with its smaller child while that child comes
before it.

## Hint 2

Sift up from `i = items.len - 1`: `parent = (i - 1) / 2`; stop when
`compare(&items[parent], &items[i]) <= 0`, otherwise swap and set `i = parent`.
Sift down from `i = 0` with `count` the new length: `left = 2 * i + 1`,
`right = left + 1`, `best = i`; if `left < count` and the left item comes
before `best`, `best = left`; the same for `right`; stop when `best == i`.
Call `heap.items()` again after `heap.pop()`: the slice should have the new
length.

## Hint 3

```
Optional(T) heap_pop(T)(Array(T)* heap, i32(*)(const T*, const T*) compare) {
    usize count = heap.len();
    if count == 0 {
        return Optional::None;
    }
    []T items = heap.items();
    T top = items[0];
    swap_items(items, 0, count - 1);
    heap.pop();
    count -= 1;
    items = heap.items();
    // sift down from index 0 using count
    ...
}
```
