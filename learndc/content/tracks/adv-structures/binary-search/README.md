# Binary search with duplicates

A sorted slice may contain equal values. Searching it well means answering
questions about positions, not only "is it there":

- `lower_bound(items, key)`: the index of the first item that is **not less**
  than `key` (where the key is, or would be inserted).
- `upper_bound(items, key)`: the index of the first item that is **greater**
  than `key`.
- `count_of(items, key)`: how many items equal `key`.
- `find_first(items, key)`: the lowest index holding `key`, or none.

For `{1, 3, 3, 3, 5, 8, 8, 13}`, `lower_bound(3)` is 1, `upper_bound(3)` is 4
and the three 3s occupy the indexes in between. `lower_bound(4)` is 4 as well:
it is where a 4 would have to be inserted to keep the order.

`src/search.dc` has the four functions with placeholder bodies. Every
comparison must go through `counter::less(counter, a, b)`, which counts it, so
that the checks can verify that the search is logarithmic.

## The invariant

Keep a half-open range of candidates `low..high` and the invariant that the
answer is in `low..high`, with `high` itself a possible answer: everything
before `low` is known to be below the key, everything from `high` on is known
not to be.

- Start with `low = 0` and `high = items.len`.
- While `low < high`, look at `mid = low + (high - low) / 2`. If the item at
  `mid` is below the key, the answer is after it: `low = mid + 1`. Otherwise
  `mid` could be the answer: `high = mid`.
- When the range is empty, `low == high` is the answer.

`upper_bound` is the same loop with another question: is the item at `mid` not
greater than the key? Swap the comparison and keep the structure.

The other two functions need no new loop. `count_of` is a difference, and
`find_first` is `lower_bound` plus one check that the item found equals the
key (an item equals the key when the key is not less than it and it is not
less than the key).

`main.dc` is fixed and prints:

```text
sorted versions
lower_bound(3) = 1
upper_bound(3) = 4
count of 3: 3
count of 8: 2
count of 4: 0
insert 4 at 4
find 13 at 7
find 6: none
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The table above matches exactly and the exit code is 0.
- The harness compares both bounds with a slow scan for every array size and
  key and limits the comparisons of each call.
