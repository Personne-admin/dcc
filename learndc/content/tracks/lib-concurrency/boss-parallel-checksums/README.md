# The Parallel Checksummer

`src/corpus.dc` holds seven documents (`corpus::DOCUMENTS`, a name and a text
each). `src/scanner.dc` declares the types and a stub for every stage.
`src/main.dc` prints the report. Everything the tool needs was in this
chapter: result slots, a lock around shared data, a thread pool, ordered output,
and (for the bonus) a child process with pipes.

A `Summary` has three numbers:

- `words`: runs of characters other than space, newline and tab.
- `lines`: the newline characters, plus one more when the text ends in the
  middle of a line (so `"a b\nc"` has 2 lines and `"one\n"` has 1).
- `checksum`: the 64-bit FNV-1a hash of the text bytes
  (`std::hash::fnv1a_64`).

A `Job` is the unit of work: name, text and a `summary` slot that
`analyze_all` fills. A `Progress` is a counter with a `Mutex` that every task
bumps when it is done.

## Stages

1. **Read the old script.** Two questions about the previous tool: a data race
   and a missing wait.
2. **summarize.** The pure function, no threads.
3. **progress.** `mark_finished` and `finished_count` with the lock.
4. **analyze-all.** A `ThreadPool` with a task per job. The checks run it with
   3 workers and with 1 and expect the same summaries.
5. **busiest.** The job with the most words, the first one on a tie.
6. **report.** `main` builds the jobs, runs `analyze_all` on 3 workers and
   prints:

```text
7 documents, 7 analysed
readme   11 words  2 lines  f2fcd8456eb8d1fa
notes    14 words  5 lines  fbca995ffaf35517
empty     0 words  0 lines  cbf29ce484222325
haiku    13 words  3 lines  3011af2dc5e7ce0d
single    1 words  1 lines  1a08aa1921ca5caf
spaces    5 words  3 lines  f482e58d1a397146
poem     18 words  4 lines  127ee44af3cc1af1
total: 62 words, 18 lines
busiest: poem
```

   The name is padded to 8 columns; words and lines are right-aligned in 3
   columns; the checksum has 16 hex digits.
7. **Bonus: ask wc.** `newlines_by_wc` runs the system program `wc -l` as a
   child process, feeds it the text on its standard input and reads the number
   it prints. This needs two pipes and the closing order from the capture
   lesson: your copies of the child's pipe ends must be closed right after the
   spawn, and the stdin writer must be closed before you wait for the answer.

Every stage is checked on its own. Because every task writes only to its own
job and the report is printed after `wait_all`, the output is identical on
every run.

## Acceptance criteria

- The project builds with `dpm build`.
- Every stage's check passes; the report matches exactly.
- The pool is destroyed on every path out of `analyze_all`, and every
  `lock()` is paired with a `defer unlock()`.
