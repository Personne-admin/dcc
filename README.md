# dc

dc is a small systems language with an LLVM backend, built with a
freestanding-first mindset: no hosted OS, no libc, and no implicit runtime are
assumed unless you ask for them. It targets the space C/C++ usually occupies,
but without C's implicit conversions and without pulling in a C++-style OOP
type system to get generics and basic ergonomics.

The core design choices follow from that:

- No implicit conversions. Integer literals are typed from context; a value
  that doesn't fit its target type is a compile error, not silent
  truncation.
- No constructors, inheritance, or member functions on structs. Structs are
  plain data; behavior is free functions, called using UFCS (`p.length()`
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

## Installation

### Debian

Prebuilt Debian packages are available from
[GitHub Releases](https://github.com/Personne-admin/dcc/releases).

Download the appropriate `.deb` package and install it with:

```bash
sudo apt install ./dcc_*.deb
```

### Windows

Prebuilt Windows MSI installers (`dcc-<version>-x86_64.msi`) are available from
[GitHub Releases](https://github.com/Personne-admin/dcc/releases). Run the installer to set up `dcc`, `dccd`, `dpm`, and standard library headers.
The installer also ships the native `ld.lld.exe` linker with its runtime DLLs and
license notices, plus the Windows import libraries libdcext needs, so
`dcc -flibdcext windows hello.dc -o hello.exe` and `dpm build` write PE
executables without a separate SDK. A custom installation directory works the same way.
The VS Code extension discovers MSI installations without manual PATH editing;
see [server discovery and diagnostics](vscode/readme.md#executable-discovery).

### Arch Linux

Arch users can build the package using the provided `PKGBUILD`:

```bash
git clone https://github.com/Personne-admin/dcc.git
cd dcc/packaging/arch
makepkg -si
```

### Nix / NixOS

dcc provides a Nix flake and can be installed directly from GitHub:

```bash
nix profile install github:Personne-admin/dcc
```

For development, enter the provided development shell with:

```bash
nix develop github:Personne-admin/dcc
```

On NixOS, the flake package can also be added directly to
`environment.systemPackages`.

### From source

On other distributions, dcc can be built directly from source. A
C++26-capable Clang with module support, LLVM, Python 3, and GNU Make are
required.

See the build instructions below.

## Getting started

With dcc and dpm installed (or `build/bin` on PATH after a source build),
create and run a project:

```sh
dpm new hello
cd hello
dpm run
```

`dpm new` writes `project.dcon`, `src/main.dc` and `.gitignore` from the
built-in executable template and works offline. `dpm run` builds the hosted
program with libdcext and runs it, which prints `Hello, world!`. Use
`dpm build` to build without running and `dpm test` to run the tests under
`tests/` (a new project has none yet, so it reports that and succeeds).

Continue with [the beginner guide](docs/getting-started.md), which explains the
generated `Status(Error)` main, arrays and formatted output. DPM manifests,
templates and tasks are described in [docs/dpm-templates.md](docs/dpm-templates.md)
and [docs/dpm-tasks.md](docs/dpm-tasks.md).

### Learn DC interactively

If you would rather learn the language by doing small exercises, `learndc` is an
offline companion tool: it creates an exercise project, runs your code and shows
what differs from the expected result. It is optional and does not replace
`dpm new` and `dpm run`. See [docs/learndc.md](docs/learndc.md); authors of
exercises start at [docs/learndc-authoring.md](docs/learndc-authoring.md).

## Targets

- `x86_64-elf`: Linux executables, hosted with `-flibdcext` or freestanding;
  LLVM and custom backends.
- `x86_64-coff`: Windows PE executables with `-flibdcext windows`; LLVM and
  custom backends.
- `x86-elf`: freestanding 32-bit ELF executables with the LLVM backend. The
  program defines `_start`; libdcext is not available for 32-bit x86.
- `x86-coff`: 32-bit COFF objects with the LLVM backend; PE32 executables are
  not supported.
- `i8086-binary` and `i8086-mz`: flat/COM and MZ images with the custom backend.

## Building the compiler

Build dependencies: a C++26-capable clang with module support, LLVM, GNU
Make.

```bash
make            # builds the driver, dccd, and libdcext
make test       # builds and runs the test suite
make install    # installs dcc, dccd, and libdcext to PREFIX (default /usr/local)
```

> Warning: If you wish to correctly install into a custom PREFIX, set the environment
> variable for both the `make` and `make install` invocations to ensure that the compiler
> has correct defaults.

`ENABLE_LLVM`, `ENABLE_ASAN`, and `BUILD_TYPE` are overridable via
`make VAR=value`; see `GNUmakefile` for the full list of targets.

## Freestanding by default

A dc program has no implicit prelude: no allocator, no I/O, nothing assumed
about the host. This is intentional, dc is meant to be as usable for a
kernel or a microcontroller as for a regular userspace binary.

`libdcext` is the opt-in hosted standard library. It's only linked in and its
prelude only injected when you pass `-flibdcext` to the driver. Its `Error` type combines known
standard-library errors without allocation; see [standard errors](docs/errors.md).
Hosted modules can use `Array(T)`, `print` and `println` without imports or
allocator setup; see [hosted library basics](docs/hosted.md) for cleanup and
error handling.
Without `-flibdcext`, you get exactly the
language and `core::*` compiler intrinsics, nothing else.

DPM projects enable libdcext for hosted products automatically; see
[Getting started](#getting-started).

## Whole-program output

By default `dcc -S` and `dcc -c` on an entry module emit only that module's own
functions and globals. Calls into imported modules stay `extern` references, so
the output can't be assembled or linked on its own.

`-fwhole-program` lowers every module in the import graph into one output with
the usual mangled names. Only reachable code is kept. The roots are the entry
module's public functions and globals, every `@nomangle` function definition
in the graph, and symbols named by module-level asm. Reachability then follows
references (calls, function and global addresses, initializers) across
modules. Imported symbols with no definition in the graph stay `extern`.

Under `-flibdcext`, whole-program builds compile the library from its
installed sources, including the selected startup code, so no library archive
is needed. An object from `-fwhole-program -c -flibdcext` links with `dcc`
like any other object. Separate (non-whole-program) builds link the static
libdcext archive instead.

## Tooling

`dccd` is the language server: completion, formatting, semantic tokens,
inlay hints, and workspace symbols, all over LSP.

The `vscode/` extension (DC Language Support) wraps `dccd` for syntax
highlighting and editor integration in VS Code. The first-party
[Neovim integration](docs/neovim.md) uses native Neovim 0.11+ LSP APIs on
Linux and Windows, with no external plugin dependencies.

Projects that already use the Zig build system can call dcc through the SDK
in `zig/dcc_sdk.zig`; dpm is the primary build tool and Zig is not needed to
build or use dcc.

## License

GPLv3 or later. See `LICENSE`.
