# Why the solution works

`135 / 60` is 2 because two full hours fit into 135 minutes, and the rest of
the division is thrown away. `135 % 60` is exactly that rest, 15. Dividing and
taking the remainder are two views of the same step, which is why the pair
turns an amount into a larger unit plus a leftover.

The last two lines show precedence: `8 + 4 * 3` multiplies first (4 * 3 is
12, plus 8 is 20), while `(8 + 4) * 3` adds first (12, times 3 is 36).
When you are unsure what an expression does, add parentheses; they cost
nothing and say what you mean.
