# Standard errors

`std::error::Error` is a closed tagged enum for everyday standard-library
failures. It stores the original error value inline, without allocation or
borrowed pointers. It has the same variants on hosted and freestanding targets.
There is no success variant; use `Result(T, Error)` or `Status(Error)`.
Library sources use explicit imports and build with `-fno-prelude`, so
compiling a library module does not inject an import back into its consumers.

`Error` is unqualified when the standard prelude is active. The compiler
injects it into user modules under `-flibdcext`, including imported helpers.
Compiler core and registered library implementation files are excluded.
`-fno-prelude` or a module's `@no_prelude` header disables automatic injection.
Explicit imports still work (use `public import` when exposing explicitly
imported types in public signatures). For qualified access, write
`import std::error;` and use `std::error::Error`.

## Supported errors

| Error variant | Original type |
| --- | --- |
| Allocation | `std::alloc::AllocError` |
| Io | `std::os::error::Error` |
| Print | `std::os::print::PrintError` |
| FormatBuffer | `std::fmt::FormatError(std::fmt::BufferError)` |
| FormatCount | `std::fmt::FormatError(std::fmt::CountError)` |
| FormatIo | `std::fmt::FormatError(std::os::error::Error)` |
| Buffer | `std::fmt::BufferError` |
| Count | `std::fmt::CountError` |
| FormatParse | `std::fmt::ParseError` |
| Number | `std::parse::NumError` |
| Utf8 | `std::utf8::Utf8Error` |
| Json | `std::json::JsonError` |
| Toml | `std::toml::TomlError` |
| Sha | `std::sha::ShaError` |

File, directory, network and other OS operations already share the OS error
type. Collections report allocation errors. The formatting variants preserve
both the formatter variant and any nested writer error. Information already
lost by a source API, such as an unmapped native OS code, is not reconstructed.

Each payload type has exactly one `@implicit_construction` variant. For example:

```dc
module example;
import std::prelude;
import std::alloc;

Status(std::alloc::AllocError) step() {
    return std::alloc::AllocError::OutOfMemory;
}

Status(Error) work() {
    step()?;
    return Status::Ok;
}
```

Here `?` constructs `Status::Err(Error::Allocation(original_error))` on failure.
Success paths are unchanged. Matching `Error` values propagate unchanged.
Ordinary construction supports `Error e = original_error;`; an ordinary return
from `Status(Error)` does not recursively construct both wrappers. Use `?` or
explicitly construct `Status::Err(Error::Allocation(original_error))`.

Inspect the preserved value with pattern matching. Enum equality compares only
the variant, not its payload:

```dc
match failure {
    Error::Allocation(cause) => handle_allocation(cause),
    Error::FormatIo(cause) => handle_formatting(cause),
    _ => handle_other(failure),
}
```

`PrintError` is available as a type on freestanding targets, but printing still
requires a hosted target. The existing `std::os::print` API writes bounded
best-effort records to standard error; this module does not change that API,
provide unqualified printing or arrays, select an allocator, or change the
supported `main` signatures. A successful short write remains a byte count,
not an error that this type can convert.

## Application-specific errors

Unrelated user enums and `FormatError(UserError)` do not implicitly convert to
`Error`. No arbitrary value, integer or pointer is treated as an error.
An application can use its own closed error enum:

```dc
enum ApplicationError {
    @implicit_construction Library(Error),
    @implicit_construction Input(InputError),
}
```

A function returning `Status(ApplicationError)` can propagate `Error` or
`InputError` with `?`. It cannot propagate `AllocError` through both
`Error::Allocation` and `ApplicationError::Library` in one operation: only one
nested error conversion is supported. First propagate into a function returning
`Status(Error)`, or explicitly construct the wrappers.

The type is not extensible and has no erased user-error storage. Supporting
arbitrary owned user errors without losing payloads requires a separate design
for bounded storage, type identity, recovery and cleanup, or an explicitly
opted-in conversion protocol. That protocol is not part of this API.
