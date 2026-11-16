# Reading what SDL tells you

SDL reports two kinds of information in forms DC cannot print directly:

- the **version**, one `i32` that packs three numbers,
- **text**, always as a C string: a `const char*` that points at bytes and
  ends at a zero byte. DC's `println` formats `[]const char`, a slice that
  carries its own length.

`src/sdlinfo.dc` has two placeholder functions. `src/main.dc` already uses
them and prints what SDL reports.

## Task 1: decode_version

`SDL_GetVersion()` returns `major * 1000000 + minor * 1000 + patch`.
`3004020` is version 3.4.20. Fill the `Version` struct with integer division
and `%`.

```text
decode_version(3004020) -> 3.4.20
decode_version(3002008) -> 3.2.8
decode_version(3000000) -> 3.0.0
```

## Task 2: c_text

`c_text(const char* text)` returns the text as a slice, without copying:

1. A `null` pointer is empty text, `""`.
2. Walk the bytes with `text[n]` until you reach the zero byte. A zero byte
   written in DC is `(0 as char)`.
3. Build the slice from the pointer and the length:
   `core::slice::from_raw_const(text, n)` (the module imports `core::slice`).

The slice *borrows* SDL's memory. Print it or compare it right away; do not
keep it across later SDL calls, because the next call may overwrite the
message (see `docs/ownership.md` of the bindings).

## See it

`learndc run` prints, for example:

```text
SDL 3.4.20
revision: SDL-release-3.4.20-0-gf00dc919c
error text: this is a test error
```

The version and revision depend on the SDL3 installed on your machine; the
last line does not. The checks use fixed numbers for the version and compare
the error text exactly.
