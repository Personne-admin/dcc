# One rule, applied everywhere

The bridge applies one rule at every C call: convert on the way in, check
everything on the way out. C reports trouble in many dialects. `strtol`
answers with a value plus an end pointer plus `errno`; `fopen` with a null
pointer plus `errno`; `sqrt` with NaN; `getenv` with null. If the rest of the
program had to know each dialect, every caller would repeat the checks and
somebody would forget one. `native.dc` is the only place that knows them, and
it speaks one dialect outward: `Result` and typed error enums.

**errno is shared state.** It must be cleared before the call whose result you
inspect, and read before any other call that might set it, including calls in
your own helper functions. Reading it inside the same function that made the
call keeps that simple.

**Order of the parse checks.** `end == start` must come before the `ERANGE`
test only because a call that consumed nothing says nothing about range; and
the terminator test comes last because it only makes sense for a number that
was found and fits.

**defer for foreign resources.** C does not close the `FILE` for you, and a
function with several returns will, sooner or later, forget one. `defer
fclose(file)` right after the successful open makes every path correct,
including the ones added next year. The harness proves it by opening and
failing 1500 times: a single leaked handle per call would exhaust the
process's descriptors long before.

**Domains are yours to check.** Math functions signal domain errors with NaN
and `errno`, which is easy to miss. Checking the argument before the call turns
"a surprising value flows on silently" into a visible `Err`.

**Views of C memory.** `env_or` returns a slice into the environment block, so
it is only valid until the environment changes. That is acceptable for a
quick lookup and wrong for something you keep; copy it if you need to.
