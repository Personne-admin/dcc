# Root cause: The Sunday that never wins

The loop in `busiest_day` ran while `i < sales.len - 1`. For seven days that means `i` takes the values 1 to 5, so index 6 (day 7) is never examined. The best value so far can therefore never be the last one.

How to find this kind of bug:

1. Reproduce it with data where the right answer is obvious and sits at the edge (the maximum in the last position).
2. Narrow it down: `total` and `quietest_day` are right, so only `busiest_day` is suspect.
3. Compare it with the working sibling. The only difference is `- 1` in the loop condition.

Loops over `0..len` or `i < len` are the normal shape. Whenever you see `len - 1` or `<=` in a loop bound, ask whether it is deliberate.
