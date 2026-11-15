# Why the solution works

- An array is a row of boxes numbered from 0, so day 2 is `steps[1]` and the
  loop that prints day numbers writes `i + 1`. Mixing up "day number" and
  "index" is the most common slip with arrays.
- `for i in 0..DAYS` stops before `DAYS`, so `steps[i]` never leaves the
  array. The last valid index is `DAYS - 1`, which is also what
  `steps[DAYS - 1]` reads directly.
- The total is a running sum: it starts at 0 and every element is added once.
  The type of `total` is `i32`, the same as the elements.
- The best day keeps an index, not a value. Remembering where the largest
  value is lets you print both the day (`best + 1`) and, if you need it,
  the value (`steps[best]`). The loop starts at 1 because index 0 is already
  the first candidate.
- The length is named once in `DAYS`. Changing the array to seven days means
  changing the constant and the list of numbers, and every loop follows.
