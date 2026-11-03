# dpm project templates

`dpm new` creates a project from a template: a directory with a `template.dcon` and the files to generate. A template is data. Creating a project never runs anything from the template, its repository or its archive; the generated project's tasks (see [dpm-tasks.md](dpm-tasks.md)) run only when you start them with `dpm task` or `dpm run`.

## Creating a project

    dpm new [DIR] [--template SOURCE] [--name NAME] [--set VAR=VALUE ...] [--ref REF] [--pin HASH] [--lib] [--offline]

- `DIR` is created if it does not exist. If it exists it must be empty (a `.git` directory is allowed). Without `DIR` the current directory is used.
- The package name is `--name`, else the directory name made into an identifier, as for `dpm init`. `--name NAME` is the same as `--set name=NAME`.
- Every variable the template declares can be set with `--set`; a required variable without a value is an error that lists what is missing. There are no prompts.
- The project is written to a temporary directory next to `DIR` and moved into place only when every file was generated and the generated `project.dcon` loads without errors. A failure leaves nothing behind.

Without `--template`, `dpm new` uses the built-in executable template, or the built-in library template with `--lib`; they produce the same files as `dpm init`.

After generating, dpm prints the number of tasks the new project declares with their commands, the assets it will download, and the template's next steps. Read `project.dcon` before running a task: a template can declare any command.

## Template sources

| SOURCE | Meaning |
|---|---|
| `builtin:executable`, `builtin:library` | the built-in templates |
| `path:DIR`, `./DIR`, `../DIR`, `/DIR` | a local directory, read in place |
| `github.com/user/repo[/subdir]`, `git+https://host/repo.git[#subdir]` | a git repository; the latest `v<version>` tag unless `@VERSION` or `--ref` is given |
| `https://host/name.tar.gz` (or `.tgz`) | a tarball |
| `name[@version]` | a template published in the registry |

`--ref` takes a branch name, a tag or a full 40-digit commit id, for repositories that have no version tags. `--pin sha256:...` makes the fetch fail (exit code 4) unless the template tree has exactly that hash.

Fetched templates go through the same pipeline as dependencies: hardened git (no hooks, no filters, no submodules, no symlinks), tarball member checks (no links, devices, absolute paths, `..`, case collisions), size and file count limits, and a content hash. dpm prints the commit and hash of every fetched template, so a script can record them:

    template github.com/user/repo commit 0123... hash sha256:abcd...

A registry template is fetched at the commit and hash its registry record lists, and its `template.dcon` must claim the registered name.

`dpm template info SOURCE` prints a template's description and variables without generating anything. `dpm template check [DIR]` validates a template directory the way `dpm new` would, generating it in memory with default or sample values.

## template.dcon

    dpm::v1::Template {
        name = "limine-x86-64",
        description = "An x86-64 kernel booted by Limine (BIOS and UEFI), runnable under QEMU.",
        variables = {
            { name = "title", description = "Boot menu entry title", default_value = "Limine Template", max_length = 48 },
        },
        substitute = { "project.dcon", "limine.conf", "README.md" },
        next_steps = "cd @@dir@@\n  dpm task iso        build the kernel and a bootable ISO",
    }

| Field | Meaning |
|---|---|
| `name` | template name; for a registry template it must equal the registered name |
| `description` | one line, shown by `dpm template info` |
| `root` | directory inside the template whose contents are generated (default `files`) |
| `variables` | the variables the template accepts (below) |
| `substitute` | globs, relative to `root`, of files whose contents get placeholders replaced; every other file is copied byte for byte |
| `exclude` | globs of files that are not generated |
| `next_steps` | text printed after generation, with placeholders replaced |
| `requires_dpm`, `requires_dcc` | minimum versions; `dpm new` refuses an older dpm or dcc |

### Variables

Each variable has a `name` (`[a-z][a-z0-9_]*`), a `description`, a `kind`, a `default_value` or `required = true`, and kind-specific limits:

| Kind | Accepted values |
|---|---|
| `VariableKind::String` | letters (including non-ASCII), digits, space and `_ - . , : / + @ ( ) # %`; at most `max_length` bytes (default 128, at most 256) |
| `VariableKind::Identifier` | a DC identifier that is not a reserved word |
| `VariableKind::Integer` | a decimal integer, optionally between `min` and `max` |
| `VariableKind::Bool` | `true` or `false` |
| `VariableKind::Choice` | one of `choices` |

The value rules are deliberately strict: no quotes, backslashes, backticks, `$`, `<`, `>`, `&`, `|`, `;`, braces or control characters, so a value cannot leave a string literal in `project.dcon`, a word in a shell script or a line in a configuration file. A template that needs other text must offer a `Choice`.

`name` and `dir` are built in. `name` is the package name; `dir` is `DIR` as typed and may only be used in `next_steps`.

### Placeholders

A placeholder is `@@` followed by a variable name and `@@`, for example `@@name@@`. `@@@@` writes a literal `@@`. Any other `@@` (such as `@@ x` or `@@Name@@`) is left as it is. A placeholder that names an undeclared variable is an error with its file and line.

Placeholders are replaced in the files that `substitute` matches, in every file and directory name (so `src/@@name@@/lib.dc` works), and in `next_steps`. A substituted file must be UTF-8 text without NUL bytes and at most 1 MiB; line endings are kept. Generated names must still be valid relative paths, and no two may collide, even ignoring case.

### Globs

Globs are relative to `root`, use `/` as separator and support `*` (within one path component), `?` (one character) and `**` (any number of whole components). A pattern ending in `/` matches a directory and everything below it. There are no brackets, braces or negation. `exclude` wins over `substitute`.

## Publishing a template

A template in a git repository is published like a package. In a directory with a `template.dcon` and no `project.dcon`, `dpm publish` checks the template with `dpm template check`, takes the version from the single `v<version>` (or `<version>`) tag at HEAD, and writes `templates/<shard>/<name>.dcon` into a registry clone. `dpm registry template LOCATION NAME` reserves a name, `dpm registry yank-template NAME VERSION [--undo]` yanks a version, and `dpm registry check` validates the `templates/` directory. Template names are separate from package aliases.

## Example: the Limine kernel template

The Limine template generates an x86-64 kernel with a linker script, the Limine and OVMF releases as pinned assets, and tasks that build the Limine host tool, an ISO or a disk image, and boot them in QEMU:

    dpm new mykernel --template github.com/Personne-admin/limine-dc-template-x86-64 --ref dpm --set title="My Kernel"
    cd mykernel
    dpm task iso
    dpm run

A copy of the template is kept in `tests/dpm/templates/limine-x86-64/`.
