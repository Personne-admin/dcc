# Linking executables and shared libraries

dcc links executables with `ld.lld`, which must be beside `dcc` or on `PATH`.
This page covers native libraries: static archives, ELF shared objects on
x86_64 Linux, and DLL import libraries on Windows. DPM projects configure the
same settings declaratively; see [DPM libraries](dpm-libraries.md).

## Calling a C library

Declare the C function with `@nomangle extern`; such declarations use the C
calling convention of the target:

```dc
module main;

@nomangle extern i32 SDL_GetVersion();

public i32 main() {
    i32 version = SDL_GetVersion();
    println("SDL {}.{}.{}", version / 1000000, (version / 1000) % 1000, version % 1000);
    return 0;
}
```

```sh
dcc main.dc -o main -flibdcext -fwhole-program -lSDL3
```

Exported data works the same way (`@nomangle extern i32 counter;`).

## Static and dynamic executables

| Option | Meaning |
| --- | --- |
| `-l<name>` | Link `lib<name>.so` or `lib<name>.a`; `-l:<file>` names an exact file |
| `-L<dir>` | Search `<dir>` for `-l` libraries at build time |
| `-static` | Link statically; only `lib<name>.a` archives are used |
| `-dynamic` | Link dynamically, preferring `lib<name>.so` |
| `-rpath <dir>` | Add a runtime search directory (DT_RUNPATH); `$ORIGIN` is the executable's directory |
| `-dynamic-linker <path>` | Program interpreter (default `/lib64/ld-linux-x86-64.so.2`, the x86_64 glibc loader) |

Without `-static` or `-dynamic`, an executable stays fully static, exactly as
before, unless it needs a shared object: a `-l` library that resolves to
`lib<name>.so`, a `.so` input file, `-rpath` or `-dynamic-linker`. Then dcc
links a dynamic executable: it gets a `PT_INTERP` program header, a
`DT_NEEDED` entry for each shared library (its soname), calls through the PLT
and data through copy relocations or the GOT. The executable itself stays
position dependent (ET_EXEC); objects need no `-fPIC`. Both backends, `-O0`
to `-O2`, whole-program and separate builds are supported.

Each directory is checked for `lib<name>.so` before `lib<name>.a`. For dynamic
links of x86_64 Linux targets, dcc searches the `-L` directories first, then
the directories in `LIBRARY_PATH`, then the standard system library
directories (`/usr/local/lib/x86_64-linux-gnu`, `/lib/x86_64-linux-gnu`,
`/usr/lib/x86_64-linux-gnu`, `/usr/local/lib64`, `/lib64`, `/usr/lib64`,
`/usr/local/lib`, `/lib`, `/usr/lib`, whichever exist), so an installed
library such as SDL3 needs no `-L`. Static links search only `-L` directories.
A missing library reports the directories that were searched.

Dynamic linking is supported for x86_64 Linux targets with a dynamic loader.
For `x86-elf`, `x86_64-elf` (freestanding) and i8086 images, dcc rejects
`-dynamic`, `-rpath`, `-dynamic-linker` and `-l` libraries that resolve only to
shared objects. `-dynamic`, `-rpath` or `-dynamic-linker` without any shared
library to link is an error, because the result could not start.

## Build time and run time

`-L` and the search directories above decide where the linker finds a library
while building. They are not recorded in the executable. When the program
starts, the system loader searches the `-rpath` directories, then
`LD_LIBRARY_PATH`, then the system cache (`/etc/ld.so.cache`) and default
directories. A library outside those needs `-rpath`, for example
`-rpath '$ORIGIN/lib'` for a `lib` directory shipped next to the executable.
A library the loader cannot find stops the program before `main` with a
message such as `error while loading shared libraries: libshim.so: cannot open
shared object file`.

`--depfile` lists the resolved `-l` libraries, so build tools such as DPM
relink when a library file changes.

## Windows DLLs

PE executables link against import libraries: `-lfoo` finds `libfoo.dll.a`,
`foo.dll.a`, `libfoo.a`, `foo.lib` or `libfoo.lib` in the `-L` directories.
The import library is needed only while building. At run time Windows loads
the DLL itself (`foo.dll`) from the executable's directory, the system
directories and `PATH`, so ship the DLL next to the program. `-rpath` and
`-dynamic-linker` do not apply to PE executables and are rejected. Import
linking is checked from Linux by inspecting the import table; running
programs that load third-party DLLs has not been verified natively.

## Shared libraries

`-shared` writes an ELF shared library (`.so`) with either backend, and a
Windows DLL with the custom backend; `-shared` implies `-fPIC`. Mark exported
functions `@nomangle public`. The LLVM backend does not write DLLs yet. A
shared library cannot be built with `-fwhole-program -flibdcext`.

## Limitations

- The hosted libdcext startup enters `main` directly and exits with
  `exit_group`. In a dynamic executable the loader runs the initializers of
  the shared libraries (including the C library) first, but C `atexit`
  handlers and buffered C stdio are not flushed at exit; call the library's
  own shutdown function (for example `SDL_Quit`) before returning.
- libdcext threads do not set up C library thread state. Call C libraries
  that use thread-local state (such as `errno`) only from the main thread.
- The default interpreter is the glibc loader. Other C libraries need
  `-dynamic-linker`, for example `-dynamic-linker /lib/ld-musl-x86_64.so.1`.
- Position-independent executables (`-pie`) are not produced.
