# Fix a broken project

Real projects fail for boring reasons: a path that is slightly wrong, a name
that does not match, a keyword that was forgotten. Each failure comes from a
different layer, so each diagnostic looks different:

- `dpm` reads `project.dcon` first and reports problems as
  `project.dcon:LINE:COLUMN: message`.
- `dcc` then compiles the sources and reports `error: message`, followed by
  `--> file:line:column` and the offending source lines.

Compilers often stop at the first layer that fails, so fixing one problem can
reveal the next. Work through them one at a time: change one thing, rebuild,
read the new message.

## Your task

The project is meant to print the largest and the average of three numbers:

```text
largest: 9
average: 6
```

It does not build. Run `learndc check`, read the diagnostic, fix the cause,
and repeat until it builds and prints the two lines above. You will pass
through three different problems, one in `project.dcon` and two in `src/`. Do
not change the printed numbers or delete code from `src/main.dc`.

## Acceptance criteria

- The project builds with `dpm build`.
- The program prints the two lines above and exits with code 0.
- `src/main.dc` still calls `stats::largest` and `stats::average`.
