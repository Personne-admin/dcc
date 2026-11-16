## Hint 1

Each function is small because the structures do the work. The heap orders the
future, the log (an `Array(Event)` that only grows in time order) is the past.
Think of `run_until` as a loop of three steps: peek, pop, re-schedule.

## Hint 2

The order of equal times comes from `seq`, so every event, including the
re-scheduled ones, must get its own number from `schedule`. The log is sorted
by time because events fire in time order, which is what makes `count_between`
a binary search on the whole log. For the kinds, a stable sort regroups the
log without disturbing the time order inside a kind.

## Hint 3

```
public Status(AllocError) schedule(Array(Event)* queue, u32* next_seq, u32 time, u32 kind, u32 period) {
    Event event = { time = time, seq = *next_seq, kind = kind, period = period };
    Status(AllocError) added = heap::heap_push(queue, event, events::by_time_then_seq);
    if !added.is_ok() {
        return added;
    }
    *next_seq += 1;
    return Status::Ok;
}
```
