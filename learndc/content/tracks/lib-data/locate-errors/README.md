# Point at the syntax error

A compiler tells you where it gave up: the line, and a caret under the
character. `std::json::parse` returns only the kind of error, so a tool that
wants to help has to find the position itself.

The `Scanner` is the tool. It reads the text event by event:

```dc
Scanner scanner = std::json::scan(text);
Result(Located(Event), JsonError) step = std::json::next(&scanner);
```

When `next` returns an error, `scanner.at` is a `Position` with `line` and
`column` (both from 1) and `offset` (the byte index in the text). When it
returns `Event::End`, the whole text was valid.

## Your task

`src/main.dc` checks four texts and prints a report for each. `main` and
`check` are finished. Implement:

1. `first_error(text)` runs a scanner over `text` until `Event::End` or an
   error. It returns `Optional::None` for valid text and
   `Optional::Some(scanner.at)` for the first error.
2. `print_context(text, at)` prints the line of `text` that contains the
   error, indented by four spaces, and under it a line with four spaces, then
   `column - 1` spaces and a `^`.

The first byte of the offending line is `at.offset - (at.column - 1)`: the
column counts bytes from the start of the line, so subtracting it from the
offset walks back to the line start. The line ends at the next `'\n'` or at
the end of the text.

The program must print:

```text
text 1: valid
text 2: line 3, column 10
      "age": ,
             ^
text 3: line 1, column 6
    [1, 2
         ^
text 4: line 2, column 1
    {"b": 2}
    ^
```

(Text 2's context line is indented by four spaces plus the two spaces that
belong to the text itself.)

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- The scanner is only used inside `first_error`; nothing needs releasing.
