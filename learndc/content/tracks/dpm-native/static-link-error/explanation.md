`Static` means what it says: only archives, and static links search only the `-L`
directories, so the exact file `libm.so.6` is not even looked for in the system
directories. A program that needs a shared object must be dynamic; dcc then
adds a program interpreter and a `DT_NEEDED` entry for `libm.so.6`, which is
what `dynamic: true` observes from inside the process.

`Default` already does the right thing here, because the library resolves to a
shared object. Writing `Dynamic` states the intent and turns a link that would
silently fall back to static into an error, which is useful when the program
must never depend on archives.
