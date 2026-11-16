# Profiles and settings

Every `dpm build` is one dcc command per product. What goes into that command
is decided by settings, and settings come from layers.

## The two built-in profiles

`--profile` selects a named group of defaults; without it dpm uses `debug`.
You can see the commands with `-v`:

```text
$ dpm build -v
... dcc src/main.dc -o .dpm/out/debug/host/harbor -Isrc ... -O0 -fwhole-program -flibdcext -g -fbounds-check
$ dpm build --profile release -v
... dcc src/main.dc -o .dpm/out/release/host/harbor -Isrc ... -O2 -fwhole-program -flibdcext
```

- `debug`: no optimization (`-O0`), debug information (`-g`) and bounds
  checks (`-fbounds-check`): easy to debug, indexing errors trap.
- `release`: `-O2`, no debug information, no bounds checks.

Both pass `-fwhole-program` and `-flibdcext` for a hosted build: dpm compiles a
product with one dcc command, and that command includes the other modules of
the import graph in whole-program mode. Output goes to
`.dpm/out/<profile>/<target>/`, so both builds can sit side by side.

## The Settings struct

The same struct appears in four places:

```text
settings = {
    optimize = Optimize::O2,
    bounds_check = Tri::On,
    backend = Backend::Custom,
}
```

A few fields: `target` (`Target::Host`, `X86_64Elf`, ...), `backend`
(`Backend::Llvm` or `Custom`, passed as `-fbackend`), `optimize` (`O0`, `O1`,
`O2`, `Size`), `debug_info`, `bounds_check`, `restricted_check`, `libdcext`,
`extra_flags`. The switches are `Tri` values: `Default` leaves the choice to
dpm, `On` and `Off` force it. A field left at its default overrides nothing.

`extra_flags` is a list of extra dcc arguments appended last; it concatenates
across layers instead of overriding, and flags that dpm itself owns are
rejected.

## Layers

From lowest to highest priority:

1. the defaults of the selected profile (debug or release)
2. `settings` of the project
3. `settings` of the selected custom profile
4. `settings` of the product
5. command-line options: `-O`, `--target`

So a product that must always be optimized can say so, whichever profile is
selected. Command-line options win over everything: `dpm build -O 1`.

## A custom profile

A profile starts from `debug` or `release` and applies its own settings. The
names `debug` and `release` are reserved.

```text
profiles = {
    {
        name = "checked",
        inherits = ProfileBase::Release,
        settings = { bounds_check = Tri::On },
    },
},
```

`dpm build --profile checked -v` now shows `-O2 ... -fbounds-check`: release
speed with release's missing debug information, but with array checks. A
profile name that does not exist is an error (`unknown profile 'nope'`).

## A per-product setting

```text
products = {
    { name = "crunch", entry = "src/crunch.dc", settings = { optimize = Optimize::O2 } },
    { name = "fleet", entry = "src/fleet.dc" },
},
```

`crunch` is compiled with `-O2` even in a debug build, while `fleet` keeps the
profile's `-O0`.

## Seeing the commands without building

```text
dpm compile-commands --profile release
```

prints the dcc commands of every product as `compile_commands.json` entries,
without building anything. Editors and the language server use it to analyse
your files with the same import roots, target and flags the build uses.

## Common mistakes

- Declaring a profile named `release`: it is built in; use another name and set `inherits`.
- Writing `Optimize::Fast` or another name that is not in the enum; the diagnostic says `unknown path`.
- Putting a setting at the project level and wondering why a product ignores it: a product's own `settings` take priority.
- Expecting `-O` on the command line to be remembered: it applies to that command only.
