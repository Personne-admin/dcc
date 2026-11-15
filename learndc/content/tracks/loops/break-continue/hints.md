## Hint 1

`continue` and `break` are statements, usually inside an `if`. Ask of each
function: is it "ignore this value" (continue) or "I have my answer, stop
looking" (break)?

## Hint 2

`sum_skipping_threes`: loop over `1..=n`, `continue` when `i % 3 == 0`, add `i`
otherwise. `terms_to_reach`: in a `while true`, increase the number of terms,
add it to the sum and `break` when the sum reaches the target.
`smallest_divisor`: remember the answer in a variable that starts at `n`, and
when `n % d == 0` store `d` and `break`.

## Hint 3

`for i32 i in 1..=n { if i % 3 == 0 { continue; } total += i; }`;
`while true { terms += 1; sum += terms; if sum >= target { break; } }`;
`for i32 d in 2..=n { if n % d == 0 { found = d; break; } }`.
