## Hint 1

Each diagnostic names a file and a position. Start with the first message you
see and ignore everything else until it is gone; the later errors may be
consequences or may only appear once the earlier layer succeeds.

## Hint 2

The three problems are: a path in `project.dcon` that points to a file which
does not exist, a `module` line that does not match the name of the file it
is in, and a function that `main` cannot see although the module is imported.
Compare each message with the file listing and with the file it points to.

## Hint 3

In `project.dcon` change `entry = "src/app.dc"` to `"src/main.dc"`. In
`src/stats.dc` change `module stat;` to `module stats;`. Then add `public` in
front of `average`; `largest` is already public.
