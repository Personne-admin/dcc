# Declare your first task

*Continues the harbor project of custom-profile and product-settings.*

The harbormaster runs the fleet program every morning with the same few
steps: build it, then start it. A task can do both with one command.

`src/main.dc` is the course's audit; leave it and `src/audit.dc` alone.

## Your task

Add a `tasks` list to `project.dcon` with one task:

- name `fleet-report`, with a `description`
- `command` runs the output of the product `fleet`: the token `${out:fleet}`
  is its path in the selected profile. Pass it as a one-element list; no shell
- `build` lists the product `fleet`, so dpm builds it first

Then try it in your workspace:

```text
dpm task                         lists fleet-report
dpm task fleet-report --dry-run  shows the command with the path filled in
dpm task fleet-report            builds fleet and runs it
```

## Acceptance criteria

- `dpm run audit` prints six `ok` lines and `6 checks, 0 failed`.
- `dpm check` is silent: the manifest loads.
