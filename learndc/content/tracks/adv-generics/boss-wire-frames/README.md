# The Frame Codec

You are writing a binary codec in `src/wire.dc` and a report in
`src/main.dc`. A `Frame` is a fixed array of 16 bytes and a length. Everything
is little endian (least significant byte first). Work stage by stage; every
stage has its own checks and hints.

## What you are building

- `Integer(T)`: the concept for the types the codec accepts. The starter
  version is too weak. Make it accept all integer types and nothing else:
  not `f64`, `f32`, `bool`, `char` or structs.
- `put(T)(frame, value)`: append `sizeof(T)` bytes of an integer. Returns
  `false`, and changes nothing, if they do not fit.
- `get(T)(frame, at)`: read an integer of type `T` at byte offset `at`.
  Returns `Optional(T)`, none when the bytes are not inside the length. Call
  it with the type: `wire::get!u16(&frame, 7)`.
- `wire_size(args...)`: the number of bytes `args` would take.
- `put_all(frame, args...)`: write every argument or none; `false` if the
  whole list does not fit.
- `build_crc_table()`: the CRC-8 table (polynomial 0x07), built by the
  compiler; `crc8(frame, count)`: the checksum of the first `count` bytes.
- `seal(frame)`: append the checksum of the whole payload. `verify(frame)`:
  the last byte is the checksum of the bytes before it (an empty frame never
  verifies).
- Bonus `unseal(frame)`: verify, then remove the checksum byte.

CRC-8 in one table entry: for a byte `i`, run 8 times: if the top bit is set,
shift left and xor with 0x07, otherwise just shift left. `crc8` then folds the
bytes one by one: `crc = TABLE[crc ^ byte]`. The checksum of `123456789` is
`0xF4`.

## The report

`main.dc` has a `dump` helper that prints a frame as hex. Make `main` print
exactly:

```text
wire frames
size: 9 bytes, capacity 16
frame: 07 f4 ff a0 86 01 00 01 02 5c
verify: true
decoded: id 7 temp -12 delta 100000 ticks 513
tampered: false
two i64: true, len 16
third i64: false, len 16
```

The record is `u8 id = 7`, `i16 temperature = -12`, `i32 delta = 100000` and
`u16 ticks = 513`, put in this order and sealed. `tampered` is the result of
`verify` after `frame.bytes[4] = 0`. The last two lines try to put two `i64`
values and then a third into a fresh frame.

## Rules

- `put`, `get` and `put_all` are generic: one implementation for all integer
  widths. Do not write a copy per type.
- Do not change the signatures or `CAPACITY`.
- Keep the table a value alias. It must be computed while compiling.

## Acceptance criteria

- The project builds with `dpm build`.
- The report above matches exactly and the exit code is 0.
- Every stage's checks pass; the bonus stage is optional.
