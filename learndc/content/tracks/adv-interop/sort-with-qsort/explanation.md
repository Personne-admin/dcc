# Why these are callbacks, and what can go wrong

`qsort` is written once for every array in the world, so it cannot know
about `Player`. It sees a block of bytes, an element count and an element
size, and calls your comparator with *addresses* of two elements. That is why
the comparators take `const void*`: casting back to `const Player*` is you
telling the compiler what C erased.

The comparator returns a sign, and it must be consistent: if a sorts before b
then b must sort after a, and equal elements must compare equal. Breaking
ties with the id makes the order total. Computing `x.score - y.score` would
give the right sign for small values and the wrong one when the difference
does not fit in an `i32`, which is why the harness tests the extremes.

`bsearch` assumes the array is sorted *in the order of the same comparator*.
Searching a score-sorted array with `by_id` would return garbage. That
precondition lives in the caller's head, not in the types: a good reason to
keep the sorting and searching functions next to each other.

`qsort_r` solves the "callbacks cannot capture" problem. The `Counter` lives in
`sort_counting`'s frame; its address travels through C as a `void*` and comes
back as the third argument. The counter is only valid while `qsort_r` runs,
which is all the callback needs.
