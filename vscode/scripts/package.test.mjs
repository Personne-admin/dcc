import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFileSync, statSync } from 'node:fs';
import { createRequire, isBuiltin } from 'node:module';
import { resolve } from 'node:path';
import { runInNewContext } from 'node:vm';

const require = createRequire(import.meta.url);

test('VSIX includes only runtime files and documentation', () => {
    const listed = execFileSync(process.execPath, [require.resolve('@vscode/vsce/vsce'), 'ls'], { encoding: 'utf8' }).trim().split(/\r?\n/).sort();
    assert.deepEqual(listed, [
        'LICENSE', 'readme.md',
        'language-configuration-dcc-dir.json', 'language-configuration-dcc-test.json', 'language-configuration.json',
        'out/THIRD_PARTY_NOTICES.txt', 'out/extension.js', 'out/terminateProcess.sh', 'package.json',
        'syntaxes/dcc-dir.tmLanguage.json', 'syntaxes/dcc-test.tmLanguage.json', 'syntaxes/dcc.tmLanguage.json',
    ].sort());
    assert.equal(readFileSync('out/terminateProcess.sh', 'utf8'), readFileSync('node_modules/vscode-languageclient/lib/node/terminateProcess.sh', 'utf8'));
    if (process.platform !== 'win32')
        assert.ok(statSync('out/terminateProcess.sh').mode & 0o111);
    const notices = readFileSync('out/THIRD_PARTY_NOTICES.txt', 'utf8');
    for (const name of ['vscode-languageclient', 'vscode-jsonrpc', 'vscode-languageserver-protocol', 'vscode-languageserver-types'])
        assert.ok(notices.includes(name), `missing notice for ${name}`);
});

test('production bundle loads without node_modules', () => {
    const stub = new Proxy(function () {}, {
        get: () => stub,
        construct: () => stub,
        apply: () => stub,
    });
    const apiNames = [...new Set([...readFileSync('node_modules/@types/vscode/index.d.ts', 'utf8').matchAll(/export (?:class|enum|namespace|function|const) (\w+)/g)].map(match => match[1]))];
    const vscode = Object.fromEntries(apiNames.map(name => [name, stub]));
    const module = { exports: {} };
    runInNewContext(readFileSync('out/extension.js', 'utf8'), {
        exports: module.exports,
        module,
        require: name => {
            if (name === 'vscode')
                return vscode;
            assert.ok(isBuiltin(name), `unbundled dependency: ${name}`);
            return require(name);
        },
        __dirname: resolve('out'),
        process, console, Buffer, setTimeout, clearTimeout, setImmediate, clearImmediate,
    });
    assert.equal(typeof module.exports.activate, 'function');
    assert.equal(typeof module.exports.deactivate, 'function');
});
