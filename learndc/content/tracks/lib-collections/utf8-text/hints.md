## Hint 1

`std::utf8` already has `count_codepoints` and `validate`; two of the stubs are one line each. Remember to `import std::utf8;`, which the file already does.

## Hint 2

For `cut_chars` keep two counters: `offset` (bytes) and `taken` (characters). The loop condition is `offset < text.len && taken < max_chars`. Each step moves `offset` forward by the decoded width, not by 1.

## Hint 3

Loop body: `offset += std::utf8::decode_lossy(text[offset..text.len]).width; taken++;`. After the loop `return text[0..offset];`.
