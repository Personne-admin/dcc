## Hint 1

Think of the output as a tree and write it in the same shape: begin the
object, then for each member call `key` and one value call, then end the
object. Each `begin_*` needs its `end_*`.

## Hint 2

`write_item` is `begin_object`, five `key`/value pairs, `end_object`. For the
note, test `item.note.len == 0` and call `null_value()` in one branch and
`string(item.note)` in the other. `write_report` first sums `qty` in a loop,
then writes its members. For `items`, call `begin_array`, loop calling
`write_item(json, &item)`, then `end_array`.

## Hint 3

```dc
json.key("items")?;
json.begin_array()?;
for item in items {
    write_item(json, &item)?;
}
json.end_array()?;
```

The emitter puts the commas and the indentation in by itself; you only
write the tokens.
