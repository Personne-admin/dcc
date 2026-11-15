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
copy DLLs, or run dependency tasks. Install native libraries separately and
configure additional search directories in the consuming project's settings,
for example `settings = { extra_flags = { "-L/path/to/libs" } }`. Runtime shared
library discovery must also be configured for the operating system.

See [tasks and assets](dpm-tasks.md) for explicitly invoked native build tasks.
Dependency settings, tasks and assets are not inherited by consumers.
