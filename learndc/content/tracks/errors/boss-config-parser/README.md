# The Config Parser

The server reads its settings from text like this:

```text
name = tiny-server
port = 8080
verbose = true
```

`src/config.dc` declares the types and one placeholder function per step.
`src/main.dc` holds four sample files and the report code. Each stage below
is checked on its own, so you can clear them in order.

```dc
public struct Settings {
    []const char name;
    u32 port;
    bool verbose;
}

public struct Pair {
    []const char key;
    []const char value;
}
```

`default_settings()` gives name `unnamed`, port 80 and verbose false, and
`trim(text)` (finished) cuts spaces from both ends of a slice. Text is
compared with `std::slice::eq(a, b)`, not with `==`.

## Stage 1: Read the old loader

A question about the retired loader. Find the line that hid bad input.

## Stage 2: Split a line

`split_line(line)` returns `Optional(Pair)`: `Some(pair)` with the trimmed
key and value around the first `=`, or `None` when the line has no `=`.
`"  port = 8080  "` gives key `port` and value `8080`; `"a=b=c"` gives `a`
and `b=c`; `"port ="` gives an empty value.

## Stage 3: Parse a port number

`parse_port(text)` returns `Result(u32, NumberError)`:

- `Ok(n)` for decimal text from 0 to 65535,
- `Err(NumberError::Empty)` for empty text,
- `Err(NumberError::NotADigit)` when any character is not `0` to `9`
  (so `-5` and ` 5` fail too),
- `Err(NumberError::TooLarge)` above 65535. A very long number must give
  `TooLarge`, not a wrapped-around value.

## Stage 4: Parse true or false

`parse_flag(text)` returns `Optional(bool)`: `Some(true)` for `true`,
`Some(false)` for `false`, `None` for anything else. Compare this stage with
the last: nothing here needs a reason, so an `Optional` is enough.

## Stage 5: Apply one line

`apply_line(settings, line)` returns `Status(Problem)`. It splits the line
and updates one field of the `Settings` that `settings` points to:

| key | effect | on failure |
|---|---|---|
| `name` | store the value | |
| `port` | store `parse_port` of the value | `Problem::BadPort(why)` |
| `verbose` | store `parse_flag` of the value | `Problem::BadFlag` |
| anything else | | `Problem::UnknownKey` |

A line without `=` fails with `Problem::NoEquals`. A failing line must not
change the setting it was meant for.

## Stage 6: Load the file and report

`load(text)` returns `Result(Settings, LoadError)`. It starts from
`default_settings()`, applies every line in order and stops at the first
problem with `LoadError { line = n, problem = p }`, where lines are numbered
from 1. The last line may or may not end with a newline.

Then finish `describe` and `report` in `src/main.dc`. `report` prints, for a
loaded file,

```text
loaded tiny-server
  port: 8080
  verbose: true
```

and for a failure `line N: ` followed by the message from `describe`:

| Problem | message |
|---|---|
| `NoEquals` | `no = in the line` |
| `UnknownKey` | `unknown key` |
| `BadFlag` | `verbose must be true or false` |
| `BadPort(Empty)` | `port is empty` |
| `BadPort(NotADigit)` | `port has a character that is not a digit` |
| `BadPort(TooLarge)` | `port is too large` |

`main` reports four files and must print exactly

```text
loaded tiny-server
  port: 8080
  verbose: true
line 2: port is too large
line 2: verbose must be true or false
line 2: unknown key
```

## Bonus: comments and blank lines

After trimming, a line that is empty or starts with `#` is skipped by
`load`. Skipped lines still count when numbering lines.
