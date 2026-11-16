# Why the solution works

- Each function writes one JSON value and leaves the emitter ready for the
  next one. `write_item` opens and closes its own object, so `write_report`
  can call it once per item inside an array without knowing what an item
  looks like. Keeping `begin` and `end` in the same function is the easiest
  way to keep the output balanced: the emitter does not check it for you.
- A key is always followed by one value call. The `note` member shows a
  choice between two value calls, `null_value` and `string`, after the same
  `key("note")`.
- `string` escapes the quotes of `Ink "blue"`, which is why the expected
  output has a backslash before them. Building the text by hand with
  `println` would have produced invalid JSON.
- The emitter is passed by pointer (`Out* json`). Both functions write into
  the same emitter, so the commas and the indentation stay consistent across
  the call boundary.
- `total_qty` is computed before the object is written. The emitter streams:
  once a byte is written it cannot be taken back, so everything that goes
  into a value must be known by the time you write it.
- The newline is written by `main`, which owns the writer. The functions
  only know about JSON.
