# Why this works

A `static if` is decided while compiling. For `kind_of!i32()` the compiler
sees that `T == i8 || ... || T == i32` is true, keeps that one `return` and
throws the rest away, so the copy of `kind_of` for `i32` is just a function
returning `"signed"`. The copy for `bool` falls through every condition to
`"other"`. There is no comparison left at run time.

That is the difference from an ordinary `if`: both branches of a normal `if`
must be valid for every `T` because both are compiled. Branches of a
`static if` that do not apply are never compiled for that type, so each
branch may use operations that only its own types support.

`size_class` asks a different compile-time question. `sizeof(T)` is a
constant for each type, so the same chain sorts `u8`, `bool` and `char`
(1 byte) into `tiny`, `i32` and `f32` into `small` and `i64` and `f64`
into `big`. The table in `main` shows that the two functions are independent:
`f32` is a float of small size and `bool` is a tiny type of no numeric kind.

Because the functions take no values, the type has nothing to be deduced
from, which is why every call names it with `!`.
