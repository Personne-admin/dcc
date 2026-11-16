# Two steps, two tools

A call to a C function needs two separate pieces of information. The compiler
needs the *declaration*, which tells it the name, the parameter types and the
result type so it can generate the call. The linker needs the *library*,
which contains the machine code the call jumps to.

With only the first piece the build stops in the compiler ("unknown name").
With the declaration but no library it stops in the linker
(`undefined symbol`). Only with both does it produce a program.

`:libm.so.6` names the shared object file exactly. Because libm is a shared
library, dcc now produces a dynamic executable that loads it at startup;
libdcext itself still needs nothing from the system.

All four functions work on C `double`, which is `f64` in DC. Declaring one of
them with `f32` or `i32` would still link, and would pass the wrong bits.
