## Hint 1

Write each function in the same shape: validate with early returns, then change
the state. If you find yourself undoing a change after a later check fails, the
order is wrong.

## Hint 2

Index math is the usual trap: `Entry.account` and the result of `find` are the
same index into `accounts.items()`. Keep that index in a local and use it for the
balance, the journal entry and the statement filter.

## Hint 3

Use the harness lines to find the first failing case. They are ordered like the
validation order in the README, so a FAIL on a refusal usually means two checks
are the wrong way round.
