# Why the toolkit is built this way

Each module has one reason to change. `words` knows what a separator is, `stats`
knows how to measure words and `report` knows what the output looks like. Because
`is_separator` and `add_one` are private, you can change the separators, or
count with a different helper, and no other module can notice.

The array of words is created once, in `main`, and lent as a pointer. `main` is
the owner, so `defer found.deinit()` sits right beside the creation and runs on
every way out. The words are slices of the built-in text, so nothing is copied;
the slices stay valid because the text outlives the array and nothing grows the
array after it has been filled.

`length_stats` uses a `Map` as a counter. The map is local to the function and
released by its own `defer`, so the caller receives only a small struct and
never sees how it was built. That is the point of a module surface: a public
function with plain inputs and outputs, and as much private machinery behind it as
the job needs.
