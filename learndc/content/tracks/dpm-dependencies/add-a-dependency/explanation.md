dcc only knows the import roots it is given. Before the dependency existed, the
only root was `src`, so `units::convert` had nowhere to resolve. With the
dependency declared, dpm adds `-Ilibs/units/src` to the dcc command, and
`units::convert` is the file `libs/units/src/units/convert.dc`.

The lock file dpm wrote lists the package with its version and an empty hash:
a `path:` package is read in place, there is no download to verify. The line
that matters for teamwork is the manifest fingerprint at the top, which changes
whenever the requirements change.
