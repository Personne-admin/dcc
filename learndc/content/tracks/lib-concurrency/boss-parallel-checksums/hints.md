## Hint 1

Work one stage at a time. `summarize` needs no threads at all: get it right and testable first. Then the lock, then the pool, then the report.

## Hint 2

Task pointers handed to a pool must stay valid until `wait_all` returns. A `Task` array that you resize once and never push into again keeps its addresses; one that grows while you are submitting does not.

## Hint 3

`analyze_task` does exactly two things with its job: store the summary and mark the job finished. `analyze_all` creates the pool, `defer`s its destroy, submits every task and calls `wait_all`. `main` prints only after `analyze_all` returned.
