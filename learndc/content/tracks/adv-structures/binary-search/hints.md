## Hint 1

`lower_bound` and `upper_bound` are the same loop. The range `low..high` holds
every index the answer can still be. Each comparison throws away half of it.
`count_of` and `find_first` call them.

## Hint 2

lower_bound: while `low < high`, take `mid = low + (high - low) / 2`; if
`less(items[mid], key)` then `low = mid + 1` else `high = mid`. upper_bound
asks `less(key, items[mid])` instead: if true then `high = mid` else
`low = mid + 1`.

## Hint 3

```
public usize count_of([]const i32 items, i32 key, Counter* counter) {
    return upper_bound(items, key, counter) - lower_bound(items, key, counter);
}

public Optional(usize) find_first([]const i32 items, i32 key, Counter* counter) {
    usize at = lower_bound(items, key, counter);
    if at < items.len && !counter::less(counter, key, items[at]) {
        return Optional::Some(at);
    }
    return Optional::None;
}
```
