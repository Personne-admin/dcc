# Why this ledger can be trusted

Every mutating function has the same two phases. The first phase only reads:
it checks the amount, looks up the accounts, compares the balance, and returns
a typed error at the first problem. The second phase only writes and cannot
fail for business reasons. A refused operation therefore never touches a
balance or the journal, which is exactly what the harness verifies after each
batch of refusals, and it is why the old transfer was wrong: it wrote before it
knew the target existed.

The fixed order of the checks is part of the specification, not a detail.
When two things are wrong at once (an unknown name and an overdraft) the
caller must get one predictable answer, and tests can pin it down.

Two arrays with different roles keep the data simple. The accounts hold the
current truth, the journal holds the history, and `statement` rebuilds each
running balance by replaying the history. Because the closing balance is read
from the account itself, a mismatch with the replayed total would expose a bug
in one of the writers. `main` owns both arrays with `defer ...deinit()` and
lends them out; no ledger function owns or frees anything.

`@implicit_construction Alloc(AllocError)` lets `push(...)?` report running out
of memory through the same error type as every business refusal, so callers
have a single `match` to write. Running out of memory is a different kind of
failure from a refusal: a journal push that fails between the two entries of a
transfer could still leave a half-written journal. Real systems reserve space
for both entries first.
