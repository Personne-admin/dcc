## Hint 1

Keep the largest value seen so far and the second largest, plus a flag that says whether a second value exists at all. A sentinel such as 0 would be wrong for negative data.

## Hint 2

For each value: bigger than the maximum, then the old maximum becomes the second; strictly smaller than the maximum and bigger than the second (or no second yet), then it becomes the second. Equal to the maximum changes nothing.

## Hint 3

Start `first` with `values[0]`, loop from index 1, and return `Optional::None` when `have_second` is still false.
