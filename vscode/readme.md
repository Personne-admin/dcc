# DC Language Support

Syntax highlighting for DC source (`.dc`), dpm manifests (`.dcon`), compiler test fixtures (`.dcc-test`), and IR dumps (`.dir`), with language server support powered by `dccd`.

For `project.dcon`, `project.lock.dcon` and other `.dcon` files, `dccd` reports manifest errors and lock mismatches, and offers hover, completion and go to definition into the dpm schema. It never builds, fetches or runs anything for a manifest; it reads the file, the neighbouring lock file and the local dpm cache directory.

Install DCC separately. Windows MSI installations are discovered automatically, including when VS Code was already running at installation time. On Linux, put `dccd` on PATH. You can always set `dcc.serverPath` to an executable path. Language server features require a desktop or remote VS Code extension host; this extension does not run in the browser.

DPM projects need no setup: open any `.dc` file, with or without its folder as a workspace, and dccd reads the nearest `project.dcon` to derive the compiler settings `dpm build` would use, before the first build. Use `dcc.includePaths` for additional module search paths and `dcc.compilationDatabase` to select a `compile_commands.json`; both take precedence over the derived settings. Inferred type and parameter hints can be configured under `dcc.inlayHints`.

## Executable discovery

An explicitly configured `dcc.serverPath` always wins. An invalid override is
an error: the extension never silently switches to another server. The default
setting value `dccd` is automatic discovery; explicitly saving that value makes
it a PATH-only override. Remove the setting to restore automatic discovery.

On Windows the automatic order is:

1. `dccd.exe` beside `dcc.compilerPath`, if configured, otherwise beside the
   first `dcc.exe` on the extension host's PATH.
2. The first `dccd.exe` on PATH.
3. Installed dcc MSI products, matched by the installer's UpgradeCode
   `BC06D996-A013-4321-9967-7BCEDBBD011B`. The newest usable registered version
   wins; equal versions are ordered by executable path.
4. The installer's `dcc/bin/dccd.exe` default beneath the Windows Program Files
   directories, only as a last resort.

MSI discovery uses the Windows Installer API for the current user and machine,
not arbitrary uninstall entries or display-name matching. It reads the recorded
install location, or the installed dccd component for older packages. It can
find custom and Unicode installation directories without refreshing VS Code's
inherited PATH. It uses built-in Windows PowerShell without a profile, with a
five-second limit; policy or registration failures are reported in diagnostics.
The MSI currently ships x86_64 tools as a per-machine installation. Discovery
also accepts current-user MSI registrations with the same product identity.

Linux/macOS default discovery remains PATH-based. Setting `dcc.compilerPath`
opts into compiler-sibling selection on those platforms too. A missing compiler
hint or sibling permits subsequent discovery tiers; use `dcc.serverPath` when
selection must be strict. Relative configured paths use the first workspace
folder, or the extension host's current directory when there is no workspace.
Remote workspaces resolve and launch the server on their extension host.

Paths are executable filenames, not shell command lines. For example:

```json
{
    "dcc.serverPath": "D:\\DC Tools\\bin\\dccd.exe"
}
```

Do not put extra quotes or arguments inside the setting. Arguments are supplied
separately by the extension, and no shell is used to launch dccd. Environment
variables are inherited normally; settings do not expand `%VARIABLE%` or `~`.

## Troubleshooting

Run **DCC: Show Server Diagnostics** from the Command Palette for the selected
executable, discovery source, launch/initialize status, failure reason and a
configuration hint. No environment dump is logged. Use **DCC: Restart Language
Server** after changing the server/compiler setting or installing dcc while
VS Code was already open. Restart retries discovery, including MSI registration;
a full VS Code restart or manual PATH edit is not required.

Open **View -> Output -> DCC Language Server** for dccd stderr and client errors.
Discovery errors mean no executable was selected. Launch errors (such as ENOENT,
access denial or a missing Windows DLL) occur after selection. A started process
that never completes LSP initialize is a protocol/startup failure; the diagnostics
keep that distinct from discovery. The language client automatically restarts a crashed server; after five crashes within three minutes it stops retrying and reports the failure in Output.

For bug reports, set `"dccd.trace.server": "verbose"` in Settings JSON, reproduce the edit, and copy the Output log. Use `"messages"` for message summaries or `"off"` to disable tracing. Verbose traffic includes source text and paths; review it before sharing. Include the dccd version, the initial source, and the edit sequence.

## Development

Run `npm ci`, then `npm test` and `npm run validate`. `npm run compile` compiles the sources and tests; `npm run watch` watches them. `npm run bundle` builds a development bundle with a source map. `npm run package` type checks and builds the production bundle; VSCE runs it automatically before packaging.

The VSIX contains the bundled extension, language configurations, grammars, license notices, and the language client's process termination helper. Sources, tests, source maps, and dependency trees stay out of the package.

The compiler test gate includes deterministic incremental edit sequences and `tests/lsp_edit_fuzzer.py`. To run longer sequences from the repository root, use `python3 tests/lsp_edit_fuzzer.py build/bin/dccd --iterations 1000 --seeds 42 8086 20261018`. Each request has a five-second limit; failures record the seed, edit, and final text in `/tmp/dccd-edit-failure.json`. The C++ LSP tests also check the exact server document text after each edit, using an independent UTF-16 position calculation.

## Verify a Windows MSI installation

Use an extension build and MSI containing these fixes for the following checks.

1. Leave VS Code open, install the MSI, and open a `.dc` file. Run **DCC: Restart
   Language Server** if activation had failed before installation.
2. Run **DCC: Show Server Diagnostics**. Check the selected path and source, then
   confirm `Running; LSP initialize succeeded`. Request completion or hover.
3. Repeat with a custom directory containing spaces and Unicode, for example
   `msiexec /i dcc-VERSION-x86_64.msi INSTALLDIR="D:\DC Tools"`.
4. Set `dcc.serverPath` to a different installed server and confirm it wins.
   Set it to a missing file and confirm the error rather than a fallback. Remove
   the override and restart to restore automatic discovery.
5. Test an upgrade, then uninstall. Restart the server and confirm removed MSI
   records/files are not selected. Multiple remaining installations follow the
   discovery order above.

For a direct byte-framing check on Windows, run
`python tests/lsp_stdio.py "C:\Program Files\dcc\bin\dccd.exe"` from a source
checkout. The test checks UTF-8 and CRLF JSON bodies, initialize, shutdown and
exact stdout framing. The release smoke uses the same test under Wine.

The Node tests mock Windows installation records and filesystem probes. On
Linux, the executable-launch test uses the existing built dccd through a spaced,
Unicode symlink and completes a real LSP handshake. A native Windows-only test
checks the MSI query when run on Windows; it is explicitly skipped elsewhere.
Mocked discovery and Linux execution are not a real Windows MSI installation test.
