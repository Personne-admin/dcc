# Server settings from TOML

A small server reads `settings.toml`. People edit that file, so the program
must produce sensible settings even when a value is missing, has the wrong
kind or is out of range. The text is built into `main` so the result is the
same everywhere.

`get_path("server.port")` reaches into the `[server]` table and returns an
`Optional(Value)`. `as_array()` and `as_table()` open the arrays of tables
made by `[[route]]` headers.

## Your task

`src/main.dc` parses the text and prints the effective settings. `main` and
`text_at` are finished. Implement:

1. `int_at(table, path, fallback)` reads an integer at `path` (like
   `text_at`, but with `as_integer`) and returns `fallback` when the key is
   missing or not an integer.
2. `port_at(table, path, fallback)` reads the integer with `int_at` and
   returns `fallback` unless it is in 1..=65535.
3. `print_routes(table, printed)` takes the `route` array of the root table.
   For every route table that has a `path`, print
   `route <path> -> <target>` (`(no target)` when there is none) and add one
   to `*printed`. Routes without a `path` are skipped silently. A document
   without any `route` prints nothing.

The program must print:

```text
name: edge
host: 0.0.0.0
port: 8080
workers: 4
max_connections: 100
route /health -> ok
route /api -> (no target)
routes: 2
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- No setting can make the program stop: only a syntax error in the TOML
  itself is an error.
