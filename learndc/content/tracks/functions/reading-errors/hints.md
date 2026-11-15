## Hint 1

Read the first line of the error, then the `~~~` note below the frame. The
note shows the function declaration the compiler compared your call with, and
a reason in parentheses.

## Hint 2

The reason is an argument count mismatch: `total_price` was declared with two
parameters and the call supplies only one. Decide what the missing argument
means, using the task description.

## Hint 3

In `src/main.dc`, change the call to `total_price(250, 3)`: the unit price in
cents, then the quantity.
