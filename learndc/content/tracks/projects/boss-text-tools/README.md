# The Newsroom Toolkit

The project has four modules, each with one job:

```text
src/
  words.dc    module words;    cut a text into words
  stats.dc    module stats;    measure an array of words
  report.dc   module report;   print the findings
  main.dc     module main;     own the data and wire the modules together
```

Words are slices of the original text: `Array([]const char)` holds one slice
per word and no copies. The array belongs to `main`; every other function only
borrows it through `Array([]const char)*` or, when it only reads,
`const Array([]const char)*`. Each stage below is checked on its own.

## Stage 1: Read the interns' split

A question about the interns' first attempt. Read it before you write your own.

## Stage 2: Seal the helpers

The interns made everything `public`. The surface of a module is what its
callers need, nothing more:

- `words`: `split` only. `is_separator` is a helper.
- `stats`: `longest`, `total_letters`, `longest_word`, `length_stats` and the
  struct `LengthStats`. `add_one` is a helper.
- `report`: `print_summary`.

Make the two helpers private. The checker brings in `words` and `stats` with
`using ...::*;` and declares its own `is_separator` and `add_one`; a leaked
helper makes the call ambiguous and the build fails.

## Stage 3: Split into words

`split(out, text)` pushes one slice per word onto `out`. The separators are
the space, the tab, the newline, the comma and the full stop, which
`is_separator` already knows. Repeated and leading or trailing separators
create no words, and a text without any separator is one word.

```dc
out.push(text[start..i])?;
```

## Stage 4: Measure the words

- `longest(words)` is the length of the longest word, 0 for no words.
- `total_letters(words)` is the sum of all lengths.

## Stage 5: Count the lengths in a Map

`length_stats(words)` returns `Result(LengthStats, Error)`:

```dc
public struct LengthStats {
    usize used;
    usize common;
    u32 common_count;
}
```

- `used` is how many different lengths occur.
- `common` is the length that occurs most often and `common_count` how many
  words have it. With a tie the checker accepts either length.
- No words gives zeros.

Count with the `Lengths` map from the starter (length to number of words).
The map allocates, so it is released with `defer`, `put` can fail and you
pass that on with `?`. A success is returned as `Result::Ok(stats)`.

## Stage 6: The morning report

`report::print_summary(words)` prints these lines, and `main` creates the
array, splits its built-in text and calls `print_summary`:

```text
words: 16
letters: 60
longest: 5
lengths used: 3
most common length: 3 (8 words)
```

Call `stats::length_stats(words)?` inside `print_summary`; `?` unwraps a
`Result` in a function that returns `Status(Error)`. Do not change the text in `main`.

## Bonus: the longest word

`longest_word(words)` returns the first word of maximum length, or `""` for no
words. It is not needed to defeat the boss.
