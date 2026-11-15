## Hint 1

Write the loop in three parts: variables set up before it, a condition in the
`while` line and an update inside the body that eventually makes the condition
false. Return the counter after the loop.

## Hint 2

`count_digits`: start `digits` at 1 and keep dividing `n` by 10 while `n` still
has at least two digits (`n >= 10`), adding 1 each time. `years_to_double`:
loop while `balance < 2 * start`, growing the balance and counting a year in
each pass. `first_multiple_above`: start `multiple` at `step` and add `step`
while `multiple <= limit`.

## Hint 3

`while n >= 10 { n /= 10; digits += 1; }`, then
`while balance < 2 * start { balance += balance * percent / 100; years += 1; }`
and `while multiple <= limit { multiple += step; }`. Each function ends with a
`return` of the variable it was counting.
