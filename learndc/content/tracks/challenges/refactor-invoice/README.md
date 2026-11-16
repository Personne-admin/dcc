# Refactor: split the invoice out of main

`src/main.dc` prints an invoice and works, but it repeats the same five lines for every item,
with the numbers written into the code. Adding a fourth item means pasting them again, and the
discount rule (10 percent off from 10 pieces) is copied three times.

```text
widget: 3 x 250 = 750
bolt: 12 x 15 = 162
gear: 10 x 80 = 720
total: 1632
```

Run the program first: this output is the behaviour you must keep.

## Your task

Create `src/invoice.dc` (module `invoice`) with exactly this interface:

```dc
public struct Line {
    []const char name;
    i32 qty;
    i32 unit;
}

public i32 line_total(Line line);
public i32 invoice_total([]const Line lines);
```

- `line_total` is `qty * unit`, minus 10 percent (integer division, `t / 10` is subtracted) when `qty >= 10`.
- `invoice_total` is the sum of the line totals.

Then rewrite `main` so that it builds an array of three `Line` values and loops over them. The
discount rule may exist only once. The program output must not change.

Two checks run: the program output (it passes before and after) and a harness that tests the
module with other data.
