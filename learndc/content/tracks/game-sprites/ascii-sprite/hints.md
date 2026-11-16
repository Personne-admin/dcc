## Hint 1

Two jobs: a palette that maps one character to a color, and a loop that stores each color at the right byte of the array. Do the palette first; its check does not need the loop.

## Hint 2

In `decode`, keep `y` counting rows and `x` counting characters in the row. For each character compute `at = (y * width + x) * 4` with `width = art[0].len`, then store `color.r`, `color.g`, `color.b`, `color.a` at `at` to `at + 3`.

## Hint 3

```dc
for row in art {
    usize x = 0;
    for c in row {
        Rgba color = color_for(c);
        usize at = (y * width + x) * 4;
        pixels[at] = color.r;
        // g, b and a at at + 1, at + 2 and at + 3
        x += 1;
    }
    y += 1;
}
```
