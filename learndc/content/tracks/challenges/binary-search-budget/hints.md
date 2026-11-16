## Hint 1

Because the data is sorted, one comparison can discard half of the range: look at the middle value.

## Hint 2

Keep a half-open range `lo..hi` of indices that may still contain the key. Compare `values[mid]` with the key: equal returns, smaller moves `lo` to `mid + 1`, larger moves `hi` to `mid`. Stop when `lo == hi`.

## Hint 3

Compute `mid` as `lo + (hi - lo) / 2`. Use `usize` for all three and the half-open form so that no value goes below 0.
