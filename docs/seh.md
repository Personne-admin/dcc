# Structured exception handling

dcc implements asynchronous structured exception handling (SEH) for the
`x86_64-coff` target with the LLVM backend. The language rules are in
docs/spec.md section 21; this page covers flags, the runtime contract and
limitations.

## Flags

- `-fseh` enables `try`/`except`/`finally`/`leave` and makes every `defer`
  run during exception unwinding. It is off by default; code compiled
  without it is unchanged.
- `-fseh-runtime=libdcext|external` selects who provides
  `__C_specific_handler`, the language handler named by every unwind table
  with an SEH scope.
  - `external`: the operating system provides it. On hosted Windows
    (`-flibdcext windows`) dcc links `ntdll`, which exports it. This is the
    default for hosted Windows and for builds without libdcext.
  - `libdcext`: libdcext's implementation (`dcext::seh`,
    `__dcext_C_specific_handler`) is used. Each object with SEH scopes
    carries the linker directives
    `/alternatename:__C_specific_handler=__dcext_C_specific_handler` and
    `/include:__dcext_C_specific_handler`, so an existing
    `__C_specific_handler` definition still takes precedence. This is the
    default for `-flibdcext freestanding`.

In project.dcon, pass them in the build settings' `extra_flags`, for
example `extra_flags = { "-fseh", "-fseh-runtime=libdcext" }`.

## Runtime contract

Unwind data follows the Microsoft x64 exception handling ABI: `.pdata`
function entries, `.xdata` unwind codes, and a C scope table
(`UNWIND_INFO` handler data: a count followed by 16-byte records with
image-relative begin, end, handler and jump target). Filters are separate
functions `i32 (ExceptionPointers*, void* establisher_frame)`; termination
handlers (`finally` blocks and defers) are funclets called as
`void (u8 abnormal, void* establisher_frame)`. A handler address of 1 means
"always handle".

An operating system that dispatches exceptions to DC code must:

- walk frames with the `.pdata`/`.xdata` tables (`RtlVirtualUnwind`
  semantics) and call the language handler with
  `(EXCEPTION_RECORD*, establisher frame, CONTEXT*, DISPATCHER_CONTEXT*)`,
  honouring `DISPATCHER_CONTEXT.ScopeIndex` for collided unwinds;
- provide `RtlUnwindEx(target_frame, target_ip, record, return_value,
  context, history)`, which runs the termination handlers of the frames
  between the faulting frame and the target and resumes at `target_ip` with
  `return_value` in RAX. libdcext's handler passes the exception code as
  `return_value`; `exception_code()` in an `except` handler reads it from
  RAX, as MSVC-compatible handlers do;
- provide `RaiseException(code, flags, count, arguments)` if programs call
  `core::seh::raise_exception`.

`-fseh-runtime=libdcext` only needs `RtlUnwindEx` (and `RaiseException` for
`raise_exception`); `-fseh-runtime=external` also needs
`__C_specific_handler`.

## Code generation

- Memory accesses inside a `try` body are volatile, so a faulting access is
  neither removed nor moved out of the guarded range, and the handler and
  `finally` blocks see the values stored before the fault.
- Functions with SEH scopes or unwinding defers skip dcc's own IR
  optimisation passes and are not inlined into other functions; LLVM still
  optimises them.
- Inline assembly inside a `try` body is covered by the guarded range but is
  not treated as a call that can throw.

## Limitations

- Only `x86_64-coff` with the LLVM backend. The custom backend writes no
  unwind data, and 32-bit Windows SEH (frame-based, SAFESEH) is not
  implemented.
- Defers in frames compiled without `-fseh` do not run during unwinding.
- Unused arithmetic that would fault (for example an unused division by
  zero) may still be removed by LLVM, as with other compilers.
