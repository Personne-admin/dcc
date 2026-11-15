# Your first DC program

Every hosted DC program starts in `main`. This one returns `Status(Error)`:
`Status::Ok` means success (exit code 0) and an error means failure (exit
code 1).

```dc
public Status(Error) main() {
    println("Hello, world!")?;
    return Status::Ok;
}
```

`println` writes one line to standard output. Printing can fail, so it
returns a status too; the `?` after the call continues when it succeeded and
returns the error from `main` when it did not.

A `{}` inside the text is a placeholder. Each placeholder is replaced by the
next argument after the format string:

```dc
println("{} + {} = {}", 2, 3, 5)?;
```

prints `2 + 3 = 5`. Text works as an argument too: write it in double quotes.

## Your task

Open `src/main.dc`. It still prints `Hello, world!`. Change it so that it
prints exactly these two lines:

```text
Hello, Ada!
Welcome to DC.
```

Produce the first line with a placeholder: the format string is
`"Hello, {}!"` and the name `"Ada"` is the argument after it.

## Acceptance criteria

- The project builds.
- Running the program prints the two lines above and exits with code 0.
