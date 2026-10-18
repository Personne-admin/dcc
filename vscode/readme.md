# DC Language Support

Syntax highlighting for DC source (`.dc`), compiler test fixtures (`.dcc-test`), and IR dumps (`.dir`), with language server support powered by `dccd`.

Install DCC separately and put `dccd` on your PATH, or set `dcc.serverPath` to its executable path. Language server features require a desktop or remote VS Code extension host; this extension does not run in the browser.

Use `dcc.includePaths` for additional module search paths and `dcc.compilationDatabase` to select a `compile_commands.json`. Inferred type and parameter hints can be configured under `dcc.inlayHints`.

## Troubleshooting

Open **View -> Output -> DCC Language Server** for dccd stderr and client errors. The language client automatically restarts a crashed server; after five crashes within three minutes it stops retrying and reports the failure in Output.

For bug reports, set `"dccd.trace.server": "verbose"` in Settings JSON, reproduce the edit, and copy the Output log. Use `"messages"` for message summaries or `"off"` to disable tracing. Verbose traffic includes source text and paths; review it before sharing. Include the dccd version, the initial source, and the edit sequence.

## Development

Run `npm ci`, then `npm test` and `npm run validate`. `npm run compile` compiles the sources and tests; `npm run watch` watches them. `npm run bundle` builds a development bundle with a source map. `npm run package` type checks and builds the production bundle; VSCE runs it automatically before packaging.

The VSIX contains the bundled extension, language configurations, grammars, license notices, and the language client's process termination helper. Sources, tests, source maps, and dependency trees stay out of the package.

The compiler test gate includes deterministic incremental edit sequences and `tests/lsp_edit_fuzzer.py`. To run longer sequences from the repository root, use `python3 tests/lsp_edit_fuzzer.py build/bin/dccd --iterations 1000 --seeds 42 8086 20261018`. Each request has a five-second limit; failures record the seed, edit, and final text in `/tmp/dccd-edit-failure.json`. The C++ LSP tests also check the exact server document text after each edit, using an independent UTF-16 position calculation.
