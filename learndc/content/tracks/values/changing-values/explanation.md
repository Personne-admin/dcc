# Why the solution works

`coins` is one variable that holds a running total. Every statement reads its
current value, computes, and stores the result: 20, then 50, then 35, then 70,
then 71, then 66. Because each step starts from the previous one, the order of
the statements matters: doubling before the book would give a different
number.

The short forms are not new abilities, only less to write and to get wrong:
`coins += 30;` cannot accidentally name a different variable on the right.

`SWEET` is a `const`, so the price lives in one place with a name. If it
ever changes, you change one line, and the compiler guarantees that nothing
else modifies it.
