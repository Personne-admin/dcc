## Hint 1

The function that handles one `Value` is a `match` with one arm per kind.
The two kinds that contain other values, `Array` and `Table`, are the
recursive ones: their arms call `emit_value` or `emit_table` again for each
element or entry.

## Hint 2

`emit_table` is `begin_object`, then a loop over `table.entries` that calls
`json.key(entry.key)` and `emit_value(json, &entry.value)`, then
`end_object`. In `emit_value`, `Value::Array(items)` does `begin_array`, a
loop over `items` calling `emit_value(json, &item)`, and `end_array`. Add a
final `_ => json.null_value()?,` for the date and time kinds that are not
handled.

## Hint 3

```dc
Value::String(text) => json.string(text)?,
Value::Integer(n) => json.integer(n)?,
Value::Float(x) => json.float(x)?,
Value::Bool(b) => json.boolean(b)?,
Value::LocalDate(date) => emit_date(json, date)?,
Value::Table(table) => emit_table(json, &table)?,
```

plus the `Array` arm described above. Every arm ends in `?`.
