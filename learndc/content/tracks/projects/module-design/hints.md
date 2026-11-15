## Hint 1

A module is a file. The new file `src/shipping.dc` must begin with
`module shipping;`, and everything private in it is simply a declaration without
`public`.

## Hint 2

Cut the three pricing functions out of `main.dc` and paste them into
`shipping.dc`. Put `public` in front of `shipping_cost` only. In `main.dc`, add
`import shipping;` below the module line.

## Hint 3

`main` now calls `shipping::shipping_cost(grams, false)` and
`shipping::shipping_cost(grams, true)`. The helpers are called inside
`shipping.dc` by their plain names, so nothing else changes.
