## Hint 1

Work from the bottom of the dependency chain up: `read_file` first, then
`apply_toml`, then the messages, then the layers. Each later stage calls the
earlier ones, and each stage has hints of its own in the objective list.

## Hint 2

Three patterns carry the whole project. A file is `open`, then
`defer f.close();` on the next line. An optional key is
`Optional(Value) found = doc.root.get("key");` followed by an `is_some()`
test. A function that can fail in a way the caller wants to name returns a
`Status(LoadError)`, and `return LoadError::Syntax;` is the failure.

## Hint 3

`load_layers` is two calls of the same helper (read a file into an Array,
apply its text), the second one guarded by `exists`. `run_scenario` is a
sequence of early returns: print the heading, load, validate, then save and
echo. Keep each early return a plain `return Status::Ok;` after printing why.
