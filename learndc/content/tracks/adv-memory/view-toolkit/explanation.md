# Why these functions look the way they do

All four functions only ever use the pointer, the count and the element size
the compiler already knows. `p + n` is the address one past the last element,
so "while p is not the end" visits exactly `n` elements, and it works for
`n == 0` without a special case.

`find_last` walks backwards from the end and decrements *before* it reads:
the end address itself must never be dereferenced. It returns a pointer,
so the caller can compute an index with pointer subtraction or modify the
element in place; `null` stands for "none".

`window` is the only place where a count turns back into a slice. The
slice borrows the original block, so the harness could write through it and
see the change in `data`. Nothing checks that the range really exists, which
is why `window` is only as safe as its caller.

`reverse` needs the guard `n < 2`. With `n == 0`, `n - 1` would wrap around
to a huge unsigned number and `hi` would point far outside the block.
Two pointers that move toward each other and stop when `lo < hi` fails
touch every pair once and leave the middle element of an odd count where it is.
