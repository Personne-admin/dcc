## Hint 1

Count bytes before you copy. A text of length n needs n + 1 bytes: the
characters and the terminator. Check that first, then copy, checking each
character for a zero as you go.

## Hint 2

In `to_c`: refuse when `text.len >= buffer.len`; loop over the text, return
`ContainsNul` on a `'\0'` character, otherwise copy it; write `'\0'` at
`buffer[text.len]`; return `Result::Ok(buffer.ptr as const char*)`. In
`from_c`: return `""` for `null`, otherwise build the view from the pointer and
the length `strlen` reports.

## Hint 3

`return core::slice::from_raw_const(text, strlen(text));` is the last line of
`from_c`, after `if text == null { return ""; }`.
