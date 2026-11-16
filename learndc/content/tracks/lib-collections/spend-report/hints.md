## Hint 1

Counting per key is one line with `get_or_put`: `u32* sum = totals.get_or_put(order.customer, 0)?;` followed by `*sum += order.cents;`.

## Hint 2

For the keys, loop `match it.next()`: `Optional::Some(entry)` pushes `*entry.key` (the key is a pointer inside the entry), `Optional::None` stops the loop. Sort after the loop, not inside it.

## Hint 3

`best_customer`: start with `Optional(u32) best = Optional::None;` and `u32 best_total = 0;`. For every `id` read `totals.get(id).some_or(0)`; when `best.is_none() || total > best_total`, set `best = Optional::Some(id);` and `best_total = total;`.
