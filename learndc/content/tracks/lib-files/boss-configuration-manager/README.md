# The Configuration Manager

A server reads its settings from layered TOML files: a shared
`data/defaults.toml` and an optional local `data/override.toml` on top. The
tool you build merges them over built-in defaults, validates the result with
plain messages and writes a normalized file. The project is split into
modules, each stage lives in one function, and each stage is checked on its
own.

```text
src/fileio.dc     read_file                               (stage 2)
src/settings.dc   Settings, apply_toml, messages, checks  (stages 3 and 4)
src/manager.dc    load_layers, save_settings              (stages 5 and 6)
src/main.dc       run_scenario and main                   (stage 7)
```

`settings.dc` already defines `Settings`, `LoadError`, `set_host`,
`host_text` and `default_settings`. Read it first.

```dc
public struct Settings {
    char[64] host;     // host_text(s) is the used part
    usize host_len;
    i64 port;
    i64 workers;
    bool debug;
}
```

The built-in defaults are host `127.0.0.1`, port 8080, 4 workers, debug off.

## Stage 1: Read the legacy saver

A question about an old saver that can destroy a config file. Answer it
before you write your own.

## Stage 2: Read a file whole

`fileio::read_file(path, out)` fills `out` (an `Array(char)`) with the
complete content of the file. It *replaces* what the array held before: after
two reads of the same 11-byte file the array has 11 bytes. A missing file is
the file error `NotFound` returned through `Status(Error)`. The handle must be
closed on every path; the check calls the function 3000 times to find leaks.

## Stage 3: Apply a TOML layer

`settings::apply_toml(s, text)` parses `text` and overwrites only the keys
it contains: `host` (string), `port` and `workers` (integers), `debug`
(boolean). Missing keys leave the field as it was. It returns `Status::Ok` or:

| Problem | LoadError |
|---|---|
| the text is not valid TOML | `Syntax` |
| `host` is not a string | `HostNotText` |
| `host` is rejected by `set_host` | `HostInvalid` |
| `port` is not an integer | `PortNotNumber` |
| `workers` is not an integer | `WorkersNotNumber` |
| `debug` is not a boolean | `DebugNotBool` |

Range checks are not made here: `port = 70000` is stored.

## Stage 4: Say it clearly

`print_load_error(error)` prints exactly one line per variant, with these
sentences:

| LoadError | Line |
|---|---|
| Syntax | `config is not valid TOML` |
| HostNotText | `host must be a string` |
| HostInvalid | `host must be 1 to 63 characters without quotes or backslashes` |
| PortNotNumber | `port must be a whole number` |
| WorkersNotNumber | `workers must be a whole number` |
| DebugNotBool | `debug must be true or false` |
| Unreadable | `config file could not be read` |

`check_settings(s)` prints one line per invalid value and returns how many it
found as `Result(usize, Error)`:

- an empty host: `problem: host is empty`
- a port outside 1..65535: `problem: port 70000 is outside 1..65535`
- workers outside 1..64: `problem: workers 0 is outside 1..64`

## Stage 5: Merge the layers

`manager::load_layers(s, defaults_path, override_path)` sets `*s` to
`default_settings()` and then applies the defaults file and the override
file, in that order. The defaults file is required: when it cannot be read
the result is `LoadError::Unreadable`. The override file is optional: when
it does not exist it is skipped without an error. Errors from `apply_toml`
are returned unchanged, so a wrong type in the override fails the load.

## Stage 6: Write the normalized file

`manager::save_settings(s, path)` writes exactly these four lines, in this
order, creating or replacing the file:

```text
host = "example.org"
port = 8443
workers = 3
debug = true
```

It returns an error when the file cannot be created (for example a folder
that does not exist) and closes its handle on every path.

## Stage 7: Run the scenarios

`main` runs five scenarios through `run_scenario(name, defaults, override,
out)`. Implement it: print `== name`, load the layers, print `error: ` and
the message when the load fails, otherwise validate; with problems print
`not saved: N problem(s)`; without problems save, print the saved file's
text exactly as it was written, and delete the file again. A scenario that
fails to load or validate is not an error of the program: `run_scenario`
returns `Status::Ok`.

Expected output of the whole program:

```text
== merged
host = "0.0.0.0"
port = 9090
workers = 2
debug = true
== defaults only
host = "0.0.0.0"
port = 8080
workers = 2
debug = false
== out of range
problem: port 70000 is outside 1..65535
problem: workers 0 is outside 1..64
not saved: 2 problem(s)
== wrong type
error: port must be a whole number
== no defaults
error: config file could not be read
```

## Bonus stage: replace the file atomically

`manager::save_settings_atomic(s, path)` writes `path` + `.tmp` with
`save_settings`, then renames it over `path`. A reader never sees a half
written file. When anything fails, the temporary file is deleted and the
error is returned; a failed save leaves the old file alone.

## Acceptance criteria

- The project builds with `dpm build` and every required stage passes.
- No handle stays open on any path, including the error paths.
- Nothing in the program stops on bad input: it explains and carries on.
