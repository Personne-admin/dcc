# Why the solution works

- `first_error` never builds a tree. It reads events and throws them away,
  so it allocates nothing and there is nothing to release. That is the
  point of the pull parser: validating or locating needs no document.
- The loop ends in exactly two ways: an error (return the position) or
  `Event::End` (return `None`). The function therefore has no statement
  after the `while true`; the compiler knows the loop never falls through.
- A `Position` carries enough to cut the line out of the text. `column`
  counts the bytes since the start of the line, so `offset - (column - 1)` is
  the line start. No second pass over the text from the beginning is needed.
- The caret goes under column `column`: `column - 1` spaces first, because
  columns start at 1. For an unexpected end (text 3) the position is one past
  the last byte, which is exactly where the caret should point: where the
  missing characters should have been.
- Text 4 shows that two values in a row are an error at the start of the
  second: the first document is complete, anything after it is not allowed.
- Columns count bytes. For text with multi-byte characters the caret would
  drift; a real tool would convert bytes to display columns. That is a
  limitation of the library worth knowing.
