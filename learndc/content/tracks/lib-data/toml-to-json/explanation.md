# Why the solution works

- The program is a *tree walk*. A TOML tree and a JSON tree have almost the
  same shape (scalars, lists, key-value tables), so each node of one maps to
  a few emitter calls for the other. `emit_table` and `emit_value` call each
  other, one level per nesting level.
- Nothing is allocated for the output tree. The emitter streams tokens into
  the buffer as the walk goes, so the memory used does not depend on the
  document, only the buffer size does.
- The commas, colons and indentation come from the emitter. The walk only
  decides the order of tokens: begin, key, value, end. Because the
  emitter does not check balance, the pairing in each function is what makes
  the output valid, and the round trip at the end is the test of it.
- The final `_` arm of the match is a decision: date-times and local times
  become `null`. The library gives no standard text form for them. Saying so
  in the code, rather than dropping the key, keeps the member in the output.
- `main` owns both documents. The TOML document is read while the emitter
  runs, and the JSON document holds a copy of the text it was parsed from,
  so releasing each with `defer` is independent.
- Order is preserved: tables keep the order written in the TOML file, and
  the emitter writes members in the order you give them.
