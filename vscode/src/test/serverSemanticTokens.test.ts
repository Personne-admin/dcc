import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    SERVER_TOKEN_MODIFIERS,
    SERVER_TOKEN_TYPES,
    serverTokenModifiersToDccMask,
    serverTokenTypeToDccName,
} from '../model/serverSemanticTokens';

test('server legend matches dccd semantic_tokens.cppm ordering', () => {
    assert.deepEqual([...SERVER_TOKEN_TYPES], [
        'namespace', 'type', 'class', 'enum', 'interface', 'struct', 'typeParameter', 'parameter', 'variable', 'property', 'enumMember',
        'function', 'method', 'macro', 'keyword', 'modifier', 'comment', 'string', 'number', 'operator', 'asmPlaceholder', 'asmRegister',
    ]);
    assert.deepEqual([...SERVER_TOKEN_MODIFIERS], ['declaration', 'readonly', 'static', 'deprecated', 'defaultLibrary']);
});

test('server token types map onto the DCC legend', () => {
    const byName = new Map<string, number>();
    SERVER_TOKEN_TYPES.forEach((n, i) => byName.set(n, i));

    assert.equal(serverTokenTypeToDccName(byName.get('type')!), 'type');
    assert.equal(serverTokenTypeToDccName(byName.get('class')!), 'type');
    assert.equal(serverTokenTypeToDccName(byName.get('struct')!), 'type');
    assert.equal(serverTokenTypeToDccName(byName.get('enum')!), 'type');
    assert.equal(serverTokenTypeToDccName(byName.get('typeParameter')!), 'type');

    assert.equal(serverTokenTypeToDccName(byName.get('function')!), 'global');
    assert.equal(serverTokenTypeToDccName(byName.get('method')!), 'global');

    assert.equal(serverTokenTypeToDccName(byName.get('keyword')!), 'keyword');
    assert.equal(serverTokenTypeToDccName(byName.get('modifier')!), 'keyword');
    assert.equal(serverTokenTypeToDccName(byName.get('operator')!), 'keyword');
    assert.equal(serverTokenTypeToDccName(byName.get('macro')!), 'keyword');

    assert.equal(serverTokenTypeToDccName(byName.get('number')!), 'constant');
    assert.equal(serverTokenTypeToDccName(byName.get('string')!), 'constant');
    assert.equal(serverTokenTypeToDccName(byName.get('enumMember')!), 'constant');

    for (const n of ['namespace', 'parameter', 'variable', 'property', 'comment', 'asmPlaceholder', 'asmRegister'])
        assert.equal(serverTokenTypeToDccName(byName.get(n)!), 'identifier');

    assert.equal(serverTokenTypeToDccName(-1), 'identifier');
    assert.equal(serverTokenTypeToDccName(999), 'identifier');
});

test('server modifier masks map onto the DCC legend mask', () => {
    const declaration = 1 << 0;
    const readonly = 1 << 1;
    const staticBit = 1 << 2;
    const deprecated = 1 << 3;
    const defaultLibrary = 1 << 4;

    assert.equal(serverTokenModifiersToDccMask(0), 0);
    assert.equal(serverTokenModifiersToDccMask(declaration), declaration);
    assert.equal(serverTokenModifiersToDccMask(readonly), readonly);
    assert.equal(serverTokenModifiersToDccMask(declaration | readonly), declaration | readonly);

    assert.equal(serverTokenModifiersToDccMask(declaration | staticBit), declaration);
    assert.equal(serverTokenModifiersToDccMask(deprecated | defaultLibrary), 0);
});
