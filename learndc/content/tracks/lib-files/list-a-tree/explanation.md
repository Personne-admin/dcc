# Why the solution works

- A directory listing is a recursive structure, so the function is
  recursive: it handles one folder and calls itself for each subfolder. The
  `depth` parameter only controls indentation; the `Totals` pointer is shared by
  every level, so one struct collects the whole walk.
- Sorting happens inside `collect_sorted` for every folder separately.
  The directory order of the file system never reaches the output, which is
  why the result is the same on every machine.
- Each level owns its own `Array(Name)` and releases it with `defer`. The
  names are copies, so recursing (which opens other iterators) cannot
  invalidate them. If the code had kept the borrowed `entry.name` slices,
  the next `next()` would have overwritten them.
- `path::join` makes the child path in a stack buffer. It uses the host
  separator, handles a trailing slash on `folder` and reports `NameTooLong`
  instead of overflowing.
- The directory is printed before its content, and its size is not counted:
  only files have bytes in this listing.
- The missing folder fails at `open` inside `collect_sorted`. The `?` carries
  the error through `list_tree` to `main`, which decides to print a line: the
  library reports, the program decides.
