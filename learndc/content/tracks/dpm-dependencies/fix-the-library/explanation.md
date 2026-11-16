Dependencies get no special treatment from the compiler: their modules are
found through an import root, compiled, and checked like yours. The module
declaration must match the path below the root (`units/convert.dc` is
`units::convert`), otherwise the module cannot be resolved. A function stays
private to its module unless it is marked `public`, which is why the second
message talks about an unknown path even though the file exists.

When errors point into a package, the fix is in the package, not in the
program that imports it.
