# Why this report is reproducible

`add_up` uses `get_or_put` as a counter: the pointer it returns is used on
the very next line, before any other insertion can move the table.

`sorted_ids` is the whole lesson in one function. The map decides its own
iteration order; the program decides the output order by copying the keys
out and sorting them. After that, `main` only looks keys up, so the report is
identical on every run and on every backend.

`best_customer` scans the sorted ids and replaces the best only on a strictly
larger total. With three customers on 1550 cents, the first in id order
wins. Writing `>=` instead would silently pick the last of the tied
customers: tie rules are part of a program's specification, not an accident
of a comparison operator.
