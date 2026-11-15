# Modules and projects

Until now every program has been one file. That works for a page of code and
stops working when a program grows: names collide, the helpers you meant to
keep to yourself are visible everywhere, and nobody can tell which part does
what. DC lets you split a program into *modules* and decide, per
declaration, what the rest of the program may use.

## One file, one module

Here is a whole program in one file:

```dc example=one_file
```

`average` and `verdict` are helpers; `main` is the program. Split by role, the
project gets one file per module:

```text
src/
  grades.dc    module grades;   average, verdict
  main.dc      module main;     main
```

The first line of every file is `module <name>;`, and the name has to match
the file: `src/grades.dc` holds `module grades;`. A file in a subfolder
includes the folder in its path, `src/text/words.dc` holds
`module text::words;`. If the declaration and the path disagree, the compiler
says `module declaration ... does not match resolved path ...`.

## Public and private

Every top-level declaration, whether function, struct, enum or constant, is
private by default. Only the module itself can use it. Write `public` as the
first word to share it:

```dc
module grades;

public i32 average(i32 a, i32 b, i32 c) {
    return total(a, b, c) / 3;
}

i32 total(i32 a, i32 b, i32 c) {
    return a + b + c;
}
```

`average` is the surface of the module: what other files may call. `total` is
a private helper. It can be renamed, rewritten or deleted, and no other file
can notice, because no other file could use it. Keep the public surface as
small as the callers need.

## import and qualified names

Another module reaches the public names after an `import`, and writes the
module path in front of the name:

```dc example=qualified_name
```

`std::slice::eq` is the module `std::slice`, then the function `eq`. A module of
your own works the same way: after `import grades;` you write
`grades::average(40, 55, 70)`. Always write the full module path as it is in
`import`: `text::words::count(...)` for the module `text::words`.

Without the `import`, even a public name is unknown. A private name is
unknown too, and the compiler words both the same way: an unknown path,
naming `grades::total`. When you see that message, ask two questions: did I import
the module, and is the name `public`?

## using

Writing the module in front of every call gets long. `using module::*;` brings
every public name of the module in, so you can leave the prefix out:

```dc example=using_all
```

Use it where the prefix only adds noise. The qualified form says where every
name comes from, which is why most code keeps it.

## public import

A plain `import` is private to the module that writes it. That is usually
right. It stops being right when a *public* declaration mentions a type from
the imported module: callers could not name that type otherwise. The compiler
tells you:

```text
error: public declaration exposes `shapes::Rect` from privately imported
module `shapes`; use `public import shapes`
```

Change the line to `public import shapes;` and the types of `shapes` become
part of this module's surface too. Use it only for that reason.

## The project

Several files need something that says what to build. That is `project.dcon`,
the manifest that the package manager `dpm` reads:

```text
dpm::v1::Project {
    name = "grades",
    version = "0.1.0",
    products = {
        { name = "grades", entry = "src/main.dc" },
    },
}
```

- `name` and `version` identify the package.
- `products` lists what to build. Each product has a `name` and an `entry`:
  the file that holds `main`.
- Every other file is found through the `src` folder: `import grades;`
  finds `src/grades.dc`. The entry file does not list its modules.

`dpm build` reads the manifest, compiles the entry and every module it
imports, and links the program. `dpm run` also starts it. A problem in
`project.dcon` is reported as `project.dcon:LINE:COLUMN: message`; a problem in
a source file as `error: message` with the file, line and column.

learndc works the same way. `learndc check` copies your project into a
staging folder and runs `dpm build`. A harness check adds a small test program
that imports your public functions the way a caller would, which is why a
function that is not `public` cannot pass: the test cannot see it.

## Common mistakes

- Forgetting `public`. The error is `unknown path`, and it looks the same as
  a missing import.
- Forgetting `import`, or misspelling it: `could not resolve module`.
- A `module` line that does not match the file path.
- Writing `grades::average` after `using grades::*;` and wondering why nothing
  changed: both forms work, the bare name is only shorter.
- Making everything public. The module then has no surface to protect.
