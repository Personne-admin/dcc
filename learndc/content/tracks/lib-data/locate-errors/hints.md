## Hint 1

A scanner is a loop: call `std::json::next(&scanner)`, stop when the result
`is_err()`, and also stop when the event is `Event::End`. Every other event
means "keep reading".

## Hint 2

For `print_context`, compute `start` as `at.offset - (at.column - 1) as usize`.
Move a second index `end` forward from `start` while `end < text.len` and
`text[end] != ('\n' as char)`. Print `text[start..end]`. For the caret, print
four spaces, then a loop that prints a space `at.column - 1` times, then `^`.

## Hint 3

```dc
while true {
    Result(Located(Event), JsonError) step = std::json::next(&scanner);
    if step.is_err() { return Optional::Some(scanner.at); }
    match step.unwrap().event {
        Event::End => return Optional::None,
        _ => {},
    }
}
```

Use `print` for the spaces and `println("^")` for the last piece. Remember `?`
on every print.
