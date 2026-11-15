# Close the till on every exit

A shop opens its till for every checkout and must close it again, whatever
happens. `checkout` in `src/main.dc` has three ways out, and only the last
one remembers to close the till:

```dc
Status(Error) checkout(i32 price, i32 paid) {
    println("till opened")?;
    if price == 0 {
        println("nothing to pay")?;
        return Status::Ok;
    }
    ...
    println("till closed")?;
    return Status::Ok;
}
```

## Your task

1. Check the program and compare its output with the expected output below.
   Work out which checkouts leave the till open.
2. Fix it with a single `defer` written right after `till opened`, and delete
   the old `println("till closed")?;` line so the message is not printed
   twice.

The program must print exactly:

```text
till opened
nothing to pay
till closed
till opened
10 more needed
till closed
till opened
change: 20
till closed
```

You could also copy the closing line in front of every `return`. It would
print the same thing, but the next person who adds an early return would
have to remember it too. With `defer`, nobody has to.
