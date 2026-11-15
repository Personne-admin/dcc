# Why this works

Every function in the toolkit is written once and compiled once per type it
is called with. `max_of` for `i32` and for `f64` are two different machine
functions generated from the same text; that is why the body may only rely on
what the types share (`>`, `<`, `==`) and why it must never assume a value of
`T`. The old `largest` assumed that `T` could start at `{}`, zero, which is
smaller than all elements only when they are positive.

The stack shows that a struct template is also a recipe. `Stack(i32)` has
`i32[8] items`, `Stack(f64)` has `f64[8] items`. Its functions are generic in
the same `T` and keep one invariant: slots below `len` are in use. `CAPACITY`
is a value alias, a name for a compile-time constant, so the array length is
known to the compiler and every copy of the struct has a fixed size.

`find_sorted` is the same idea as the library's binary search, written by you
for any comparable `T`. The key is the invariant: the answer, if there is
one, is in `low..high`. Looking at the middle element either finds it or
discards half of the range, so the search takes about log2(n) steps. It
needs ascending order, which is why `main` sorts before searching.

`main` combines what the chapters taught: the toolkit with two element types,
the stack to reverse, `std::sort` from the library and `{:.2}` for the floats.
`T` is deduced at every call from the arguments, and each distinct type adds
exactly one compiled copy of the function.
