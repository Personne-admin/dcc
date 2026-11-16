Nothing in dcc cares what a module is called as long as the file path and the
`module` line agree. The package-name convention exists for the day somebody
else depends on you: the import namespace is shared by every package in a
build, so `ships` would collide with another package's `ships`, while
`harbor::ships` belongs to harbor alone.

The `public import` matters for the same reason as `public` itself: a caller of
`harbor::report::print_line` must be able to name the type of its parameter,
so the module that exposes the signature has to expose the import too.
