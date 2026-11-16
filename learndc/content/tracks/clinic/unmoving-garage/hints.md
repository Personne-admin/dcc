## Hint 1

Compare how `enter` and `leave` take their parameter.

## Hint 2

A function can only change the caller's struct when it works on the original, not on a copy.

## Hint 3

Change the parameter of `enter` to `Garage* g` and pass `&g` at the three call sites.
