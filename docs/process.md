# Process commands

Enable libdcext for a hosted target (`-flibdcext linux` or `windows`). Import
`std::process` explicitly; Command is not part of the automatic prelude.

```dc
module main;
import std::process;
using std::process::{Command, ExitStatus};

Status(Error) main() {
    Command command = Command::new("git")?;
    defer command.deinit();

    command.arg("--version")?;
    ExitStatus status = command.status()?;
    if !status.success() {
        println("git exited with code {}", status.code)?;
    }
    return Status::Ok;
}
```

`Command::new` copies the program name. Construction and mutations are fallible
because they allocate. `?` converts the underlying `std::os::error::Error` into
`std::error::Error::Io`; allocation failures become `Io(OutOfMemory)`.
An unsuccessful child exit is an `Ok(ExitStatus)`, not a launch error. The
example reports a nonzero child exit but deliberately returns success itself.
Choose your application's error or exit policy explicitly.

## API

The default `Command` is `Command(DefaultAllocator)`. Its public operations are:

| Operation | Result | Meaning |
| --- | --- | --- |
| `Command::new(program)` | `Result(Command, os::error::Error)` | Copy a nonempty program name |
| `std::process::with_allocator(allocator, program)` | `Result(Command(A), os::error::Error)` | Use a static allocator policy A |
| `command.arg(value)` | `Status(os::error::Error)` | Copy one argument |
| `command.args(values)` | `Status(os::error::Error)` | Copy a slice of `std::os::process::Arg` (string slices) |
| `command.cwd(path)` | `Status(os::error::Error)` | Copy a nonempty child directory |
| `command.env(key, value)` | `Status(os::error::Error)` | Set or replace a child environment edit (Linux) |
| `command.env_remove(key)` | `Status(os::error::Error)` | Remove a child environment key (Linux) |
| `command.stdin(handle)` | `void` | Set borrowed standard input |
| `command.stdout(handle)` | `void` | Set borrowed standard output |
| `command.stderr(handle)` | `void` | Set borrowed standard error |
| `command.spawn()` | `Result(Child, os::error::Error)` | Launch and transfer responsibility for the child |
| `command.status()` | `Result(ExitStatus, os::error::Error)` | Launch and wait |
| `command.deinit()` | `void` | Release owned configuration |

String parameters are `[] const char`. Invalid embedded NUL bytes are rejected;
environment keys must also be nonempty and contain no `=`. Empty argument and
environment values are valid. Failed mutations preserve the previous logical
configuration, including `args` when an intermediate allocation fails. Array
capacity can change during a failed bulk append.

## Ownership and reuse

Command owns independent copies of all configured strings, so the caller can
modify or release source buffers after a setter returns. Call `deinit` exactly
when done (repeated deinit is harmless). Do not copy an owning Command: DC does
not automatically transfer ownership or destroy values. A deinitialized command
must not be used except for another deinit.

Custom allocator policies are copied into the command and its internal arrays,
following the usual Array contract. Stateful policies must refer to shared state
that outlives the command. Temporary launch storage uses the same allocator and
is freed on both success and failure. No allocator storage is retained by Child.

Launching does not consume or clear configuration. Repeated calls use the same
arguments, environment edits, directory and streams. Inherited environment and
relative directory/program names are evaluated for each launch. Do not mutate,
launch or deinitialize the same Command concurrently. Concurrent callers must
also obey the allocator policy's own synchronization requirements.

Child owns a PID on Linux or process/thread handles on Windows. Do not copy it.
After `spawn`, the command can be mutated or deinitialized independently. Call
`child.wait()` once to reap and release resources; `try_wait()` returns
`Optional(ExitStatus)` and consumes the child only when it reports completion.
Polling a running child leaves it owned by the caller. Repeated waits after
completion report `BadHandle`. `child.kill(signal)` requests termination but
does not reap or release: always wait afterwards. Windows uses TerminateProcess
with that numeric exit code rather than POSIX signals.

A failed explicit wait retains the child for the caller to handle. `status`
retries interrupted Linux waits through the existing wait primitive; on Windows
it closes its local handles even if waiting fails. Closing Windows handles does
not terminate a still-running process. No global registry or implicit cleanup
is installed. Applications must not reap the library's children themselves or
install a SIGCHLD policy that discards their wait statuses.

## Child configuration

Standard streams initially inherit the parent. Use `std::process::INHERIT` to
restore that setting, `std::process::NULL_STREAM` for `/dev/null` or `NUL`, or a
`std::os::file::Handle` for redirection. File and pipe handles are borrowed: keep
them valid until spawn returns and close your copies explicitly. On Windows,
caller-supplied handles must be inheritable. The builder opens and closes its
own null-device handles. Linux snapshots redirect sources before installing
streams, so shared or swapped source handles are supported.

No output capture helper is provided. If supplying pipes yourself, drain both
output streams appropriately and close unused pipe ends; waiting before draining
large output can deadlock.

`cwd` affects only the child and does not change the parent directory. Linux
inherits the complete environment and applies the saved edits at launch,
without changing the parent. Keys are case-sensitive. Repeated `env` replaces
the prior edit; `env_remove` hides an inherited or configured key, and a later
`env` sets it again. Inherited entries are not limited by the separate bounded
`std::os::process::Vars` iterator.

Linux searches the effective child PATH when the program contains no slash.
An absent PATH uses `/bin:/usr/bin`; an empty PATH or empty path component means
the child's working directory. Relative PATH entries are interpreted after
changing the child's directory. Programs containing a slash are executed directly.
Arguments remain separate strings, including empty strings, spaces and shell
metacharacters. There is no implicit shell and no shell fallback for executable
text files without an interpreter header. Invoke a shell explicitly only when
you intend shell semantics.

Linux launch errors include directory, redirection and exec failures and are
reported before a Child is returned; failed children are reaped. `ExitStatus`
has `code` and `signal`: a normal exit has signal 0; a POSIX signal exit has
signal N and code `128 + N`. `success()` tests code 0. Thus a normal exit 143 can
be distinguished from SIGTERM. Windows reports its process exit code and
signal 0; program lookup and quoting use the existing CreateProcessW machinery.

## Compatibility and limits

The existing borrowed `std::os::process::Command`, `new_command`, `spawn`,
`Child` and waiting functions remain accessible. The owning module aliases the
same Child and ExitStatus types and delegates to the same spawn engine. Legacy
Linux spawn retains direct-path execution and its historical exit-127 behavior
for exec failure. The extended borrowed `SpawnOptions`/`spawn_with_options`
interface exposes child cwd/environment, checked launch and PATH lookup when
lower-level callers need them.

Windows supports the builder, cwd and stream options, but `env` and `env_remove`
return `NotSupported`; inherited Unicode environment editing requires a future
implementation. Windows lookup follows CreateProcessW rather than Linux PATH
rules. The existing Windows argument-taking-main limitation is unchanged.
These APIs are hosted-only; they do not provide process execution on
freestanding targets. There is no automatic error printing, timeout management,
process group support or pipe capture in this milestone.
