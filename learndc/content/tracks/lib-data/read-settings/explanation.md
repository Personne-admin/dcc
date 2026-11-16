# Why the solution works

```dc
[]const char text_or(const Value* root, []const char key, []const char fallback) {
    Value found = root.get(key).some_or(Value::Null);
    return found.as_string().some_or(fallback);
}
```

- `get` fails softly: a missing key is an empty Optional, not an error. A
  JSON document is data from outside, so absence is normal and the helper
  decides what it means.
- Replacing the empty Optional with `Value::Null` gives one value to ask the
  next question of. `as_string` on `Null`, on a number or on an array is
  empty for the same reason: the value is not a string. That is why
  `max_players: "many"` and `motd: null` fall back without any special
  case.
- The three helpers differ only in the conversion. If the settings grew to
  twenty keys, the program would still be twenty one-line calls.
- The helpers return slices that point into the document. `main` created
  the document and has `defer doc.deinit();`, so the strings stay valid until
  `main` returns. A helper that parsed its own document and returned a string
  from it would return freed memory.
