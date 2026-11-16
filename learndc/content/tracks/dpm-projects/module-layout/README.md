# Put the modules under the package name

*Continues the harbor project of second-product: its ship code moves under the package name.*

The harbor project grew in one module, `src/legacy.dc`. Soon other projects
will want to import the ship code, and a module called `legacy` would collide
with anything else that uses that name. The convention for a package is that
its modules live under a directory with the package's name:
`src/harbor/ships.dc` is `module harbor::ships;`.

`src/main.dc` is already written against the new layout. It does not compile
until the two modules it imports exist.

## Your task

1. Create `src/harbor/ships.dc` (`module harbor::ships;`) holding the `Ship`
   struct, the `FLEET` table and `total_tons`.
2. Create `src/harbor/report.dc` (`module harbor::report;`) with the public
   function `print_line(Ship ship)` that prints one table row. It must import
   `harbor::ships` to name the `Ship` type.

Inside a module of the `harbor` namespace, an import of `harbor::ships` drops
the shared prefix: the module is reached as `ships::`, so the parameter type
is written `ships::Ship`. (Callers outside `harbor` still write the full path.)

`src/legacy.dc` stays as a reference; copy from it. Declarations must be
`public` to be used across modules, and a public signature that mentions a
type from another module needs that module imported with `public import`.

The program prints:

```text
Aurora 120 t
Brisa 80 t
Corvo 200 t
Delta 45 t
total 445 t
```

## Acceptance criteria

- The project builds with `dpm build`.
- Both new files exist under `src/harbor/` and declare the right module names.
- The output is exactly the table above.
