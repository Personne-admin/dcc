# Comparing text

A guest list is a list of names, and names are text: `[]const char`. The
program in `src/names.dc` checks names with `==`, and it does not build.
The compiler says:

```text
error: slice values cannot be compared for equality
       (use .ptr/.len comparison or a library function)
```

## Why == is refused

A slice is a pointer plus a length. Two slices with the same characters
usually sit at different addresses, as in this program:

```dc
[]const char a = "hi";
[]const char b = "hi";
```

Comparing the pointers answers "are these the very same bytes in memory?",
which is almost never what you mean. Comparing the characters answers "do
these two texts read the same?". Because the first would be a surprising
answer to the second question, DC does not let `==` guess: it stops and asks
you to say which one you want.

## The library function

The standard library compares the contents for you:

```dc
import std::slice;

bool same = std::slice::eq(a, b);   // true: same length, same characters
```

Put the `import std::slice;` line at the top of the module, below its
`module` line. `eq` is `true` only when both slices have the same length and
every character matches, so `"adm"` and `"admin"` are different.

## Your task

Open `src/names.dc`. Add the import and replace each of the three `==`
comparisons with `std::slice::eq(...)`:

- `is_admin(name)` is true for exactly the text `admin`.
- `count_name(names, who)` counts how many entries of the list equal `who`.
- `has_name(names, who)` is true when at least one entry equals `who`.

`names` is a `[]const []const char`: a slice whose elements are themselves
texts. `for name in names` hands you one text at a time.

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- Running it prints exactly:

```text
admin: true
guest: false
ann appears 2 times
zed appears 0 times
bob is on the list: true
zed is on the list: false
```
