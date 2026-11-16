# Native libraries and linking

DC programs can call functions written in C or in any language that follows
the platform's C conventions: the maths library, SDL, zlib, your own shared
object. Two things are needed: a declaration the compiler can type-check
against, and a library the linker can find.

## Declaring the function

```text
@nomangle extern f64 sqrt(f64 x);
```

`@nomangle` keeps the symbol name exactly `sqrt`, without DC's name mangling.
`extern` says the body lives elsewhere, and the function is called with the
C calling convention of the target. The declaration is a promise: the linker
matches symbol names only, never signatures. If you declare `f32 cos(f32)`
while libm defines `double cos(double)`, the program builds, runs and prints
nonsense. The declaration must match the C header exactly: `f64` for
`double`, `i32` for `int`, `usize` for `size_t`.

## Telling dpm which libraries

`link` is a list of library names; each becomes `-l<name>` on the dcc command:

```text
dpm::v1::Project {
    name = "geometry",
    version = "0.1.0",
    link = { "m" },
    products = { { name = "geometry" } },
}
```

The same field exists in three places:

| Where | Applies to |
|---|---|
| project level `link` | every product and every test of the project, and every project that depends on it |
| product level `link` | that product only |
| `link` in a library package | everything that depends on the library |

The last one is how a binding works: a package that wraps SDL declares
`link = { "SDL3" }` once and its users only name the dependency. dpm passes
the libraries of the product first, then the project's, then those of the
dependencies. Order is kept, and repeats are not removed, because static
archives sometimes need to be listed twice.

## Finding the library

`-lm` finds `libm.so` or `libm.a`. The search is: the `-L` directories (from
`library_paths`), `LIBRARY_PATH`, then the system directories (`/lib64`,
`/usr/lib64`, `/usr/lib/x86_64-linux-gnu`, ...). A name that starts with a colon
is an exact file name: `":libm.so.6"` becomes `-l:libm.so.6`, which works
on systems that have the runtime library but not the development symlink
`libm.so`.

```text
library_paths = { "vendor/lib" },       directories searched at build time (-L)
```

## Static or dynamic

A program that needs no shared library is a static executable. As soon as a
`-l` library resolves to a `.so`, dcc produces a dynamic executable: it gets
a program interpreter (the system's `ld-linux`), a `DT_NEEDED` entry per library,
and the loader maps the libraries at start-up. `link_mode` forces the choice:

| link_mode | Meaning |
|---|---|
| `Default` | static unless something needs a shared object |
| `Static` | only `lib<name>.a`; static links search only `-L` directories |
| `Dynamic` | prefer shared objects; an error if there is nothing dynamic to link |

Dynamic linking is available for x86_64 Linux executables.

## Running a dynamic program

`-L` is a build-time search path and is not recorded in the executable. At
start-up the loader searches the program's `runtime_paths` (`-rpath`, with
`$ORIGIN` meaning the executable's directory), then `LD_LIBRARY_PATH`, then the
system cache. A library it cannot find stops the program before `main`:

```text
error while loading shared libraries: libshim.so: cannot open shared object file
```

## Building your own shared library

A product with `kind = Kind::SharedLibrary` produces `lib<name>.so` in the
output directory. Export functions with `@nomangle public`. A shared library
cannot be built with whole-program mode and libdcext, which are the default
for hosted products:

```text
{
    name = "plug",
    kind = Kind::SharedLibrary,
    entry = "src/plug.dc",
    settings = { whole_program = Tri::Off, libdcext = Libdcext::Off },
},
```

A program in the same project can then use `link = { "plug" }`,
`library_paths = { ".dpm/out/debug/host" }` and `runtime_paths = { "$ORIGIN" }`.
Remember that dpm builds products in the order they are listed.

## Reading linker errors

| Message | Meaning |
|---|---|
| `undefined symbol: cos` | a function is declared and called but no linked library defines it; add the library to `link` |
| `undefined symbol: sqr` ... `did you mean: sqrt` | the name is misspelled; the linker suggests the library's symbol |
| `unable to find library -lmm` | the library name is wrong or not installed; the message lists where it looked |
| `unable to find library -l:libm.so.6` with `library search path: none` | a static link, which searches only `-L` directories |
| `error while loading shared libraries` | the build worked, the loader cannot find the library at run time |

## Limitations to know

C `atexit` handlers and buffered C stdio are not flushed when a DC program
exits; call the library's own shutdown function (for example `SDL_Quit`)
before `main` returns. Call C code that uses thread-local state, such as
`errno`, only from the main thread. On Windows a program links against import
libraries and ships the DLL beside the executable.

## Common mistakes

- A signature that does not match the C header: no error, wrong results.
- Putting a requirement on a product when a dependent needs it too.
- Treating a missing `-l` library as a compiler problem: it is a linker message.
