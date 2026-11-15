# Why the solution works

```dc
public usize count_words([]const char text) {
    usize count = 0;
    bool in_word = false;
    for c in text {
        if is_space(c) {
            in_word = false;
        } else {
            if !in_word {
                count++;
            }
            in_word = true;
        }
    }
    return count;
}
```

- The loop is a two-state machine. `in_word` remembers the previous
  character, so a word is counted exactly once, where it starts. Counting
  spaces or splitting on single spaces would miscount repeated, leading and
  trailing separators; the state variable makes all of them harmless.
- `longest_word` runs the same machine but tracks a length instead of a
  count: reset on a separator, grow on a letter, keep the maximum in `best`.
- `count_lines` counts `'\n'` characters and then adds the last line when it
  has no terminator. Starting `last` as `'\n'` is a deliberate trick: empty
  text never changes it, so no extra line is added, and `"a\n"` ends with a
  newline so it counts one line only.
- The helper module exports three `public` functions and keeps `is_space`
  private, so `main` can only depend on the interface. `main` reaches them
  through `words::`, and the same module can be reused by any other program.
- The built-in text `"the quick brown fox\njumps over the lazy dog\n\nend"`
  has three newlines and an unterminated last line, so 4 lines; 4 + 5 + 1 =
  10 words; and `quick`, `brown` and `jumps` share the longest length, 5.
