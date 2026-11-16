# Why the solution works

- **One handle, one defer.** `read_file` and `save_settings` each open one
  file and put `defer f.close();` on the next line. The 3000-iteration checks
  would fail by running out of handles if either forgot, on the success path
  or on the early `?` returns.
- **A custom error says what went wrong.** `apply_toml` returns
  `Status(LoadError)` rather than the library's `TomlError`: the parse can
  succeed and the document still be wrong. `LoadError` names the setting that
  has the wrong kind, and `print_load_error` turns each into a sentence. Printing
  inside the match arms is the portable way to give an enum a text.
- **Layers are just repeated application.** Each file only overwrites the
  keys it contains, so the later file wins and the earlier one fills the gaps.
  Starting every load from `default_settings()` makes the result depend on the
  files alone, not on what the settings held before.
- **Required and optional differ in one check.** A missing defaults file is
  `Unreadable`; a missing override is skipped with `exists`. Testing first is
  fine for a policy decision like this: the real read still reports its own
  error if the file disappears in between.
- **Validate before you write.** `check_settings` runs on the merged
  result, and `run_scenario` only calls `save_settings` when it found no
  problems. The legacy saver truncated the file first and checked later,
  which is the one order that destroys data.
- **Atomic replacement.** Writing `<path>.tmp` and renaming it over the
  target makes the new file appear in one step. A crash or a failed write
  leaves the old file intact, and the cleanup of the temporary file runs on the
  failure paths so nothing is left behind.
- The scenarios in `main` are pure program logic around the library calls:
  every failure is reported and the next scenario runs. Only a failure of
  the program's own file work (the saved file cannot be read back) would
  stop it.
