## Hint 1

Both functions are one `static if` chain. A chain is `static if cond { ... }
else static if cond { ... } else { ... }`, and every branch ends in a
`return` of a text literal.

## Hint 2

For `kind_of`, join the types of one group with `||`:
`static if T == i8 || T == i16 || T == i32 || T == i64`. Use one
`else static if` for the unsigned types and one for the floats; the final
`else` returns `"other"`. For `size_class`, compare `sizeof(T)` with `1` and
then with `4`.

## Hint 3

```dc
public []const char size_class(T)() {
    static if sizeof(T) == 1 {
        return "tiny";
    } else static if sizeof(T) <= 4 {
        return "small";
    } else {
        return "big";
    }
}
```

`kind_of` has the same shape with type comparisons in the conditions.
