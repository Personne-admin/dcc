Private functions are visible to everything in the same module, so the helpers
still work for `mean` and `max`. What changes is who may call them from outside.
Making a name private costs nothing now; making a public name private later breaks
everyone who used it. That asymmetry is the whole argument for starting small.

You can check the surface without reading the code: count the `public`
keywords. The audit does exactly that, and so can a code review.
