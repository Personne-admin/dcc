## Hint 1

Start with the lowest layer you have to write, `store::add`, then `load`
(which uses `add`), then `app::run` (which uses `load`) and the two edge
functions. Each layer only talks to the one below it.

## Hint 2

`add` loops over `tickets.items()` and compares ids. `load` walks the lines with
a counter: `Result(Ticket, ParseError) parsed = parse::parse_ticket(line);`
`if parsed.is_err() { ... }`. In `run`, `store::load(tickets, lines)?;` is
all the conversion needs, because `AppError::Store` is the implicit variant.
`exit_code` and `explain` are nested `match` statements: the outer one on the
`AppError`, the inner on the `StoreError`, the innermost on the `ParseError`.

## Hint 3

```
LineProblem problem = { line = number, cause = parsed.unwrap_err() };
StoreError failure = StoreError::Bad(problem);
return failure;
```
and in `add`:
```
StoreError failure = StoreError::Duplicate(ticket.id);
return failure;
```
