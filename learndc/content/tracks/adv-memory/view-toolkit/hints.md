## Hint 1

Every function receives a pointer and a count, not a slice. Compute the end
address `p + n` once and compare with it. `window` is a single call to
`core::slice::from_raw`.

## Hint 2

For `find_last` start with `end = p + n` and loop `while end != p`: step
`end--` first, then test `*end`. For `reverse` use `lo = p` and
`hi = p + (n - 1)`, swap `*lo` and `*hi`, and loop `while lo < hi`. Guard
`n < 2` first, because `n - 1` on a `usize` of 0 wraps around.

## Hint 3

`window` is `return core::slice::from_raw(base + start, len);`. `sum` keeps an
`i32 total`, loops `while p != end` adding `*p` and doing `p++`. `reverse`
swaps through a temporary: `i32 tmp = *lo; *lo = *hi; *hi = tmp; lo++; hi--;`.
