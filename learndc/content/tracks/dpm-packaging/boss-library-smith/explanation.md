The library is small on purpose. What makes it reusable is not the code but the
decisions around it: the namespace (`tabulate::...`), the three modules each
with one job, a handful of public names that are promises, helpers that stay
private so they can change, a manifest that says what the package is and a
version below 1.0 because the API has not met its users yet. The README and the
changelog are the interface for people; the tests are the interface for you.

The application depends on the library by path, which is the right tool while
both live in one workspace. Everything that makes a package publishable is
already in place except one thing: a published package may not depend on
`path:` packages. Chapter by chapter you have seen all the pieces: products and
modules, dependencies and locks, profiles, native links and now a package of
your own.
