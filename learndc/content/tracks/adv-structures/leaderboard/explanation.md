# Why this works

Insertion sort is correct because of one invariant: after step `i`, the first
`i + 1` items are sorted. Step `i` picks up `items[i]`, shifts every
neighbour that must come later one place to the right and puts the item into
the gap, so the sorted prefix grows by one. Stability comes from the strict
comparison: an equal neighbour is not shifted, so the new item lands after
it, which is where it arrived.

The comparator makes the algorithm independent of the data. The same sort
orders integers, orders players by score or arrival, and the same code, once
compiled for `Player`, sorts a leaderboard. `by_score_then_name` shows the
usual way to build a total order: compare the first key, return when it
differs, and fall through to the next key. A total order is what makes a
sort's result unique, and that is why your sort and `std::sort` agree even
though `std::sort` is not stable.

The second sort in `main` shows what stability is for. The scores alone
leave `mira`, `ada`, `bo` tied; they come out in the order they entered,
`mira ada bo`. With an unstable algorithm the order inside a tie could differ
between runs, between versions of the library or between input sizes.

Ranks are a pass over sorted data, not a property of the sort. Once equal
scores sit next to each other, one comparison with the previous neighbour
decides whether a new rank starts here (`i + 1`, which skips the tied places)
or continues the previous one.
