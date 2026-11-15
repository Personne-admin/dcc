## Hint 1

`push` is your model. Every function on the stack is generic: it starts with
`name(T)(Stack(T)* self ...)`. The last value sits at index `len - 1`, and
when `len` is 0 there is no value to return.

## Hint 2

`pop` and `peek` both return `Optional(T)`. Return `Optional::None` when
`self.len == 0`; otherwise return `Optional::Some(value)`. `pop` also has to
shrink the stack by decreasing `self.len`. `size` is just `self.len`.

## Hint 3

```dc
public Optional(T) pop(T)(Stack(T)* self) {
    if self.len == 0 {
        return Optional::None;
    }
    self.len -= 1;
    return Optional::Some(self.items[self.len]);
}
```

`peek` is the same without the `-= 1` and reads `self.items[self.len - 1]`.
Give it a `const Stack(T)* self` because it never changes the stack.
