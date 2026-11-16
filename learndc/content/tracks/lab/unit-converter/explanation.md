# Why this converter works

A table with two numbers per unit replaces a pile of special cases. Metres
to kilometres, pounds to kilograms and Celsius to Fahrenheit all use the
same two functions; only the numbers in `UNITS` differ. The offset is what
lets temperature fit in: a pure multiplication cannot turn 0 C into 32 F,
but `base = value * factor + offset` can, and going back simply undoes the
two operations in the opposite order.

`convert` decides before it computes. Both lookups come first so that an
unknown name wins over a dimension problem, and the dimension is compared
before any arithmetic so a nonsense conversion never produces a number.
Returning a typed `ConvertError` instead of a magic value such as 0 or -1
means the caller has to say what happens on each failure, and `match` makes
the compiler check that both are handled.

`print_table` and `main` show the same pattern twice: call `convert`, match
on the `Result`, and handle `Ok` and each `Err` where the output happens.
The shared `print_error` keeps the wording in one place. Rounded printing
with `{:.2}` hides the tiny binary rounding errors that would otherwise
show up as 211.99999999999997.
