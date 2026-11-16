## Hint 1

The program imports `report::lines`, so the package that provides it is the direct dependency. The other library is report's business.

## Hint 2

Add one entry to `dependencies` in the harbor manifest: `path:` followed by the directory of the report package, relative to the project root.

## Hint 3

`dependencies = { { location = "path:libs/report" } },` before `products`.
