## Hint 1

Each failure is a different stage. First read the line after `ld.lld: error:`; then, once the program links, read its output.

## Hint 2

The library short name is the file name without `lib` and `.so`. For the symbol, the linker prints `did you mean:` followed by the real name. For the wrong value look at the types in the extern declaration of `cos`.

## Hint 3

In project.dcon the link list is `{ "m" }`. Rename `power` to `pow`. Declare `@nomangle extern f64 cos(f64 x);`.
