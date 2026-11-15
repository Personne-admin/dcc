# Why this works

Every function here is a thin wrapper: the library already knows how to
count, reverse, sort and search a slice of any element type, so the program
states what it wants instead of how to get it.

`rank_desc` shows how small tools combine. The library sorts ascending only,
and reversing an ascending slice gives descending order, so two calls do the
job. Both work in place on the caller's elements, which is why the parameter
is a mutable `[]i32` and why `main` sees the new order without any return
value.

`binary_search` looks at the middle element, then discards half of the range,
and repeats. That needs order: on `93 93 65 42 ...` (descending) it would
decide wrongly which half to keep. `median` leaves the scores ascending, so
the search in `main` is valid. If you moved the `rank_desc` call after the
median, the search would run on the wrong order and miss `65`.

`find_score` returns the library's `Optional(usize)` untouched. Passing an
optional on is cleaner than unwrapping and rebuilding it, and the caller
still decides what a missing score means.
