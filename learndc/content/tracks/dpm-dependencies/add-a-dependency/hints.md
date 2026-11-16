## Hint 1

The error names a module no dependency provides. Dependencies are a list in project.dcon: `dependencies = { { location = "..." } },`.

## Hint 2

A local package is `path:` followed by the directory, relative to the project that declares it. Here the directory is libs/units.

## Hint 3

Add `dependencies = { { location = "path:libs/units" } },` before `products`, or run `dpm add path:libs/units` in the workspace.
