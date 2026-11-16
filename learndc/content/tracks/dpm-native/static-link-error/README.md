# Static or dynamic?

*Continues the geometry project: the same libm, now checked from inside the running program.*

The `probe` program calls `sqrt` from libm and reports whether the executable
it is running from is dynamic: `src/elf.dc` reads the program headers of
`/proc/self/exe` and looks for a program interpreter. Somebody set the
product's `link_mode` to `Static`, and the build fails.

The project links `:libm.so.6`, the exact runtime file of the C maths library,
which is a shared object. Read the linker's message: a static link looks for
archives and only in the `-L` directories.

## Your task

Fix `link_mode` of the product `probe` so the program builds and runs as a
dynamic executable. Either `LinkMode::Default` (static unless a shared object
is needed) or `LinkMode::Dynamic` (prefer shared objects) works; think about
which one says what you mean. Do not touch the sources.

The program prints:

```text
dynamic: true
sqrt(2) = 1.4142
```

## Acceptance criteria

- `dpm build` succeeds and the program prints the two lines above.
