## Hint 1

`profiles` is a project-level list, next to `products`. Each entry is a struct with `name`, `inherits` and `settings`.

## Hint 2

`inherits` takes a `ProfileBase` value (`ProfileBase::Debug` or `ProfileBase::Release`). `bounds_check` is a `Tri` value, and `settings` is a struct in braces.

## Hint 3

`profiles = { { name = "checked", inherits = ProfileBase::Release, settings = { bounds_check = Tri::On } } },`
