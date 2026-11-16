# Convert TOML to JSON

Many tools want JSON, and people prefer to write TOML. The standard library
reads TOML and writes JSON, so the bridge between them is a small program: a
walk over the parsed tree that feeds an `Emitter`. It is the last piece of
this chapter and uses all of it.

A TOML `Value` is one of: `String`, `Integer`, `Float`, `Bool`, the four
date and time kinds, `Array([]Value)` and `Table(Table)`. A `Table` is a list
of `entries`, each with a `key` and a `value`. The matching JSON is:

| TOML value | JSON written |
|---|---|
| `String(text)` | `json.string(text)` |
| `Integer(n)` | `json.integer(n)` |
| `Float(x)` | `json.float(x)` |
| `Bool(b)` | `json.boolean(b)` |
| `LocalDate(d)` | a string such as `"2026-11-16"` (the finished `emit_date`) |
| `Array(items)` | `begin_array`, every item, `end_array` |
| `Table(t)` | `begin_object`, a `key` and a value per entry, `end_object` |
| any other date or time | `null` |

## Your task

`src/main.dc` parses a TOML text, writes the JSON into a buffer, prints it,
and then parses the output again with `std::json` to prove it is valid. `main`
and `emit_date` are finished. Implement the two functions that call each
other:

1. `emit_table(json, table)` writes an object with one member per entry, in
   the order of `table.entries`.
2. `emit_value(json, value)` matches on the value and writes it as shown in
   the table above. Arrays and tables call back into the emitter functions.

The program must print:

```text
{
  "title": "Demo \"quoted\"",
  "count": 3,
  "ratio": 2.500000,
  "debug": true,
  "released": "2026-11-16",
  "server": {
    "host": "localhost",
    "ports": [
      80,
      443
    ]
  },
  "user": [
    {
      "name": "ann",
      "roles": [
        "admin",
        "dev"
      ]
    },
    {
      "name": "bob",
      "roles": []
    }
  ]
}
round trip: 7 members, 2 users
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- The last line proves the JSON can be parsed: 7 members at the top, 2 users.
