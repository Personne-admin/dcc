# Release a breaking change

*Continues small-api: the stats library gets its first breaking change.*

The harbor team found that `mean` is a misleading name next to the median they
want to add later. The decision: `stats::summary::mean` is renamed to
`average`. The library is at version `0.1.0` and its first users are in the same
workspace.

`libs/stats/src/stats/summary.dc` still has the old name and `src/main.dc` (the
course's audit) already calls the new one. Leave the audit and `src/audit.dc`
alone.

## Your task

1. Rename `mean` to `average` in `summary.dc`.
2. Choose the version number for a breaking change before 1.0 and write it in
   `libs/stats/project.dcon`.
3. Edit `libs/stats/CHANGELOG.md`: add a section `## 0.2.0` above the existing
   `## 0.1.0` section with one line saying that `mean` was renamed to
   `average`. Keep the 0.1.0 section.

## Acceptance criteria

- `dpm run audit` prints seven `ok` lines and `7 checks, 0 failed`.
