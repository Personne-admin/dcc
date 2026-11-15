# Track stock with a map

`std::map::Map(K, V)` stores values by key. It is not in the automatic
prelude, so import it explicitly, and it owns heap memory, so release it with
`defer map.deinit();`. A `{}` initializer makes an empty map.

```dc
import std::map;

using Counts = std::map::Map(u32, u32);

Counts counts = {};
defer counts.deinit();

counts.put(7, 1)?;                       // insert or replace; can fail to allocate
Optional(u32) one = counts.get(7);       // a copy of the value, or None
u32 zero = counts.get(8).some_or(0);     // 0 when the key is missing
Optional(u32*) slot = counts.get_ptr(7); // a pointer into the map, or None
```

`get_ptr` lets you change a stored value in place by writing through the
pointer: `*slot.unwrap_some() = 2;`. The pointer is only valid until the next
`put` that grows the map, so use it right away.

## Your task

`src/main.dc` keeps spare-part counts in a `Stock` map (part number to
quantity). The three helper functions are stubs; `main` is finished.
Implement:

1. `add_stock(stock, part, qty)` adds `qty` to the part's count, inserting
   the part when it is not stored yet.
2. `quantity(stock, part)` returns the stored count, or 0 for an unknown part.
3. `take(stock, part, qty)` removes `qty` from the count and returns `true`,
   or returns `false` and changes nothing when the part is unknown or has
   fewer than `qty` in stock.

Do not change `main`. The program must print:

```text
part 101: 15
part 202: 4
part 303: 0
take 12 of 101: true
take 9 of 202: false
part 101: 3
part 202: 4
parts tracked: 2
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches the eight lines above exactly and the exit code is 0.
- `add_stock` propagates a failed insert with `?` instead of ignoring it.
