# DC Language Support

Syntax highlighting for DC source (`.dc`), compiler test fixtures (`.dcc-test`), and IR dumps (`.dir`), with language server support powered by `dccd`.

Install DCC separately and put `dccd` on your PATH, or set `dcc.serverPath` to its executable path. Language server features require a desktop or remote VS Code extension host; this extension does not run in the browser.

Use `dcc.includePaths` for additional module search paths and `dcc.compilationDatabase` to select a `compile_commands.json`. Inferred type and parameter hints can be configured under `dcc.inlayHints`.

## Development

Run `npm ci`, then `npm test` and `npm run validate`. `npm run compile` compiles the sources and tests; `npm run watch` watches them. `npm run bundle` builds a development bundle with a source map. `npm run package` type checks and builds the production bundle; VSCE runs it automatically before packaging.

The VSIX contains the bundled extension, language configurations, grammars, license notices, and the language client's process termination helper. Sources, tests, source maps, and dependency trees stay out of the package.
