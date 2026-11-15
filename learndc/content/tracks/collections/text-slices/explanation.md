# Why the solution works

```dc
public []const char first_word([]const char text) {
    for usize i = 0; i < text.len; i++ {
        if text[i] == ' ' {
            return text[0..i];
        }
    }
    return text;
}
```

- The loop index stops at `text.len`, so `text[i]` is always inside the
  slice. An empty text never enters the loop, which is why every function
  handles `""` without a special case.
- `count_char` only needs one pass and a counter. It never returns early,
  because every position matters.
- `has_char` returns on the first match: once one occurrence is found the
  rest of the text cannot change the answer. Falling out of the loop means
  every position was checked and nothing matched, so `false` is correct.
- `text[0..i]` is a half-open range: it includes position 0 and excludes `i`,
  which holds the space. The result borrows the same memory as `text`, so no
  copy is made, and when the space is at position 0 the range `0..0` is an
  empty slice.
- The library's `std::slice` module offers `index_of`, `count` and `split`
  with similar behavior; writing the loops once by hand shows exactly how
  they work.
