# Paths and directories

A path is only text: `data/logs/app.log`. Splitting and joining paths is text
work, and the library does it without touching the disk. Listing a directory
is the opposite: it asks the system what is there, and the answer comes in an
order nobody promises.

## Paths are text

```dc example=path_parts
```

- `basename`, `dirname`, `extension` and `stem` return slices of the input.
  Nothing is copied or allocated, and nothing is checked on disk.
- `extension` includes the dot and is empty when there is none. A leading dot
  (`.bashrc`) is a name, not an extension.
- `join(base, tail, buffer)` and `normalize(path, buffer)` build new text, so
  you supply the buffer and get back the part you used. A buffer that is too
  small gives `NameTooLong`. An absolute `tail` replaces the base: that is the
  safe reading of a user-supplied path.
- `normalize` resolves `.` and `..` by text alone. It does not know about
  symbolic links.
- On Windows `join` and `normalize` use `\` as separator, and the other
  functions accept both `/` and `\`. The examples print paths built with `/`
  as on Linux.

## Listing a directory

`std::os::dir::open(path, &iterator)` starts a listing, `next()` returns the
entries one by one as `Optional(Entry)`, and `close()` ends it. Three
properties shape every listing program:

1. **The order is unspecified.** It depends on the file system, and the same
   directory can list differently on two machines. A program with
   reproducible output sorts.
2. **Dot entries are reported.** `.` and `..` come back as ordinary entries;
   skip them.
3. **A name borrows the iterator's buffer** and is only valid until the next
   `next()`. To keep it, copy it: that is why the example stores each name in
   a small `Name` struct with its own `char[64]`.

```dc example=list_dir
```

- The iterator is a plain struct, so you declare it (`Iterator it;`) and
  `open` fills it. `defer it.close();` goes right after the successful
  open, as with files.
- `sort_by` takes a comparison function returning a negative, zero or
  positive number. `std::mem::compare` gives that for byte strings.
- `entry.kind` is `File`, `Directory`, `Symlink` or another kind, so a listing
  can tell folders from files without a second call.

## Metadata and existence

```dc example=metadata_walk
```

`metadata(path)` returns the size and kind of a path without opening it, or
an error: a missing path is the error `NotFound`. Symbolic links are not
followed: you get information about the link itself. `exists`, `is_file` and
`is_directory` answer the common yes/no questions. Remember that the answer
can change a moment later, so for "create unless present" call the operation
and handle the error instead of asking first.

`make_directory_all("a/b/c")` creates the missing levels and succeeds when
they exist. `make_directory` creates one level and fails with `AlreadyExists`.
`remove_directory` only removes an empty directory (`DirectoryNotEmpty`
otherwise); to remove a tree you delete its files first.

## Common mistakes

- Printing a directory in the order it was returned and calling the output
  deterministic.
- Keeping `entry.name` after the next `next()`.
- Forgetting to skip `.` and `..` and recursing forever.
- Checking `exists` and then acting, as if nothing could change in between.
- Building paths with `+` or by hand with `/`: use `join` so the separator
  and the buffer size are right.
