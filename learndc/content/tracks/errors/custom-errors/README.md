# Your own error type

The standard `Error` covers library failures. Your program has failures of
its own, such as a missing name or a bad port number. Describe them with an
enum and use it as the error type of `Status`:

```dc
public enum ConfigError {
    @implicit_construction Print(PrintError),
    MissingName,
    BadPort,
}

public Status(ConfigError) validate_port(i32 port) {
    if port < 1 || port > 65535 {
        return ConfigError::BadPort;
    }
    return Status::Ok;
}
```

Returning a value of the error type from a `Status(ConfigError)` function
makes it the failure. A hosted `main` can return `Status(ConfigError)`
directly: success exits with code 0, any failure exits with code 1, and the
error is not printed for you.

A custom error does not convert to or from `Error` by itself. `println`
fails with `PrintError`, so a function that prints and returns
`Status(ConfigError)` needs a variant that wraps it. Marking exactly one
variant `@implicit_construction` lets `?` build that variant from a
`PrintError`. Only one such wrapping step is allowed.

When `main` names a type from another module in its signature, import that
module with `public import config;`, otherwise the compiler rejects the
public signature.

## Your task

Open `src/config.dc`. `ConfigError` only knows about print failures, and the
two validators accept everything.

1. Add the variants `MissingName` and `BadPort` to `ConfigError`.
2. `validate_name` must fail with `MissingName` when the name is empty.
3. `validate_port` must fail with `BadPort` unless the port is from 1 to
   65535 inclusive.

`src/main.dc` is finished. It validates the name `server`, port 8080 and
port 70000 and must now print

```text
name ok
port 8080 ok
```

and exit with code 1 when port 70000 is rejected.

## Acceptance criteria

- The project builds with `dpm build`.
- The three validators behave as described for the boundary ports 1, 65535,
  0 and 70000, and for an empty name.
- The program prints the two lines above and exits with code 1.

Run `learndc check` when you are ready, or `learndc run` to see the output.
