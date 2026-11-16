## Hint 1

Work in the order of the stages. First the concept, because every other
function depends on it. Then the byte-level functions, then the variadic
layer on top of them, then the checksum, last the report. Each layer uses
only the one below it.

## Hint 2

All numbers go through `u64`: `value as u64` on the way out, `wide as T` on
the way back, with shifts by `((8 * i) as u64)`. The variadic functions only
loop over `args` with `static for` and call the single-value functions.
The CRC builder is the usual two nested loops; `crc8` is a loop of table
lookups.

## Hint 3

`put_all` must test the total before writing the first byte:

```
if wire_size(args...) > CAPACITY - frame.len {
    return false;
}
static for item in args {
    put(frame, item);
}
return true;
```
