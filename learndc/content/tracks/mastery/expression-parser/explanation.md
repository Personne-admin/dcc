# Why the design is layered

The three error classes in a fixed order are the specification's way of telling
you the architecture. If the evaluator ran while the parser was still reading,
`1/0 +` would report a division by zero although the text is not even a valid
expression. Separating lexing, parsing and evaluating makes each phase small,
and makes "syntax beats evaluation" automatic: the evaluator never sees input
that did not parse.

A recursive descent parser mirrors the grammar. `expression` calls `term`,
`term` calls `unary`, and the tighter-binding operators are simply parsed deeper
in the call chain, so their operands are already combined by the time the looser
operator sees them. Left associativity comes from the loop that wraps the
previous result as the left operand of the next node. Parentheses restart the
chain at the top. Unary minus is right-recursive because `- - 5` nests to the
right.

Nodes refer to their children by index into one owning array instead of by
pointer. Indexes survive the array growing and relocating, which a pointer would
not, and the whole tree is released with one `deinit`. The tokens array and the
nodes array are owned by `evaluate` with `defer`, so every early return and every
`?` frees them; the harness runs two thousand evaluations in a row to prove
nothing is kept between calls.

Overflow checks come before the operation because the operation itself would
silently wrap. Addition and subtraction are checked against the limits with a
comparison that cannot itself overflow (`a > MAX - b` for positive `b`). A
product is checked by multiplying and dividing back, after handling the two cases
the division cannot detect: the smallest value times -1 and zero. Reporting the
position of the operator instead of the operand makes the error useful to point at.

Typed errors with a position are the output a real tool needs: the caller can
underline the character, count error kinds and decide what to do, which a message
string or a magic value would not allow.
