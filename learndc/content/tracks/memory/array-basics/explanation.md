# Owning one array from start to end

`main` owns `squares`: it creates it with `{}` and registers
`defer squares.deinit();` on the very next line, so the heap block is given
back whichever way `main` exits. A `push` that cannot allocate returns an
error, and `?` passes it on; the deferred `deinit` still runs.

The elements are read through `items()`, a borrowed slice. Nothing is pushed
while the loop runs, so the borrowed view stays valid. `len()` reports the
number of elements in use, independent of how much room the array reserved.
