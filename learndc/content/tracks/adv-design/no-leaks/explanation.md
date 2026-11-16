# Why this works

The starter has the classic shape of code that was written for the happy path:
acquire, work, release at the end. Every `?` between the acquisition and the
last line is an exit that skips the release. With `defer` the release is
registered at the moment of acquisition and the compiler places it on every
exit, including the ones added later.

`all` is a temporary: the deferred `deinit` is the only cleanup it needs. It
runs when the function returns, successfully or not, after the sort and the
copy loop are done with it.

`result` is different, because on success its memory outlives the function.
One mechanism cannot both free and keep it, so the cleanup is conditional. The
flag expresses the ownership state in the code: while it is false the function
owns the array; once true the caller does. Setting it after the last `?`
makes the transfer atomic. The ordering matters in both directions. A flag set
too early leaks when a later `push` fails; a flag never set frees the array
the caller is about to use.

The harness verifies the whole contract rather than a sample of cases. It first
counts the allocations of a clean run, then fails each one in turn. For a
function that grows its arrays by reallocation the number of failure points
grows with the input, which is why it also sweeps a long word list: a leak
that needs the third growth of `all` shows up only there. The check is
`allocs == frees` after an error, which holds for any implementation that does
its cleanup correctly, however it is written.
