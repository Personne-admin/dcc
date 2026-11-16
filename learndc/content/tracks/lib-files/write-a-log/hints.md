## Hint 1

`write_only()` creates the file and truncates it. For a log you want the
opposite of truncating: set `truncate = false` and `append = true` on the
flags. Creation still happens, because `create` is on in `write_only()`.

## Hint 2

`print_numbered` and `count_level` both start with
`Array(char) text = {}; defer text.deinit(); read_text(path, &text)?;`, then
split `text.items() as []const char` on `'\n'` and loop until `next` is
empty. A line of length 0 is the piece after the last newline: skip it in
`print_numbered`.

## Hint 3

```dc
char[16] tag = {};
[]const char wanted = std::fmt::format_buf(tag[0..16], "[{}]", level)?;
...
if std::slice::starts_with(next.unwrap_some(), wanted) {
    count += 1;
}
```

`format_buf` returns the part of the buffer it filled. The tag array must stay
alive while `wanted` is used, so declare both in the same function.
