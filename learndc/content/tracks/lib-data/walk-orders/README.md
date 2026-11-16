# Total the orders

A shop exports its orders as JSON: an object with an `orders` array, each
order holding an `items` array. Exports from different tools disagree:
some quantities are strings, some prices are whole numbers, one order has no
customer. The total must still come out, and the program must report how
many items it could not use.

Iterate an array with its length and `at`, which returns an Optional:

```dc
Value items = order.get("items").some_or(Value::Null);
for usize i = 0; i < items.len(); i++ {
    Value item = items.at(i).some_or(Value::Null);
    // ... read item ...
}
```

`len()` is 0 for a missing or non-array `items`, so the loop simply does not
run. `as_float()` accepts both `2.5` and `2`.

## Your task

`src/main.dc` parses a small export and prints one line per order. `main` is
finished. Implement:

1. `item_total(item)` returns `qty * price` as an `Optional(f64)`. If `qty`
   is not an integer or `price` is not a number, return `Optional::None`.
2. `order_total(order, skipped)` adds up the items of the order's `items`
   array. For every item whose `item_total` is `None`, add one to
   `*skipped` and go on. A missing or empty `items` gives 0.

The program must print:

```text
order 101 ann: 14.50
order 102 bob: 0.00
order 103 cy: 0.00
order 104 unknown: 4.00
grand total: 18.50
skipped items: 1
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- A malformed item never stops the program; it is counted once in `skipped`.
