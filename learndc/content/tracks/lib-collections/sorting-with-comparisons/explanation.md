# Why the ranking is stable

A comparison function only answers "which of these two goes first?". The
sorting algorithm is the library's business; yours is a consistent answer.
`by_score_desc` is written with explicit `>` and `<` because subtracting two
`u32` scores would wrap around instead of going negative.

Bo and Eli both have 85. An unstable sort may swap them; the stable merge sort
keeps the roster order, so Bo is always second and Eli third. When a
ranking is shown to people, a stable order is the least surprising choice, and
it makes output reproducible. The price is the scratch array, an allocation
that can fail, which is why `rank` returns a status.

The binary search reuses `by_name`. It only works because the array was
sorted by exactly that comparison first: searching with one rule in an array
sorted by another gives wrong answers, not errors.
