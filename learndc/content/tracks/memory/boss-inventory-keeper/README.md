# The Inventory Keeper

The storeroom is one owning `Array(Item)`, the shelf. `src/inventory.dc`
declares the item, the error type and every function you need; each one
returns a placeholder. `src/main.dc` holds tonight's deliveries and orders.

```dc
public struct Item {
    []const char name;
    u32 count;
}

public enum StockError {
    Unknown,
    NotEnough,
}
```

Every function in `inventory` takes the shelf as a pointer,
`Array(Item)* shelf` (or `const Array(Item)*` when it only reads). The shelf
belongs to whoever created it; `inventory` only borrows it.

Each stage below is checked on its own, so you can clear them in order and
watch them light up. A stage that passed earlier stays in your record, but
the objective list shows whether it still passes with your current code.

## Stage 1: Stock the shelf

- `add(shelf, name, count)` pushes `Item { name = name, count = count }`.
  Pushing can fail to allocate; pass that on with `?`.
- `kinds(shelf)` returns how many entries the shelf holds.

## Stage 2: Find it again

- `position(shelf, name)` returns `Optional::Some(index)` of the entry with
  that name, or `Optional::None`.
- `count_of(shelf, name)` returns the count of that entry, or 0.

Text is a slice, and `==` does not compare slices. `std::slice::eq(a, b)`
(already imported) is true when both hold the same characters.

## Stage 3: One entry per item

`add` with a name that is already on the shelf raises that entry's count.
Only a new name gets a new entry.

## Stage 4: Refuse what you cannot pack

`take(shelf, name, count)` returns `Status(StockError)`:

- `StockError::Unknown` when the name is not on the shelf,
- `StockError::NotEnough` when the entry holds fewer than `count`,
- otherwise it subtracts `count`. An entry that reaches 0 leaves the shelf:
  `shelf.remove(index)` takes it out and keeps the others in order.

A refused order must not change anything.

## Stage 5: The night shift

Complete `main` in `src/main.dc`:

1. Create the shelf and register its cleanup with `defer` straight away.
2. `add` every delivery, then print `delivery: N kinds`.
3. `take` every order and print one line per order, numbered from 1.
4. Print `left on the shelf:` and one indented line per remaining entry.

```text
delivery: 3 kinds
order 1: 5 x apple packed
order 2: 6 x pear refused, not enough in stock
order 3: 1 x fig refused, unknown item
order 4: 4 x pear packed
order 5: 2 x plum packed
left on the shelf:
  apple 10
  plum 5
```

A `Status` can be matched like any tagged enum, and a `match` arm can hold
another `match`:

```dc
match take(&shelf, "fig", 1) {
    Status::Ok => println("packed")?,
    Status::Err(e) => match e {
        StockError::Unknown => println("unknown")?,
        StockError::NotEnough => println("too few")?,
    },
}
```

## Bonus: the ledger

`total_units(shelf)` returns the sum of all counts. The bonus is not needed
to defeat the boss.
