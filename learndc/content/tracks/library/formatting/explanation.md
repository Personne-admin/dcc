# Why the solution works

```dc
println("{:<8}{:>5}{:>9.2}", name, qty, price)?;
println("id {:04} flags {:08b} mask {:X}", id, flags, mask)?;
```

- A spec has the shape `{:[fill][align][+][0][width][.precision][type]}`.
  Every part is optional, and the placeholder still consumes the next
  argument.
- `{:<8}` pads the name with spaces on the right to 8 columns; `{:>5}` pads
  the quantity on the left. The padding is what lines the columns up with the
  header, so the table works for any value that fits the width.
- `{:>9.2}` rounds the float to two decimals first (`0.5` becomes `0.50`),
  then right-aligns the result in 9 columns.
- `{:04}` writes `42` as `0042`. `{:08b}` writes the `u8` value 5 in binary
  and pads to eight digits. `{:X}` writes 255 in upper-case hexadecimal.
- A value wider than its width is never truncated; the width is a minimum.
