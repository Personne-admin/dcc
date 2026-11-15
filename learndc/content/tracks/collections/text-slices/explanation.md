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
- `find_char` returns on the first match, so the position is the smallest
  one. Falling out of the loop means no match, and `Optional::None` says so
  without inventing a magic value such as `-1`.
- `text[0..i]` is a half-open range: it includes position 0 and excludes `i`,
  which holds the space. The result borrows the same memory as `text`, so no
  copy is made, and when the space is at position 0 the range `0..0` is an
  empty slice.
- The library's `std::slice` module offers `index_of`, `count` and `split`
  with the same behavior; writing them once by hand shows exactly how they
  work.
