# Why this works

The codec has three layers and each uses a different compile-time tool.

The bottom layer is **constrained generics**. `put` and `get` are written
once and compiled for each integer width. The `Integer` concept is the
contract: it probes `<<`, `%` and `&`, which exist only for integers. The old
`a + a` probe was a bug of the kind you read in the first stage: a float
satisfied it, the body's `as u64` quietly dropped the fraction and the wrong
data went out. The new concept moves that mistake to the call as a compile
error that names the concept and the failing operation.

The middle layer is a **pack**. `wire_size` and `put_all` accept any number
of integers of any width and unroll into one statement per argument.
`put_all` checks the combined size before the first write, so the frame is
either extended by all values or not at all. A caller never has to undo a
half-written frame.

The top layer is **CTFE**. `build_crc_table` is an ordinary function. The
compiler runs it for the value alias `CRC_TABLE` and the program contains a
256-byte constant, so `crc8` is one table lookup per byte with no setup work
at start. The table is also easy to trust: it is computed from the polynomial
instead of typed in, and the harness checks the standard test vector
`123456789` = `0xF4`.

The report ties the layers together and also shows the limit of the design:
`wire::get!u16(&frame, 7)` needs the type spelled out because nothing in the
arguments says which integer to read.
