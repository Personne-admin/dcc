# Why this works

The scheduler is four data structures, each answering one question cheaply.

The **heap** answers "what fires next?" in log n and accepts new events in
log n. The old scheduler re-sorted the whole array before every step, which
costs n log n per step; the heap never orders more than it has to. The
comparator is `time` first and `seq` second. Because `seq` is unique, no two
events compare equal, so the order is deterministic even though a heap is not
stable by itself. The sync at time 20 fires before the tick at time 20
because its successor was scheduled first, not because of any special rule.

The **log** is an `Array` that only appends, and since events fire in time
order it is always sorted by time. That makes it searchable. `count_between`
does not scan: `lower_bound_by` finds the first event not before `from`,
`upper_bound_by` the first event after `to`, and their difference is the
count. The old query's off-by-one in the first stage is precisely the
difference between those two functions: `<` against `<=` in the same loop.

The **Map** counts by kind in constant time per event, which is the
operation it is made for (a key you look up, not an order you walk). The
**stable sort** regroups the log by kind. Stability is what keeps each group
in time order: sorting by kind with an unstable sort would produce the right
groups but could shuffle the events inside each group, and a report that says
"the ticks, in order" would be wrong.

Every function that can allocate returns its error and does the fallible step
before the state change it describes: `schedule` bumps `next_seq` only after
the push succeeded, so a failed schedule leaves the counter unchanged and a
later success reuses the number.
