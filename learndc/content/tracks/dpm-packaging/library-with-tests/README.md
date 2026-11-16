# Tests inside a library

*This lesson starts the stats library that small-api and release-a-version continue.*

`libs/stats` is a library package with one module, `stats::summary`, that has
`mean` and `max` over a slice of `i32`. It has no tests yet, and the harbor
workspace uses it through the path dependency in the root `project.dcon`.

`src/main.dc` is the course's audit: it calls the library and reads your test
file. Leave `src/main.dc` and `src/audit.dc` alone and use `dpm run audit` to
see what is missing.

## Your task

1. Create `libs/stats/tests/summary_test.dc`. It is a test program: `module
   summary_test;`, `import stats::summary;`, a `public i32 main()` that prints
   one line per check and returns the number of failures. Write at least
   four checks, for `mean` and for `max`, including a slice of only negative
   numbers.
2. Run it from the package directory: `cd libs/stats && dpm test`.
3. A check will fail. Fix the library, not the test.

## Acceptance criteria

- `cd libs/stats && dpm test` ends with `1 passed, 0 failed`.
- `dpm run audit` in the workspace root prints nine `ok` lines and
  `9 checks, 0 failed`.
