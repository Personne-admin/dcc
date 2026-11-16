# Root cause: Seventy megabytes that became six

`(bytes / 1024) as u16` computes 71680 correctly and then casts it to a type that cannot hold it. An explicit cast in DC truncates: only the lowest 16 bits survive, and 71680 modulo 65536 is 6144. No error, no warning, because you asked for the conversion.

The method: when only large values are wrong, compute the expected number by hand. If the wrong result equals the right one modulo a power of two (here 2^16), a type is too narrow. Look for `as` and for narrow variable types on the path of the value.

Use the widest type the data can need, and cast only where the narrower type is a real requirement (a file format, a hardware register).
