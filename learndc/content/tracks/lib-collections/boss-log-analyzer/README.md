# The Log Analyzer

A service writes one line per event:

```text
LEVEL source millis message words...
```

- `LEVEL` is `INFO`, `WARN` or `ERROR`.
- `source` is the component that wrote the line (`auth`, `db`, `web`, ...).
- `millis` is how long the operation took, in whole milliseconds (a `u32`).
- The message is free text, and may be missing. It must be valid UTF-8.

Empty lines are ignored. Any other line that does not fit is **malformed**,
and the report names it by its **line number** (the first line is line 1,
empty lines count) and by the reason:

| Reason | When |
| --- | --- |
| `MissingField` | the source or the duration is missing |
| `BadLevel` | the level is none of the three |
| `BadNumber` | the duration is not made only of digits, or does not fit a `u32` |
| `BadText` | the message is not valid UTF-8 |

The check order is the order of that table.

## The project

- `src/log_data.dc` holds the 17-line log text as `log_data::TEXT`. Do not
  change it.
- `src/analyzer.dc` declares the types and has a stub for every stage. A
  comment names the stage. `take_field` is finished.
- `src/main.dc` prints the report. Its `say_problem` is finished.

## Stages

1. **Read the old parser.** Questions only: find the bug in the previous
   on-call's code.
2. **parse_line.** One line into an `Entry` or a `LineError`.
3. **read_log.** The whole text: entries in one `Array`, problems (line number
   and error) in another.
4. **tally.** `count_level`, and `top_sources`: the `limit` sources with the
   most entries, highest first. Sources with the same count stay in the order
   in which they first appeared in the log.
5. **latency.** `latency_buckets`: a `Map(u32, u32)` counts entries per 100 ms
   bucket (`millis / 100 * 100`); the result is sorted by bucket start.
6. **report.** `main` prints everything.
7. **Bonus: fingerprint.** The first 12 hex characters of the SHA-256 digest
   of the log text.

Text cannot be a `Map` key here, so sources are counted in an `Array` that you
search. Never print or return anything in map iteration order.

Every stage is checked on its own, so work on them one by one.

## The report

```text
13 entries, 3 malformed
levels:
  INFO  8
  WARN  3
  ERROR 2
busiest sources:
  web   5
  db    4
  auth  3
latency:
    0-99  8
  100-199 1
  200-299 1
  300-399 1
  500-599 1
  900-999 1
malformed lines:
  line 7: unknown level
  line 10: missing field
  line 14: bad duration
```

- The first line counts parsed entries and malformed lines.
- Levels are listed INFO, WARN, ERROR.
- Sources are the top three, the name padded to 6 columns. The three sources
  with the most entries are `web` (5), `db` (4), `auth` (3); `cache` has one.
- A latency bucket prints its first and last millisecond, `start` and
  `start + 99`, right and left aligned in 3 columns, then the count. Empty
  buckets are not printed.
- Malformed lines come last, in line order.

## Acceptance criteria

- The project builds with `dpm build`.
- Every stage's check passes; the report matches exactly.
- Every array is released with `defer` right after its declaration.
