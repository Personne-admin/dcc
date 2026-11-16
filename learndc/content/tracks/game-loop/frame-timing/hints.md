## Hint 1

`tick` has to remember something between calls, and the `Clock` struct is where it goes. `steps_due` is a `while` loop with two conditions.

## Hint 2

`tick`: handle `!clock.started` first. Then `u64 elapsed_ns = now_ns - clock.last_ns;`, update `last_ns`, convert with `(elapsed_ns as f32) / 1000000000.0`, clamp with an `if`. `steps_due`: loop `while stepper.accumulator >= step && steps < MAX_STEPS`.

## Hint 3

```
if steps == MAX_STEPS {
    stepper.accumulator = 0.0;
}
return steps;
```
