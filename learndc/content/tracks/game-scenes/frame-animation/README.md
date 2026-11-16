# Frame animation

A moving character does not count frames; it asks the clock. If the animation advanced one frame per loop pass, it
would run twice as fast on a monitor that refreshes twice as often. Time-based animation keeps the **elapsed time**
and computes the frame from it, so the speed is the same on every machine.

`src/lamp.dc` holds a four-frame sprite sheet of a lamp (red, yellow, green, blue) in a single row and a ready
`frame_rect(index)`. Edit `src/anim.dc`.

## The animation record

```dc
public struct Anim {
    u32 elapsed;     // milliseconds since the animation started
    u32 frame_ms;    // how long each frame is shown
    i32 count;       // number of frames
    bool looping;    // true: start over, false: stop on the last frame
}
```

`make`, `restart` and `current` are provided. `current(a)` calls your `frame_at` with the animation's own numbers.

## frame_at

`frame_at(elapsed_ms, frame_ms, count, looping)` returns the frame to show: `index = elapsed_ms / frame_ms`. A looping
animation shows `index % count`; a one-shot animation shows `index` but never more than `count - 1`, so it
rests on its last frame.

## advance and finished

`advance(a, dt_ms)` adds the time that passed since the last frame (the caller measures it with `SDL_GetTicks`). Keep
the time bounded:

- looping: wrap it, `elapsed = elapsed % total`, with `total = frame_ms * count`. A game that runs for hours still
  has a small number.
- one-shot: never go past `total`.

`finished(a)` is true for a one-shot animation whose `elapsed` has reached `total`, and never true for a loop.

## draw

`draw(renderer, sheet, a, x, y, scale)` copies the current frame to (x, y), enlarged by `scale`: the source is
`lamp::frame_rect(current(a))`, the destination is the source size times `scale`.

## What you should see

Two lamps, each enlarged eight times, cycling every 250 ms. The left one loops. The right one plays once and stays on
blue; hold Space to restart it. `learndc run` opens the window; Esc quits. The pace is the same whatever the frame
rate, which is the point.

The first two checks test the time logic with no window. The third renders four frames offscreen and reads pixels back;
none of the checks can judge how smooth the animation looks on screen.
