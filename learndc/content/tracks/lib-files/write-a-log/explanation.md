# Why the solution works

- `log_line` is a complete open-write-close cycle with `defer f.close();`
  right after the open. Because `append` is on, the write lands after the
  existing content; `truncate = false` keeps the file from being emptied by
  the open. Either flag wrong and the log keeps only the last line, which
  is exactly what the numbered output would show.
- `FileWriter` wraps the handle so the same `format` call used for
  `println` writes the line. One `format` call per line also keeps the line
  together in the file, which matters when several programs append at once.
- `read_text` is the lecture's `load` as a function: size, resize, read. The
  readers split on `'\n'`. A log that ends with a newline has one empty piece
  at the end, so `print_numbered` skips empty lines instead of printing a
  numbered blank.
- `count_level` compares a prefix. The prefix is built once per call in a
  stack buffer: no allocation, and the array outlives every use of the
  slice that `format_buf` returned.
- `main` deletes any old `app.log` first, so each run starts from the same
  state. The second session appends to the same file: seven lines in
  all, proving earlier lines survive.
