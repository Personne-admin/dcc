# Write a stock report as JSON

A shop wants its stock as JSON for another program. The data lives in a
small array of structs; your job is to write it out with the `Emitter`.

```dc
json.begin_object()?;
json.key("name")?;
json.string(item.name)?;
json.end_object()?;
```

`Out` is an alias for `Emitter(FileWriter)`, an emitter that writes to a
file (here, standard output). The emitter was created with an indent of 2.

## Your task

`src/main.dc` defines `Item` and a finished `main`. Implement:

1. `write_item(json, item)` writes one object with the members, in this
   order: `name` (string), `qty` (integer), `price_cents` (integer),
   `in_stock` (boolean, true when `qty` is above 0) and `note`. A `note` with
   no text (length 0) is written as `null`, otherwise as a string.
2. `write_report(json, shop, items)` writes an object with `shop` (string),
   `total_qty` (the sum of all quantities), and `items`, an array holding one
   `write_item` object per item.

`main` adds the final newline; the emitter never writes one.

The program must print:

```text
{
  "shop": "Night Market",
  "total_qty": 11,
  "items": [
    {
      "name": "Pen",
      "qty": 3,
      "price_cents": 250,
      "in_stock": true,
      "note": null
    },
    {
      "name": "Ink \"blue\"",
      "qty": 0,
      "price_cents": 700,
      "in_stock": false,
      "note": null
    },
    {
      "name": "Pad",
      "qty": 8,
      "price_cents": 399,
      "in_stock": true,
      "note": "A5, ruled"
    }
  ]
}
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly, including the escaped quotes of `Ink "blue"`,
  and the exit code is 0.
- Every emitter call is checked with `?`.
