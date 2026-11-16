A profile is a named bundle of settings with a starting point. `checked` inherits
release, so it gets `-O2` and no debug information, and then its own settings
turn on `-fbounds-check`. Nothing else changes, which is the point of
inheriting: the profile says only what is different.

Selecting is a command-line matter (`--profile checked`), never a manifest
one, so the same project builds all three ways from one file. The outputs sit
in separate directories, `.dpm/out/checked/host/` next to `debug` and `release`,
so switching profiles never forces a rebuild of the others.
