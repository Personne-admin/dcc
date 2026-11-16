# Writing JSON: Emitter and writers

Reading is half of the job. A tool that converts, reports or saves data has
to write JSON too, with correct quoting and commas. `std::json` does not
build a text in memory for you. It sends the pieces to a *writer*, and you
choose where they go.

## Writers

A writer is any type with a `write_all` method that the formatter can call:

- `FileWriter` (from `std::os::file`) writes to an open file, including
  `stdout()`.
- `BufferWriter` (from `std::fmt`) fills a buffer you provide, and
  `std::fmt::written_slice(&buffer)` gives back what was written.
- `CountingWriter` only counts bytes, which tells you the size before you
  allocate.

`print` and `println` are the same machinery with standard output as the
writer. Every write can fail, so every call below ends with `?`.

## The Emitter

An `Emitter(W)` borrows a writer and a number of spaces per level. You call
one method per token and it takes care of commas, colons, quotes, escapes and
indentation.

```dc example=emit_object
```

- `begin_object`/`end_object` and `begin_array`/`end_array` must be paired
  by you. Inside an object, call `key(name)` and then exactly one value call:
  `string`, `integer`, `float`, `boolean`, `null_value`, a nested begin, or
  `value(&v)` for a whole `Value`.
- `string` escapes quotes, backslashes and control characters, so any text is
  safe to emit.
- An indent of 0 writes compact JSON without any newline; a larger number
  indents each level by that many spaces.
- The emitter does not check structure. A `end_object` where an array
  should end writes a `}` and carries on, so the output is only valid when your
  calls are balanced.
- Floats are written with six decimals (`2.5` becomes `2.500000`).

## Into a buffer

```dc example=buffer_output
```

The buffer has a fixed size. When the output does not fit, the write fails
and `?` turns the failure into `Error::FormatBuffer`. Size the buffer for the
largest output you expect, or use a file.

## Writing a parsed tree back

`std::json::write(&writer, &value, indent)` renders a whole `Value` in one
call. That makes a pretty-printer or a minifier from a parse and a write.

```dc example=rewrite
```

Members keep their original order. Parsing and writing again does not
preserve the original spacing, number spellings or `\u` escapes: it writes
the same data, not the same bytes.

## Common mistakes

- Writing a value without a key inside an object, or a key without a value:
  the output is invalid JSON and nothing warns you.
- Forgetting the final newline: the emitter never writes one.
- Building JSON with `println` and string pieces. A name with a quote in it
  breaks the output. Let `string` escape.
- Using a buffer that is too small and ignoring the error.
