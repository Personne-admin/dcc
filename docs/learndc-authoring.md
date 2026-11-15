# Writing learndc exercises

This guide is for people who write exercises for [learndc](learndc.md). An exercise is a directory of plain files: a TOML description, Markdown text, a starter project, a reference solution and, when needed, small trusted test programs. Nothing in it is code that learndc executes by itself: checks are a fixed set of kinds that learndc interprets.

## Layout

    content/
      catalog.toml
      tracks/<track>/track.toml
      tracks/<track>/<name>/exercise.toml
      tracks/<track>/<name>/README.md        instructions shown to the learner
      tracks/<track>/<name>/hints.md         exactly three hints
      tracks/<track>/<name>/explanation.md   why the reference solution works
      tracks/<track>/<name>/starter/...      copied to the learner's workspace
      tracks/<track>/<name>/solution/...     files that differ from the starter (overlay)
      tracks/<track>/<name>/checks/*.dc      harness programs, never copied to the workspace

The bundled content in `learndc/content/` is the reference. Any other top-level file or directory inside an exercise directory is an error. Two files whose paths differ only by letter case are an error.

All files are text: `.toml`, `.md`, `.dc`, `.dcon` and `.txt` (plus `.gitignore`). Symbolic links and dot-files are not allowed. Every path written in a content file must be relative, with no `..`, no backslash, no drive letter and no empty or `.` component.

## ASCII only

Every TOML and Markdown file in a content directory must be ASCII only. Em dashes, typographic quotes, arrows and the ellipsis character are rejected with `non-ASCII character (content text must be ASCII only)` and the position. This keeps terminals, screen readers and Windows consoles safe. Use `--`, straight quotes and `...`.

## catalog.toml

    schema = 1
    title = "Learn DC"
    tracks = ["fundamentals", "types", "errors", "library", "projects"]

`schema` must be 1 (`unsupported schema N (this learndc supports 1)` otherwise). `tracks` is the order of the tracks; each must have a directory under `tracks/`.

## track.toml

    schema = 1
    id = "fundamentals"
    title = "Fundamentals"
    summary = "Programs, values, functions, branches, loops and reading compiler errors."
    exercises = ["hello-world", "variables"]

`id` must equal the directory name. `exercises` is the order of the exercise directories and must name each one exactly once.

## exercise.toml

Unknown keys are errors. Errors are reported as `file:line:column: message`.

| Key | Type | Default | Rule |
|---|---|---|---|
| `schema` | integer | required | must be 1 |
| `id` | string | required | `<track>/<name>` using lowercase letters, digits and single hyphens; must equal the track and directory name |
| `revision` | integer | required | at least 1; raise it when the starter or the checks change |
| `title` | string | required | |
| `summary` | string | required | one sentence |
| `kind` | string | required | `repair-compilation`, `fix-behavior`, `implement-function`, `complete-program`, `understand-diagnostics`, `refactor`, `multi-module`, `libdcext` or `challenge` |
| `chapter` | string | required | free-form grouping label |
| `difficulty` | string | required | `beginner`, `intermediate` or `advanced` |
| `estimated_minutes` | integer | required | 1 to 1000 |
| `concepts` | array of strings | required | used by search |
| `tags` | array of strings | `[]` | used by search |
| `objectives` | array of strings | required | at least one |
| `entry_file` | string | required | relative path that exists in `starter/`; opened by `learndc edit` |
| `product` | string | required | the dpm product name; an identifier `[a-z_][a-z0-9_]*`, and `starter/project.dcon` must mention it |
| `prerequisites` | array of strings | required (may be empty) | exercise ids that exist; no cycles |
| `requires` | array of strings | required | `dcc`, `dpm` or `dccd` |
| `min_dcc` | string | required | `major.minor.patch` |
| `platforms` | array of strings | required | `linux`, `windows` |
| `backends` | array of strings | required | `llvm`, `custom`: the code generators the solution is validated on |
| `starter_state` | string | required | `fails-build` or `fails-checks`; validation proves the starter really does that |
| `build_timeout_seconds` | integer | learner setting | 10 to 600; override for a slow build |

At least one `[[checks]]` table is required. If none has `kind = "build"`, learndc inserts a build check named `Compiles` first; a build always runs first anyway. Two checks may not share a name.

### Checks

    [[checks]]
    kind = "run"
    name = "Prints the greeting"
    expect_stdout = """
    Hello, Ada!
    Welcome to DC.
    """
    expect_exit = 0
    max_seconds = 10
    hint = "Check the comma and the final full stop."

Keys by kind (a key from another kind is rejected with `key 'x' is not valid for a <kind> check`):

| Key | Kinds | Type | Default | Meaning |
|---|---|---|---|---|
| `kind` | all | string | required | `build`, `run`, `harness`, `diagnostic` |
| `name` | all | string | required | shown to the learner; unique per exercise |
| `hint` | all | string | none | shown when the check fails |
| `args` | run | array of strings | `[]` | program arguments |
| `stdin` | run | string | empty | inline standard input |
| `harness` | harness | string | required | `checks/<stem>.dc`, stem `[a-z][a-z0-9_]*`; the file must exist |
| `expect_stdout` | run, harness | string | not compared | expected output |
| `stdout_match` | run, harness | string | `exact` | `exact`, `contains` or `trimmed` |
| `line_endings` | run, harness | string | `portable` | `portable` normalizes CRLF to LF on both sides; `exact` does not |
| `expect_stderr_contains` | run, harness | string | not compared | substring of stderr |
| `expect_exit` | run, harness | integer | 0 | 0 to 255 |
| `max_seconds` | run, harness | integer | 10 | 1 to 60 |
| `expect_build` | diagnostic | string | required | `success` or `failure` |
| `diagnostic_contains` | diagnostic | string | empty | substring of the sanitized compiler diagnostics |

A `diagnostic` check compiles only and asserts on the diagnostics; use it to teach reading compiler errors. Output comparison happens after control sequences are stripped.

## README.md, hints.md, explanation.md

- `README.md`: the instructions. Start with a `# Title` heading. State the task, the expected output and where to edit. Show short examples in fenced `dc` blocks. Keep it self-contained; the learner reads it in a terminal.
- `hints.md`: exactly three sections titled `## Hint 1`, `## Hint 2`, `## Hint 3`, in order, with nothing else at heading level 2 (fenced code is ignored). Escalate: a concept, then the approach, then nearly the answer. The learner reveals them one at a time.
- `explanation.md`: shown with the solution. Explain why it works, not just what it does.

## Starter and solution

`starter/` is a complete dpm project: `project.dcon` and `src/*.dc`. It is copied into the learner's workspace once, never overwritten later. `solution/` holds only the files that differ from the starter, in the same relative paths; validation overlays it on the starter. Both directories must exist and be non-empty, and `starter/project.dcon` must exist.

Product and package names in `project.dcon` must be single identifiers (`[a-z_][a-z0-9_]*`). A hosted starter looks like:

    dpm::v1::Project {
        name = "hello",
        version = "0.1.0",
        products = {
            { name = "hello", entry = "src/main.dc" },
        },
    }

## Harness programs

A `harness` check tests code the learner writes in a library module without depending on how `main` prints. Put the learner's code in a module other than `main` (for example `src/numbers.dc`, module `numbers`) and add `checks/<stem>.dc`:

    module square_checks;

    import numbers;
    using numbers::*;

    public i32 main() {
        i32 failures = 0;
        if square(5) != 25 { failures += 1; }
        return failures;
    }

Rules:

- The file must contain the line `module <stem>;` where the stem is the file name without `.dc`.
- It defines `public i32 main()` returning 0 on success and prints what it compared, so a failure explains itself.
- The harness never reaches the learner's workspace. For each harness check learndc builds in a separate staging directory: a copy of the learner's `src/` tree, each harness at `tests/<stem>.dc`, and a manifest that learndc generates:

      dpm::v1::Project {
          name = "learndc_check",
          version = "0.1.0",
          products = {
              { name = "check_<stem>", entry = "tests/<stem>.dc" },
          },
      }

  dpm passes `-Isrc`, so harness modules import the learner's modules by name. You do not write this manifest.

## Validating

    learndc dev validate --content-dir path/to/content
    learndc dev validate --backend all --content-dir path/to/content
    learndc dev validate --id types/structs --backend llvm --content-dir path/to/content

Validation first checks the schema, then, per exercise and backend, builds the starter and checks that it fails as `starter_state` says, then overlays `solution/` and checks that every check passes. `--backend` is `dpm` (default, through `dpm build`), `llvm`, `custom` or `all`. The direct backends run dcc with the same flags dpm uses for a host product (`-Isrc -O0 -fwhole-program -flibdcext -fbounds-check`, `-g` for llvm only). Example result:

    Content learndc/content: 23 exercise(s) loaded, schema valid
    EXERCISE                  BACKEND  STARTER             SOLUTION              RESULT
    fundamentals/hello-world  dpm      fails 1/2           passes 2/2            ok
    fundamentals/hello-world  llvm     fails 1/2           passes 2/2            ok
    fundamentals/hello-world  custom   fails 1/2           passes 2/2            ok

    3 check(s) of 1 exercise(s): 0 failed

Exit status is 0 when everything passes. A content error exits with 3. Validation compiles and runs the starter and solution files with your permissions: run it only on content you trust.

During authoring, `--content-dir DIR` (or `LEARNDC_CONTENT_DIR`) makes every learndc command use your directory instead of the built-in set, so you can `start` and `check` as a learner would.

## Embedding at build time

The bundled content is packed into the executable. `make learndc` runs `learndc/tools/bundle_content.py --root learndc/content`, which writes a single archive (magic `LDCB1`, then the relative path and bytes of each file) that is linked in as data. Only the allowed file types are accepted, symbolic links fail the build, and dot-files are skipped. Adding an exercise means adding its directory, listing it in its `track.toml`, and rebuilding; nothing else is registered anywhere. `learndc --version` prints the number of exercises embedded.

## Trust model

Content has no executable fields. Check kinds, matching modes and limits are a closed set interpreted by learndc; no key can name a program to run, a shell command or a script. The only code that runs is the learner's own project (starter plus edits), and the harness programs in `checks/`, which are compiled and run like any DC program.

A content directory is therefore as trustworthy as the source code in it, no more. Third-party exercise packs are not trusted plugins: loading one with `--content-dir` makes learndc compile and run its `.dc` files with your permissions, and there is no sandbox (see [safety](learndc.md#safety)). Read a pack before you check it. The tests and solutions of every pack, including the bundled one, are plain files and visible to the learner.
