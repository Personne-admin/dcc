# Files and handles

A file is not in your program: it belongs to the operating system. When you
open one, the system gives you a *handle*, a small number that stands for
the open file. Everything else (reading, writing, closing) is asking the
system to do something with that handle. This lecture covers what the handle
costs you, and how to give it back on every path.

## File, open and create

`std::os::file::File` is a one-field struct that holds the handle. It does
not close itself. Three functions give you one:

- `create(name)` opens for writing, creates the file if needed and
  *truncates it to zero length* when it exists.
- `open(name, flags)` opens with the flags you choose. `read_only()`,
  `write_only()` and `read_write()` build the flags.
- `stdin()`, `stdout()` and `stderr()` return the standard streams. Never
  close those.

Both can fail, so they return a `Result(File, Error)`. The error is the
portable `std::os::error::Error`: `NotFound`, `AccessDenied`,
`AlreadyExists`, `IsDirectory`, `NoSpace` and so on.

## Write, read, close

```dc example=write_read
```

- `write_all` writes every byte or fails. `write` can write fewer bytes than
  you gave it and tells you how many; use it only if you want to handle that
  yourself.
- Bytes are `u8`. A text is `[]const char`, so you convert with
  `text as []const u8`. The file system does not know about text at all.
- `size()` tells you how long the file is. `read_all(buffer)` reads until
  the buffer is full or the file ends and tells you how many bytes it got.
  A buffer that is too small silently truncates, so size first, then read:
  that is why `load` resizes an `Array(char)` to the file size.
- `defer f.close();` goes on the line after the successful open. Whatever
  happens next, a `?` that returns early or the normal end, the handle is
  given back. The two helpers `save` and `load` each own exactly one handle.
- `delete_file` removes a file, `exists` asks if it is there.

Handles are limited: a process may have only a few hundred or thousand open
files. A function that forgets to close works in every test and fails in
production after enough calls.

## When it fails

```dc example=open_failures
```

An open that fails returns no file, so there is nothing to close. The
`Error` enum has no text of its own, so the program matches the variants it
cares about and has one general arm for the rest.

Two things worth knowing:

- Closing ignores errors. For files you wrote, `sync()` before the close
  forces the data to disk if that matters to you.
- Copying a `File` copies the handle, not the file. Two variables, one open
  file: close it once.

## Appending

`create` throws the old content away. To add to a file, open it with
`append` set:

```dc example=append_mode
```

The flags are a plain struct: start from `write_only()` (which creates and
truncates), switch `truncate` off and `append` on. Every write then goes to
the end, whatever else has happened to the file.

`metadata(name)` returns the size and the kind (file, directory) of a path
without opening it.

## Common mistakes

- Opening a file with `create` when you meant to add to it: the content is
  gone.
- A `return` or a `?` between the open and the `close` with no `defer`.
- Reading with a buffer smaller than the file and not noticing.
- Closing a handle twice through two copies of the `File` value.
- Matching on text of error names: the enum has variants, not messages.
