# A bridge for C strings

Every program that talks to C needs the two conversions from the lecture.
Write them once, carefully, in `src/cbridge.dc`, and the rest of the program
stays clean. `src/main.dc` is finished: it uses `strtol` (parse a number),
`getenv` (look up an environment variable) and `strerror` (message for an
error number) through your functions.

```dc
public Result(const char*, CTextError) to_c([]const char text, []char buffer)
public []const char from_c(const char* text)
```

## to_c

Copies `text` into `buffer`, ends it with a zero byte and returns a pointer
to the start of the buffer, ready to hand to C.

- `CTextError::TooLong` when the text and its terminator do not fit.
- `CTextError::ContainsNul` when the text itself contains a zero byte, since
  C would stop reading at it and see a different string.
- The buffer belongs to the caller. The pointer is only valid while that buffer is.

## from_c

Returns a `[]const char` view of the C string `text`: the characters up to,
but not including, the terminator. The function `strlen` is declared at the
top of the file for the length. A `null` pointer gives an empty slice,
because C functions such as `getenv` use `null` for "nothing".

## What the program prints

```text
parsed 42, rest " apples"
too long: refused
embedded NUL: refused
unset variable: none
strerror(2): No such file or directory
```

The project links `libc` already (`link = { ":libc.so.6" }`). A second check
calls your functions with edge cases, and compares what C sees with what you
copied.
