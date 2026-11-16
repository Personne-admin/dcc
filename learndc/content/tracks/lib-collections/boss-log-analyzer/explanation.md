# Why this analyzer is trustworthy

**Parsing is classification.** `parse_line` does not just succeed or fail:
it names the first reason a line is unusable, in a fixed order, so the same
bad line always produces the same message. The duration check combines the
two properties of `std::parse::accumulate_digits` that the old parser missed:
the digits must cover the whole field (cursor at the end), and the `u64`
result must fit the `u32` the entry stores.

**Failures are data.** `read_log` never stops at a bad line. It records a
`Problem` with the line number and goes on, so one corrupt line cannot hide
the other sixteen. The line counter increments for every line, empty ones
too, because humans find lines in an editor by number.

**Order is decided, never inherited.** Two places could have leaked
unspecified order into the report. Sources are counted in an `Array` and
ranked with a *stable* sort, so ties follow the log, not the sort algorithm.
Latency buckets live in a `Map` for cheap counting, but the output order
comes from sorting the keys first. The report is therefore identical on every
run, every backend and every library version, which is what makes it
checkable with exact expected output.

**Ownership is boring on purpose.** `main` owns two arrays and releases them
with `defer`; every function only borrows. Each `Entry` borrows its source
from the log text, which is a program-lifetime constant, so no string is ever
copied or freed.
