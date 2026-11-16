# Two errors inside a dependency

*Continues the harbor project: the units library from the previous lessons has two mistakes.*

The dependency is declared correctly this time, and the lock is fine. The build
still fails, and the error locations point into `libs/units/src`. A library is
compiled with the program that imports it, so its mistakes appear in the
program's build.

dcc reports one problem first; fix it, build again and read the next.

## Your task

Make the project build without changing `src/main.dc` or `project.dcon`. Only
files under `libs/units` need edits. Run `dpm build` after each change.

The program prints:

```text
Aurora 120 t is 120000 kg
Delta 45 t is 45000 kg
```

## Acceptance criteria

- `dpm build` succeeds.
- The output above, and no other lines.
