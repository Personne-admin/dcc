# Designing a reusable library

A library is code written for people you do not control, including yourself in
six months. Everything it exposes is a promise; everything it hides is free to
change. This chapter is about keeping the promises small and visible.

## Layout

```text
libs/tabulate/
    project.dcon              name, version, description, license; no products
    README.md                 what it is, a usage example, the API list
    CHANGELOG.md              what changed in each version
    src/tabulate/cell.dc      module tabulate::cell
    src/tabulate/table.dc     module tabulate::table
    tests/tabulate_test.dc    a test program: exit code 0 means pass
app/ (or the workspace root)
    project.dcon              dependencies = { { location = "path:libs/tabulate" } }
    src/main.dc               the consuming application
```

- The package name is also its directory under `src/`. Modules are
  `<name>::something`, so two packages can never collide.
- The manifest has no products: nothing in the library is an executable. dpm
  builds it as part of whoever depends on it.
- Tests live in `tests/` (the `test_dir`). Inside the library directory,
  `dpm test` builds and runs every file there against the library's own
  modules. A test program prints what it checks and returns the number of
  failures.
- Keep a demo or example as a product of the consuming application, not
  of the library.

## A small public surface

Declarations are private unless marked `public`. Every `public` is a promise to
keep that name, signature and behaviour.

- Expose the operations a user wants, not the steps you used to build them.
  A table printer needs `print_table`; padding helpers stay private.
- Prefer a few functions that take plain values (slices, integers, structs)
  over many that expose internal state.
- If a public function mentions a type from another module, that module must be
  imported with `public import` so callers can name it.
- Name things in the module's own terms: `tabulate::stats::mean`, not
  `tabulate::util::do_it`.

You can review the surface mechanically: every `public` in the source is an
entry on the list in the README. If one is not, either document it or make it
private.

## Versions

The manifest has a `version` (`major.minor.patch`, an optional pre-release
suffix). dpm resolves dependencies by series: the lowest version satisfying the
requirements within the same major version, or within the same minor version
while the major is 0. So for a package before 1.0:

| Change | Bump | Why |
|---|---|---|
| bug fix, nothing visible changes | patch (0.1.0 to 0.1.1) | users in the 0.1 series get it |
| removed or renamed public name, changed signature | minor (0.1.x to 0.2.0) | a new series: dependents must choose to move |
| new public function, compatible | patch before 1.0 (or minor after) | existing code still compiles |

Use `0.y.z` while the design is still moving. `1.0.0` is a statement that the
public API is stable, so do not write it on the first day. Record each bump in
`CHANGELOG.md`: version, date or heading, and one line per user-visible change.

## Metadata

`description` (one line), `license` (an SPDX expression such as `MIT`),
`requires_dcc` (the oldest compiler that builds the package) and `alias`
(a short registry name) help the people who find the package. They are
informational except `requires_dcc`.

## From workspace to published

Path dependencies are the way to develop a library next to its first user.
A package that is published (`dpm publish` writes the registry record for a
tagged, clean git tree) may not depend on `path:` packages: others cannot see
your directories. Publishing is out of scope here; the layout above is what you
would publish.

## Common mistakes

- Making every helper public "just in case".
- Naming a module `util` or `table` instead of `<package>::table`.
- Starting at 1.0.0, then having no way to break the API.
- Breaking a function and bumping the patch number.
- Tests that import nothing from the library, or no tests at all.
- A README that never shows a complete example.
