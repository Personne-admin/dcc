# Dependencies, the lock file and the cache

A project that grows past one team's patience splits into packages: a
library that several programs share, a binding to a native library, someone
else's code. dpm's job is to find those packages, make sure every build uses
the same bytes, and hand the right `-I` directories to dcc.

## Declaring a dependency

Dependencies are a list in `project.dcon`:

```text
dpm::v1::Project {
    name = "harbor",
    version = "0.1.0",
    dependencies = {
        { location = "path:libs/units" },
    },
    products = { { name = "harbor" } },
}
```

`location` says where the package comes from. This chapter works with
`path:` locations: a directory with its own `project.dcon`, usually inside the
same workspace. The same list also takes `github.com/user/repo`, a git URL, a
tarball URL or a registry alias; those are covered below, but they need the
network the first time.

Two rules about `path:`:

- The path is relative to the project that declares it, not to where you
  happen to run dpm. A library in `libs/report` that depends on its neighbour
  writes `path:../units`.
- A path dependency takes no `version`; the directory is the version.

`dpm add path:libs/units` edits the list for you (keeping comments and layout)
and writes the lock; `dpm remove path:libs/units` undoes it.

## What a package contributes

A package without products is a source library. Its `source` directory
(default `src`) becomes an import root for everyone who depends on it:

```text
libs/units/project.dcon
libs/units/src/units/convert.dc     module units::convert;
```

The consumer writes `import units::convert;`. dpm gives dcc one `-I` per
package in the build, and transitive packages are included: if `report`
depends on `units`, a project that depends on `report` can also reach `units`
modules. By convention a package puts its modules under a directory named like
the package, so two packages never fight over a module name.

## The lock file

The first build with dependencies writes `project.lock.dcon` next to the
manifest. It records, for every package in the build:

```text
location, version  the identity of the package
source             where it was fetched from (git URL, tarball, path:...)
commit             the exact git commit, for git sources
hash               sha256 of the unpacked source tree (empty for path: packages)
dependencies       the locations it needs
```

A real excerpt for a git package:

```text
{
    location = "github.com/Personne-admin/sdl3-dc",
    version = "0.1.0",
    source = "git+https://github.com/Personne-admin/sdl3-dc.git",
    commit = "ba36329ff6908a205de2e4e837993b071f6e3c8d",
    hash = "sha256:f7136ef844c8f98525b1a69be81293152ab06c829c703c0f80bf4f0c8bf3efb1",
}
```

Commit the lock file. It is what makes `0.1.0` mean the same source tree on
your machine, a colleague's and the build server: a moved tag or a changed
tarball no longer matches the hash and dpm stops with an integrity error
(exit code 4) instead of building different code. dpm rewrites the lock when
the manifest's requirements change; never edit it by hand.

```text
dpm lock           resolve and write project.lock.dcon
dpm lock --check   write nothing; exit 4 if the lock would change
dpm fetch          download everything the lock names
```

`dpm lock --check` is the continuous-integration form: it fails when somebody
changed a dependency and forgot to commit the lock.

## Versions

A dependency on a remote package has a minimum version:
`{ location = "github.com/user/pkg", version = "1.2.0" }`. dpm picks the
lowest version that satisfies every requirement in the same series (the same
major version, or the same minor version while the major is 0). Newer releases
are never picked up by accident; `dpm update` raises the minimums to the newest
release of the same series and rewrites the lock. Two packages that need
different series of the same dependency are a resolution error (exit code 4).

## The cache and offline work

Downloaded packages are unpacked, read-only, into one cache shared by all
your projects:

```text
dpm cache path     print the cache directory (DPM_HOME, else ~/.cache/dpm)
dpm cache verify   re-hash every cached tree against its directory name
```

Editors open the sources of a dependency from there. `--offline` (or
`DPM_OFFLINE=1`) forbids the network: everything must already be in the
cache, otherwise dpm exits with code 5. `dpm fetch` fills the cache from the
lock so later builds work offline. Path dependencies never touch the cache;
they are read in place.

## Seeing the graph

```text
dpm tree           the resolved packages as a tree
dpm why LOCATION   which packages require LOCATION
```

```text
app@0.1.0
`-- path:libs/report@0.3.0
    `-- path:libs/units@1.0.0
```

## Reading the messages

| Message | Usual cause |
|---|---|
| `cannot resolve path:libs/report/libs/units: cannot read ...` | a relative path written from the wrong directory; the message shows where dpm looked |
| `path dependencies take no version` | a `version` field on a `path:` entry |
| `could not resolve module X::y` | the package that provides it is not in `dependencies` (or its namespace directory is named differently) |
| `module declaration A does not match resolved path B` | the `module` line and the file path disagree |
| `unknown path X::f` after the import resolved | `f` exists but is not `public` |
| exit code 5 | something was not in the cache while `--offline` |

## Common mistakes

- Writing a path from the project root inside a library's own manifest.
- Forgetting to commit `project.lock.dcon`, or committing a lock that no longer matches.
- Expecting `dpm build` to fetch a newer release: it never does; `dpm update` does.
- Naming a library's modules `convert` instead of `units::convert`.
