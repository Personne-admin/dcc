# List a directory tree

`data/tree` ships with the exercise: seven small files in three nested
folders. Your program prints it like the `tree` command: sorted, indented,
with sizes, and with totals at the end.

The lecture's listing code is already in `src/main.dc` as
`collect_sorted(folder, names)`: it fills an `Array(Name)` with the sorted
entries of one folder (names copied, `.` and `..` skipped). What is left is
the recursion.

## Your task

Implement `list_tree(folder, depth, totals)`:

1. Collect the sorted entries of `folder`. The `Array` is yours: release it
   with `defer`.
2. For each entry print two spaces per level of `depth`, then:
   - a **folder**: `name/` on its own line, add one to `totals.folders` and
     call `list_tree` on the child path with `depth + 1`.
   - **anything else**: `name (N bytes)`, where N is the size from
     `std::os::file::metadata(child)`; add one to `totals.files` and the size to
     `totals.bytes`.
3. The child path is `std::os::path::join(folder, name, buffer)` with a
   `char[256]` buffer. Never build it with string pieces by hand.

If the folder cannot be opened, the error goes up with `?`: `main` calls
`list_tree` on a missing folder and prints that it failed.

The program must print:

```text
tree/
  alpha.txt (6 bytes)
  docs/
    guide.txt (21 bytes)
  readme.txt (22 bytes)
  src/
    deep/
      notes.txt (31 bytes)
    main.txt (8 bytes)
    util.txt (8 bytes)
  zeta.txt (15 bytes)
files: 7
folders: 3
bytes: 111
missing folder listed: false
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- The iterator is closed on every path (it is inside `collect_sorted`), and
  every `Array` is released.
