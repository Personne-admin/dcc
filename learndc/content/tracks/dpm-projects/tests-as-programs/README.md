# Test programs and dpm test

*Continues the harbor project: the harbor::ships module from module-layout gets a test program.*

`tests/ships_test.dc` is a test program for the harbor project: a `main` that
prints one line per check and returns the number of failures. `dpm test`
builds every file in `tests/` and runs it; exit code 0 is a pass.

Run it in your workspace:

```text
dpm test
```

It fails, and the output tells you why. There are two separate problems:

1. `total_tons` returns the wrong number.
2. The test calls `harbor::ships::lightest_tons()`, which does not exist yet,
   so the test does not even compile.

## Your task

Make `dpm test` pass: fix `total_tons` in `src/harbor/ships.dc` and add
`public i32 lightest_tons()`, which returns the smallest tonnage in the fleet.
Leave `tests/ships_test.dc` as it is.

`src/main.dc` prints the total; once it is right the program prints:

```text
total 445 t
```

## Acceptance criteria

- `dpm test` ends with `1 passed, 0 failed`.
- `dpm run` prints the line above and exits with 0.
