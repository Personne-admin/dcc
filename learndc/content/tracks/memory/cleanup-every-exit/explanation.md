# One cleanup for every way out

`checkout` has three exits. Writing the cleanup in front of each `return`
works today, but the code then depends on everyone remembering it at every
future exit, including the invisible ones: each `?` can return early too.

`defer println("till closed");` is registered as soon as the till is open,
so every path out of the function, whichever `return` it takes and whether
or not a print fails, closes it exactly once. The deferred statement has no
`?` because it runs while the function is already leaving.
