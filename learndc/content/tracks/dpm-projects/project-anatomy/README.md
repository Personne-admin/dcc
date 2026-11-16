# Anatomy of a dpm project

`dcc` compiles the files you name. A real program has more to say: which
files are entry points, where the other modules live, which libraries to link,
how a debug build differs from a release build. `dpm` keeps those answers in
one file, `project.dcon`, and turns them into `dcc` command lines. In this
chapter you work with projects the way a maintainer does: several products,
tests, a layout other people can build.

## project.dcon

Every project has a `project.dcon` at its root. It is a struct literal of
type `dpm::v1::Project`:

```text
dpm::v1::Project {
    name = "harbor",
    version = "0.1.0",
    products = {
        { name = "harbor" },
        { name = "tonnage", entry = "src/tonnage.dc" },
    },
}
```

Only `name` and `version` are required. Fields are separated by commas, text
is quoted, lists are written `{ a, b }`. Check a manifest without building
anything:

```text
dpm check            reads project.dcon and prints diagnostics
dpm check --tree     also prints every field, defaults included
```

The `name` is one identifier: letters, digits and underscores. `version` is
`major.minor.patch`. `dpm init` writes a manifest and a hello-world program
into the current directory, `dpm new NAME` does the same in a new directory.

## Products

A product is one artifact the project builds. A product named `harbor` with no
other fields is an executable whose entry module is `src/main.dc`. Give it
another entry file when the project has several programs:

```text
{ name = "tonnage", entry = "src/tonnage.dc" },
```

The commands:

```text
dpm build            builds every product
dpm build tonnage    builds one product
dpm run tonnage      builds it, then starts it
dpm run harbor -- a b   arguments after -- go to the program
```

With one product `dpm run` needs no name. With several it stops with
`several products; name one of them`.

## Modules and the source root

A module is a file. Its first line names it and the name must match the path
below the source root, which defaults to `src`:

| Module declaration | File |
|---|---|
| `module main;` | `src/main.dc` |
| `module ships;` | `src/ships.dc` |
| `module harbor::ships;` | `src/harbor/ships.dc` |

dpm passes `-Isrc` to dcc, so `import harbor::ships;` in any module of the
project finds that file. The entry file is where dcc starts; the other
modules are pulled in by imports. Two products can import the same modules.

By convention a project puts its own modules under a directory named like the
package (`src/harbor/...`). That does not matter while the project is only a
program, and matters a lot once other projects depend on it.

## What the build leaves behind

Builds write into `.dpm/` inside the project:

```text
.dpm/out/debug/host/harbor        the executable (profile, target, product)
.dpm/out/debug/host/harbor.d      dependency list for rebuild decisions
.dpm/out/debug/host/harbor.stamp  what the product was built from
```

`dpm build` prints `Building NAME` when it compiles and `Fresh NAME` when
nothing changed. `dpm clean` removes `.dpm/out`. Keep `.dpm/` out of version
control (`dpm new` writes a `.gitignore` for that).

## Tests are programs

Files in `tests/` (the `test_dir`) are test programs. `dpm test` builds each
one and runs it; a test passes when the program exits with 0.

```dc example=exit_status
```

Print what failed before returning a non-zero number, so the report explains
itself. Tests can import every module of the project.

## A project without products

A manifest that declares no products is a library: a bundle of modules for
other projects. `dpm build` refuses to build it (`the package has no products`),
because there is nothing to link. Its modules are compiled when a project that
depends on it imports them. Chapter two and five come back to this.

## Common mistakes

- Putting the module name and the path out of step: `module ships;` in
  `src/harbor/ships.dc` fails to resolve.
- Running `dpm run` in a project with two products and forgetting the name.
- Forgetting a comma between fields; dpm reports it at the following line.
- Committing `.dpm/`.
