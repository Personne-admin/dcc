## Hint 1

Every function visits the elements with `static for item in args { ... }`.
Inside the loop `item` has the element's own type, so `item as f64` turns
any number into a common type for adding or comparing.

## Hint 2

`sum_all` adds into an `f64`, `count_negative` increments a `usize` when
`(item as f64) < 0.0`. `largest_of` starts with `f64 best = first as f64;`
and loops over `rest`. `log_line` prints the tag with `print`, then uses
`bool first = true;` in the loop to choose between `print(" {}", item)?` and
`print(", {}", item)?`, and ends with `println("")?`.

## Hint 3

```
public Status(Error) log_line(T...)(const []const char tag, T args) {
    print("[{}]", tag)?;
    bool first = true;
    static for item in args {
        if first {
            print(" {}", item)?;
            first = false;
        } else {
            print(", {}", item)?;
        }
    }
    println("")?;
    return Status::Ok;
}
```
