# Depend on a local library

*Continues the harbor project of chapter one: it now converts tonnage with a local library.*

The harbor program wants to print every ship's tonnage in kilograms. The
conversion lives in a library package of its own, `libs/units`, which already
exists in your workspace:

```text
libs/units/project.dcon              name = "units", no products
libs/units/src/units/convert.dc      module units::convert;
```

`src/main.dc` imports `units::convert`, but the project does not know the
package yet: the build stops with `could not resolve module units::convert`.

## Your task

Make `harbor` depend on `libs/units`. You can edit `project.dcon` by hand or
run `dpm add path:libs/units`. Then run `dpm build` and read the new
`project.lock.dcon`.

The program prints:

```text
Aurora 120 t = 120000 kg
Brisa 80 t = 80000 kg
Corvo 200 t = 200000 kg
Delta 45 t = 45000 kg
```

## Acceptance criteria

- `project.dcon` lists `path:libs/units` in `dependencies`.
- `dpm build` succeeds and the program prints the table above.
