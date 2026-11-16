# TOML documents: tables, paths and arrays

TOML is the format people write by hand: comments are allowed, there are
no braces around the file, and sections are named in square brackets. It is
the format of many build and package tools, and a good fit for the
settings of your own programs. `std::toml` reads it with the same ideas as
`std::json`: parse into an owned document, read with Optional lookups,
release with `deinit`.

## The tree

`std::toml::parse(text)` returns a `Result(Document(), TomlError)`. The
document's `root` is a `Table`: a list of `entries`, each a key and a
`Value`, in the order they were written.

```text
Value::String([]const char)        Value::Integer(i64)
Value::Float(f64)                  Value::Bool(bool)
Value::Array([]Value)              Value::Table(Table)
Value::OffsetDateTime(..)          Value::LocalDateTime(..)
Value::LocalDate(..)               Value::LocalTime(..)
```

Dates and times are real values, not strings: `2026-11-16` becomes a
`LocalDate` with `year`, `month` and `day`. There is no `Null` variant,
because TOML has no null. A missing key is an empty Optional.

## Reading with paths

A `Table` has `get(key)` for one level and `get_path("a.b.c")` for nested
tables. Both return `Optional(Value)`. The `as_*` conversions are the same as
in JSON, plus `as_array` and `as_table`.

```dc example=toml_read
```

- `[server]` and `[server.tls]` create the nested tables that `get_path`
  walks. A dotted key `a.b = 1` creates them too.
- There is no `Null` to fall back on, so the helper tests `is_none()` first.
  `some_or(Value::Bool(true))` also works when a placeholder value is
  convenient.
- Wrong kind and missing key are told apart only if you ask: `server.port`
  exists, but `as_string` is empty for an integer.
- Comments (`#`) are skipped. Integers may use `_`, hex (`0x1F`) and so on.
- `deinit` releases everything, like a JSON document. The strings you read
  stay valid until then.

## Arrays and arrays of tables

`as_array()` gives a slice of values, so a `for` loop walks it. A repeated
`[[user]]` header makes an *array of tables*: one table per header.

```dc example=toml_arrays
```

`get_path` stops at an array. You cannot write `user.0.name`; take the
array with `get("user")` and loop.

A TOML array may mix kinds (`[1, "x"]` parses). Check each element with an
`as_*` conversion.

## Errors

`TomlError` has a variant for each way a text can be wrong. Two groups matter
for reporting:

- *Syntax errors* (`UnexpectedByte`, `UnexpectedEnd`, `InvalidString`,
  `InvalidNumber`, `InvalidDate`, `ExpectedNewline`, ...) are found by the
  scanner too, so `std::toml::scan` and `next` give the line and column, in
  the same way as in the JSON lecture.
- *Document errors* (`DuplicateKey`, `KeyConflict`) are only noticed while the
  tree is built. The scanner sees nothing wrong, so there is no position.

```dc example=toml_errors
```

The last two lines of the output show `ExpectedNewline` and `InvalidString`
located by the scanner; the two duplicates have no position. A real tool
would report the key name itself, which the library does not hand you.

## Limits

- Nesting is limited (32 levels) and a dotted key may have at most 16 parts.
- `get_path` cannot index arrays and keys with dots in their names cannot be
  reached through it; use `get` level by level.
- There is no TOML writer in the library. To produce TOML you print it
  yourself, or you convert the document to JSON and use the JSON emitter.
- Lookup is linear in the number of entries. Fine for settings, not for
  millions of keys.

## Common mistakes

- Using the string `"8080"` in the file and `as_integer` in the code. The
  conversion is empty, and a default silently wins. Decide whether a wrong
  kind is an error or a default.
- Reading a string after `deinit`.
- Expecting `Null`: a key set to nothing is not valid TOML.
- Looking for a position for `DuplicateKey`.
