# Text across the boundary

C has no string type. A "string" is a pointer to bytes that ends at the first
zero byte, the NUL terminator. DC's text, `[]const char`, is a slice: a
pointer *and a length*. Moving text between the two worlds means converting
between "length known" and "length found by searching".

## DC text has no terminator

A string literal in DC is a slice of exactly the characters you wrote. The
compiler does not append a zero byte. The bytes after it are whatever the
compiler stored next, so a C function that searches for a terminator can
read well past the end, and which bytes it finds can change with the
optimization level or the backend.

You add the terminator yourself, as part of the literal:

```dc example=terminator
```

`"abc\0"` is a four-byte slice; its pointer, `terminated.ptr`, is a
`const char*` that a C function can read safely. The slice length counts the
zero. This is the whole trick for text that is known at compile time. Writing
`"abc"` where a `const char*` is expected also compiles, but then nothing
guarantees the terminator.

## Passing a slice is an error

C wants a pointer, DC has a slice, and the compiler does not guess:

```text
error: no matching call for `strlen`
```

A call `strlen(text)` with `[]const char text` fails. Pass `text.ptr`, and
make sure the bytes behind it really end with a zero.

## From C to DC: find the length, then view it

A C function returns a `const char*` (for example the environment variable's
value from `getenv`, or an error message from `strerror`). To use it as DC
text you need a length. Find it by scanning for the zero, then build a view
with `core::slice::from_raw_const`, which you met in the memory chapter:

```dc example=own_strlen
```

You could call C's own `strlen` for the length, which is what the next
exercise does. Writing the loop once shows what that function does: C does
not know the length either, it walks the bytes.

Two properties follow:

- The result is a view, not a copy. It stays valid only as long as the C
  memory it points into. For `getenv` that is until the environment is
  changed; for a buffer you filled it is until you reuse the buffer.
- A pointer has no range indexing: `p[0..n]` is rejected with "cannot
  range-slice a pointer". `from_raw_const` is the way to say "n elements
  starting here".

`null` is a valid answer from many C functions ("no such variable"). Compare
with `null` before you search for a terminator.

## From DC to C for text built at run time

Text that is not a literal, for example a path read from a file or typed by a
user, has no terminator and may be a slice of a bigger buffer. Copy it into a
buffer you own, add the zero, and pass the buffer:

```dc example=to_buffer
```

`copy_text` refuses text that does not fit, and the check is `>=`, not `>`:
the terminator needs one byte too. A complete conversion also has to refuse a
text that already contains a zero byte, because C would stop reading there
and see a shorter string than you meant.

## Common mistakes

- Passing a literal without `\0` to a C function.
- Passing a slice instead of its `.ptr`.
- Writing the terminator at `out[text.len]` after checking only
  `text.len > out.len`: one byte past the end.
- Keeping a view of a C string after the C library reused the memory.
- Forgetting to test for `null` before reading what C returned.
