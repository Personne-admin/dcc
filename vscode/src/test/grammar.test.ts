import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import * as vsctm from 'vscode-textmate';
import * as onig from 'vscode-oniguruma';

const ROOT = path.resolve(__dirname, '..', '..');
const GRAMMAR = path.join(ROOT, 'syntaxes', 'dcc.tmLanguage.json');
const SAMPLE = path.join(ROOT, 'src', 'test', 'fixtures', 'grammar-sample.dc');

interface Tok {
    text: string;
    scope: string;
}

async function tokenizeSample(): Promise<Map<string, Tok[]>> {
    const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer as ArrayBuffer;
    await onig.loadWASM(wasm);
    const registry = new vsctm.Registry({
        onigLib: Promise.resolve({
            createOnigScanner: (patterns: string[]) => new onig.OnigScanner(patterns),
            createOnigString: (s: string) => new onig.OnigString(s),
        }),
        loadGrammar: async (scope: string) =>
            scope === 'source.dc' ? vsctm.parseRawGrammar(fs.readFileSync(GRAMMAR, 'utf8'), GRAMMAR) : null,
    });
    const grammar = await registry.loadGrammar('source.dc');
    assert.ok(grammar);
    const lines = new Map<string, Tok[]>();
    let state = vsctm.INITIAL;
    for (const line of fs.readFileSync(SAMPLE, 'utf8').split('\n')) {
        const result = grammar.tokenizeLine(line, state);
        state = result.ruleStack;
        lines.set(
            line.trim(),
            result.tokens
                .map((t) => ({ text: line.slice(t.startIndex, t.endIndex), scope: t.scopes[t.scopes.length - 1] }))
                .filter((t) => t.text.trim() !== ''),
        );
    }
    return lines;
}

function words(tokens: Tok[], word: string): string[] {
    return tokens.filter((t) => t.text === word).map((t) => t.scope.replace(/\.dc$/, ''));
}

const KW = 'keyword.control';
const ID = 'source';
const SEG = 'constant.language.segment-register';

const IN_CASES: Array<[string, string[]]> = [
    ['for x in xs { use(x); }', [KW]],
    ['for usize i in 0..4 { use(i); }', [KW]],
    ['for in in xs { use(in); }', [ID, KW, ID]],
    ['for usize in in 0..4 { use(in); }', [ID, KW, ID]],
    ['for Item in in items { use(in); }', [ID, KW, ID]],
    ['for usize in = 0; in < 4; in++ { use(in); }', [ID, ID, ID, ID]],
    ['for &r in xs { use(r); }', [KW]],
    ['static for item in args { use(item); }', [KW]],
    ['static for in in args { use(in); }', [ID, KW, ID]],
    ['asm @[inputs(v in al = in, port in dx = port)] { "outb %[v], %[port]" };', [KW, ID, KW]],
    ['asm @[output(dst = y in rax), inputs(in in rbx = in)] { "mov %[dst], %[in]" };', [KW, ID, KW, ID]],
    ['u32 r = asm @[output(u32 in eax), inout(in in ecx), inputs(a in edx = arr[in])] { "nop" };', [KW, ID, KW, KW, ID]],
    ['asm @[output(in eax)] { "nop" };', [KW]],
    ['struct Holder { i32 in; i32 out; }', [ID]],
    ['i32 in_fn(i32 in) { return in + 1; }', [ID, ID]],
    ['u32 in = 5;', [ID]],
    ['u32 k = in + in;', [ID, ID]],
];

test('in is a keyword only in for heads and asm operand clauses', async () => {
    const lines = await tokenizeSample();
    for (const [line, expected] of IN_CASES) {
        const tokens = lines.get(line);
        assert.ok(tokens, `sample line missing: ${line}`);
        assert.deepEqual(words(tokens, 'in'), expected, line);
    }
});

test('segment registers after ^ and before a single colon', async () => {
    const lines = await tokenizeSample();
    const params = lines.get(
        'void far_ptrs(u8^ p, u8^CS a, u8^DS b, u8^ES c, u8^SS d, u8^FS e, u8^GS f, i32(^)(i32) fp, const u8^GS g) {',
    );
    assert.ok(params);
    for (const reg of ['CS', 'DS', 'ES', 'SS', 'FS', 'GS']) {
        assert.ok(words(params, reg).every((s) => s === SEG), reg);
    }
    assert.equal(words(params, 'GS').length, 2);
    assert.ok(words(params, '^').every((s) => s === 'keyword.operator'));
    assert.equal(words(params, '^').length, 9);

    assert.deepEqual(words(lines.get('u8^ a = GS:0x200;')!, 'GS'), [SEG]);
    assert.deepEqual(words(lines.get('u8^ b = FS:off;')!, 'FS'), [SEG]);
    assert.deepEqual(words(lines.get('u8^* pp;')!, '^'), ['keyword.operator']);
});

test('registers stay identifiers elsewhere and xor and paths are unaffected', async () => {
    const lines = await tokenizeSample();
    assert.deepEqual(words(lines.get('u32 GS = 1;')!, 'GS'), [ID]);
    assert.deepEqual(words(lines.get('u32 h = GS + CS;')!, 'GS'), [ID]);
    assert.deepEqual(words(lines.get('u32 h = GS + CS;')!, 'CS'), [ID]);
    assert.deepEqual(words(lines.get('u32 g = std::GS::in;')!, 'GS'), [ID]);
    assert.deepEqual(words(lines.get('u32 g = std::GS::in;')!, '::'), ['keyword.operator', 'keyword.operator']);
    assert.deepEqual(words(lines.get('u32 e = x ^ y;')!, '^'), ['keyword.operator']);
    assert.deepEqual(words(lines.get('u32 f = x^y;')!, '^'), ['keyword.operator']);
    assert.deepEqual(words(lines.get('u8^ c = 0xB800:0;')!, '0xB800'), ['constant.numeric.hex']);
});
