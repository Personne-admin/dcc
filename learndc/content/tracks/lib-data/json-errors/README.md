# JSON errors: what failed, and where

Text from outside is wrong more often than you expect. `std::json::parse`
reports failure as a value, `Result(Document(), JsonError)`, and the program
decides what a failure means. This lecture covers the error values, three
ways to react, and how to find the line and column of a syntax error.

## The error values

`JsonError` is a plain enum. The ones you meet in practice:

| Variant | Meaning |
|---|---|
| `UnexpectedByte` | a character that cannot appear here: a trailing comma, a single quote, a comment |
| `UnexpectedEnd` | the text stops inside a value: `{"a": ` or `[1, 2` |
| `InvalidNumber` | a malformed number such as `01` or `1.` |
| `NumberOutOfRange` | an integer that does not fit in i64, or a float that overflows |
| `InvalidEscape` | a backslash escape JSON does not have, such as `\q` |
| `ControlCharacter` | a raw tab or newline inside a string |
| `TrailingBytes` | a complete value followed by more text |
| `DepthExceeded` | nesting deeper than 64 levels |
| `OutOfMemory` | the allocator could not provide the tree |
| `TypeMismatch` | for your own use: "valid JSON, wrong shape" |

The parser is strict JSON: no comments, no trailing commas, no single quotes.
Leading and trailing whitespace is fine, and an empty text is
`UnexpectedEnd`.

An enum has no built-in text, and `println("{}", error)` does not compile.
Match on it and print a message of your own.

## Three ways to react

```dc example=handle_errors
```

- `read_port` returns `Result(i64, JsonError)`. A `?` on `parse` returns the
  parse error to the caller unchanged, and the deferred `deinit` still runs
  on every exit path (a failed parse returns no document to release).
- `JsonError::TypeMismatch` is not produced by the parser. It is there so
  your own code can say "the JSON was fine, the shape was wrong" in the same
  type.
- In `report`, a `match` with a final `_` arm gives specific messages for
  the errors users can fix and one general message for the rest. Print
  inside the arms.
- A `Status(Error)` function such as `main` can simply write `?` on the
  parse. The error converts to `Error::Json(e)` and the program exits with
  status 1, without any message. Good for a quick tool, bad for a user.

## Where is the error?

The parse result carries only the variant, not the position. To report a
line and column, run the pull parser `Scanner` over the same text. It
produces events (`Event::ObjectBegin`, `Event::Key(name)`, `Event::Integer(n)`
and so on) one at a time, each wrapped in `Located(Event)`. When it fails,
`scanner.at` holds the position where it stopped.

```dc example=locate
```

- `scan(text)` allocates nothing and copies nothing, so there is nothing to
  release.
- `Position` has `line` and `column` (both start at 1) and `offset`, the
  byte index. Columns count bytes, not characters.
- The scanner stops at the offending byte, or just after it when it was in
  the middle of a token (the misspelled `tru}` is reported one column past
  the `}`). Report it as "near", and show the line.
- A failure that is not about syntax, such as running out of memory, does
  not stop the scanner, so it reports `valid`.

## Common mistakes

- Printing nothing on failure. Exit code 1 alone tells a user nothing.
- Using `unwrap()` on a parse result of external text: a bad file crashes
  the program.
- Reading the position of `parse`: it has none. Rescan with `Scanner`.
- Forgetting that the scanner needs the same text. Keep the text alive while
  you report.
