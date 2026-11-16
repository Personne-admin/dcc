# Teach println your own types

`println("{}", value)` knows integers, floats, `bool`, `char` and text. For
any other type it looks for a function named `format` that takes the value, a
writer and the placeholder's options. Write one and `{}` works for your type
everywhere: `println`, `print`, `std::fmt::format` into a buffer.

```dc
public import std::fmt;

public Status(std::fmt::FormatError(std::fmt::WriterError(W)))
format(W)(const Span* self, W* out, const std::fmt::Options* opts)
    if std::fmt::Writer(W)
{
    return std::fmt::format(out, "{}..{}", self.low, self.high);
}
```

- The function is generic in the **writer** `W`: stdout, a file, a fixed
  buffer and a counting writer all take part. `if std::fmt::Writer(W)` says "any
  type with a `write_all` method". You do not call `println` inside; you
  write to `out`.
- The return type is spelled exactly as above. It means "formatting can fail
  either because the format is bad or because the writer failed".
- `self` is a pointer: read fields with `self.low`.
- Write pieces with `out.write_all("-")?` (raw text) or with
  `std::fmt::format(out, "...", args)` (the placeholders you know). A failed
  write returns early through `?`.
- `opts` carries the placeholder's width, fill and alignment (`{:>6}`).
  `std::fmt::write_padded(out, text, opts)?` writes `text` padded as
  requested. A hook that writes several pieces (like a money amount) is free
  to ignore `opts`; a hook that writes one word should honour it.
- The file needs `public import std::fmt;`: the hook is a public function that
  mentions types from that module.

Write into a fixed buffer with a `BufferWriter`:

```dc
char[64] storage = {};
std::fmt::BufferWriter line = std::fmt::new_buffer_writer(storage[0..64]);
std::fmt::format(&line, "total {}", amount)?;
println("{}", std::fmt::written_slice(&line))?;
```

When the buffer is too small, `format` fails with an error. A piece that does not fit
is not written at all, and the pieces before it stay in the buffer; nothing
ever writes past the end.

## Your task

`src/main.dc` has three hooks with the right signatures and empty bodies;
`main` is finished. Fill in:

1. `Span`: print `low..high`, for example `3..9`.
2. `Money`: print dollars and cents with two digits for the cents: `$12.34`.
   A negative amount has a `-` in front of the dollar sign: `-$0.05`. Work with
   the absolute value of `cents` (as `i64`) and divide by 100 for dollars,
   remainder for cents. `{:02}` pads the cents to two digits.
3. `Level`: write the word `INFO`, `WARN` or `FAIL` with `write_padded`, so
   `{:<6}`, `{:>6}` and `{:^6}` all work. Use a `match` with one
   `write_padded` call per arm.

Expected output:

```text
price: $12.34
refund: -$0.05
[INFO] [WARN  ] [  FAIL] [ INFO ]
spans: 3..9 and -2..2
[ WARN] total $2500.00
buffer too small
```

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly.
- `Level` follows the placeholder's width and alignment.
