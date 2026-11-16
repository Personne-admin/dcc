## Hint 1

Three small pieces of arithmetic on milliseconds: frame = elapsed / frame_ms, then wrap it or clamp it depending on `looping`.

## Hint 2

`frame_at`: `i32 index = (elapsed_ms / frame_ms) as i32;` then `index % count` for a loop, and `count - 1` when the index is past the end otherwise. `advance`: add `dt_ms`, then `% total` for a loop or `min(elapsed, total)` otherwise, where `total` is `frame_ms * (count as u32)`.

## Hint 3

```dc
a.elapsed += dt_ms;
if a.looping {
    a.elapsed = a.elapsed % total;
} else if a.elapsed > total {
    a.elapsed = total;
}
```

`finished` is `!a.looping && a.elapsed >= total`. `draw` makes `src` from `lamp::frame_rect(current(a))`, `dst` from its size times `scale`, and calls `SDL_RenderTexture`.
