# Settings for one product

*Continues custom-profile: the harbor project gets a compute-heavy product.*

`crunch` is a number cruncher: a loop that sums squares. Built in the debug
profile it runs at `-O0`, which is slow; the harbormaster wants it to be
optimized every time, while `fleet` should still follow whatever profile is
selected, so it stays easy to debug.

`src/main.dc` is the course's audit again; leave it and `src/audit.dc` alone.

## Your task

Give the product `crunch` its own settings: `optimize = Optimize::O2`. Do not
change the project-level settings and do not add a profile. Then compare:

```text
dpm build -v
```

The dcc line of `crunch` has `-O2` and `-g`, the line of `fleet` has `-O0` and
`-g`.

## Acceptance criteria

- `dpm run audit` prints five `ok` lines and `5 checks, 0 failed`.
- `dpm run crunch` prints `sum of squares: 333833500`.
