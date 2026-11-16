# Why cut_chars walks with widths

Cutting `text[0..max_chars]` is wrong for any non-ASCII text: it counts
bytes and can end between the bytes of one character, leaving a malformed
tail. `cut_chars` instead advances by the **width** of each character, so
`offset` only ever lands on character boundaries. Because `decode_lossy`
reports malformed bytes with width 1, the loop terminates even on invalid
text.

`validate` and `count_codepoints` solve different problems. A program that
accepts input from files or other programs should validate first and decide
what to do with malformed text; counting is only meaningful afterwards. The
lossy count in sample 5 shows why: the bytes are counted, one each, but the
text is still invalid.

Byte offsets, not character positions, are what slicing needs. That is why
`validate` reports an offset and why `parse::Position` in the library
documents that its columns count bytes.
