## Hint 1

Which package contains the `extern` declarations of sqrt and cos? A requirement belongs to the package that has the declaration.

## Hint 2

Open libs/mathx/project.dcon. A package manifest takes the same `link` field as a project; dpm adds it to every product that depends on the package.

## Hint 3

In libs/mathx/project.dcon add `link = { "m" },` after the version.
