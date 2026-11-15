# learndc: learn DC interactively

learndc is a small offline tool for learning the DC language by doing exercises. Each exercise gives you a starter project, you edit it in your own editor, and learndc compiles and runs it and tells you what is different from the expected result. It has a command line, and a full-screen terminal interface when you run it without a command.

learndc is an adjacent learning option. It does not replace `dpm new` and `dpm run`: those remain how you create and build your own projects (see [getting started](getting-started.md)). learndc uses dcc and dpm underneath, and every exercise workspace is an ordinary dpm project that you can open, build and keep.

Status of this release:

- The command line described below is complete and was tested on Linux.
- The full-screen interface opens when you run `learndc` without a command in a terminal. Everything it does is also available as commands.
- Windows: learndc is cross-compiled for Windows and packaged in the MSI, but has not been validated natively. See [Platform status](#platform-status).

## Contents

- [Quickstart](#quickstart)
- [The terminal interface](#the-terminal-interface)
- [Command reference](#command-reference)
- [Exit codes](#exit-codes)
- [JSON output](#json-output)
- [Editors](#editors)
- [Checks and feedback](#checks-and-feedback)
- [Safety](#safety)
- [Where your data lives](#where-your-data-lives)
- [Progress export and import](#progress-export-and-import)
- [Reset and backups](#reset-and-backups)
- [Content revisions](#content-revisions)
- [Watch mode](#watch-mode)
- [Troubleshooting](#troubleshooting)
- [Platform status](#platform-status)

## Quickstart

learndc needs `dcc` and `dpm`. It looks for them in the settings (`tools.dcc`, `tools.dpm`), then the environment (`LEARNDC_DCC`, `LEARNDC_DPM`), then next to the learndc executable, then on `PATH`. Run `learndc doctor` to see what it found.

From a source checkout:

    make                 # builds the compiler, dpm and libdcext
    make learndc         # builds build/bin/learndc
    make install         # installs learndc next to dcc and dpm (PREFIX, default /usr/local)

`LEARNDC_STANDALONE=1 make learndc` builds only learndc, for an already installed dcc and dpm.

The Linux `.deb` and the Windows MSI install `learndc` beside `dcc`, `dccd` and `dpm`. Exercises, hints and reference solutions are built into the executable; there are no separate content files.

### Windows installer

The MSI installs `dcc.exe`, `dccd.exe`, `dpm.exe` and `learndc.exe` into `bin` under the installation directory (default `C:\Program Files\dcc`, or the `INSTALLDIR` you choose), adds a **Learn DC** shortcut to the `dcc` folder of the Start Menu, and appends that `bin` directory to the system `PATH`.

- The shortcut opens learndc's dashboard in an ordinary console window, starting in your Documents folder. Windows Terminal is not required.
- The `PATH` entry is the real installation directory, appended to the end of the system `PATH`; existing entries are kept. Repair and reinstall do not add it twice, and uninstall removes only this entry.
- To install without changing `PATH`, pass `ADDTOPATH=0`, for example `msiexec /i dcc-0.6.0-x86_64.msi ADDTOPATH=0`. The choice is remembered for repairs and upgrades; pass `ADDTOPATH=1` to turn it back on. The installer has no options dialog, so this property is the only switch.
- Terminals and programs that were already open keep their old environment. Open a new terminal after installing before running `dcc`, `dpm` or `learndc` by name.
- Uninstall removes the shortcut, the `dcc` Start Menu folder and the `PATH` entry.

Check the setup, then do the first exercise entirely from the command line:

    learndc doctor
    learndc start fundamentals/hello-world
    learndc edit
    learndc check
    learndc hint
    learndc next

`start` creates the workspace and makes the exercise active. The commands after it use the active exercise (or the exercise whose workspace is your current directory) when you leave out the id. Real output of a first failing check, then a passing one after the solution was applied (shown with `--ascii --no-color`):

    $ learndc start fundamentals/hello-world
    Created the workspace for fundamentals/hello-world: <data>/workspaces/fundamentals/hello-world
    Next: learndc edit fundamentals/hello-world   then   learndc check fundamentals/hello-world

    $ learndc check
    Checking fundamentals/hello-world in <workspace> (active exercise)
    fundamentals/hello-world: 1 of 2 checks passed
      [ok] pass  Compiles
      [x] FAIL  Greets Ada, then welcomes her
          the output is different from what was expected (stage: compare)
          Expected:
            | Hello, Ada!
            | Welcome to DC.
          Observed:
            | Hello, world!
          Hint: Both lines must match exactly, ...
    Full log: <cache>/logs/fundamentals-hello-world/last-check.log
    Note: learndc compiles and runs your code with your own permissions. ...

    $ learndc check
    fundamentals/hello-world: 2 of 2 checks passed
      [ok] pass  Compiles
      [ok] pass  Greets Ada, then welcomes her

    Exercise completed.
    Next recommended: fundamentals/variables - Variables and types
      learndc start fundamentals/variables

`learndc hint` reveals one more hint each time (three per exercise) and remembers how many you have seen. `learndc solution --show` prints the reference solution with an explanation; looking at it is recorded in your progress and the exercise appears in `learndc review`.

The bundled exercises are in five tracks: Fundamentals (8), Types and memory (5), Errors and results (4), Everyday libdcext (3) and DPM projects (3). `learndc list` shows them all.

## The terminal interface

Run `learndc` with no command in an interactive terminal (stdin and stdout both a terminal) to open the full-screen interface. In any other situation, `learndc` with no command prints the help text.

Screens: Home (continue where you stopped, the recommended next exercise, progress per path), Learn (every exercise with status, filters and search), Exercise (instructions, files, results), Progress, Bookmarks, Doctor, Settings, Help and a first-run Onboarding. The number keys 1 to 7 switch between the main screens.

Each screen follows two rules. The bottom bar is a short recap of the keys that matter in the current context. It stays the same while you move between panes, drops the least important keys (never Tab, `?` and `q`) when the window is narrow, and is empty while a dialog is open because the dialog shows its own choices. A status message such as "Created your files" is shown for a few seconds and disappears as soon as you open a dialog or change screen. The Help screen (`?`) is the complete key reference and lists the keys of the screen you came from. The keymap is written down once, here:

| Key | Where | Action |
| --- | --- | --- |
| `1` to `7` | everywhere | Home, Learn, Exercise, Progress, Bookmarks, Doctor, Settings |
| `?` | everywhere | open Help |
| `Esc` | everywhere | go back, or close a dialog |
| `q` | everywhere | quit (asks first while a check is running) |
| `Ctrl+C` | everywhere | cancel a running check; with none running, quit |
| `Up` `Down` | lists, panes | move or scroll |
| `Enter` | lists | open the chosen item |
| `n` | Home, Exercise | open the recommended or next exercise |
| `/` | Learn | search; type to filter live, `Enter` keeps the search, `Esc` clears it |
| `f` `d` `t` `y` | Learn | cycle the status, difficulty, track and kind filters |
| `0` | Learn | clear every filter |
| `b` | Learn, Bookmarks, Exercise | bookmark or remove the bookmark |
| `Tab` | Exercise | move between Instructions, Files and Results |
| `Enter` | Exercise | create the files (first time) or edit the chosen file |
| `e` `o` | Exercise | edit the chosen file; open the workspace folder |
| `c` `x` | Exercise | run the checks; cancel a running check |
| `r` | Exercise | build and run your program |
| `w` | Exercise | turn watch on or off |
| `h` `s` | Exercise | reveal the next hint; show the solution (asks first) |
| `l` | Exercise | show or hide the raw compiler and program output |
| `R` | Exercise | reset to the starter files (backup first, asks for a typed `y`) |
| `r` `t` | Doctor | check again; check again with a real test build |

Exercise screen. The instructions, hints and explanations are rendered from Markdown: headings, wrapped paragraphs, bullet and numbered lists with hanging indents, fenced code blocks (clipped, never re-wrapped), inline code, bold and italic, pipe tables (a table that does not fit the width is shown as stacked `Header: value` lines, keeping the spaces inside code) and links shown as `text (url)`. Inline code is drawn in its own color, and in the `no-color` and `high-contrast` themes in inverse video. The sidebar shrinks to a narrow column of numbers on this screen so the instructions get the width.

Watch mode. While watch is on, a banner at the top of the exercise screen shows the watched workspace path and the live state: waiting for changes, change detected and checking, or the time of the last check (the results themselves are in the Results pane). On every other screen, including Help, the header shows `WATCHING` instead, and watch keeps running there. Watch ends when you turn it off with `w` or open a different exercise (a message says so); the banner and the indicator then disappear completely.

Running your program. `r` clears the terminal, prints a one-line header naming the program, runs it with the terminal attached, and returns to learndc after you press Enter.

Completing an exercise. When the last failing check of an exercise passes for the first time, a modal dialog titled "Exercise complete" (on whatever screen you are, when watch ran the check) shows the exercise, how many checks passed, how many attempts and hints it took, and the next recommended exercise (or "All exercises complete"). It offers to open the next exercise or stay where you are, and it stays open until you choose; the same dialog appears when a watch run is the one that passes.

Doctor. The terminal size item always shows the current size and the layout in use, and updates when you resize the terminal.

Layout follows the terminal size: wide (at least 100x30), normal (at least 80x24), compact (smaller) and a "terminal too small" message below 40x12. Resizing re-lays the screen out. Ctrl+C handling and terminal restoration are fixed: the terminal is always restored on exit, and while your editor or your program runs, the interface steps aside and takes the terminal back afterwards.

Themes (`ui.theme`): `dark` (default), `light`, `high-contrast` and `no-color`. ASCII mode (`ui.ascii`: `auto`, `on`, `off`) replaces box drawing and symbols with plain ASCII; `auto` turns it on when the locale is not UTF-8. `--ascii` forces it for one run. Colors are off when stdout is not a terminal, when `NO_COLOR` is set, with `--no-color`, or with the `no-color` theme. Reduced motion (`ui.reduced_motion`) disables the spinner shown while a check runs.

Accessibility: nothing depends on color alone (every result carries a word such as pass or FAIL), the ASCII and no-color modes need no special fonts, and there is no required animation. If a full-screen interface does not suit your screen reader, use the command line: every command prints linear, plain text, `--ascii --no-color` removes decoration, and `--json` gives structured data. The command line is a complete way to use learndc. Where the command line prints Markdown (`show`, `hint`, `solution --show`) it renders it as plain text: headings are plain lines without `#` marks, the exercise title is printed once, inline code keeps its backticks (inside table cells only when it has leading or trailing spaces), bold and italic markers are removed, code blocks are indented, and tables are aligned columns without box drawing (or `Header: value` lines when they are wider than 80 columns). Both the command line and the interface leave out the README's "Acceptance criteria" section, because the checks are listed anyway, and the solution views leave out explanation code blocks that repeat the solution files already shown.

## Command reference

    learndc [global options] <command> [arguments]

Global options, accepted by every command:

| Option | Meaning |
|---|---|
| `--home DIR` | keep all learndc data under DIR (see [Where your data lives](#where-your-data-lives)) |
| `--content-dir DIR` | load exercises from DIR instead of the built-in set (environment: `LEARNDC_CONTENT_DIR`) |
| `--no-color` | never use colors |
| `--ascii` | plain ASCII output |
| `--help` | show help (`learndc help [command]` works too) |
| `--version` | print the version and the content set, for example `learndc 0.6.0-dev (c1fd9c9)` and `content: 23 exercises (embedded)` |

Commands that take an optional `id` use the exercise of the current directory's workspace, otherwise the active exercise. Output has no ANSI sequences when stdout is not a terminal.

| Command | What it does |
|---|---|
| `learndc list [--track ID] [--status STATUS] [--difficulty LEVEL] [--kind KIND] [--json]` | List exercises. Status is `not-started`, `in-progress`, `completed` or `unfinished`; difficulty is `beginner`, `intermediate` or `advanced`. |
| `learndc search WORDS... [--json]` | Search ids, titles, summaries, concepts and tags; ranked. |
| `learndc show ID [--json]` | Instructions, checks and your status. |
| `learndc start ID [--dir PATH]` | Create the workspace, or resume it. Never overwrites: a directory that is not this exercise's workspace is refused (exit 6). Makes the exercise active. |
| `learndc next [--json]` | Recommend the next exercise (first unfinished one whose prerequisites are done). |
| `learndc edit [ID] [--editor NAME] [--folder]` | Open the entry file (`--folder`: the workspace) in your editor. `--editor` takes a preset name or a command line for this run. |
| `learndc run [ID] [-- ARGS...]` | Build and run your program with its output on your terminal, without comparing anything. Arguments after `--` go to your program. |
| `learndc check [ID] [--json] [--verbose] [--backend dpm\|llvm\|custom]` | Run the checks. `--verbose` shows full output of every check. The default backend is `dpm`; `llvm` and `custom` call dcc directly with that code generator. |
| `learndc watch [ID]` | Re-run the checks whenever a file changes. Ctrl+C ends it. |
| `learndc hint [ID] [--level N]` | Reveal the next hint, or hint N (1 to 3). |
| `learndc solution [ID] --show\|--diff` | Print the reference solution and its explanation, or a unified diff of your files against it. Either marks the solution as viewed. |
| `learndc reset ID [--yes] [--no-backup]` | Restore the starter files. See [Reset and backups](#reset-and-backups). |
| `learndc progress [--json]` | Completion by track, the active exercise and recent exercises. |
| `learndc review [--json]` | Exercises worth revisiting: completed after looking at the solution, or whose content has a newer revision than the one you completed. |
| `learndc bookmark ID [--remove]` | Bookmark an exercise (shown in `list --json` as `bookmarked`). |
| `learndc config get KEY \| set KEY VALUE \| unset KEY \| list [--json]` | Read and change settings. |
| `learndc doctor [--json] [--build]` | Check toolchain, folders, editor and terminal; `--build` also compiles a small program. |
| `learndc export --output FILE [--yes]` | Write your progress to a file. An existing file needs `--yes`. |
| `learndc import --input FILE [--apply]` | Preview, or with `--apply` merge, an export. |
| `learndc dev validate [--backend dpm\|llvm\|custom\|all] [--id ID]` | Validate content, for authors. See [the authoring guide](learndc-authoring.md). |

### Settings

`learndc config list` prints every key with its current value:

| Key | Values | Default |
|---|---|---|
| `editor.preset` | `auto`, `nvim`, `vim`, `nano`, `code`, `codium`, `notepad`, `custom`, `manual` | `auto` |
| `editor.command` | JSON array of strings, used when the preset is `custom` | empty |
| `editor.kind` | `auto`, `terminal`, `gui` | `auto` |
| `editor.wait` | `true` or `false`; make a GUI editor block until closed | `false` |
| `tools.dcc`, `tools.dpm` | full path to the program | empty |
| `ui.theme` | `dark`, `light`, `high-contrast`, `no-color` | `dark` |
| `ui.ascii` | `auto`, `on`, `off` | `auto` |
| `ui.reduced_motion` | `true` or `false` | `false` |
| `ui.open_continue` | `true` or `false` | `true` |
| `checks.timeout_seconds` | 1 to 600; limit for each run check | `10` |
| `checks.build_timeout_seconds` | build time limit | `120` |
| `checks.max_output_kib` | kept output per stream in the log | `256` |
| `checks.watch_debounce_ms` | quiet time before watch re-runs | `400` |
| `learner.level` | `new`, `experienced` | `new` |
| `onboarding.done` | `true` or `false` | `false` |

An invalid value is rejected with the allowed values (exit 2). Unknown keys are rejected.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | checks failed, or the operation failed (including a failed editor) |
| 2 | usage error: unknown option, bad value, missing argument |
| 3 | content, config or progress file error (for example `--content-dir` that does not exist) |
| 4 | toolchain problem: dcc or dpm missing or unusable (also `check` when a check ended with a toolchain error) |
| 5 | not found: unknown exercise id, no workspace, no active exercise |
| 6 | refused to avoid data loss: `reset` without `--yes` when not on a terminal, `export` over an existing file, `start` into a foreign directory |
| 130 | interrupted (Ctrl+C or SIGINT) |

## JSON output

`--json` prints one JSON document on stdout. Fields are stable; new fields may be added, so ignore unknown ones. Shapes below are real output, abbreviated where marked.

### list

`learndc list --json`:

    {
      "exercises": [
        {
          "id": "fundamentals/hello-world",
          "track": "fundamentals",
          "name": "hello-world",
          "title": "Your first DC program",
          "summary": "Greet someone by name with a formatted println.",
          "kind": "complete-program",
          "difficulty": "beginner",
          "estimated_minutes": 5,
          "revision": 1,
          "concepts": ["main", "println", "format placeholders", "Status"],
          "tags": ["output", "strings"],
          "prerequisites": [],
          "prerequisites_met": true,
          "status": "not-started",
          "bookmarked": false,
          "needs_revisit": false
        }
      ]
    }

`search --json` is the same list of entries with an extra top-level `"query"` string.

### show

`learndc show ID --json` is one exercise object with the fields above plus `chapter`, `objectives`, `entry_file`, `requires`, `platforms`, `backends`, `readme` (the instructions as Markdown), `checks`, `workspace` and `hint_level`.

### check

`learndc check --json`:

    {
      "exercise": "fundamentals/hello-world",
      "revision": 1,
      "workspace": "/path/to/workspace",
      "backend": "dpm",
      "started_at": "2026-11-15T11:23:27Z",
      "duration_ms": 31,
      "passed": 1,
      "total": 2,
      "outcome": "failed",
      "first_failure_stage": "compare",
      "results": [
        {
          "name": "Greets Ada, then welcomes her",
          "kind": "run",
          "verdict": "fail",
          "stage": "compare",
          "summary": "the output is different from what was expected",
          "expected": "Hello, Ada!\nWelcome to DC.\n",
          "observed": "Hello, world!\n",
          "expected_visible": "",
          "observed_visible": "",
          "exit_code": 0,
          "signal": null,
          "timed_out": false,
          "duration_ms": 0,
          "stdout_preview": "Hello, world!\n",
          "stderr_preview": "",
          "diagnostics": "",
          "locations": [],
          "hint": "Both lines must match exactly, ..."
        }
      ],
      "log_path": "/path/to/cache/logs/fundamentals-hello-world/last-check.log",
      "safety_note": "",
      "completed_now": false,
      "next": null
    }

`outcome` is `passed`, `failed`, `error` (for example the toolchain was missing) or `cancelled`. `verdict` is `pass`, `fail`, `error`, `skipped` or `cancelled`. `kind` is `build`, `run`, `harness` or `diagnostic`. `stage` says where a check stopped: `toolchain`, `build`, `link`, `launch`, `run`, `compare`, `harness`, `timeout`, `cancelled`, `output-limit` or `passed`. `exit_code` and `signal` are numbers or null. Each entry of `locations` has `file`, `line`, `column`, `severity` and `message`. `next` is the id of the next recommended exercise after a pass, otherwise null; `completed_now` is true on the check that completed the exercise.

### progress

`learndc progress --json`:

    {
      "total": 23,
      "completed": 0,
      "in_progress": 1,
      "not_started": 22,
      "needs_revisit": 0,
      "active_exercise": "fundamentals/hello-world",
      "recent": ["fundamentals/hello-world"],
      "bookmarks": [],
      "tracks": [
        { "id": "fundamentals", "title": "Fundamentals", "total": 8, "completed": 0, "in_progress": 1 }
      ],
      "exercises": [
        {
          "id": "fundamentals/hello-world",
          "status": "in-progress",
          "attempts": 2,
          "passes": 0,
          "hint_level": 1,
          "solution_viewed": false,
          "first_started_at": "2026-11-15T11:23:22Z",
          "last_checked_at": "2026-11-15T11:23:27Z",
          "completed_at": "",
          "completed_revision": 0,
          "last_passed": 1,
          "last_total": 2,
          "needs_revisit": false
        }
      ]
    }

`tracks` lists every track; the example shows one.

### doctor

`learndc doctor --json` (exit 4 when an item has status `error`, otherwise 0):

    {
      "items": [
        {
          "id": "dpm",
          "label": "dpm",
          "status": "ok",
          "detail": "dpm 0.6.0-dev (c1fd9c9) at /usr/local/bin/dpm (from next to learndc)",
          "fix": ""
        }
      ],
      "errors": 0,
      "warnings": 0,
      "ok": true
    }

Item ids are `dcc`, `dpm`, `dccd`, `dir.config`, `dir.data`, `dir.cache`, `content`, `editor`, `terminal.tty`, `terminal.term`, `terminal.size`, `terminal.utf8`, `terminal.color` and `safety`. `status` is `ok`, `warn`, `error` or `info`; `fix` says what to do for a problem.

### next

`learndc next --json`:

    {
      "next": { "id": "fundamentals/variables", "title": "Variables and types", "status": "not-started" },
      "all_done": false
    }

`next` holds the same exercise fields as a `list` entry (abbreviated here) and is null when everything is completed, in which case `all_done` is true.

## Editors

`learndc edit` chooses an editor by this precedence, first match wins:

1. `--editor NAME` (a preset name or a command line).
2. The settings: `editor.preset` other than `auto` (and `editor.command` for `custom`).
3. `VISUAL`, then `EDITOR`.
4. A detected editor: `nvim`, `vim`, `nano`, `code`, and `notepad` on Windows.
5. Manual: learndc prints the file to open and why no editor was chosen, and exits 0.

Presets:

| Preset | Command | Kind |
|---|---|---|
| `nvim`, `vim` | `nvim +{line} {file}` | terminal |
| `nano` | `nano +{line},{column} {file}` | terminal |
| `code`, `codium` | `code --goto {file}:{line}:{column}` | GUI |
| `notepad` | `notepad.exe {file}` | GUI, Windows |

Terminal editors run in your terminal with learndc waiting. GUI editors are started detached and learndc returns at once. VS Code returns immediately by design; set `learndc config set editor.wait true` to add `--wait` and block until you close the file. `--folder` opens the workspace folder in editors that can (VS Code); a terminal editor cannot, and says so.

### A custom editor

Set the preset to `custom` and give the command as a JSON array. Real session, with a small script that records its arguments:

    $ learndc config set editor.command '["/path/to/myed.sh","--line={line}","--col={column}","{file}"]'
    $ learndc config set editor.preset custom
    $ learndc edit fundamentals/variables
    Closed myed.sh
    $ cat myed.args
    --line=1
    --col=1
    <workspace>/src/main.dc

Rules:

- Placeholders are `{file}`, `{workspace}`, `{line}` and `{column}`. They are replaced inside single arguments only: `--goto={file}:{line}` stays one argument, however many spaces the path contains.
- The program is started directly, with no shell. Quoting for a shell, pipes and redirections do not exist. If no argument contains `{file}`, learndc appends the file as the last argument.

### VISUAL and EDITOR, and `--editor`

These are one command line, split into words like a POSIX shell (single quotes, double quotes and backslash) on Linux, and with the standard Windows command-line rules on Windows. `VISUAL="code --wait"` works. A line containing any of `| & ; < > $ ` ( )` or a newline outside quotes is rejected, as is an empty one. A rejected `VISUAL` or `EDITOR` is skipped with a warning (`learndc: warning: VISUAL ignored: the command contains the shell character ';'; ...`) and the next source is tried; a rejected `--editor` falls back to manual editing and prints the reason.

### Manual editing

You never have to use `learndc edit`. The workspace is a normal directory: `learndc start` prints its path, and any editor can open it. Then run `learndc check`.

## Checks and feedback

`learndc check` runs these stages for each check of the exercise:

1. toolchain: dcc and dpm are found.
2. build: your workspace is synchronized into a staging copy under the cache directory and built there with `dpm build --offline`. Unchanged files keep their time stamps, so rebuilds are incremental. Your workspace never receives build output; `.dpm/`, `.learndc/` and editor temporary files are not copied.
3. launch and run: learndc starts the built program itself, with a time limit, an output cap and its own process group, so a cancel or timeout reaches your program.
4. compare: stdout (and for some checks stderr or the exit status) is compared with the expectation.
5. harness checks additionally build a small trusted test program that imports your modules and prints what it compared.

A check kind is one of `build` (must compile), `run` (run the program and compare), `harness` (run a test program against your code) or `diagnostic` (compile and look at the diagnostics). What is compared is stated in the exercise (`learndc show ID`): exact, `contains` or trimmed output; line endings are normalized by default so CRLF versus LF never fails a check. Where output differs, the expected and observed text are shown, and `--verbose` or `--json` adds visible-whitespace versions.

Compiler diagnostics are shown with paths rewritten to your workspace (`at src/main.dc:6:5 error: ...`) and with terminal control sequences stripped.

The full output of the last check of each exercise is kept in `<cache>/logs/<track>-<name>/last-check.log` (up to `checks.max_output_kib` per stream). The path is printed after each failed check.

Only one check runs at a time per exercise; a second one reports that another check is running.

## Safety

Checks compile and run your code with your permissions. learndc builds the exercise and executes the result as you, like any program you run from a terminal. Time limits, output limits and cleanup of the process group stop runaway programs; they are not a sandbox. A program could read or delete your files. Only run exercises and code you trust, and read code you copied from elsewhere before you check it.

The tests and solutions of the bundled exercises are visible, not secret. They are plain files in the source tree and in the executable; learndc does not hide them, and `learndc solution` is there on purpose.

Exercise content cannot name a program to run: check kinds are a fixed set interpreted by learndc. A content directory you load with `--content-dir` is still code you are asked to compile and run (starter and solution files), so treat third-party content as you treat any downloaded source. See the [authoring guide](learndc-authoring.md#trust-model).

## Where your data lives

| | Linux | Windows |
|---|---|---|
| settings | `$XDG_CONFIG_HOME/learndc/config.json` (default `~/.config/learndc`) | `%APPDATA%\learndc\config.json` |
| progress, workspaces, backups | `$XDG_DATA_HOME/learndc/` (default `~/.local/share/learndc`): `progress.json`, `workspaces/`, `backups/` | `%LOCALAPPDATA%\learndc\` |
| staging copies, logs | `$XDG_CACHE_HOME/learndc/` (default `~/.cache/learndc`): `staging/`, `logs/` | `%LOCALAPPDATA%\learndc\cache` |

`LEARNDC_HOME=DIR` or `--home DIR` puts everything under one directory: `DIR/config.json`, `DIR/progress.json`, `DIR/workspaces`, `DIR/backups`, `DIR/cache`. Use it for a second profile, a USB stick, a classroom machine or tests. The path may contain spaces and non-ASCII characters.

Writes are atomic and guarded by a lock file (`progress.lock`), and the previous progress file is kept as `progress.json.bak`. A progress file that cannot be read is moved aside as `progress.json.corrupt-<time>`, learndc starts empty and tells you. Workspaces are `workspaces/<track>/<name>`; each has a `.learndc/workspace.json` that records the exercise and revision. Deleting a workspace directory is safe; `learndc start` creates it again.

## Progress export and import

    learndc export --output progress.json
    learndc import --input progress.json
    learndc import --input progress.json --apply

The export is a JSON file (`"format": "learndc-progress"`, `"schema": 1`, the time, and the progress record). `import` without `--apply` only prints what would change, one line per field. With `--apply` it merges:

- completed wins over in-progress and not-started;
- counters (attempts, passes, hint level) take the larger value;
- the earliest `completed_at` is kept;
- bookmarks are united;
- the active exercise stays the current one;
- on a conflict your current value is kept, and the number of conflicts is reported.

Importing the same file twice changes nothing the second time. Workspaces and your code are not part of an export.

## Reset and backups

`learndc reset ID` restores the starter files. If your files differ from the starter, it asks on a terminal; elsewhere it exits with 6 until you pass `--yes`. Before anything is changed, every file of yours is copied to `backups/<track>/<name>/<time>/`, and learner-created files are removed only after the backup succeeded:

    $ learndc reset fundamentals/hello-world --yes
    Reset fundamentals/hello-world in <workspace>
    Backup of your files: <data>/backups/fundamentals/hello-world/20261115T112338Z
    Restored 3 starter file(s)

`--no-backup` skips the backup.

## Content revisions

Each exercise has a `revision`. It goes up when the starter or the checks change. Your completion is never lost: an exercise you completed at an older revision stays completed and is listed by `learndc review` and as `needs_revisit` in the JSON output. Your existing workspace is not modified when content changes; to try a new revision, start it again.

## Watch mode

`learndc watch [ID]` runs the checks once, then again after every save. Files are polled; changes in `.dpm/`, `.learndc/`, swap files, `*~`, `.#*`, `4913` and `*.tmp` are ignored, so a save through an atomic rename in your editor is noticed and editor temporary files are not. Saves in quick succession are combined (`checks.watch_debounce_ms`). Ctrl+C stops it, after cancelling a check in progress. It prints `Watch stopped.`.

## Troubleshooting

Start with `learndc doctor`.

- dcc or dpm missing: `learndc: dpm was not found. Searched: ...` lists every place tried, and exit status is 4. Put them on `PATH`, next to learndc, or set `tools.dpm` / `LEARNDC_DPM` (and the dcc equivalents). An environment variable that points at a non-executable is reported and the next location is tried.
- An unexpected old dcc: `doctor` prints the path and version of the dcc it found. Exercises require dcc 0.6.0 or newer.
- Editor not found or not opening: `learndc doctor` shows what was chosen and why. A terminal editor started from a non-terminal fails (`Standard output is not a terminal`, exit 1). Use `--editor`, set `editor.preset`, or edit the files yourself (manual editing).
- Terminal too small: widen to at least 80x24 for the normal layout. The commands work in any size.
- Colors look wrong, or you want none: `--no-color`, `NO_COLOR=1` or `learndc config set ui.theme no-color`. Compiler diagnostics shown inside check results are re-rendered without the compiler's escape sequences. For symbols that render badly, `--ascii` or `learndc config set ui.ascii on`.
- A check says another check is running: wait for it, or end the other `learndc watch`.
- A program works with `learndc run` but fails `check`: compare the expected and observed text; trailing spaces are shown with `--verbose`.

## Platform status

- Linux: tested natively, including the end-to-end journeys in `tests/learndc/e2e.py` (start, failing and passing checks, progress, export and import, reset with backup, paths with spaces and Unicode).
- Windows: learndc is cross-compiled from Linux and packaged in the MSI. It has not been validated natively: not the commands, not the editor handoff, not process cleanup, not the paths, not the installer's shortcut and `PATH` handling. The release workflow runs `ci/release/smoke-msi-windows.ps1` (install, custom directory, repair, `ADDTOPATH=0`, uninstall) and `tests/learndc/windows-smoke.ps1` on a Windows runner; neither has run yet. Treat Windows as unverified until they pass.
