# Why the solution works

```dc
Status(Error) add_stock(Stock* stock, u32 part, u32 qty) {
    Optional(u32*) slot = stock.get_ptr(part);
    if slot.is_some() {
        u32* count = slot.unwrap_some();
        *count = *count + qty;
    } else {
        stock.put(part, qty)?;
    }
    return Status::Ok;
}
```

- `get` returns a copy of the value, so changing the copy would not change the
  map. `get_ptr` returns a pointer to the stored value, which is what an
  in-place update needs. The pointer is used before any `put`, so it cannot be
  invalidated by growth.
- Calling `put` for an existing key would replace the count with `qty`; that
  is why `add_stock` looks the key up first and only calls `put` for a new
  part. `put` can fail to allocate, so its `Result` is propagated with `?`.
- `quantity` uses `get(part).some_or(0)`: a missing key becomes 0 without a
  branch, and the map is only read, so it takes `const Stock*`.
- `take` checks both failure cases (`None` and too few in stock) before it
  modifies anything, so a refused request leaves the map unchanged. That is
  why `part 202` still has 4 after the failed take of 9.
- `main` owns the map: `defer stock.deinit();` releases its memory when `main`
  returns, whether it returns normally or early through `?`.
