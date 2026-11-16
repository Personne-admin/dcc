# Why this works

The loop keeps one fact true: the answer lies in `low..high`. Initially that
is the whole slice (an answer of `items.len` means "after everything").
When `items[mid]` is below the key, `mid` and everything before it are
below the key too, because the slice is sorted, so the answer cannot be in
`low..=mid`, and `low = mid + 1` keeps the invariant. When it is not below the
key, `mid` itself may be the answer, so the range shrinks to `low..mid` and
not `low..mid - 1`. The range strictly shrinks every round, so the loop ends,
and when it is empty the invariant says that `low` is the answer.

Writing `mid = low + (high - low) / 2` instead of `(low + high) / 2` avoids an
overflow for very large slices. With `usize` it is rarely visible, but the
habit is free.

`upper_bound` changes only what "below" means. That is the real lesson of
duplicates: there is one loop and two questions. `lower_bound` finds the
boundary between items less than the key and the rest; `upper_bound` finds the
boundary between items not greater than the key and the rest. Everything else
is arithmetic on those two boundaries: the equal items are exactly the ones
between them.

The harness also limits the comparisons. A slice of 24 items has a range of
24 candidates; each step halves it, so at most five comparisons are needed
(24, 12, 6, 3, 1). A linear scan would pass every correctness test and fail
this one, which is the point of counting cost.
