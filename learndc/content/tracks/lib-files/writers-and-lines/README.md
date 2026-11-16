# Writers and lines

You have used `println` for output since the first lesson. It formats text and
sends it to *a writer* that happens to be standard output. A file is a
writer too, and so is a buffer in memory. Once your output code is written
against a writer, it works with all of them.

## One function, many destinations

```dc example=row_everywhere
```

- `FileWriter::new(file)` wraps an open `File`. It does not own it: the
  `defer f.close();` still belongs to the code that opened the file.
- `std::fmt::format(&writer, "{:<8}|{:>4}\n", name, qty)` is `println`
  without the fixed destination. `{:<8}` pads to 8 columns on the left side,
  `{:>4}` right-aligns in 4. Each writer reports its own failures.
- `write_row(W)` is generic: it accepts any `W` that satisfies
  `std::fmt::Writer(W)`. The same function wrote to the console, to a file and
  to a buffer. The `?` converts each writer's error into the prelude
  `Error`.
- `FileWriter` over `stdout()` is what `println` does internally.

Nothing here adds a newline for you. `format` writes exactly what the format
says, so a line ends with `\n` in the format string.

## Measuring and bounded buffers

```dc example=measure_first
```

- `CountingWriter` discards the bytes and counts them. It answers "how big
  will this be?" before you allocate or open anything.
- `BufferWriter` fills a fixed buffer. When the output does not fit, the write
  fails with an error instead of overflowing, and what was written before
  the failure stays in the buffer. The second row above wrote part of itself
  before it ran out of room: a failed write is not an undo.

## Reading lines

A file is bytes. To work line by line, read the whole file into memory and
split it. `std::slice::split(text, '\n' as char)` returns a splitter, and
`next` gives each piece as an `Optional`.

```dc example=read_lines
```

- The splitter gives empty pieces too. The empty third line is a blank line
  of the file; the empty fifth piece is what follows the final `\n`. Decide
  what you do with empty lines.
- Files written on Windows may end lines with `\r\n`. The `\r` stays at the
  end of the piece. A program that must read such files strips a trailing
  `'\r'` from each line.
- Each piece is a slice into `text`, not a copy. It stays valid while `text`
  is alive and unchanged.

## Common mistakes

- Forgetting the `\n` at the end of the format string.
- Treating a failed `BufferWriter` write as if nothing was written.
- Closing the `File` through the `FileWriter`: it has no `close`; the
  `File` is closed where it was opened.
- Splitting a text and holding the lines after the `Array` was released.
