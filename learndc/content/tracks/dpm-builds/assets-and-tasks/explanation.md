The command is a shell script because it redirects, and redirection is shell
syntax; everything else about it is an argument list. The two paths come in as
`$1` and `$2` instead of being spliced into the script text, which keeps odd
file names harmless. `sh` after the script is the name the shell uses for itself
(`$0`).

`inputs` and `outputs` are what make the second run `Fresh`: dpm records the
command, the contents of the input and the state of the output in
`.dpm/tasks/count-ships.stamp`, and skips the task while they are unchanged.
Change a line of `data/ships.txt` and the task runs again. A task without
`outputs` always runs, and a file the command reads but you did not list is not
tracked.

A `path:` asset is read in place and is not hashed in the lock, which is why
it suits local data and tests; a remote asset would be pinned by hash.
