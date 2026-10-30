import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import * as vsctm from 'vscode-textmate';
import * as onig from 'vscode-oniguruma';

const ROOT = path.resolve(__dirname, '..', '..');
const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, 'package.json'), 'utf8'));

test('dcon is a registered language with a grammar and activation event', () => {
    const language = pkg.contributes.languages.find((l: { id: string }) => l.id === 'dcon');
    assert.ok(language);
    assert.deepEqual(language.extensions, ['.dcon']);
    assert.ok(fs.existsSync(path.join(ROOT, language.configuration)));
    const grammar = pkg.contributes.grammars.find((g: { language: string }) => g.language === 'dcon');
    assert.ok(grammar);
    assert.equal(grammar.scopeName, 'source.dcon');
    assert.ok(pkg.activationEvents.includes('onLanguage:dcon'));
});

test('the dcon grammar reuses the DC grammar', async () => {
    const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer as ArrayBuffer;
    await onig.loadWASM(wasm);
    const files: Record<string, string> = {
        'source.dc': path.join(ROOT, 'syntaxes', 'dcc.tmLanguage.json'),
        'source.dcon': path.join(ROOT, 'syntaxes', 'dcon.tmLanguage.json'),
    };
    const registry = new vsctm.Registry({
        onigLib: Promise.resolve({
            createOnigScanner: (patterns: string[]) => new onig.OnigScanner(patterns),
            createOnigString: (s: string) => new onig.OnigString(s),
        }),
        loadGrammar: async (scope: string) => (files[scope] ? vsctm.parseRawGrammar(fs.readFileSync(files[scope], 'utf8'), files[scope]) : null),
    });
    const grammar = await registry.loadGrammar('source.dcon');
    assert.ok(grammar);
    const line = 'dpm::v1::Project { name = "app", /* note */ version = "1.0.0" } // end';
    const result = grammar.tokenizeLine(line, vsctm.INITIAL);
    const scopeOf = (word: string) => result.tokens.filter((t) => line.slice(t.startIndex, t.endIndex).includes(word)).map((t) => t.scopes.join(' ')).join('|');
    assert.match(scopeOf('app'), /string/);
    assert.match(scopeOf('note'), /comment/);
    assert.match(scopeOf('end'), /comment/);
    const scopes = new Map(result.tokens.map((t) => [line.slice(t.startIndex, t.endIndex), t.scopes.join(' ')]));
    assert.ok([...scopes.values()].every((s) => s.startsWith('source.dcon')));
});
