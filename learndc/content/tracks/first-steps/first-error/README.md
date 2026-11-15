# Reading your first compiler errors

When the compiler cannot understand your program, it prints an error and
builds nothing. That is not a failure of yours: it is the proofreader doing
its job. Every error message has the same parts:

```text
error: unknown name `printn`
  --> src/main.dc:4:5
   |
 3 | public Status(Error) main() {
 4 |     printn("Hello")?;
   |     ^^^^^^
```

- The first line says what is wrong, here that the compiler does not know the
  name `printn`.
- The `-->` line says where: file `src/main.dc`, line 4, column 5. The path
  may be longer on your computer.
- Below it, the compiler shows that line with `^` marks under the spot.

Two habits make errors easy:

- Fix the *first* error first. The compiler may report several, and later
  ones are sometimes only a result of the first.
- Build again after every fix, so you always know which change did what.

A missing semicolon is reported at the *start of the next statement*, because
that is where the compiler notices that the previous one never ended. If the
`^` points at a line that looks fine, look at the line above it.

## Your task

`src/main.dc` has two mistakes and does not build. Fix them so that it
prints:

```text
Reading the first error
Every error has a line number
```

1. Build and read the first error. Fix the line it points to.
2. Build again. Read the next error and fix it too.

## Acceptance criteria

- The project builds.
- The program prints the two lines above and exits with code 0.
