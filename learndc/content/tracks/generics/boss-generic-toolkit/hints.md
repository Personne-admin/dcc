## Hint 1

Work one stage at a time. The generic functions share one pattern: take a
slice or a `Stack(T)*`, keep a candidate or a counter of type `T` or
`usize`, loop, and return it. Start every search for an extreme from the
first element, never from a made-up number.

## Hint 2

The stack is a fixed array plus a `len`. Everything else follows from one
rule: the first `len` slots are valid. `push` writes at `len` and grows it,
`pop` shrinks it and reads the slot that just became free. The binary search
keeps a range and halves it; the range is empty when `low == high`.

## Hint 3

In `main`, build the report one line at a time and compare with the expected
text. `toolkit::max_of(temperatures)` deduces `T` from the array. The floats
need `{:.2}`. Sort `ordered` before `find_sorted`: the search is only valid
on an ascending slice, and `std::sort::sort(ordered)` sorts the original array
because the slice is a view of it.
