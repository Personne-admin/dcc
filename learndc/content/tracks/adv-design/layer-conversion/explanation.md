# Why this works

Each function in the importer says one thing about failure. `parse_ticket`
knows what a bad line looks like but not which line it was. `load` knows the
position and adds it: the `LineProblem` is exactly the context that the
parser could not have. `run` knows that no input at all is a failure of the
program, not of the store. None of them knows what an exit code is: that
belongs to `exit_code` at the edge, so changing the numbers touches one
function.

The `?` operator does the boring conversions. In `add`, a failed `push`
returns an `AllocError` that becomes `StoreError::Memory` because that
variant is marked implicit. In `run`, a `StoreError` becomes
`AppError::Store` for the same reason. Conversions with meaning, the
duplicate id and the line number, are written by hand, because they add
information.

The nested `match` in `explain` and `exit_code` is exhaustive at each level.
If someone adds `ParseError::TitleTooLong`, the compiler points at both
functions until each decides what to print and which code to return. That is
the practical benefit of closed error enums: the error vocabulary and its
handling cannot drift apart silently.

Stopping at the first bad line and keeping the earlier tickets is a design
decision, not an accident. The alternatives (all or nothing, collect every
error) are different contracts, and the callers and tests must agree on one.
Here it is written in the contract and checked by the harness.
