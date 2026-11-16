# Three mistakes in one program

*Continues the geometry project of link-libm.*

This program should print the cosine, square root and a power from libm, and
it does not build. There are three separate mistakes. They are different kinds
of failure, and the third gives no error at all.

```text
cos(0) = 1.0000
sqrt(2) = 1.4142
pow(2, 10) = 1024.0000
```

## Your task

Work through the failures in the order dpm shows them:

1. The linker cannot find a library. The message says which `-l` name it
   tried and where it looked.
2. The linker finds the library but one symbol is undefined. It suggests the
   name it thinks you meant.
3. The program builds and prints a wrong number. Compare the output with the
   table above and fix the declaration: libm's `cos` works on doubles.

Only `project.dcon` and `src/main.dc` need changes.

## Acceptance criteria

- `dpm build` succeeds and the program prints the three lines above.
