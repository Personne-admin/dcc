# Why it works

SDL packs its version into one integer so that versions compare with `<` and `>=`: `3004020 >= 3002000` means "at least 3.2.0". The split is integer arithmetic: dividing by a million drops minor and patch, and `%` keeps the low digits.

A C string has no length field. Its end is a zero byte, so the only way to know the length is to walk until the zero byte. Once you know it, `from_raw_const` pairs the pointer with the length and gives DC a slice over the same bytes. Nothing is copied, which is why the result is only valid while SDL does not touch the buffer. That is the rule for borrowed strings: use them at once.

Checking `null` first matters because SDL functions that return `const char*` can return `NULL` (for example when a hint is not set). Reading `text[0]` through a null pointer would crash the program.
