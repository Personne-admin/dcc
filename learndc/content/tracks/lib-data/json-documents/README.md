# JSON documents: parse, read, release

Programs meet JSON everywhere: settings files, API replies, saved games.
`std::json` turns the text into a tree of values you can walk, and it does
so without a hidden garbage collector: someone owns the tree, and that one
owner gives it back.

## From text to a tree

`std::json::parse(text)` reads a whole JSON text and returns a
`Result(Document(), JsonError)`. A `Document` has two parts: the allocator
that built the tree and `root`, the top value. Every value is an enum:

```text
Value::Null
Value::Bool(bool)
Value::Integer(i64)
Value::Float(f64)
Value::String([]const char)
Value::Array([]Value)
Value::Object([]Member)      a Member is { key, value }
```

An object keeps its members in the order they were written. Whole numbers
become `Integer`; a number with a fraction or exponent becomes `Float`.

## Parse, read, release

```dc example=parse_read
```

- `Document() doc` uses the default allocator, which is the heap in a
  hosted program. The empty parentheses mean "no allocator argument".
- `parse` copies every key and string out of `text`, so the document does
  not depend on the text staying alive.
- `defer doc.deinit();` frees the whole tree, keys and strings included, in
  one call. It is the same pattern as `Array`: the variable that holds the
  document releases it.

## Lookups return Optional

`get(key)` on an object and `at(index)` on an array return an
`Optional(Value)`: the key may be missing, and the value may be of another
kind than you hoped. The conversions repeat that idea: `as_string`,
`as_integer`, `as_float`, `as_bool` each return an `Optional` that is empty
when the value has a different kind. `some_or(fallback)` turns an empty
Optional into a default.

Read the example again: `visits.as_string()` does not turn 3 into "3". It is
empty, so the program prints the fallback. `as_float` is the one conversion
that is generous: an `Integer` widens to a float.

`len()` counts the members of an object or the items of an array, and is 0
for everything else.

## Walking a whole tree

When the shape is not known in advance, match on the `Value` and recurse.

```dc example=walk_tree
```

A `match` on a value needs every variant. The `Array` and `Object` arms
carry slices, so a plain `for` loop visits them. The values inside belong to
the document: you read them, you never free them one by one.

## Who owns what

```dc example=own_copy
```

- `clone()` builds a second, independent tree. Each copy needs its own
  `deinit`.
- `take()` moves the tree out and leaves the old document empty (its root is
  `Null`). Use it to hand a document to a function that will release it.
- `deinit` is safe on an empty document and safe to call twice.
- Copying the variable (`Document() other = doc;`) copies only the small
  handle. Two owners would free the same memory: never do that, use `clone`.

## Choosing the allocator

A document stores the allocator it was built with. The default is the heap,
but any allocator policy works, for example a fixed buffer on the stack
that never touches the heap.

```dc example=fixed_storage
```

The type changes to `Document(FixedHandle)`. A tree that does not fit in the
buffer fails with `JsonError::OutOfMemory` instead of crashing.

`std::json::parse_borrowed` skips the copy of strings that need no
unescaping and returns a bare `Value` that points into your text. Nothing
records the allocator then, and the library has no public function that
frees such a tree. Use it only with an allocator that you discard as a
whole, and keep the text alive as long as the value. When in doubt, use
`parse`.

## Common mistakes

- Reading `doc.root` after `deinit()` or after `take()`.
- Keeping a `[]const char` from `as_string()` after the document is released:
  the bytes were freed with it. Copy what you need first, or finish using
  it before the deinit runs.
- Treating `get` as infallible: calling `unwrap_some()` on a missing key
  stops the program. Use `some_or` or test `is_some()`.
- Expecting `as_integer()` on `3.0` to work. A JSON `3.0` is a `Float`.
- Expecting numbers beyond i64 to parse: they fail with
  `JsonError::NumberOutOfRange`.
