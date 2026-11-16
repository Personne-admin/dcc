# Delta time and fixed steps

Games need to be as fast on a slow computer as on a fast one. This lesson
writes the two tools that make that so. `src/timing.dc` has a `Clock` and a
`Stepper` with placeholder functions. `src/main.dc` is a finished demo with
two squares crossing the window at the same speed: the orange one moves with
the variable `dt`, the blue one with fixed steps of 1/60 s. They should stay
side by side.

## Task 1: tick

`SDL_GetTicksNS()` returns nanoseconds since SDL started, as a `u64`. Call
`tick(&clock, now_ns)` once per frame with that reading. It returns the seconds
since the previous call:

- The first call has no previous reading: remember `now_ns`, set `started`,
  and return `0.0`.
- Later calls compute `now_ns - clock.last_ns`, store `now_ns`, and divide by
  `1000000000.0` after converting to `f32` (`x as f32`).
- Never return more than `MAX_DT` (0.25 s). When the window is dragged or the
  program is stopped in a debugger, the next frame would otherwise get a huge
  `dt` and objects would leap across the world.

## Task 2: steps_due

`steps_due(&stepper, frame_time, step)` implements the accumulator from the
lecture and returns how many whole updates the caller must run now:

1. Add `frame_time` to `stepper.accumulator`.
2. While the accumulator holds at least `step` and fewer than `MAX_STEPS`
   updates were counted, subtract `step` and count one.
3. If the cap was reached, throw the rest of the accumulator away (set it to
   0). A computer that cannot keep up must not try to catch up forever: every
   frame would take longer and need even more steps (the "spiral of death").

Whatever is left (less than one step) stays for the next frame.

## Try it

`learndc run` shows the two squares. Drag the window or make the machine busy:
the squares still move at the same speed because both are driven by time,
not by frames. Remove the `MAX_DT` clamp and drag the window to see why it
exists.
