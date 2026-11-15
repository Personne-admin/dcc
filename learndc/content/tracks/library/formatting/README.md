# Format a receipt

A `{}` placeholder prints a value in its default form. Inside the braces a
spec after a colon changes how the value is laid out:

| Spec    | Meaning                                | `42` prints as |
| ------- | -------------------------------------- | -------------- |
| `{:6}`  | pad to width 6 (numbers and text align right by default) | `    42` |
| `{:<6}` | left-align in width 6                  | `42    `        |
| `{:^6}` | center in width 6                      | `  42  `        |
| `{:*>6}`| fill character `*`, right-aligned     | `****42`        |
| `{:06}` | zero-pad to width 6                    | `000042`        |
| `{:+}`  | always show the sign                   | `+42`           |
| `{:x}` `{:X}` `{:b}` `{:o}` | hexadecimal, binary, octal | `2a` `2A` `101010` `52` |
| `{:.2}` | two decimals for a float               | `3.14` for 3.14159 |

Specs combine: `{:>9.2}` is a float right-aligned in 9 columns with two
decimals, and `{:08b}` is binary padded with zeros to 8 digits.

## Your task

`src/main.dc` prints a receipt, but every field is run together and the
floats show six decimals. The header is already correct. Change only the
format strings in `row` and `footer` so the program prints exactly:

```text
Item      Qty    Price
apple       3     0.50
melon      12    12.25
fig       100     3.75
id 0042 flags 00000101 mask FF
```

Each row uses the same column widths as the header: name left-aligned in 8,
quantity right-aligned in 5, price right-aligned in 9 with two decimals. In
the footer, `id` is zero-padded to 4 digits, `flags` is 8 binary digits and
`mask` is upper-case hexadecimal.

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches the five lines above exactly and the exit code is 0.
- `header`, the data and the argument lists are unchanged.
