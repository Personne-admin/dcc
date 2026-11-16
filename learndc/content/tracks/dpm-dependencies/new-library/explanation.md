A library is a package whose manifest declares no products. dpm needs only a
name and a version to treat a directory as a package, and the source root
(`src` by default) is added to the import path of every project that depends
on it. Placing the modules under `src/ports/` is what gives them the names
`ports::lookup`: the namespace directory is how the package claims its part of
the module namespace.

Because `dpm build` has nothing to build in `libs/ports`, you test it by
building a program that uses it, as you just did. Chapter five adds test
programs to a library.
