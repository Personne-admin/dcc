# Refactor: integer codes to an enum

`src/main.dc` computes shipping fees and delivery times for three speeds, which it calls
0, 1 and 2. Nothing stops a caller from passing 7, and every function repeats an `if` chain
over the same numbers.

```text
standard: fee 8, days 5
fast: fee 15, days 2
overnight: fee 29, days 1
```

Run the program first: this output is the behaviour you must keep.

## Your task

Create `src/shipping.dc` (module `shipping`) with exactly this interface:

```dc
public enum Speed { Standard, Fast, Overnight }

public i32 fee(Speed speed, i32 kilos);
public i32 days(Speed speed);
```

The rules are the ones in the starter: standard `5 + kilos`, fast `9 + 2 * kilos`,
overnight `20 + 3 * kilos`; delivery in 5, 2 and 1 days. Use `match` so that adding a speed
later makes the compiler point at every place that must change. Then rewrite `main` to use the
enum: no integer codes may remain.

Two checks run: the program output (it passes before and after) and a harness that tests the
module.
