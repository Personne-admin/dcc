Each mistake belongs to a different tool. A wrong library name is the linker
looking in the system directories and not finding `libmm.so`. An undefined
symbol is the linker finding the library but not the name; because it knows
the library's symbols it can suggest `pow`. The `f32` declaration is invisible
to every tool: the compiler trusts the declaration, the linker matches only the
name, and at run time the float is passed where libm reads a double.

When native results look wrong, compare the declaration with the C header
before suspecting the library. On x86-64 the first floating-point argument is in
a register whose width the callee assumes.
