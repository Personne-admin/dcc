# The Event Scheduler

A discrete-event simulation keeps a queue of future events and repeatedly
fires the earliest one. You are building the engine: a priority queue driven
by the heap you wrote, a log of everything that fired, and a report. Work stage
by stage; every stage has its own checks and hints.

## What is provided

- `src/events.dc`: the `Event` struct (`time`, `seq`, `kind`, `period`), the
  kinds `KIND_TICK`, `KIND_ALARM` and `KIND_SYNC`, and three comparators:
  `by_time_then_seq` (the queue order), `by_time` (for searching the time-ordered
  log) and `by_kind` (for regrouping).
- `src/heap.dc`: `heap_push`, `heap_pop`, `heap_peek` and `is_heap`, the
  generic min-heap of the previous lesson, finished. Use it.
- `src/main.dc`: the scenario and the report. It must stay unchanged.

An event with `period > 0` repeats: after it fires, the next occurrence is
scheduled `period` time units later with the same kind and period. `period == 0`
is a one-shot event.

## What you write in `src/sched.dc`

- `schedule(queue, next_seq, time, kind, period)`: build an event whose `seq`
  is the current `*next_seq`, push it on the heap, and increase `*next_seq`
  only when the push worked. The sequence number is the tiebreaker: events at
  the same time fire in the order they were scheduled.
- `run_until(queue, next_seq, limit, log)`: while the front event is due at or
  before `limit`, pop it, append it to `log`, and if it is periodic schedule its
  next occurrence. Events later than `limit` stay queued.
- `count_between(log, from, to)`: how many events of the time-ordered log
  have `from <= time <= to`, in logarithmic time, using
  `std::sort::lower_bound_by` and `upper_bound_by`.
- `count_kinds(log, counts)`: for every event add one to `counts` under its
  kind (a `Map(u32, u32)`); counts already in the map are kept.
- `sort_by_kind(log)`: regroup the log by kind with a **stable** sort, so
  events of one kind stay in time order. `std::sort::sort_stable_alloc` with
  `std::os::heap::Heap` as allocator does the work.
- Bonus `cancel_kind(queue, kind)`: remove every queued event of that kind,
  keep the queue a valid heap and return the number removed.

All status-returning functions return the first failing `Status(AllocError)`
and must not lose events when an allocation fails.

## Expected report

```text
event scheduler
scheduled 4
fired 9 events, 2 still waiting
  t=5 sync
  t=10 tick
  t=20 sync
  t=20 tick
  t=25 alarm
  t=30 alarm
  t=30 tick
  t=35 sync
  ...
events between 20 and 30: 5
events between 41 and 90: 0
ticks 4, alarms 2, syncs 3
by kind:
  t=10 tick
  t=20 tick
  t=30 tick
  t=40 tick
  ...
```

## Acceptance criteria

- The project builds with `dpm build`.
- The report above matches exactly and the exit code is 0.
- Every stage's checks pass; the bonus stage is optional.
