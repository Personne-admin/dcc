## Hint 1

A tile is a number in an array. Text goes in with one loop over rows and an inner loop over columns; the grid position becomes an array index with `row * COLS + col`.

## Hint 2

In `parse`, check the row count first, then each row's length, then match each character; remember whether you saw `@`. In `is_wall`, return `true` early when `col` or `row` is outside `0..COLS` and `0..ROWS`. In `draw`, loop over all tiles and subtract the camera from the world position.

## Hint 3

```dc
if c == '#' { m.cells[at] = 1; }
else if c == '.' { m.cells[at] = 0; }
else if c == '@' { m.cells[at] = 0; m.start_col = col; m.start_row = row; found = true; }
else { return false; }
```

with `usize at = (row * COLS + col) as usize;` and `return found;` at the end.
