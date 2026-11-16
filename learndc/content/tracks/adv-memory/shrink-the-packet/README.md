# Shrink the packet

A network service keeps millions of `Packet` values in memory. The struct in
`src/packet.dc` was written in the order the protocol document lists the
fields, and it is wasteful:

```dc
public struct Packet {
    u8 kind;
    u64 id;
    u8 flags;
    u32 length;
    u16 port;
}
```

`src/main.dc` prints a layout report. Run the check first and look at what
the starter prints: how large is a `Packet`, and where does each field sit?

## Your task

1. Fill in `field_bytes()` in `src/packet.dc`. It returns the sum of the
   sizes of the five fields, written with `sizeof` (for example
   `sizeof(u8)`), not as a number. `main` subtracts it from `sizeof(Packet)`
   to print the padding.
2. Reorder the fields of `Packet` so that no padding is left: largest
   alignment first. Because several fields share an alignment, use exactly
   this order: `id`, `length`, `port`, `kind`, `flags`.

Only `src/packet.dc` needs to change. The program must print:

```text
Packet: 16 bytes, align 8
fields: 16 bytes, padding: 0 bytes
id at 0
length at 8
port at 12
kind at 14
flags at 15
```

Reordering is safe here because `Packet` is only ever used through its field
names. For a struct that a file format or a C library defines, the order is
part of the contract and you would leave it alone.
