# Static, dynamic and shared

A finished program can include the code it uses, or find that code in a
file when it starts. Three words describe the choices, and the lessons so
far already used two of them without naming them.

## Static and dynamic executables

A *static* executable is one self-contained file. A *dynamic* executable
lists the shared libraries it needs, and the system's loader (the
*interpreter*, `ld-linux-x86-64.so.2` on x86-64 Linux) maps them into memory
before `main` runs.

dcc decides by what you link:

```text
dcc app.dc -o app -fwhole-program -flibdcext
file app          ->  ELF 64-bit LSB executable, statically linked

dcc app.dc -o app -fwhole-program -flibdcext -l:libm.so.6
file app          ->  ELF 64-bit LSB executable, dynamically linked,
                      interpreter /lib64/ld-linux-x86-64.so.2
```

A program without native libraries stays static, which is why everything in
the first chapters ran anywhere. As soon as a `lib<name>.so` (or an exact
`-l:name.so`), `-rpath` or `-dynamic-linker` appears, the result is dynamic:
it gets an interpreter, one `NEEDED` entry per library (`readelf -d app`
shows them) and calls through the procedure linkage table.

C libraries are used as shared objects for a practical reason: in a dynamic
executable the loader runs the C library's initializers before `main`, while
the libdcext startup simply enters `main` itself. Pure code that needs no
initialization, such as the math functions or your own archives, can still be
linked statically.

## Static libraries

A static library is an archive of object files, `lib<name>.a`. The linker
copies the members it needs into the executable, so nothing is needed at run
time:

```text
dcc mylib.dc -c -o mylib.o
ar rcs libmylib.a mylib.o
dcc app.dc -o app -fwhole-program -flibdcext -static -L. -lmylib
```

`-static` allows only `.a` archives, and only from the `-L` directories.
`link_mode = dpm::v1::LinkMode::Static` asks the same of a DPM product. A
`-l` that can only be satisfied by a shared object is then an error.

## Shared libraries written in DC

DC can produce the other side too. A module of `@nomangle public` functions
becomes a shared object, exactly the shape a C library has:

```dc example=exports
```

The `main` above is only there so the lesson can run the functions. The
library is the same file without it, in a module named for the library:

```text
dcc shim.dc -shared -o libshim.so
dcc app.dc -o app -fwhole-program -flibdcext -L. -lshim
```

`-shared` implies `-fPIC` and works with either backend. Two restrictions
matter:

- A shared library cannot be built with `-fwhole-program -flibdcext`, so it
  has no standard library behind it: no `println`, no `Array`, no allocation
  from libdcext. Keep it to computation on the arguments, or call C.
- Everything that crosses the boundary uses C types and C names, as in the
  first lessons of this chapter.

## Finding the library when the program starts

`-L` tells the *linker* where `libshim.so` is while building. It is not
recorded in the executable. At startup the loader searches its own places, and
a library outside them stops the program before `main`:

```text
./app: error while loading shared libraries: libshim.so:
       cannot open shared object file: No such file or directory
```

Fix it by recording a run-time search directory, `-rpath`. `$ORIGIN` stands for
the directory of the executable, so a library shipped next to the program is
found wherever the pair is moved:

```text
dcc app.dc -o app -fwhole-program -flibdcext -L. -lshim -rpath '$ORIGIN'
```

In a DPM product the same settings are fields:

```dcon
{
    name = "app",
    entry = "src/main.dc",
    link = { "shim" },
    library_paths = { "vendor/lib" },
    runtime_paths = { "$ORIGIN/lib" },
}
```

`library_paths` is the build-time `-L`; `runtime_paths` is the run-time
`-rpath`. `LD_LIBRARY_PATH` also works for a quick test, but it is
environment, not something the executable carries.

## Common mistakes

- Adding `-L` and expecting the program to find the library at run time.
- Using `-static` with a library that only exists as a `.so`.
- Putting `println` or an `Array` into a shared library.
- Forgetting `@nomangle` on an exported function: C sees a mangled name.
- Shipping the program without the `.so` files it names in `NEEDED`.
