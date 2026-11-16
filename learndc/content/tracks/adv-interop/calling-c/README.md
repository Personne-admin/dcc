# Calling C from DC

Operating systems, databases, audio and graphics libraries and thousands of
other tools expose a C interface. DC can call it directly: no wrapper
generator and no glue code. This lesson covers the three things a call needs:
a declaration, matching types and a link step.

## Declaring a C function

A C function is described to DC by a declaration without a body:

```dc
@nomangle extern i32 abs(i32 x);
@nomangle extern f64 sqrt(f64 x);
```

- `extern` says the body lives somewhere else, in a library the linker will
  find.
- `@nomangle` keeps the symbol name exactly as written. A normal DC function
  gets a long mangled name that encodes its module and parameter types, so
  that two modules can both have a `helper`. C has no such scheme: the symbol
  for `abs` is just `abs`.
- The declaration also fixes the calling convention to the C one of the
  target. After that you call `abs(-5)` like any other function.

You can see the difference without any C library. This program defines one
function normally and one with `@nomangle`, and asks the compiler for the
symbol it emits:

```dc example=symbol_names
```

A DC function marked `@nomangle public` is exactly what a C library looks
like from the outside, and C code can call it by that name.

## You are the header

DC does not read C header files. Your declaration *is* the header, and
nothing checks it against the library. If you declare `i32 strlen(...)` where
C returns a `size_t`, the program still links and runs, and quietly uses half
of a register. Every type in a declaration must match the C type exactly:

| C type | DC type | Note |
| --- | --- | --- |
| `int` | `i32` | `unsigned int` is `u32` |
| `long` | `i64` | on 64-bit Linux; it is 32 bits on Windows, which is why this chapter is Linux-only |
| `size_t` | `usize` | `ssize_t` is `isize` |
| `double`, `float` | `f64`, `f32` | |
| `char*`, `const char*` | `char*`, `const char*` | text, see the next lesson |
| `void*` | `void*` | any pointer converts with `as` |
| `T*` | `T*` | out parameters and arrays |
| `struct S` | a DC struct with the same fields in the same order | sizes and offsets must agree |

A cheap safety net is to print `sizeof` and `offsetof` of your DC types and
compare them with the C documentation:

```dc example=c_types
```

## Linking the library

Declaring a function does not find it. The linker needs the library that
defines it, and on Linux the two libraries you will meet first are separate:

- `libc` holds `strlen`, `abs`, `qsort`, `open`, `read`, `getenv` and the rest
  of the C standard library and POSIX.
- `libm` holds the math functions: `sqrt`, `pow`, `floor`, `hypot`.

In a DPM project, list them in `project.dcon`:

```dcon
dpm::v1::Project {
    name = "calc",
    version = "0.1.0",
    link = { ":libc.so.6", ":libm.so.6" },
    products = {
        { name = "calc", entry = "src/main.dc" },
    },
}
```

With plain dcc the same request is `-l:libc.so.6 -l:libm.so.6`. A leading
colon asks for an exact file name. The shorter `link = { "c", "m" }` (and
`-lc -lm`) looks for `libc.so` and `libm.so`, which are *development* files:
distributions install them only with the C library's developer package,
while `libc.so.6` and `libm.so.6` are on every glibc system. The exact names
are the dependable choice.

Once a program links a shared library, dcc produces a dynamic executable on
its own: it names `libc.so.6` and `libm.so.6` as needed libraries and the
system loader finds them at startup. Without any shared library the output
stays fully static. The last lesson of the chapter looks at the choices.

## Reading link errors

Two messages cover most failures, and both come from the linker, `ld.lld`,
through dcc:

```text
ld.lld: error: undefined symbol: strlen
>>> referenced by main.dc:9 (src/main.dc:9)
```

The declaration compiled, but no linked library defines `strlen`. Add the
library, or check the spelling of the function: the name must match the C
name exactly. When a similar name exists in a linked library the linker adds
`did you mean: ...`.

```text
ld.lld: error: unable to find library -lnosuchlib
```

The `link` entry names a library that is not installed, or whose file is
spelled differently. The message goes on to list the directories that were
searched.

Both are errors about the *build*, which is why you meet them before the
program ever runs. A wrong type in a declaration is far worse: it links
cleanly and misbehaves at run time.

## What to avoid for now

- Variadic C functions such as `printf`. This chapter uses ordinary functions.
- C's buffered standard I/O (`puts`, `printf`, `fwrite`). The libdcext
  startup leaves the program with `exit_group`, so C's buffers are never
  flushed and output can vanish. Use `println`, or the unbuffered `write`.
- C libraries that keep thread-local state (`errno` is one) from threads
  other than the main thread.

## Common mistakes

- Forgetting `@nomangle`: the linker then looks for a mangled name.
- Declaring `int` as `i64`, or `size_t` as `i32`. It links; it is wrong.
- Linking only `libm` and calling `strlen`, or the other way around.
- Using `"m"` and `"c"` on a machine without the development files.
