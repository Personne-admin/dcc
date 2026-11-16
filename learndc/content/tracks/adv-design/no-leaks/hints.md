## Hint 1

List the exits of the function: each `push(...)?` can leave early. Ask for
each exit which arrays exist and who owns them. `all` is always yours.
`result` is yours until you hand it to the caller.

## Hint 2

`defer all.deinit();` goes on the line after `all` is created. For `result`
write `bool handed_over = false;` and a deferred block
`defer { if !handed_over { result.deinit(); } }`. Move the explicit
`all.deinit();` away and set `handed_over = true;` right before the return.

## Hint 3

```
Array(u32, A) all = std::array::new_array!(u32, A)(alloc);
defer all.deinit();
Array(u32, A) result = std::array::new_array!(u32, A)(alloc);
bool handed_over = false;
defer {
    if !handed_over {
        result.deinit();
    }
}
...
handed_over = true;
return Result::Ok(result);
```
