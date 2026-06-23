# dc

dc is a small systems language with an LLVM backend, built with a
freestanding-first mindset: no hosted OS, no libc, and no implicit runtime are
assumed unless you ask for them. It targets the space C/C++ usually occupies,
but without C's implicit conversions and without pulling in an C++ style OOP
type system to get generics and basic ergonomics.

The core design choices follow from that:

- No implicit conversions. Integer literals are typed from context; a value
  that doesn't fit its target type is a compile error, not silent
  truncation.
- No constructors, inheritance, or member functions on structs. Structs are
  plain data; behavior is free functions, called with UFCS (`p.length()`
  instead of `length(p)`) when convenient.
- Templates, constraints (`if Concept(T)`), and variadics give you generic
  code without runtime cost or a class hierarchy.
- Codegen-affecting choices (stack protector, frame pointer, calling
  convention, alignment, sections, atomics) are explicit attributes and
  driver flags, not compiler heuristics.
- Minimal implicit control flow. There is no operator overloading or destructors,
  all control flow transfers are explicit with call syntax, compiler-aided cleanup
  is available via the `defer` keyword.

See [docs/spec.md](docs/spec.md) for the language reference. It's a living draft, the
implementation moves faster than the document, but I try to keep it in sync after
every major feature addition or a large number of smaller improvements.

## Status

Solo project, pre-release. The language, ABI, and CLI surface are all
expected to keep moving. Treat anything here as subject to change.

## Building the compiler

Build dependencies: a C++26-capable clang with module support, LLVM, GNU
Make.

```bash
make            # builds the driver, dccd, and libdcext
make test       # builds and runs the test suite
make install    # installs dcc, dccd, and libdcext to PREFIX (default /usr/local)
```

`ENABLE_LLVM`, `ENABLE_ASAN`, and `BUILD_TYPE` are overridable via
`make VAR=value`; see `GNUmakefile` for the full list of targets.

## Using dc from a Zig build system

As there is currently no custom build tool for dc projects, a DCC SDK is provided
for the Zig build system for use in any user projects. In order to create a DCC
project which uses ZIg build, add the SDK to your project:

```bash
zig fetch --save git+https://github.com/yeint-herp/dcc.git
```

then in `build.zig`:

```zig
const dcc = @import("dcc");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const artifact = dcc.sdk.compile(b, .{
        .name = "hello",
        .source_file = b.path("src/main.dc"),
        .target = target,
        .optimize = optimize,
    });

    b.installFile(artifact.output_file, "bin/hello");
}
```

`dcc.sdk.CompileOptions` mirrors the driver's flags directly: `libdcext`,
`position_independent_code`, `no_red_zone`, `no_stack_protector`, `code_model`,
`bounds_check`, and so on.  
Supported targets are `x86_64-elf`, `x86-elf`, `x86_64-coff`, and `x86-coff`,  
dc doesn't assume a hosted OS, so Zig's `linux` and `freestanding` os tags both
map to the plain ELF target.

## Freestanding by default

A dc program has no implicit prelude: no allocator, no I/O, nothing assumed
about the host. This is intentional, dc is meant to be as usable for a
kernel or a microcontroller as for a regular userspace binary.

`libdcext` is the opt-in hosted standard library. It's only linked in and its
prelude only injected when you pass `-flibdcext` to the driver (or set
`.libdcext = true` in the Zig SDK options). Without it, you get exactly the
language and `core::*` compiler intrinsics, nothing else.

## Tooling

`dccd` is the language server: completion, formatting, semantic tokens,
inlay hints, and workspace symbols, all over LSP.

The `vscode/` extension (DC Language Support) wraps `dccd` for syntax
highlighting and editor integration in VS Code. Neovim support is planned,
built on the same `dccd` server.

## License

GPLv3. See `LICENSE`.
