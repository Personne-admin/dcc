# Hosted library basics

Enable the library with `-flibdcext` (the default for DPM host products).
The prelude is supplied to user modules, including imported helpers. Library
implementation files and compiler core modules do not receive it. A module
can opt out with `module package::low_level @no_prelude;`; `-fno-prelude`
disables automatic injection for a compilation without disabling library
linking or the hosted default allocator.

## Heap and arrays

Hosted library builds select `std::os::heap::Heap` as `DefaultAllocator` unless
`-fdefault-allocator=module::Policy` selects another policy. SDK sessions can
set `CompileOptions.libdcext`; enabling the prelude also opts into the hosted
default. Freestanding targets retain the unselected default and require an
explicit policy. Use consistent allocator settings for separately compiled
objects: allocator arguments are part of container types.

`Heap` is zero-sized and needs no initialization. The initial implementation
uses OS page allocations. It returns a slice of exactly the requested length,
accepts power-of-two alignments up to the platform page size, and reports
`AllocError::InvalidAlignment` or `OutOfMemory`. A zero-length allocation returns
an empty slice and can be freed. The policy itself owns no allocations; the
caller owns every returned block.

`resize_in_place` preserves the address and contents on success and leaves the
allocation unchanged on failure. The caller updates its slice length only after
success. A nonempty block cannot resize in place to zero; free it or use the
existing moving `std::alloc::reallocate` helper. Moving reallocation allocates
first, copies the common prefix and frees the old block only after success.

Arrays grow through the existing static allocator contract. Register cleanup
with `defer values.deinit()`. Growth can invalidate borrowed pointers and slices.
Copying an owning array does not transfer ownership, and deinit does not destroy
resources owned by its elements. These ownership rules are unchanged.

Custom default allocator modules must be leaf dependencies of the allocator
configuration. They do not receive automatic prelude imports, which would
create a configuration cycle; use explicit imports there. A module-level
`@no_prelude` makes that policy explicit when compiling it in other contexts.

The public Heap policy does not expose page ownership or require raw page API
calls. Page-backed allocation is intentionally simple and can waste memory for
small objects; its internals can be replaced without changing `Array(T)` or
requiring per-instance heap state.

## Stdout printing

`std::io::print` and `println` use the general formatter and a stdout
`FileWriter`. They return `Status(std::io::PrintError)`, where `PrintError` is
`std::fmt::FormatError(std::os::error::Error)`. Formatting failures preserve
their kind; write failures preserve the OS error. `?` converts these errors
into `Error::FormatIo` without allocation.

Built-in formatting uses stack storage and has no 2048-byte record limit.
Writes can be partial before an error, and separate calls are not atomic.
Short writes are retried through `write_all`. `println` appends exactly one
newline after successful formatting, even if the format already ends in one.
User formatting hooks obey the existing formatter contract and may have their
own allocation behavior. Malformed formats are validated before output starts.

The explicitly imported `std::os::print` APIs retain their bounded, best-effort
stderr behavior. Use explicit imports to select those APIs where needed.

## Without explicit imports

The shared automatic names are `Result`, `Status`, `Optional`, `Error`, `Array`
and `AllocError`. Hosted modules additionally receive `print`, `println` and
`PrintError`. Explicit imports and lexical bindings override these defaults.
Import `std::prelude::hosted` explicitly if selecting its exports by hand;
`std::prelude` itself remains shared with freestanding targets.

```dc
module main;

Status(Error) do_work() {
    println("Hello, world!")?;
    Array(i32) numbers = {};
    defer numbers.deinit();
    numbers.push(42)?;
    numbers.push(100)?;
    for value in numbers.items() {
        println("Value: {}", value)?;
    }
    return Status::Ok;
}

public i32 main() {
    return if do_work().is_ok() { 0 } else { 1 };
}
```

This program is exercised by `tests/stdlib/beginner.dc`. Use `{}` for an empty
array; `[]` is not an array constructor. `Status::Ok` is a payload-less value,
not a function call. Hosted main can instead return `Status(Error)` directly;
success exits 0 and failure exits 1 without automatic error printing.
`Status(Error) main([] const char* args)` receives the command-line
arguments like `i32 main([] const char* args)`; `args[0]` is the program
path on Linux, and Windows currently passes an empty slice.

Run `dpm new hello` and `dpm run` for a generated fallible Hello World.
See [getting started](getting-started.md) for a direct Status main with arrays.
The existing integer-main forms remain supported. Imported
user helpers receive the same prelude when compiled under the same library,
target and allocator settings. Packages that require independence from the
prelude should pin `@no_prelude` and explicit imports in their module headers.

## External commands

Use the explicitly imported `std::process::Command` for owned, reusable,
shell-free commands. See [Process commands](process.md) for arguments,
child-specific cwd and environment, stream redirection and child ownership.
