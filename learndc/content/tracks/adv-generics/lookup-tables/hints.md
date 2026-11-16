## Hint 1

A builder is an ordinary function. Make a `u8[128] table = {};`, loop with
`for usize code = 0; code < 128; code++`, and assign `table[code]`. The
compiler runs the whole function once; the result becomes `CLASSES` or
`ROT13`.

## Hint 2

For the classes, an `if / else if` chain over `code` ranges: 48 to 57 digit,
65 to 90 upper, 97 to 122 lower, 32, 9 or 10 space, anything else `OTHER`.
For rot13, letters map to `(code - base + 13) % 26 + base` where `base` is
65 for upper case and 97 for lower case; every other code maps to `code`.
Cast the result with `as u8`.

## Hint 3

```
} else if code >= 65 && code <= 90 {
    table[code] = ((code - 65 + 13) % 26 + 65) as u8;
} else if code >= 97 && code <= 122 {
    table[code] = ((code - 97 + 13) % 26 + 97) as u8;
} else {
    table[code] = code as u8;
}
```
