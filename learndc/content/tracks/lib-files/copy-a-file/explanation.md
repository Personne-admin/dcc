# Why the solution works

- Each handle gets its `defer ... close()` on the line after the call that
  produced it. If `create` fails, the source is already registered for
  closing; if the loop fails halfway through a `?`, both are. No path
  needs to remember to close anything.
- The deferred closes run in reverse order: the destination first, then
  the source. For files the order does not matter, but it is the same rule
  as for any resource: last acquired, first released.
- The loop writes `chunk[0..n]`, not the whole chunk. A read can return
  fewer bytes than the buffer holds, and the last chunk almost always does.
  Writing all 16 bytes would append garbage to the copy.
- `read` returning 0 is the normal end of file, not an error. A missing file
  or an I/O failure comes back as an error value instead.
- Opening the source before creating the destination matters: `create`
  truncates, so a failed copy that created `to` first would destroy an
  existing file and leave an empty one behind. The last output line
  proves that the failed copy changed nothing.
- `count_lines` reads the copy, not the original, so it also shows that the
  copy is complete and readable.
