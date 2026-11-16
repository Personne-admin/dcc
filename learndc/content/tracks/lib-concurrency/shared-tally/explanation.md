# Why defer belongs right after lock

`value += 1` reads, adds and writes. Two threads that read the same old value
both write old + 1: one increment is gone. With a thousand increments the loss
is rare; with hundreds of thousands it happens every run, which is why the
exercise is large enough to show it. The mutex turns the three steps into one
indivisible region.

`defer tally.lock.unlock();` is the same idea as `defer array.deinit();`: a
release written next to the acquisition, executed on every way out. `try_add`
has two exits and `words_so_far` returns a value; neither needs a manual
unlock before each `return`, and a later edit that adds a third exit cannot
break the pairing.

The refusal logic is the second lesson: a *check followed by an update* is one
operation, so it needs one lock. Locking only the update would let two threads
both pass the check and both add, overshooting the limit.

A last caution: this program can only show that it is wrong sometimes. A test
that passes does not prove the absence of a race, because the schedule may
simply have been kind. The protection comes from always following the rule
"every access to shared data holds the lock", not from running the program
many times.
