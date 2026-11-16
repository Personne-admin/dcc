# A task that reads an asset

*Continues first-task: the same harbor project, now with a data directory.*

The harbor project keeps a data file, `data/ships.txt`, with one ship per
line. The harbormaster wants to count the ships with a task, without anything
being copied around by hand.

`src/main.dc` is the course's audit; leave it and `src/audit.dc` alone.

## Your task

In `project.dcon` declare:

1. An asset named `ships` whose location is `path:data`.
2. A task named `count-ships` with:
   - `command`: `sh`, `-c`, the script `wc -l < "$$1" > "$$2"; cat "$$2"`
     (the doubled dollars are how dpm spells a literal `$`), then `sh` as the
     script's name, then the asset file `${asset:ships}/ships.txt` and the
     output file `${task_dir}/count.txt`
   - `tools`: `sh` and `wc`
   - `inputs`: the asset file `${asset:ships}/ships.txt`
   - `outputs`: `${task_dir}/count.txt`

Then run `dpm task count-ships` twice. The first run prints `4`; the second
prints `Fresh task count-ships`, because the input and the output did not
change.

## Acceptance criteria

- `dpm run audit` prints nine `ok` lines and `9 checks, 0 failed`.
- `dpm check` is silent.
