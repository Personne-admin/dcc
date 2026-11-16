# Read settings with defaults

A server reads its settings from JSON. The file is written by people, so
keys are missing and values have the wrong kind. The program must never stop
for that: every setting has a default.

`std::json` gives you three pieces for this:

```dc
Value found = root.get("port").some_or(Value::Null);  // missing key: Null
i64 port = found.as_integer().some_or(80);            // wrong kind: 80
```

`get` returns an `Optional(Value)`. `some_or(Value::Null)` makes a missing
key look like JSON `null`, and every `as_*` conversion on a `Null` (or on any
value of the wrong kind) is empty, so the second `some_or` supplies the
default. One lookup pattern covers both failures.

## Your task

`src/main.dc` parses a settings text and prints eight settings. `main` is
finished. Implement the three stubs:

1. `text_or(root, key, fallback)` returns the string stored under `key`, or
   `fallback`.
2. `int_or(root, key, fallback)` returns the integer, or `fallback`.
3. `bool_or(root, key, fallback)` returns the boolean, or `fallback`.

`root` is the document's root object. Notice that the document is owned by
`main`: the strings the helpers return point into it, and `main` prints them
while the document is still alive.

The program must print:

```text
name: Night Market
port: 8080
max_players: 16
timeout: 30
debug: true
verbose: false
motd: (none)
region: unset
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches the eight lines above exactly and the exit code is 0.
- Missing keys and keys of the wrong kind (a string where a number is
  expected, `null` where text is expected) give the default.
