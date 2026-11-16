# Characters are not bytes

A DC text is a `[]const char`: a slice of bytes. The bytes are expected to
be UTF-8, where one character (a *code point*) takes 1 to 4 bytes:

| Character | Bytes in UTF-8 | Example |
| --------- | -------------- | ------- |
| ASCII letters and digits | 1 | `n` is `0x6E` |
| most accented letters | 2 | the i with two dots is `0xC3 0xAF` |
| the euro sign, most CJK | 3 | `0xE2 0x82 0xAC` |
| emoji and rarer symbols | 4 | |

So `text.len` is the number of **bytes**. A person counting letters wants the
number of code points, and slicing `text[0..3]` can cut a character in half.
`std::utf8` has the tools:

```dc
import std::utf8;

usize n = std::utf8::count_codepoints(text);          // characters
Optional(usize) bad = std::utf8::validate(text);      // offset of the first bad byte
std::utf8::Decoded d = std::utf8::decode_lossy(text[i..text.len]); // d.codepoint, d.width
```

- `validate` returns `None` for valid text, or `Some(offset)` of the first
  malformed sequence (a stray continuation byte, a truncated sequence, an
  overlong encoding or a surrogate).
- `decode_lossy` decodes the sequence at the start of a slice. Its `width`
  is the number of bytes it took; a malformed byte is reported as one
  replacement character with width 1, so walking with it always makes
  progress. `count_codepoints` counts that way.
- `decode` (not lossy) returns a `Result(Decoded, Utf8Error)` when you want
  to know why it failed.

## Your task

The samples in `main` are written with `\x` byte escapes, so the file stays
plain ASCII. Implement the three stubs; `describe` and `main` are finished.

1. `char_count` returns the number of code points.
2. `first_bad_byte` returns the offset of the first malformed byte, or `None`.
3. `cut_chars(text, max_chars)` returns the longest prefix holding at most
   `max_chars` characters. Walk from offset 0: while the offset is inside the
   text and fewer than `max_chars` characters were taken, add the `width` of
   the character at the offset. Never cut inside a sequence.

Expected output:

```text
#1: 5 bytes, 5 chars, valid
#2: 6 bytes, 5 chars, valid
#3: 6 bytes, 4 chars, valid
#4: 5 bytes, 5 chars, invalid at byte 2
#5: 4 bytes, 4 chars, invalid at byte 2
first 2 chars of #3: 4 bytes
first 9 chars of #3: 6 bytes
first 0 chars of #3: 0 bytes
first 3 chars of #2: 4 bytes
```

Sample 5 ends in an incomplete three-byte sequence: it is invalid, and
`count_codepoints` counts each of its two stray bytes as one character.

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly.
- `cut_chars` never returns half a character.
