# DPM libraries

A package without `products` is a source library. Its `source` directory
(default `src`) becomes an import root for consumers. For example,
`src/sdl3/lib.dc` declares `module sdl3::lib;` and consumers write
`import sdl3::lib;`. The package's `name` is its import namespace.

## Native library requirements

Declare native libraries needed by the package at project level:

```dcon
dpm::v1::Project {
    name = "sdl3",
    version = "0.1.0",
    link = { "SDL3" },
}
```

A consumer only needs the dependency:

```dcon
dpm::v1::Project {
    name = "game",
    version = "0.1.0",
    dependencies = {
        { location = "github.com/you/dc_sdl3", version = "0.1.0" },
    },
    products = { { name = "game" } },
}
```

DPM passes `-lSDL3` when linking the consumer's products and tests. Requirements
propagate through transitive dependencies. They apply to every product using
that dependency set, even when its entry module does not import the binding.
Dev-only packages contribute requirements only when dev dependencies are enabled
(for tests and `dpm run --dev`).

Product-level `link` remains local to that product. Link arguments are emitted
in this order: product libraries, the root project's libraries, then dependency
libraries with each package before its dependencies. Unrelated packages are
ordered by location. Order within a `link` list is preserved, including repeats,
so packages can express ordered native archive requirements. Requirements are
not deduplicated across packages. Cyclic package groups use location order to
break ties; native archive cycles may need explicit repeated libraries in a
product's `link` list.

The root project's `link` also applies to its own tests, which do not inherit
product-level link settings. Changing a dependency's link requirements changes
the build command and invalidates its build stamp.

These declarations name libraries; they do not install SDL, build C sources,
copy DLLs, or run dependency tasks. Install native libraries separately.

## Search paths and shared libraries

A product configures where its libraries are found and how it is linked:

```dcon
dpm::v1::Project {
    name = "game",
    version = "0.1.0",
    products = {
        {
            name = "game",
            link = { "SDL3" },
            library_paths = { "vendor/sdl3/lib" },
            runtime_paths = { "$ORIGIN/lib" },
            link_mode = dpm::v1::LinkMode::Default,
        },
    },
}
```

- `library_paths`: directories searched at build time (`dcc -L`), relative to
  the package root or absolute. Libraries installed in the system library
  directories, such as a distribution's SDL3, need none.
- `runtime_paths`: directories the program searches when it starts
  (`dcc -rpath`, DT_RUNPATH). `$ORIGIN` is the directory of the executable.
  Windows executables load DLLs from their own directory and `PATH` instead.
- `link_mode`: `Default` links statically unless a library resolves to a
  shared object (`libSDL3.so`), `Static` uses only `lib<name>.a` archives, and
  `Dynamic` prefers shared objects. Dynamic linking is available for x86_64
  Linux executables.

Changing these fields, or replacing a linked library file, relinks the
product. See [linking](linking.md) for the search order, the difference
between build-time and run-time library discovery, Windows import libraries
and the current limitations.

See [tasks and assets](dpm-tasks.md) for explicitly invoked native build tasks.
Dependency settings, tasks and assets are not inherited by consumers.
