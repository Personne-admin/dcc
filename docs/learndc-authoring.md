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
      tracks/<track>/<name>/lecture.toml     instead of exercise.toml: a lecture or drill
      tracks/<track>/<name>/examples/*.dc    lecture examples, compiled and run by validation

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
| `chapter` | string | none | optional free-form label used by search |
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
| `optional` | boolean | false | a bonus lesson, not needed to complete the chapter |
| `[boss]`, `[[stages]]`, `[[questions]]` | tables | none | see Stages and Boss Fights, and Activities |

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
| `stage` | run, harness, diagnostic | string | none | the stage the check belongs to |
| `diagnostic_contains` | diagnostic | string | empty | substring of the sanitized compiler diagnostics |

A `diagnostic` check compiles only and asserts on the diagnostics; use it to teach reading compiler errors. Output comparison happens after control sequences are stripped.

## Chapters

A track is a chapter. `catalog.toml` lists the chapters in learning order, and each `track.toml` may add a one-sentence `goal`:

    schema = 1
    id = "memory"
    title = "Ownership and Cleanup"
    summary = "Understand when values live and die, clean up with defer and own growable memory."
    goal = "Release every resource exactly once with defer, and build, lend and search owning Array and Map values."
    exercises = ["scope-and-lifetime", "defer-order", "boss-inventory-keeper"]

`exercises` names every lesson directory of the chapter, lectures and drills included. A chapter counts as complete when every lesson that is not `optional = true` is complete. At most one Boss Fight per chapter; put it last.

### Renaming lessons

Lesson ids are the keys of learner progress and workspaces. When a lesson moves, keep its progress by listing the old id in `catalog.toml`:

    [renamed]
    "types/owned-array" = "memory/filling-arrays"

The old id must no longer exist and the new one must. On start, learndc moves the learner's records, bookmarks, recent list and active lesson to the new id, keeps an existing workspace where it is (its metadata is retargeted and the path remembered) and writes a one-time copy of a schema 1 progress file to `progress.json.schema1.bak`. Commands accept the old id too. A prerequisite that names an old id is an error that names the new one.

## Lectures and drills

A lecture directory holds `lecture.toml`, `README.md` and optional `examples/<name>.dc`; nothing else. Each `## ` heading of the README is a page in the terminal interface. A fenced block whose info string is `dc example=<name>` and whose body is empty is replaced by the example file, so the code a learner reads is exactly the code validation compiles and runs:

    ```dc example=defer_once
    ```

| Key | Type | Rule |
|---|---|---|
| `schema`, `id`, `revision`, `title`, `summary`, `difficulty`, `estimated_minutes`, `concepts`, `objectives`, `prerequisites` | | as in exercise.toml |
| `tags`, `optional` | | optional, as in exercise.toml |
| `kind` | string | `lecture` (default) or `drill`; a drill needs at least one question |
| `[[examples]]` | `name`, `expect_stdout` | every example is compiled and run by validation and must print `expect_stdout` |
| `[[questions]]` | see below | activities; a lecture is complete when every question has been answered right |
| `[[cards]]` | `id`, `front`, `back` | flashcards; they join the learner's review once the lesson is complete |

A lecture without questions is completed by marking it as read.

## Activities

Questions appear in lectures, drills and multi-stage exercises. Every question has a slug `id` (default `q1`, `q2`, ...), a `type`, a `prompt` and an `explanation`. The explanation is shown when the answer is right or after the second attempt; until then the learner sees targeted feedback. Grading is deterministic and offline.

| `type` | Extra keys | Graded by |
|---|---|---|
| `choice` | `choices` (2 to 6), `answer`, optional `why` | the chosen number |
| `multi` | `choices`, `answers = [..]`, optional `why` | the exact set; extra picks show their `why` |
| `predict` | `code` or `example`, `expect` | output lines with trailing spaces and final empty lines ignored |
| `trace` | `code` or `example`, `accept = [..]`, optional `expect` | a short answer |
| `bug` | `code`, `lines = [..]`, optional `fixed` and `expect` | the chosen line number |
| `error` | `code`, `diagnostic`, `choices`, `answer`, optional `why` | the chosen number |
| `order` | `code` (2 to 14 distinct lines), optional `example` | the exact line order |
| `short` | `accept = [..]` | lowercase, trimmed, spaces collapsed, surrounding backticks or quotes and a final period removed |

`why` holds one entry per choice, empty for the right one, explaining why each wrong choice is wrong. Keep `short` and `trace` answers to one word, name or number: free text is never graded. An `order` prompt must make the order unique.

Validation runs the code: a `predict` program must print `expect`; a `trace` program with `expect` must print it; an `error` program must fail to compile with `diagnostic` in the compiler output; for a `bug` with `fixed`, the fixed program must print `expect` and the buggy one must not. The code of an `order` question must appear verbatim in its `example`.

Learners answer in the terminal interface (one activity per page), or with `learndc answer ID --question Q ANSWER`. Every attempt is recorded per question; questions whose last attempt was wrong come back on the Review screen and in `learndc cards`. Flashcards are scheduled with five Leitner boxes (again, hard, good and easy move a card between boxes; box intervals are 1, 2, 4, 8 and 16 days).

## Stages and Boss Fights

An exercise can be split into stages that are checked and recorded one by one:

    [[stages]]
    id = "stock-up"
    title = "Stock the shelf"
    summary = "add pushes a new Item for every name, and kinds counts the entries."
    hints = ["The shelf is an Array(Item) that belongs to the caller."]

    [[checks]]
    kind = "harness"
    name = "add and kinds stock new items"
    stage = "stock-up"
    harness = "checks/stock_up.dc"

| Stage key | Rule |
|---|---|
| `id`, `title`, `summary` | required |
| `optional` | a bonus stage, never needed for completion |
| `revision` | raise it when the stage changes; learners who passed an older revision are asked to pass it again |
| `hints` | up to three, revealed one at a time for the current stage |
| `questions` | ids of the exercise's own `[[questions]]`; a stage may consist of questions only |

With stages, every check except the build names its stage, every stage has a check or a question, every question belongs to exactly one stage and at least one stage is required. Harness checks of a staged exercise build independently of the main program, so an early stage can pass while a later module is broken: give each stage its own functions and its own harness. An exercise is complete after one check in which every required stage passes and whose required questions are answered.

The progress file keeps, per stage, when it was first passed and the result of the last check, together with a fingerprint of the files that were checked. The interface therefore tells apart a stage that passes with the current files, one that passed before and fails now, and one that passed before but has not been checked since the files changed.

A Boss Fight is a staged exercise with a `[boss]` table:

    [boss]
    rating = 3
    story = """
    The old storekeeper of the night market has retired ...
    """

It needs at least two stages and a hint for each. Validation requires the starter to fail every stage and the solution to pass every stage, bonus stages included.

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
