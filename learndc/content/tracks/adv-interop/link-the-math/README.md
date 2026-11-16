# Link the math library

`src/main.dc` prints four results from the C math library. It declares
only one of the functions it uses, and `project.dcon` does not mention
libm, so the program breaks twice. You fix both, and the errors tell you in
which order.

## The C prototypes

```c
double sqrt(double x);
double pow(double base, double exponent);
double floor(double x);
double hypot(double x, double y);
```

C's `double` is DC's `f64`.

## Your task

1. Build the project. The first error is a compile error: `pow`, `floor`
   and `hypot` are not declared. Add `@nomangle extern` declarations for them
   next to the one for `sqrt`.
2. Build again. Now every name is known, so the build reaches the linker,
   which reports `undefined symbol: sqrt` (and the others): no linked library
   defines them. Add `link = { ":libm.so.6" },` to the project in
   `project.dcon`, next to `version`.

The program must print:

```text
sqrt(2) = 1.4142
2^10 = 1024.0
floor(-2.5) = -3.0
hypot(3, 4) = 5.0
```

The two errors come from different tools. A *compile* error mentions a line in
your source; a *link* error names a symbol and the library search that failed.
Knowing which one you are looking at tells you whether to edit a `.dc` file
or `project.dcon`.
