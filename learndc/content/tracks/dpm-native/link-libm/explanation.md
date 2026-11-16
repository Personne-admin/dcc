The two failures show the two stages. The compiler type-checks against
declarations only; with no declaration of `hypot` it stops at the call. Once
the declaration exists the program compiles to an object whose call refers to a
symbol `hypot` by name, and the linker has to find a definition: `-lm` makes
libm.so one of the places it looks.

Because `link` is at the project level, every product and every test program of
the project is linked against libm, and so is every project that depends on this
one. That is the right scope for a library everyone here needs; the next
lessons look at narrower ones.
