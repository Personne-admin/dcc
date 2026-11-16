# Why this works

A pack function is a template that the compiler instantiates once for each
list of argument types. `static for` has no counter and no array: it
repeats its body at compile time, so `sum_all(1, 2.5, 3 as u8)` becomes
three statements, `total += 1 as f64`, `total += 2.5 as f64` and
`total += 3 as f64`, each type-checked on its own. That is why a mixed list
is no problem here, and why a type that cannot be cast to `f64` would be
reported at the call, not at run time.

`largest_of(F, T...)(F first, T rest)` shows a common design. A pack may be
empty, and there is no largest element of nothing. Giving the function a
mandatory first parameter makes the empty call a compile error with no extra
code and gives the loop a natural starting value. Starting from `0.0` would
be the same mistake as in the generic `largest` of the toolkit boss: all
negative arguments would lose to the invented zero.

`log_line` needs state across the unrolled copies. The `first` flag is an
ordinary local: the first copy sees it `true`, sets it `false`, and the
following copies take the other branch. The `?` inside the loop returns
the first print failure, like in any other function.
