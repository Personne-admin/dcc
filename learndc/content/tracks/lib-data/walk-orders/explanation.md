# Why the solution works

```dc
Optional(f64) item_total(const Value* item) {
    Value qty = item.get("qty").some_or(Value::Null);
    Value price = item.get("price").some_or(Value::Null);
    if qty.as_integer().is_none() || price.as_float().is_none() {
        return Optional::None;
    }
    return Optional::Some(qty.as_integer().unwrap_some() as f64 * price.as_float().unwrap_some());
}
```

- A function that may have no answer returns an `Optional`, the same type
  the library uses. The caller decides what "no answer" means: here it counts
  the item and moves on. Returning 0.0 instead would hide the problem and
  make a bad item look like a free one.
- `unwrap_some()` is safe here because the `is_none()` test above already
  returned. Never call it without such a test.
- `as_float` widens an Integer, so `"price": 2` works, while `"qty": "two"`
  is a String and has no integer reading. That is the line between a lenient
  reader (accept every number) and a silent one (accept anything).
- `order_total` takes `usize* skipped` so one counter spans all orders. The
  function only borrows it; `main` owns the variable.
- The loop uses `len()` and `at(i)` because the items are a JSON array. A
  missing `items` key gives `Null`, whose `len()` is 0, so order 102 and the
  customer-less order need no special cases.
