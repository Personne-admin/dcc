## Hint 1

`Array` has methods for every step: `reserve(n)?`, `push(v)?`, `insert(index, v)?`, `remove(index)` and the read-only `len()` and `capacity()`.

## Hint 2

`cancel` first asks `std::slice::index_of(queue.items(), number)`. When the answer `is_none()`, return `false`; otherwise remove that index and return `true`. `serve` is `remove(0)`: it already returns an `Optional(u32)`.

## Hint 3

`open_queue`: `queue.reserve(expected)?; return Status::Ok;`. `vip`: `queue.insert(0, number)?;`. `spare`: `return queue.capacity() - queue.len();`. In `cancel`, `queue.remove(at.unwrap_some());` after the check.
