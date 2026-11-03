# dpm tasks and assets

A task is a command that a project declares in its own `project.dcon`, for the work that is not compiling: building a host tool, assembling a boot image, starting an emulator. An asset is a pinned download that tasks read but nothing imports, such as a bootloader release or firmware images.

## When tasks run

Tasks run only when you ask:

- `dpm task NAME` runs the task, after the tasks it needs.
- `dpm run` runs the task named by the product's `run` field, after building the product.

No other command runs a task: `dpm build`, `test`, `run` of a product without `run`, `lock`, `fetch`, `check`, `tree`, `clean`, `new`, `publish`, the `registry` commands and the editor support never start one. Tasks declared by dependencies are ignored, and so are the tasks of a template until it has generated your project.

Tasks are not sandboxed. A task runs with your permissions, in your environment, like a command you type. Read the tasks of a project you did not write before running them; `dpm task NAME --dry-run` prints the expanded commands without building or running anything.

## Declaring a task

    tasks = {
        {
            name = "iso",
            description = "Build the kernel and a bootable ISO",
            command = { "sh", "scripts/make-iso.sh", "${out:kernel}", "${asset:limine}", "${task_dir}/kernel.iso" },
            tools = { "xorriso" },
            build = { "kernel" },
            needs = { "limine-tool" },
            inputs = { "scripts/make-iso.sh", "limine.conf", "${out:kernel}" },
            outputs = { "${task_dir}/kernel.iso" },
        },
    },

| Field | Meaning |
|---|---|
| `name` | `[a-z][a-z0-9-]*`, unique in the project |
| `description` | shown by `dpm task` |
| `command` | the program and its arguments. They are passed to the program exactly as written, never to a shell: write `{ "sh", "-c", "..." }` to use one. The program is a name looked up on `PATH`, or a path inside the project |
| `cwd` | working directory, relative to the project root (default the root) |
| `env` | `{ name = "VAR", value = "..." }` entries added to the inherited environment; names are `[A-Z_][A-Z0-9_]*` and cannot start with `DPM_` |
| `tools` | programs that must be on `PATH`; a missing one is reported before the task starts |
| `build` | products built (or found up to date) before the task runs |
| `needs` | tasks that run first, depth first in the order listed, each at most once |
| `inputs` | files the command reads; globs (`*`, `?`, `**`) and directories are allowed |
| `outputs` | files and directories the command writes; no globs |
| `timeout` | seconds before the task is stopped; 0 (default) means no limit |

### Tokens

`command`, `cwd`, `env` values, `inputs` and `outputs` may contain these tokens, which dpm replaces before running:

| Token | Value |
|---|---|
| `${root}` | the absolute project root |
| `${profile}` | the selected profile (`--profile`, default `debug`) |
| `${dcc}` | the dcc that dpm runs |
| `${out:PRODUCT}` | the output file of a product in the selected profile |
| `${target:PRODUCT}` | the product's target (`host`, `x86_64-elf`, ...) |
| `${asset:NAME}` | the read-only directory of an asset |
| `${task_dir}` | `.dpm/tasks/<task>/`, a scratch directory for this task |
| `${task_dir:TASK}` | the scratch directory of another task |
| `$$` | a literal `$` |

Any other `$` is an error, as is an unknown token or a name that the project does not declare. The replaced text is never scanned again.

`cwd`, `inputs`, `outputs` and a program given as a path must stay inside the project: they are relative to the project root, or start with `${root}`, `${task_dir}`, `${task_dir:TASK}`, `${out:PRODUCT}` or (for inputs and programs) `${asset:NAME}`. Absolute paths and `..` are rejected. Arguments are free text.

### Environment

A task inherits your environment, plus `DPM_ROOT`, `DPM_PROFILE`, `DPM_TASK`, `DPM_OUT_<PRODUCT>` for every product (upper case, other characters as `_`), `DPM_OFFLINE=1` with `--offline` (a hint: dpm cannot keep a command off the network), and the task's own `env`. Standard input, output and error are the terminal's. The task runs in its own process group; Ctrl-C reaches it, and a timeout stops the whole group (SIGTERM, then SIGKILL two seconds later).

## Running tasks

    dpm task                        list the tasks
    dpm task NAME [-- ARGS...]      run a task; ARGS are appended to its command
    dpm task NAME --dry-run         print the expanded commands

`--profile`, `--target`, `-O`, `--locked`, `--offline`, `-v` and `--manifest-path` work as for `dpm build`. Arguments after `--` go to the named task only, not to the tasks it needs. A task that fails stops the run: dpm prints `task NAME failed (exit N)` (or the signal) and exits with 1.

A task that builds no product and reads no asset does not read or write `project.lock.dcon`.

### Skipping up-to-date tasks

A task with `outputs` is skipped (`Fresh task NAME`) while nothing it depends on changed: the expanded command, working directory and environment, the list of outputs, the contents of the products and assets it names, the input files (size and modification time, then content), and the outputs themselves (files by size and modification time, directories by content). A task without `outputs` always runs. Files the command reads but that are not listed in `inputs` are not tracked. The record is `.dpm/tasks/NAME.stamp`; `dpm clean` removes it with the task directories.

### dpm run and Product.run

A product whose `run` field names a task is run through that task:

    products = { { name = "kernel", linker_script = "linker-scripts/x86_64.lds", run = "run", settings = { target = Target::X86_64Elf } } },

`dpm run [PRODUCT] [-- ARGS]` builds the product and then runs the task as `dpm task NAME -- ARGS` would. `--runner` does not apply to such a product. Products without `run` behave as before: host programs are started directly, other targets need `--runner`.

## Assets

    assets = {
        { name = "limine", location = "https://github.com/Limine-Bootloader/Limine/releases/download/v12.9.6/limine-binary.tar.gz", version = "v12.9.6" },
        { name = "ovmf", location = "https://github.com/osdev0/edk2-ovmf-stable-bins/releases/download/20261026T212116Z/edk2-ovmf-bins.tar.gz", max_size = 536870912 },
        { name = "fonts", location = "github.com/user/fonts", version = "1.2.0" },
        { name = "local", location = "path:../shared/data" },
    },

| Field | Meaning |
|---|---|
| `name` | `[a-z][a-z0-9_]*`, unique among the assets; tasks use `${asset:NAME}` |
| `location` | an `https` `.tar.gz` or `.tgz`, a git location as for a dependency, or `path:DIR` |
| `version` | for git, the release tag to fetch (default the latest `v<version>` tag); informational for a tarball; not allowed for `path:` |
| `max_size` | the largest unpacked size in bytes; 0 (default) keeps the 256 MiB limit that applies to every download; at most 1 GiB; not allowed for `path:` |

Assets are fetched by the same code as dependencies, with the same checks and limits, into the shared cache, where they are read-only. A task that changes asset files must copy them first.

`dpm lock` (and `dpm build` or `dpm task` when the lock is out of date) records each asset in `project.lock.dcon` with its source, the version it was requested with, the commit for git, and the tree hash. The first time an asset is locked dpm prints its hash (trust on first use). Afterwards a download that does not match the lock is an error (exit code 4), `--locked` fails if an asset would change, and `dpm fetch` downloads every locked asset so that later commands work with `--offline`. Changing an asset's location or version resolves that asset again; other locked assets keep their pins. `dpm tree` lists the assets with their pins. Assets declared by dependencies are ignored.

`path:` assets are read in place and are not hashed in the lock; they are meant for local data and tests.

## Limitations

Tasks run one at a time. There are no conditionals, per-platform variants or argument defaults. Tasks and `Product.run` need the POSIX process runner and are not available on Windows hosts yet.
