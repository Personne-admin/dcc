## Hint 1

Work one stage at a time and run the checks after each: the objective list
shows which stage is current. Every stage has hints of its own, too.

## Hint 2

Stages 2 to 4 all start with the same question: where on the shelf is this
name? Write `position` carefully once and call it from `add`, `count_of` and
`take`.

## Hint 3

In `take`, find the entry, refuse unknown names, take a pointer to the entry
with `&shelf.items()[index]`, refuse when its count is too small, subtract,
and only then remove the entry if its count is 0.
