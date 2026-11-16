# Call the maths library

*This lesson starts the geometry project that fix-the-linker-errors continues.*

The harbor surveyors need a distance from two legs, `hypot(x, y)`, and libm
has it. `src/main.dc` already declares `cos` and `sqrt` and calls `hypot`.
The project does not build yet, and the problems come in two layers: first
the compiler, then the linker.

## Your task

1. Run `dpm build` and read the first error. Declare `hypot` the way `cos`
   and `sqrt` are declared: it takes two `double` values and returns one.
2. Build again. Now the compiler is happy and the linker is not. Read the
   message and fix it in `project.dcon`.

The program prints:

```text
cos(0) = 1.0000
sqrt(2) = 1.4142
hypot(3, 4) = 5.0000
```

On a system without the C development package `-lm` may not resolve; the exact
library file name `":libm.so.6"` works there.

## Acceptance criteria

- `dpm build` succeeds and the program prints the three lines above.
