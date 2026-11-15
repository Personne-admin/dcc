# Why the solution works

```dc
public i32 area(Rect r) {
    return r.width * r.height;
}

public void grow(Rect* r, i32 amount) {
    r.width += amount;
    r.height += amount;
}

public Rect square(i32 side) {
    return Rect { width = side, height = side };
}
```

- `area` takes a `Rect` by value. The function gets its own copy, which is all
  a read-only calculation needs.
- `grow` takes `Rect*`. Writing through the pointer modifies the caller's
  rectangle, so `hall.grow(2)` in `main` turns 4x3 into 6x5. With a by-value
  parameter the change would be made to a copy and thrown away.
- `hall.area()` and `hall.grow(2)` are UFCS calls: the compiler looks for a
  function named `area` or `grow` whose first parameter is a `Rect` (or a
  pointer to one) and passes the receiver, taking its address when needed.
  The functions must be `public` to be callable from another module.
- `square` returns a struct literal, which names every field it initializes.
