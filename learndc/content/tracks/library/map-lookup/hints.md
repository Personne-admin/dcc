## Hint 1

A `Map` answers "is this key present?" through `Optional`: `get` and
`get_ptr` return `None` for a missing key. Use `is_some()` to branch and
`unwrap_some()` to read the value.

## Hint 2

`quantity` is a single expression: `get(part)` followed by `some_or(0)`.
For `add_stock` and `take`, call `get_ptr(part)` first. If the part exists,
change the count through the pointer (`*count = *count + qty;`). In
`add_stock`, when the part does not exist, insert it with `put`.
In `take`, check `*count < qty` before subtracting.

## Hint 3

In `add_stock`: `Optional(u32*) slot = stock.get_ptr(part);`, then an
`if slot.is_some()` branch that does `u32* count = slot.unwrap_some();
*count = *count + qty;` and an `else` branch with `stock.put(part, qty)?;`.
In `take`, return `false` if `slot.is_none()` or `*count < qty`; otherwise
subtract and return `true`.
