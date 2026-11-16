# The Library Smith

*A new project that uses everything from the DPM path: products, a path dependency, a library with tests, a README and a version.*

The harbor town's shipwrights keep copying one table printer between their
programs. You are going to forge it once, properly: a reusable library
package `tabulate`, with a small public surface, a manifest and README another
yard could read, and tests of its own. The application that uses it,
`smith`, is already written and lives in the workspace root.

```text
project.dcon                  smith: dependencies = { path:libs/tabulate }
src/main.dc                   the application and the course's audit (leave it)
src/audit.dc                  the audit helper (leave it)
libs/tabulate/project.dcon    the library manifest (yours)
libs/tabulate/src/tabulate/   cell.dc, table.dc, stats.dc (stubs: implement them)
```

`smith` reads one command from standard input and runs it. Try a command in
your workspace:

```text
echo table | dpm run smith
```

## The library

`tabulate::cell`

| Function | Behaviour |
|---|---|
| `usize display_width([]const char text)` | the number of characters |
| `Status(Error) print_left(text, width)` | prints the text, then spaces up to the width |
| `Status(Error) print_right(text, width)` | prints spaces up to the width, then the text |

Text longer than the width is printed whole. A helper `spaces` prints a number
of blanks; it is an implementation detail.

`tabulate::table` takes `headers` and `cells`, both `[]const []const char`.
`cells` is the table body in row-major order, so with three headers cell `i`
belongs to column `i % 3`.

| Function | Behaviour |
|---|---|
| `usize column_width(headers, cells, column)` | the widest of the header and the cells in that column |
| `Status(Error) print_table(headers, cells)` | the header row, a rule of dashes, then every row |

Column 0 is left-aligned, the others right-aligned, columns are separated by
one space and every column is padded to its width. The rule uses `-` for each
character of the width.

```text
ship   tons crew
------ ---- ----
Aurora  120   14
Brisa    80    9
```

`tabulate::stats` works on `[]const i32`: `total` (as `i64`), `min`, `max`,
`mean` (as `f64`) and `range` (`max` minus `min`). An empty slice gives 0.

## The stages

1. Review the legacy library (questions).
2. Cells: implement `tabulate::cell`.
3. The table: implement `tabulate::table`.
4. Statistics: implement `tabulate::stats` except `range`.
5. Seal the helpers: exactly the functions above are public.
6. Package: manifest metadata, a pre-1.0 version, a README.
7. Tests: a test program in the library's `tests/` directory.
8. Bonus: implement `range` and release version 0.1.1 with a changelog.

Each stage has its own check. The package and test stages are checked by the
audit commands built into `smith` (`echo package | dpm run smith`); they list
what they found, `FAIL` lines first. Use `dpm test` inside `libs/tabulate` for
your own tests.
