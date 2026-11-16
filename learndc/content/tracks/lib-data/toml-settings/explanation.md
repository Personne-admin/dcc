# Why the solution works

- The three readers all have the same shape: ask for the value, give up with
  the fallback if it is absent or of another kind. `port_at` adds one rule on
  top of `int_at` instead of repeating the lookup, so the range check is
  written once.
- A wrong kind (`max_connections = "many"`) and an absent key behave the
  same: the default. That is a policy, not a law. A stricter program would
  report the file and key. The helpers are the single place to change it.
- `some_or({})` turns an empty Optional into an empty slice or an empty
  table, so a document with no `[[route]]` simply loops zero times. There
  is no special case.
- `route.as_table()` gives each element of the array as a `Table`, so the same
  `text_at` helper works on a route as on the root. Small helpers that take a
  `const Table*` work at every level.
- The route without a `path` is skipped with `continue`, not reported as an
  error. Whether to skip, warn or fail is a decision for the program; the
  library only tells you the key is absent.
- All strings printed point into `doc`, and `main` releases it with
  `defer doc.deinit();` only after the last print.
