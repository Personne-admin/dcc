# Define a custom profile

*Continues the harbor project of chapter two, with audit and fleet products.*

The harbor team ships release builds, which are fast but trap on nothing: an
index past the end of an array silently reads garbage. For a pilot they want
the same optimization with bounds checks turned back on.

The project has three products. `src/main.dc` is the course's audit, which
reads `project.dcon` and prints a checklist; leave `src/main.dc` and
`src/audit.dc` alone and use `dpm run audit` to see what is missing.

## Your task

Add one profile to `project.dcon`:

- name `checked`
- it starts from the release profile (`inherits = ProfileBase::Release`)
- its settings turn bounds checking on (`bounds_check = Tri::On`)

Then compare the three commands. Their dcc lines differ in exactly the flags
you expect:

```text
dpm build -v
dpm build --profile release -v
dpm build --profile checked -v
```

## Acceptance criteria

- `dpm run audit` prints five `ok` lines and `5 checks, 0 failed`.
- `dpm build --profile checked` builds.
