# A library that depends on a library

*Continues add-a-dependency: the harbor project, now with a library that has a dependency of its own.*

The harbor program prints through a second library, `libs/report`, which in
turn uses `libs/units` for its conversion:

```text
harbor  ->  libs/report  ->  libs/units
```

Open `libs/report/project.dcon`. It names its neighbour as `path:../units`:
a path dependency is relative to the manifest that declares it, so the library
works wherever the workspace is.

`src/main.dc` imports `report::lines`, but the harbor manifest declares no
dependency yet.

## Your task

Declare the one dependency harbor needs directly, `path:libs/report`. Do not
declare `units`: dpm finds it through `report`. Then run:

```text
dpm tree
dpm why path:libs/units
```

and read how `units` got into the build.

The program prints:

```text
Aurora: 120000 kg
Brisa: 80000 kg
Corvo: 200000 kg
Delta: 45000 kg
```

## Acceptance criteria

- `project.dcon` lists `path:libs/report` and builds.
- The program prints the table above.
