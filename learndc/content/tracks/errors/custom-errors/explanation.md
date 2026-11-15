# Why the solution works

```dc
public enum ConfigError {
    @implicit_construction Print(PrintError),
    MissingName,
    BadPort,
}
```

- Each failure the program can have is one variant, so callers can tell them
  apart with `match` or `==`, and the compiler checks that a `match` covers
  all of them.
- `Print(PrintError)` is the one variant marked `@implicit_construction`.
  When `println("...")?` fails in `main`, `?` wraps the `PrintError` as
  `ConfigError::Print(...)` and returns it. Without this variant the `?` on
  `println` would not compile, because `PrintError` does not convert to a
  user enum on its own.

```dc
public Status(ConfigError) validate_port(i32 port) {
    if port < 1 || port > 65535 {
        return ConfigError::BadPort;
    }
    return Status::Ok;
}
```

- The condition lists the invalid ports, so 0 and 70000 fail while 1 and
  65535 pass. A `ConfigError` returned from a `Status(ConfigError)` function
  becomes `Status::Err(...)` automatically.
- In `main`, `config::validate_port(70000)?` finds the error and returns it
  from `main` immediately. The line `port 70000 ok` is never printed, and the
  hosted startup turns the error into exit code 1. Nothing prints the error
  itself; a program that wants a message must print one before returning.
- `main` exposes `config::ConfigError` in its public signature, so
  `src/main.dc` uses `public import config;`.
