# tabulate

Print aligned text tables and compute small statistics.

## Usage

```text
import tabulate::table;

tabulate::table::print_table(headers, cells)?;
```

## API

- tabulate::cell: display_width, print_left, print_right
- tabulate::table: column_width, print_table
- tabulate::stats: total, min, max, mean, range
