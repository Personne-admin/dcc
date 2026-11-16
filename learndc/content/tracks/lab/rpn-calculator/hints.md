## Hint 1

Work from the bottom up and run the checks after every function: numbers, then
tokens, then one operator step, then the whole expression. The harness for each
stage only needs that stage's function.

## Hint 2

Stack bugs are almost always order bugs. Write `a b -` on paper, mark which
value is on top of the stack, and compare with the operand you read first in
your code. The top is the right operand.

## Hint 3

When an error test fails, check what you changed before you returned. A refused
`apply` must not pop anything: read both operands, decide, and only then pop.
