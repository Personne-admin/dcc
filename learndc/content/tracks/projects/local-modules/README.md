# Split a program into modules

A DC program can span several files. Each file is one module; its first line
names it, and the file name must match: `src/temperature.dc` holds
`module temperature;`.

Declarations are private by default. Another module can only use a function,
struct or constant that is marked `public`, and only after importing the
module:

```dc
module temperature;

public i32 to_fahrenheit(i32 celsius) {
    return celsius * 9 / 5 + 32;
}
```

```dc
module main;

import temperature;

public Status(Error) main() {
    println("{}", temperature::to_fahrenheit(100))?;
    return Status::Ok;
}
```

The caller reaches the function through the module name, `temperature::`.

## Your task

The project in `src/` already has a `temperature` module and a `main` that
imports it, but it does not build.

1. Run `learndc check` and read the diagnostic. Find out what the compiler
   cannot see and why.
2. Fix it so the three conversion lines print.
3. Add a second function to `src/temperature.dc`,
   `bool is_freezing(i32 celsius)`, that is true when the temperature is below
   zero, and make `main` print one more line using it:

```text
0 C = 32 F
37 C = 98 F
100 C = 212 F
-5 C freezing: true
```

## Acceptance criteria

- The project builds with `dpm build`.
- The program prints the four lines above and exits with code 0.
- `main` reaches both functions through `temperature::`.
