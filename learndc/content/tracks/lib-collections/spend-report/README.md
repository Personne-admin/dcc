# A spending report in customer order

`src/main.dc` holds seven orders, each with a customer id and an amount in
cents. `main` builds a `Totals` map (customer id to cents), asks for the ids
in order and prints a report. Three helpers are stubs:

1. `add_up(totals, orders)` adds every order's `cents` to the total of its
   customer. A customer that is not in the map yet starts at 0
   (`get_or_put`). Pass an allocation failure on with `?`.
2. `sorted_ids(totals, ids)` pushes every key of the map into `ids` and then
   sorts `ids` ascending, so that the report does not depend on the order the
   map iterates in. Iterate with `totals.iterate()` and `next()` as in the
   lecture, and sort with `std::sort::sort(ids.items())`.
3. `best_customer(totals, ids)` returns the id with the largest total, or
   `None` for an empty list. When two customers have the same total, the one
   that comes first in `ids` wins, so scan `ids` in order and replace the
   best only when a total is strictly larger.

Do not change `main`. Expected output:

```text
4 customers
customer  7:   15.50
customer 12:   15.50
customer 31:   15.50
customer 40:    0.99
top spender: 7
```

Three customers end with exactly 1550 cents on purpose: the tie rule decides
who the top spender is.

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- The program never prints in map iteration order.
