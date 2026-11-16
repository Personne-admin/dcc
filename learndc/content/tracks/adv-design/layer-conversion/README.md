# Three layers of errors

A ticket importer reads lines of the form `id:priority:title` and keeps the
tickets in an `Array`. The program has three layers, each with its own errors:

| module | layer | error type | what it knows |
|---|---|---|---|
| `parse.dc` | one line | `ParseError` | a field is missing, the id or priority is wrong, the title is empty |
| `store.dc` | many lines | `StoreError` | memory ran out, an id appears twice, a line was bad (with its line number) |
| `app.dc` | the program | `AppError` | a store failure, or no input at all |

`parse.dc` is finished. The enums are declared; the bodies are placeholders.
Your work:

- `store::add(tickets, ticket)`: if a ticket with the same id is already in
  the array, fail with `StoreError::Duplicate(id)`; otherwise push it. A failed
  push must propagate its `AllocError` (it converts into `StoreError::Memory`
  with `?`).
- `store::load(tickets, lines)`: for every line, starting at line number 1,
  parse it; a parse failure becomes `StoreError::Bad(problem)` where `problem`
  holds the line number and the `ParseError`; a good ticket goes through
  `add`, whose error propagates with `?`. Loading stops at the first failure
  and keeps the tickets stored before it.
- `app::run(tickets, lines)`: fail with `AppError::NothingToLoad` when there are
  no lines; otherwise `store::load(...)?` converts a `StoreError` into an
  `AppError`.
- `app::exit_code(failure)`: bad line 2, duplicate 3, nothing to load 4,
  allocation failure 70.
- `app::explain(failure)`: print one line per error, as in the output below.

Payload variants in a `Status(E)` function: build the variant in a local
variable (`StoreError failure = StoreError::Duplicate(id);`) and return the
variable.

`main.dc` runs four scenarios and must stay unchanged. It prints:

```text
tickets
good: loaded 3 tickets, code 0
duplicate: ticket 2 appears twice
  code 3
bad line: line 3: priority must be 1 to 5
  code 2
empty: nothing to load
  code 4
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The output above matches exactly and the exit code is 0.
- The harness checks every parse cause, the line numbers, duplicates, the
  early stop and each exit code.
