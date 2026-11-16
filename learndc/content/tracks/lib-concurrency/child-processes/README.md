# Starting child programs

Sometimes the best way to do a job is to let another program do it: compress
a file, ask `git` for a version, run a test. `std::process::Command` starts
programs and tells you how they ended. This chapter's lessons run on Linux:
the examples use `true`, `false`, `sh`, `echo` and `sleep`, which every Linux
system has. (The library also supports Windows, but the programs and signals
used here are Linux ones.)

## Build, run, read the status

```dc example=exit_codes
```

- `Command::new(program)?` copies the program name. A name without a slash is
  looked up in `PATH`, like a shell does.
- `command.arg(text)?` adds **one** argument. Arguments are never split and no
  shell is involved: spaces, quotes and `;` mean nothing special. Each call
  allocates a copy, so it can fail, hence `?`.
- `command.status()?` starts the program and waits until it ends. The
  result is an `ExitStatus` with a `code`: 0 is success, anything else is the
  program's own way of saying "something went wrong". `success()` is the
  test for 0.
- `defer command.deinit();` on the next line gives the copied strings back.
  Do not copy a `Command` value: one owner, one `deinit`.

There are two layers of failure, and the example shows both. A program that
runs and returns 1 (`false`) or 3 (`sh -c "exit 3"`) is an `Ok(status)`. A
program that cannot start (a name that does not exist, a permission
problem) is an `Err`, and `status()` never produced a status at all. Treat
them differently: the first is the child's answer, the second is yours to
handle.

## Spawn, wait, kill

`status()` blocks. `spawn()?` starts the program and returns at once with a
`Child`, so your program can do other work meanwhile:

```dc example=kill_and_wait
```

- `try_wait()?` asks without blocking: `None` means still running. Once it
  reports `Some(status)` the child has been reaped.
- `kill(signal)` only *requests* termination (9 is the signal that cannot be
  ignored). The child still has to be reaped.
- `wait()?` blocks until the child has ended, reaps it and releases its
  resources. **Call it exactly once for every spawned child**, also after a
  kill. A second call fails: the child is gone.
- A child killed by a signal has `signal` 9 and `code` 128 + 9 = 137. A
  normal exit has signal 0.

A child that is never waited for stays in the process table as a zombie until
your program ends. `defer` is the way to make sure the wait happens on every
path, but `wait` returns a value, so inside a `defer` write it as an ordinary
statement without `?` (never use `?` inside a deferred statement).

## What the builder can also do

`cwd(path)` sets the child's working directory, `env(key, value)` and
`env_remove(key)` change its environment (Linux), and `stdin`, `stdout` and
`stderr` redirect its standard streams. Redirection is the next lesson.

## Common mistakes

- Treating `Ok` as "the program succeeded": always look at the code.
- Building a shell command line in one string and passing it as the program
  name. Use arguments, or `sh` with `-c` if you must have a shell.
- Forgetting `wait` after `spawn` (a zombie) or waiting twice.
- Never calling `deinit` on the `Command`.
