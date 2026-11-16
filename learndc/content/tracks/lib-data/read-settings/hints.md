## Hint 1

`root.get(key)` is an `Optional(Value)`: empty when the key is missing.
Each `as_*` conversion is again an `Optional`, empty when the value has
another kind. `some_or(default)` ends an Optional with a fallback.

## Hint 2

Do the lookup in two steps. First turn the Optional(Value) into a plain
`Value` with `some_or(Value::Null)`. Then call the conversion that matches the
function (`as_string`, `as_integer`, `as_bool`) on it and finish with
`some_or(fallback)`. The three helpers differ only in the conversion.

## Hint 3

```dc
Value found = root.get(key).some_or(Value::Null);
return found.as_string().some_or(fallback);
```

Use `as_integer()` in `int_or` and `as_bool()` in `bool_or`.
