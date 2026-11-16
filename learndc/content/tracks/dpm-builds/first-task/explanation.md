The command list has one element, the path of the built program, so there is no
shell and nothing to quote. The token does two jobs: it names the file for
the command and it ties the task to the product, so `dpm task fleet-report
--dry-run` shows a path inside `.dpm/out/debug/host/`. The `build` field makes
the dependency explicit: dpm builds `fleet` first (or reports it is fresh)
instead of failing because the file does not exist yet.

dpm validated the manifest when it loaded it: a misspelled token, a product
that does not exist or a bad task name stops `dpm check` with a position, long
before anything runs. Nothing in this exercise's checks started the task;
tasks never run on their own.
