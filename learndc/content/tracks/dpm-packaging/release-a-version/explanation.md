Dependents ask for a series, not a number. Somebody who wrote `version =
"0.1.0"` will receive 0.1.1 and 0.1.9 when they exist but never 0.2.0; to
move they must edit their manifest, and that edit is the moment they read your
changelog. Bumping only the patch would have delivered the rename to people who
did not ask for it.

The changelog is written for those readers. One line per user-visible change,
newest version on top, older sections kept: the history is the reason to open
the file.
