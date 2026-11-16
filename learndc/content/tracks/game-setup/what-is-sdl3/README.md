# What SDL3 is and how DC talks to it

This path teaches game programming with SDL3, written in DC. This first
lecture is about the ground under every game: what SDL3 is, how a DC program
reaches a library written in C, what has to be installed, and who is
responsible for cleaning up what.

## What SDL3 is

A game needs a window, a way to draw into it, a stream of keyboard and mouse
events and a clock. Each operating system offers these in its own way. SDL3
(Simple DirectMedia Layer) is a C library that hides the differences behind
one small API: the same calls open a window on Linux, Windows and macOS.

SDL3 does not decide what your game is. It gives you *building blocks*:

| Area | What you get | Where it appears in this path |
|---|---|---|
| Video | windows, a 2D renderer, textures | drawing, sprites |
| Events | quit, keyboard, mouse | the game loop |
| Timing | milliseconds and a high resolution counter | frame timing |
| Surfaces | pixel buffers in memory, BMP files | sprites |

Everything else, the rules of your game, is ordinary DC code that you write.

## A native library, called from DC

SDL3 is not written in DC. It is a *shared library*: a file (on Linux
`libSDL3.so`) that is installed on the machine and loaded when a program
starts. DC calls C functions with `@nomangle extern`, which says "this
function exists elsewhere, with this exact C name and the C calling
convention":

```dc
@nomangle extern i32 SDL_GetVersion();
```

You could declare the few hundred functions you need by hand. The *bindings*
already did that: a package called `sdl3` contains those declarations,
together with SDL's structs, enums and constants, as ordinary DC modules.

```dc
import sdl3;

i32 packed = sdl3::SDL_GetVersion();
```

SDL packs its version into one number: `major * 1000000 + minor * 1000 +
patch`. Decoding it is plain integer arithmetic:

```dc example=version_decode
```

## The bindings are a DPM package

A package is a dependency, and dependencies belong in the project's
manifest. A game project lists the bindings in `project.dcon`:

```text
dpm::v1::Project {
    name = "mygame",
    version = "0.1.0",
    products = {
        { name = "mygame", entry = "src/main.dc" },
    },
    dependencies = {
        { location = "github.com/Personne-admin/sdl3-dc", version = "0.1.0" },
    },
}
```

Next to it sits `project.lock.dcon`. The lock records the exact commit and a
content hash of every package, so everybody who builds the project gets the
same bindings. The first time a package is needed, dpm downloads it into the
*dpm cache* on your machine. After that, builds can run `dpm build --offline`:
no network, only what is in the cache and the lock. That is how learndc builds
every lesson of this path.

The package's own manifest says which native library its code needs:
`link = { "SDL3" }`. dpm passes that to the compiler as `-lSDL3`, so the
linker looks for `libSDL3.so`.

## Why the executable becomes dynamic

Until now every DC program you built was *static*: one self-contained file.
A program that needs `libSDL3.so` cannot be. When the compiler sees that
`-lSDL3` resolves to a `.so` file, it produces a *dynamic* executable
(`docs/linking.md` in the compiler repository has the full story):

- the file names the library it needs (`DT_NEEDED`, here `libSDL3.so.0`),
- it names a program interpreter, the system's dynamic loader, which starts
  first, finds the library and maps it before `main` runs,
- calls into SDL go through a table filled in by the loader.

Two search paths are involved and they are different things. At *build* time
the linker finds `libSDL3.so` in `-L` directories, then `LIBRARY_PATH`, then
the standard system directories; an installed SDL3 needs no `-L`. At *run*
time the loader looks in the executable's `-rpath` directories, then
`LD_LIBRARY_PATH`, then the system library cache. Installing SDL3 the normal
way satisfies both.

## When SDL3 is missing

The path never asks you to guess. Two commands check the prerequisites:

```text
learndc setup sdl3          print the guide and check the library and the bindings
learndc setup sdl3 --fetch  also download the bindings into the dpm cache (needs the network)
learndc doctor              reports SDL3 as an optional item
```

If the library is not installed, learndc shows a *setup needed* result with
instructions instead of a wall of compiler errors, and validation of these
lessons is skipped. If you try anyway, the linker is honest about it:

```text
dcc: error: backend: linker error:
ld.lld: error: unable to find library -lSDL3
library search path: none; pass -L <dir>; no lib<name>.so was found there
or in the system library directories (...) either
```

And if the program was built but the library later disappears, the loader
stops it *before* `main`: `error while loading shared libraries: libSDL3.so.0:
cannot open shared object file`. No DC code is involved in that message.

## Who owns what

C has no destructors, so the rule is always: *whoever creates a handle
destroys it*. The bindings' `docs/ownership.md` states three kinds of
pointers:

- **Borrowed strings** such as `SDL_GetError()` and `SDL_GetRevision()` point
  into SDL's own memory. Read them at once, never free them. The next SDL call
  may overwrite them.
- **Caller-owned allocations** (`SDL_LoadFile`, `SDL_GetPrefPath`, ...) are
  freed with `SDL_free`.
- **Opaque handles** (window, renderer, texture, surface) come from a
  `Create` function and go back to the matching `Destroy` function.

Handles are destroyed in the reverse order of creation, which is exactly what
`defer` gives you:

```dc example=cleanup_order
```

## The shape of every SDL program

Every SDL3 program in this path has the same skeleton:

```dc
sdl3::SDL_SetMainReady();
if !sdl3::SDL_Init(sdl3::SDL_INIT_VIDEO) {
    return 1;
}
defer sdl3::SDL_Quit();
```

`SDL_SetMainReady` tells SDL that your program has its own `main` (the
bindings' README requires it before `SDL_Init`). `SDL_Init` returns `bool`:
`false` means failure, and `SDL_GetError()` says why. Every SDL call that can
fail returns `false` or `null`, so *every call is checked*, and a failure
undoes whatever succeeded before it:

```dc example=check_every_call
```

## Common mistakes

- **Forgetting `SDL_SetMainReady()` or ignoring the result of `SDL_Init`.**
  The next call then fails in confusing ways.
- **Passing `SDL_GetError()` to `println`.** It is a C string
  (`const char*`), not a DC slice. The next lesson converts it.
- **Destroying in creation order.** Destroy the renderer before its window
  and `SDL_Quit` last.
- **Calling `SDL_Quit` twice, or `SDL_free` on a borrowed string.** Pair each
  creation with exactly one release.
- **Expecting a window in a harness or on a server.** Without a desktop
  session there is nothing to show a window on. The checks in this path use
  SDL's `offscreen` video driver, which runs the real SDL calls without a
  screen. They prove your code works; they do not prove how it looks on your
  desktop. `learndc run` shows the real window.
