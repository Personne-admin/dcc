## Hint 1

`as_integer()` and `as_float()` are empty when the value has another kind.
That is how you detect a malformed item without any special comparison:
`is_none()` on either conversion.

## Hint 2

In `item_total`, read `qty` and `price` with `get(...).some_or(Value::Null)`,
return `Optional::None` if either conversion `is_none()`, and otherwise
return `Optional::Some(...)` of the product. Convert the integer with
`as f64` before multiplying.

In `order_total`, loop `i` from 0 to `items.len()`, take `items.at(i)`, call
`item_total` and test `is_some()`.

## Hint 3

```dc
if line.is_some() {
    total += line.unwrap_some();
} else {
    *skipped += 1;
}
```

with `Optional(f64) line = item_total(&item);` before it, and
`f64 total = 0.0;` before the loop.
