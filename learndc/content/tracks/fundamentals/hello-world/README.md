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

## Your task

Open `src/main.dc`. It still prints `Hello, world!`. Change it so that it:

1. stores the name `Ada` in a variable called `name` of type `[]const char`
   (borrowed text, which is what a string literal is), and
2. prints exactly these two lines:

```text
Hello, Ada!
Welcome to DC.
```

The first line must come from a placeholder and the `name` variable, not
from typing `Ada` into the format string.

## Acceptance criteria

- The project builds with `dpm build`.
- Running the program prints the two lines above and exits with code 0.

Run `learndc check` when you are ready, or `learndc run` to see the output.
