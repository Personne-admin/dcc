# The Native Bridge

The lab's instrument writes numbers into a text file; its tools for sorting
and square roots are in C. You build the module in between, `src/native.dc`,
so that the rest of the program never sees a raw `errno` or an unchecked C
call.

The starter project already links the C library and libm:

```dcon
link = { ":libc.so.6", ":libm.so.6" },
```

and provides, finished:

- `src/cstr.dc`: your `to_c` and `from_c` from the string bridge lesson.
- `src/text.dc`: `next_line(data, &position)`, which returns the next line of
  a text as an `Optional([]const char)`.
- `data/readings.txt`: the lab's file, nine lines long, with some bad lines.
- the extern declarations at the top of `src/native.dc`, and the error enums.

Every function in `native.dc` returns a placeholder. Each stage is checked on
its own.

## Stage 1: Read the old glue

Answer three questions about the legacy wrappers.

## Stage 2: Parse with typed errors

```dc
public Result(i64, ParseError) parse_int([]const char text)
```

- `Empty` for empty text.
- `NotANumber` when strtol consumes no characters, and for text that cannot
  be converted (a zero byte inside, or longer than 63 characters).
- `OutOfRange` when strtol sets `errno` to `ERANGE` (34).
- `TrailingText` when characters remain after the number. strtol itself skips
  leading spaces, so `"  5"` is fine and `"7 "` is trailing text.

`errno` is read through `*__errno_location()`. Clear it before the call and
read it straight after.

## Stage 3: Sort with a context

```dc
public void sort_values([]i32 values, bool descending)
```

Sort the slice in place with `qsort_r`. The direction travels in a struct
passed as the `void*` context; the comparator returns -1, 0 or 1.

## Stage 4: Math with a domain check

- `checked_sqrt(x)` is `Ok(sqrt(x))` for `x >= 0` and `MathError::Domain`
  for negative numbers and NaN.
- `checked_log(x)` is `Ok(log(x))` for `x > 0`, and `Domain` otherwise.
- `distance(x1, y1, x2, y2)` is `hypot(x2 - x1, y2 - y1)`.

## Stage 5: Read a file, leak nothing

```dc
public Result(usize, FileError) read_file([]const char path, []u8 buffer)
```

Read the whole file into `buffer` and return the number of bytes.

- Path conversion fails: `BadPath`.
- `fopen` fails: map `errno` 2 to `NotFound`, 13 to `PermissionDenied`, 21 to
  `IsDirectory`, anything else to `Other`. (Opening a directory succeeds on
  Linux; the error appears when you read it.)
- The file does not fit: `TooBig`. A buffer of exactly the file's size is
  enough; to tell the difference, try to read one more byte when the buffer is
  full.
- The `FILE` is closed on every path, success or error. The check calls
  `read_file` 1500 times in a row, and a leaked handle runs out long before.

## Stage 6: The report

Write `main` in `src/main.dc`. It must print exactly:

```text
line 3: not a number
line 5: trailing text
line 6: out of range
line 8: empty
read 9 lines
sorted: 144 49 25 9 -4
roots: 12.0 7.0 5.0 3.0 domain
data/missing.txt: not found
distance (0,0)-(3,4): 5.0
ln(1) = 0.0, ln(0) is an error: true
```

1. Read `data/readings.txt` into a `u8[1024]` buffer with `read_file`.
2. Go through the lines with `text::next_line`, counting from 1. Print
   `line N: ...` for every parse error and keep the good numbers (as `i32`).
3. Print `read N lines`, sort the numbers descending, print `sorted:` and
   `roots:` (square roots with one decimal, `domain` for an error).
4. Read `data/missing.txt` and print `data/missing.txt: not found`, then the
   distance and the two logarithms.

A `match` on an error enum can print inside each arm (`println(...)?`);
returning the text from the `match` is a known problem for the custom backend.

## Bonus: the environment

`env_or(name, fallback)` returns the value of an environment variable as a
view of C's memory, or `fallback` when the variable is not set.
