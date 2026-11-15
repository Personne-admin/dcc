# Neovim

The first-party runtime is `neovim/` in this repository. It requires Neovim
0.11 or newer and an installed `dccd`. It uses native `vim.lsp.config`,
`vim.lsp.enable` and `vim.system`; nvim-lspconfig, Mason, Node.js and Python
are not required. Only `.dc` files receive the `dc` filetype.

## Installation

The repository root is not a Neovim runtime. Plugin managers must load its
`neovim/` subdirectory, not the repository root.

With an existing checkout, lazy.nvim can use a local plugin directory:

```lua
require("lazy").setup({
  {
    dir = "/home/me/dcc/neovim",
    name = "dcc.nvim",
    main = "dcc",
    opts = {},
  },
})
```

On Windows, use a path such as `C:/Users/me/src/dcc/neovim`. This example
assumes lazy.nvim itself is already configured; lazy.nvim is optional.

For native packages on Linux, extract only the plugin subtree from a checkout:

```sh
mkdir -p ~/.local/share/nvim/site/pack/dcc/start/dcc.nvim
cd /path/to/dcc
git archive HEAD:neovim | tar -x -C ~/.local/share/nvim/site/pack/dcc/start/dcc.nvim
```

Repeat extraction when updating. Alternatively, copy the contents of `neovim/` into
that package directory. On Windows the equivalent package directory is
`%LOCALAPPDATA%/nvim-data/site/pack/dcc/start/dcc.nvim`; copy the contents
there without an extra `neovim/` directory level.

A minimal `init.lua` for a native package is:

```lua
require("dcc").setup()
```

To use a checkout directly without a plugin manager:

```lua
vim.opt.runtimepath:prepend("/path/to/dcc/neovim")
require("dcc").setup()
```

Run `:helptags /path/to/dcc/neovim/doc` to index the bundled help if your
plugin manager does not generate help tags.

## Configuration and discovery

```lua
require("dcc").setup({
  -- Paths are unquoted strings. Spaces and Unicode are supported.
  dccd = "/opt/dcc/bin/dccd",
  dpm = "/opt/dcc/bin/dpm",
  dpm_commands = true,
  settings = { dcc = { includePaths = { "src" } } },
  -- Optional: a directory, or function(bufnr) returning a directory/nil.
  -- root_dir = function(bufnr) return vim.fs.root(bufnr, "project.dcon") end,
})
```

Omit executable overrides for normal installations. An invalid explicit path
is an error and never falls back to another executable. Bare executable names
are looked up on PATH; relative paths containing separators are resolved from
Neovim's current directory. Do not include shell quotes or command arguments.

Automatic dccd discovery uses this order:

1. Explicit `dccd` option.
2. PATH (`dccd.exe` on Windows).
3. A sibling of the `dcc` executable found on PATH.
4. On Windows, installed MSI products belonging to dcc's UpgradeCode
   `BC06D996-A013-4321-9967-7BCEDBBD011B`, newest version first.
5. Windows Program Files defaults, as a final fallback.

MSI discovery uses the registered install location or the registered dccd
component for older installers. It handles custom locations and a stale PATH
in an already-running editor. The bounded PowerShell query uses Windows
Installer registration, not arbitrary registry executable paths. No elevation
is required. The language server is launched directly, without a shell.

DPM uses an explicit `dpm` override, PATH, then the compiler's sibling directory.
On Windows it then uses the same MSI and Program Files discovery as `dccd`; the
MSI installs `dpm.exe` beside `dcc.exe` and `dccd.exe`. When no DPM is found,
`:DccInfo` reports it and the LSP keeps working; only DPM commands are
unavailable. This plugin does not run Linux executables through WSL implicitly.

## Roots and language features

The nearest `project.dcon` selects the project root. Otherwise the nearest
`compile_commands.json`, then `dcc.json`, is used. A standalone file uses its
parent directory. A `root_dir` override replaces this policy; returning nil
disables attachment for that buffer. Buffers sharing a root reuse one client,
including while initialization is pending. Different roots have separate
clients.

