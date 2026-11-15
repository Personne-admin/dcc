# Why the solution works

```dc
public Status(Error) print_receipt(i32 bread, i32 milk) {
    print_banner("Corner Shop")?;
    print_item("bread", bread)?;
    print_item("milk", milk)?;
    print_item("total", bread + milk)?;
    return Status::Ok;
}
```

- Each `?` asks whether the call before it failed. If it did, the error is
  returned from `print_receipt` at once, so a broken banner never leads to a
  half-printed items list that looks fine.
- `print_receipt` has no `println` of its own. The format of a row lives in
  `print_item` only, so changing `name: price` to `name = price` needs one
  edit. That is the same reuse as `sum_of_squares` calling `square`.
- `bread + milk` is evaluated before the call, because arguments are
  evaluated first. The total row is just another item.
- The checker cannot see a missing `?`, since a forgotten one still prints
  the right text. The `?` matters when printing fails, for example when the
  output is closed: with it the program reports failure by exiting with code
  1, and without it the failure vanishes.
