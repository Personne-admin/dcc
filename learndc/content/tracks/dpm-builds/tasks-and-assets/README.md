# Tasks and assets

Compiling is not the only work a project has: building a host tool, producing a
disk image, starting an emulator, copying data. A task is a command the
project declares in `project.dcon` for that work.

## Tasks run only when you ask

```text
dpm task                     list the tasks
dpm task NAME                run one, after the tasks and products it needs
dpm task NAME -- ARGS        append ARGS to its command
dpm task NAME --dry-run      print the expanded commands, run nothing
```

Only `dpm task NAME` and `dpm run` (for a product that has a `run` field)
start a task. `dpm build`, `dpm test`, `dpm lock`, `dpm fetch`, `dpm check`,
`dpm tree`, `dpm clean`, `dpm new` and the editor support never do. Tasks of
dependencies and templates are ignored. This is a safety rule: opening and
building a project you downloaded must not run its code.

Tasks are not sandboxed, though. A task runs with your permissions in your
environment. Read the `tasks` of a project you did not write, or run
`dpm task NAME --dry-run` first.

## A task

```text
tasks = {
    {
        name = "fleet-report",
        description = "Run the fleet program",
        command = { "${out:fleet}" },
        build = { "fleet" },
    },
},
```

- `name` is lowercase letters, digits and hyphens, starting with a letter.
- `command` is a list: the program and its arguments, passed as they are.
  It is never given to a shell. `{ "echo hello from home" }` looks for a
  program with that whole text as its name and fails. To use shell syntax say
  so: `{ "sh", "-c", "echo $$HOME | wc -c" }`.
- `build` names products that are built (or found up to date) first.
- `needs` names tasks that run first, each at most once.
- `tools` are programs that must be on `PATH`; a missing one is reported before
  the task starts.
- `cwd`, `env`, `timeout` and the rest are in docs/dpm-tasks.md.

## Tokens

dpm replaces these before running:

| Token | Value |
|---|---|
| `${root}` | absolute project root |
| `${out:PRODUCT}` | the output file of a product in the selected profile |
| `${asset:NAME}` | the read-only directory of an asset |
| `${task_dir}` | `.dpm/tasks/<task>/`, scratch space for this task |
| `${profile}`, `${dcc}`, `${target:PRODUCT}` | selected profile, compiler, product target |
| `$$` | a literal `$` |

A bare `$` is an error: `a '$' must start a ${...} token; write '$$' for a
literal '$'`. An unknown token, a product that does not exist and a bad task
name are all reported when dpm loads the manifest, so `dpm check` finds them.
`cwd`, `inputs` and `outputs` must stay inside the project.

## Skipping work

A task with `outputs` is skipped (`Fresh task NAME`) while nothing it depends on
changed: its command, its inputs (listed in `inputs`), the products and assets it
names and its outputs. A task without outputs always runs. List the files the
command reads in `inputs`; files that are not listed are not tracked.

## Assets

An asset is a pinned input that is not a package: data or tools the tasks read
but nothing imports.

```text
assets = {
    { name = "ships", location = "path:data" },
},
```

`location` may be an https tarball, a git location as for a dependency, or
`path:DIR` for a local directory. Remote assets are fetched into the shared
cache, read-only, and recorded in `project.lock.dcon` with their hash (trust on
first use; a later mismatch is an error). A `path:` asset is read in place and
not hashed. Tasks refer to the directory as `${asset:ships}`; a task that must
change files copies them first.

## Running a product through a task

A product whose `run` field names a task is started by `dpm run` through that
task, after it is built. This is how kernels and boot images are run: the
program cannot run on the host, so the task starts an emulator.

## Limitations

Tasks run one at a time, there are no conditionals or per-platform variants,
and tasks and `run` need a POSIX host (not Windows yet).

## Common mistakes

- Writing shell text as one argument instead of `sh -c`.
- A literal `$` without doubling it.
- Expecting `dpm build` to run a task that "belongs" to the build.
- Listing outputs but forgetting inputs, then wondering why a changed file does not re-run the task.
