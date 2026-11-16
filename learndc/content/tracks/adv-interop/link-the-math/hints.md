## Hint 1

Read the first error of the failed build. It says "unknown name" and points at
a call in `main`: the compiler has never heard of that function, because
nothing declares it.

## Hint 2

Each declaration copies the C prototype: `@nomangle extern f64 pow(f64 base,
f64 exponent);`. After the declarations compile, the linker still needs the
math library. It is not part of libdcext, so name it in the `link` list of
the project.

## Hint 3

Four declarations (sqrt, pow, floor, hypot, each with f64 parameters and an f64
result) and one line in `project.dcon`: `link = { ":libm.so.6" },`.
