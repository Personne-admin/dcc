# Why the solution works

`std::slice::eq(a, b)` first compares the lengths and then the characters one
by one, so `"adm"` is not equal to `"admin"` and two texts stored in different
places are equal when they read the same. The solution changes only the
comparisons; counting and early return work exactly as in the earlier text
lesson:

```dc
for name in names {
    if std::slice::eq(name, who) {
        total++;
    }
}
```

`==` still works for numbers, characters, booleans and enum variants, because
for those the value itself is the thing being compared. A slice is only a
description of where some values live, which is why DC makes you choose
between "same place" and "same characters" instead of guessing.

The `import std::slice;` line makes the module `std::slice` available. The
function is then reached by its full path, `std::slice::eq`.
