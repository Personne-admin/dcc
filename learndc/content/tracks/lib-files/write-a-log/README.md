# A log file you can append to

A log is the classic append-only file: every call adds one line at the end,
and the earlier lines must survive. A program may also be started many times,
so the log is reopened for each message instead of being kept open. That is
slower than keeping a handle, and always safe: a crash cannot lose a line
that was written and closed.

The pieces are all in the last two lectures:

```dc
OpenFlags flags = std::os::file::write_only();
flags.truncate = false;
flags.append = true;
File f = std::os::file::open(path, flags)?;
defer f.close();
FileWriter out = FileWriter::new(f);
std::fmt::format(&out, "[{}] {}\n", level, message)?;
```

## Your task

`src/main.dc` has a finished `read_text(path, out)` (read a whole file into an
`Array(char)`) and a finished `main`. Implement:

1. `log_line(path, level, message)` appends `[LEVEL] message` and a newline.
   The file is created when it does not exist yet. Earlier lines must stay.
2. `print_numbered(path)` prints every non-empty line of the file, with its
   number starting at 1: `1 [INFO] server started`.
3. `count_level(path, level)` returns how many lines start with `[LEVEL]`.
   Build the tag `[ERROR]` in a `char[16]` with `std::fmt::format_buf`, then
   use `std::slice::starts_with(line, tag)` on each line.

The program must print:

```text
1 [INFO] server started
2 [WARN] disk at 91 percent
3 [ERROR] write failed
4 [INFO] retrying
5 [ERROR] write failed again
errors: 2
after second session: 7 lines
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- Every file you open is closed on every path.
