# Getting started with DC

Install dcc and DPM together, or build this repository with
`CXX=clang++ CC=clang AR=ar make -j2`. For a source build, add `build/bin` to
PATH. Keep the compiler, library headers and library archives from the same
build. See the [README](../README.md) for installation and toolchain details.

## Create, build and run

```sh
dpm new hello
cd hello
dpm build
dpm run
```

The built-in executable template works offline. It creates `project.dcon`,
`src/main.dc` and `.gitignore`. Its hosted product enables libdcext and the
hosted default allocator automatically. DPM may print build-progress messages;
the program writes `Hello, world!` to stdout and exits successfully.

The generated `src/main.dc` is:

```dc
module main;

public Status(Error) main() {
    println("Hello, world!")?;
    return Status::Ok;
}
```

`Status(Error)` represents success or a standard-library error. `?` continues
when an operation succeeds and returns its error when it fails. At the hosted
entry boundary, success exits with code 0 and failure with code 1. Errors are
not automatically printed. `Status::Ok` is a payload-less variant value;
write it without parentheses. Integer-returning mains remain supported.

## Values, arrays and formatted output

Replace `src/main.dc` with this complete program, then run `dpm run` again:

```dc
module main;

public Status(Error) main() {
    println("Hello, DC!")?;

    Array(i32) numbers = {};
    defer numbers.deinit();

    numbers.push(10)?;
    numbers.push(20)?;

    for number in numbers.items() {
        println("Number: {}", number)?;
    }

    return Status::Ok;
}
```

It prints:

```text
Hello, DC!
Number: 10
Number: 20
```

`i32` is a signed 32-bit integer. `{}` constructs an empty array; `[]` is not
an array constructor. `push` can grow the array and can fail to allocate memory.
`items()` borrows its elements as a slice for iteration. A string literal such
as `"Hello, DC!"` is borrowed text; printing it does not require an owned string.
Formatting placeholders consume arguments in order, for example
`println("{} + {}", 10, 20)` inside a fallible function, followed by `?`.

Cleanup is explicit. `defer numbers.deinit()` runs when the function exits,
including an early return through `?`. Do not copy an owning array and deinit
both copies. Growth can invalidate borrowed pointers and slices. Deinit frees
the array's backing storage; it does not clean up resources owned by elements.

The current hosted Heap is a simple page-backed policy, not an optimized
general-purpose allocator. Applications need no heap initialization. Allocation
contents are not a substitute for explicitly initializing values. Details and
alignment limits are in [hosted library basics](hosted.md).

## Functions, modules and errors

Move reusable work into functions returning `Status(Error)` and call them
with `?`. Ordinary imported user modules receive the same prelude, so helper
modules can use `Array`, `println`, `Status` and `Error` without library imports.
Declare helpers `public` to make them available to another module; import that
module explicitly in the caller, for example `import helper;`, and call
`helper::work()?` from a fallible function.

The shared prelude supplies `Result`, `Status`, `Optional`, `Error`, `Array` and
`AllocError`. Hosted builds add `print`, `println` and `PrintError`. Printing
uses stdout, can produce partial output on failure and returns a precise error.
Known library errors convert into `Error` through `?` without heap allocation.
Arbitrary user-defined errors do not implicitly convert to `Error`. A main can
instead return `Status(CustomError)` when an application uses its own error
type; the same exit-code rule applies.

Explicit bindings and imports override automatic defaults. Use `-fno-prelude`
to disable automatic imports while retaining library linking and the hosted
allocator, or put `@no_prelude` after a module path to opt out in that module's
header. Such code must import and qualify its library names explicitly. Types
in public signatures need public imports. Separately compiled modules must use
consistent library, target and allocator settings.

Read the [language specification](spec.md) for syntax and type rules,
[standard errors](errors.md) for supported conversions and payload inspection,
and [hosted library basics](hosted.md) for allocation and printing contracts.
Use [DPM templates](dpm-templates.md) and [DPM tasks](dpm-tasks.md) for packages,
custom projects and execution settings. Library and freestanding/kernel
projects retain their own templates and startup rules.
