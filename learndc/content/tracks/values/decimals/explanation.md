# Why the solution works

`total / 3` divides two `i32` values, so the fraction is dropped and 23 / 3
becomes 7. That is the reason `whole_average` prints `7`, and why an average
of whole numbers needs more care than a sum.

`total as f64 / 3.0` first converts the total to `23.0` (`as` binds tighter
than `/`), then divides two `f64` values and keeps `7.666666...`. The
`{:.2}` in the `println` rounds that to two digits, `7.67`. The stored value
is untouched: the format only affects how it is shown.

`price * cups as f64` converts `cups` the same way before multiplying, so both
sides of `*` are `f64`. The compiler would refuse the mix otherwise, which
protects you from a calculation that silently loses information.
