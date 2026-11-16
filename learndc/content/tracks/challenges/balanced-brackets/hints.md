## Hint 1

A closing bracket may only match the most recently opened bracket that is still open: that is a stack. Use `stack[depth]` and `depth`.

## Hint 2

On an opening bracket store it and raise `depth`. On a closing bracket fail when `depth == 0` or when the top (`stack[depth - 1]`) is not the partner; otherwise lower `depth`.

## Hint 3

Write the partner test as three alternatives `(c == ')' && open == '(') || ...`. After the loop return `depth == 0`. Check `depth == 256` before pushing.
