## Hint 1

All four functions are short. `range`, `unit` and `chance` build on `next`.

## Hint 2

`next`: `r.state = r.state * 1664525 + 1013904223; return r.state;`. `range`: `u32 span = (hi - lo + 1) as u32;` then `lo + (((next(r) >> 16) % span) as i32)`. `chance`: one comparison with `range(r, 0, 99)`.

## Hint 3

```
u32 bits = next(r) >> 8;
return (bits as f32) / 16777216.0;
```
