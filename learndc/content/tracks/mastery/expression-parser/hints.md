## Hint 1

Each line of the grammar becomes one function, and a function calls the next
tighter one. Precedence is not something you compute: it is the shape of the
calls. Left associativity is a loop that keeps combining while it sees one of its
operators. Handle the three error classes as three separate phases, in order.

## Hint 2

Because syntax errors beat evaluation errors even when the evaluation error comes
first in the text, evaluation cannot happen while parsing. Cut the text into an
array of tokens (each with its kind, value and position), parse the tokens into a
tree stored in an owning array where nodes refer to each other by index, and only
then walk the tree. A tagged enum with one payload per variant is a natural node.

## Hint 3

Parse functions can take the token array, the node array and a position cursor
(`usize*`) and return `Result(usize, CalcError)`, the index of the node they built.
Evaluate recursively: left, right, then check and perform the operation. To
return an error with a payload from a function that returns `Result`, write
`Result::Err(CalcError::Overflow(position))`. Keep the whole thing in one module
first; split it into a lexer, a parser and an evaluator when it works.
