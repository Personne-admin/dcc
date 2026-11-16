# Shrink the public surface

*Continues library-with-tests: the same stats library, now with a wider API than it should have.*

`stats::summary` publishes five functions. Users asked for two: `mean` and
`max`. The other three (`sum_of`, `count_of`, `is_empty_set`) are steps `mean`
and `max` use internally. They are public only because somebody wrote
`public` on everything, and each one is now a promise.

`src/main.dc` is the course's audit; leave it and `src/audit.dc` alone.

## Your task

Edit `libs/stats/src/stats/summary.dc` so that exactly `mean` and `max` are
public. Everything else must keep working: the audit calls both functions
as an application would.

## Acceptance criteria

- `dpm run audit` prints six `ok` lines and `6 checks, 0 failed`.