Files inside DPM's package cache (a `project.dcon` in `<cache>/src/<hash>/`)
stay with an existing dccd client instead of starting one rooted in the cache:
the client of the buffer you came from (for example through Go to Definition),
otherwise the oldest client. dccd then analyses the file with the compile
command of the project that depends on it.

DPM's compilation database and dccd settings control compiler options. The
plugin does not invent compiler flags from `project.dcon`. Build the project
to generate `compile_commands.json`; configure include paths when needed
before the first build. See [the VS Code integration](../vscode/README.md) for shared dccd settings.

The server implements diagnostics, hover, completion, definition, references,
prepare-rename/rename, document/range/on-type formatting, signature help,
quick fixes, semantic tokens, inlay hints and workspace symbols. Use Neovim's
native operations, for example:

```lua
vim.keymap.set("n", "K", vim.lsp.buf.hover)
vim.keymap.set("n", "gd", vim.lsp.buf.definition)
vim.keymap.set("n", "grr", vim.lsp.buf.references)
vim.keymap.set("n", "<leader>rn", vim.lsp.buf.rename)
vim.keymap.set("n", "<leader>f", function() vim.lsp.buf.format({ async = true }) end)
-- Optional native completion:
vim.api.nvim_create_autocmd("LspAttach", {
  callback = function(event)
    vim.lsp.completion.enable(true, event.data.client_id, event.buf)
  end,
})
```

No global key mappings are installed by the plugin. Native Neovim diagnostics
and completion presentation remain under the user's control.

Buffer changes use normal LSP synchronization. Saving DC files or project
configuration sends file-change notifications, including on Linux where
native workspace watching may be disabled. External filesystem changes use
Neovim's native watchers where enabled; use `:DccRestart` after external
changes if needed. Shutdown uses Neovim's LSP lifecycle.

## Commands

- `:DccInfo`: resolved executable, discovery source, launch/initialize status,
  clients and roots, log path and troubleshooting hints. dccd does not expose
  a CLI version flag; where available, a bounded asynchronous query reports
  the sibling compiler's version, explicitly labelled as such.
- `:DccRestart`: rediscover executables and gracefully restart all dccd clients,
  with forced termination after two seconds if shutdown stalls.
- `:DccBuild [arguments]`: asynchronously run `dpm build` at this buffer's
  nearest `project.dcon`.
- `:DccRun [arguments]`: asynchronously run `dpm run` there.
- `:checkhealth dcc`: inspect configuration and platform limitations.

Examples: `:DccBuild --offline` and `:DccRun hello -- hello\ world`.
Neovim command arguments are passed as separate argv entries; backslash-escape
spaces in a single argument. Quotes are not interpreted by a shell.

Commands do not save buffers automatically. DPM reads files on disk. Only one
DPM job runs per project at a time. Compiler errors populate quickfix
(`:copen`); failed builds and program output appear in a small read-only
scratch split. Output is capped at 128 KiB. Commands are noninteractive,
without terminal stdin; use an external terminal for interactive programs.
Closing Neovim terminates its active DPM jobs. Set `dpm_commands = false` to
omit both commands.

## Troubleshooting and tests

Start with `:DccInfo` and `:checkhealth dcc`. Set an unquoted absolute `dccd`
path if discovery fails, then run `:DccRestart`. Check the displayed LSP log
for a launch or protocol failure. Use Neovim's `vim.lsp.log.set_level("debug")`
only while investigating; logs can grow quickly. An executable being found
is distinct from an initialized LSP connection.

From the repository root, after building dccd, dpm and the library:

```sh
ulimit -c 0
nvim --headless -u NONE -l neovim/tests/run.lua
```

The test uses real Linux dccd/DPM processes in temporary spaced/Unicode paths
and mocks Windows executable and MSI discovery. Mocked Windows checks are not
a native Windows MSI or Neovim execution test.
