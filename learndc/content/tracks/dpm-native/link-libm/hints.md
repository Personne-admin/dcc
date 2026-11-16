## Hint 1

The first error comes from dcc: a name it does not know. The second comes from ld.lld and says `undefined symbol`, followed by the symbol and where it was referenced.

## Hint 2

`hypot(double, double)` returns a `double`: write it as `@nomangle extern f64 hypot(f64 x, f64 y);`. The linker needs the library that defines the function: libm, whose short name is `m`.

## Hint 3

Add `@nomangle extern f64 hypot(f64 x, f64 y);` below the other declarations, and `link = { "m" },` in project.dcon before `products`.
