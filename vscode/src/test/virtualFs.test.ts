import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DccTestDocument } from '../model/dccTestDocument';
import {
    decodeBase64Url,
    encodeBase64Url,
    virtualUriFor,
    containerUriFromVirtual,
    virtualPathFromUri,
    isVirtualUri,
    mapVirtualPositionToContainer,
    mapContainerPositionToVirtual,
    mapVirtualRangeToContainer,
    mapContainerRangeToVirtual,
} from '../model/virtualFs';

test('base64url round-trips', () => {
    const samples = ['file:///tmp/a.dcc-test', 'dccv:abc/def', 'résumé://π/日本語', '', 'a'.repeat(300)];
    for (const s of samples)
        assert.equal(decodeBase64Url(encodeBase64Url(s)), s);
});

test('virtual URIs are isolated per container document', () => {
    const c1 = 'file:///cases/a.dcc-test';
    const c2 = 'file:///cases/b.dcc-test';
    const u1 = virtualUriFor(c1, 'main.dc');
    const u2 = virtualUriFor(c2, 'main.dc');
    assert.notEqual(u1, u2);
    assert.ok(isVirtualUri(u1));
    assert.ok(!isVirtualUri('file:///x.dc'));
    assert.equal(containerUriFromVirtual(u1), c1);
    assert.equal(containerUriFromVirtual(u2), c2);
    assert.equal(virtualPathFromUri(u1), 'main.dc');
    assert.equal(virtualPathFromUri(u2), 'main.dc');
    assert.equal(containerUriFromVirtual('file:///nope.dc'), null);
});

test('nested virtual paths survive the round trip', () => {
    const c = 'file:///cases/a.dcc-test';
    const u = virtualUriFor(c, 'sub/dir/lib.dc');
    assert.equal(virtualPathFromUri(u), 'sub/dir/lib.dc');
    assert.equal(containerUriFromVirtual(u), c);
});

test('container <-> virtual position mapping on a CRLF fixture', () => {
    const text = ['=== FILE: main.dc ===', 'module test;', '', 'public void f() {}', '=== EXPECT-ERRORS ==='].join('\r\n') + '\r\n';
    const doc = DccTestDocument.parse(text, 'file:///cases/map.dcc-test');
    const vf = doc.virtualFiles[0];
    assert.ok(vf);

    const mapped = mapVirtualPositionToContainer(vf, 2, 7);
    assert.deepEqual(mapped, { line: 3, character: 7 });
    const back = mapContainerPositionToVirtual(vf, 3, 7);
    assert.deepEqual(back, { line: 2, character: 7 });

    assert.equal(mapVirtualPositionToContainer(vf, 2, 999)?.character, 'public void f() {}'.length);
    assert.equal(mapContainerPositionToVirtual(vf, 3, 999)?.character, 'public void f() {}'.length);

    assert.equal(mapVirtualPositionToContainer(vf, 3, 0), null);
    assert.equal(mapVirtualPositionToContainer(vf, -1, 0), null);
    assert.equal(mapContainerPositionToVirtual(vf, 4, 0), null);
});

test('range mapping round-trips', () => {
    const text = ['=== FILE: main.dc ===', 'module test;', '', 'public void f() {}', '=== EXPECT-ERRORS ==='].join('\n');
    const doc = DccTestDocument.parse(text, 'file:///cases/range.dcc-test');
    const vf = doc.virtualFiles[0];
    const vr = { start: { line: 0, character: 2 }, end: { line: 2, character: 8 } };
    const cr = mapVirtualRangeToContainer(vf, vr);

    assert.ok(cr);
    assert.deepEqual(cr, { start: { line: 1, character: 2 }, end: { line: 3, character: 8 } });

    const back = mapContainerRangeToVirtual(vf, cr);
    assert.deepEqual(back, vr);
});

test('unmappable ranges are rejected', () => {
    const text = ['=== FILE: main.dc ===', 'x', '=== EXPECT-ERRORS ==='].join('\n');
    const doc = DccTestDocument.parse(text, 'file:///cases/nomap.dcc-test');
    const vf = doc.virtualFiles[0];

    assert.equal(mapVirtualRangeToContainer(vf, { start: { line: 0, character: 0 }, end: { line: 1, character: 0 } }), null);
});

test('exact delimiter rule protects embedded DC bodies from mis-splitting', () => {
    const text = [
        '=== FILE: main.dc ===',
        '// "=== FILE: not-a-header ===" inside a comment',
        'const s = "=== still not a header ===";',
        '=== EXPECT-ERRORS ===',
    ].join('\n');

    const doc = DccTestDocument.parse(text, 'file:///cases/protect.dcc-test');
    assert.equal(doc.virtualFiles.length, 1);
    assert.equal(doc.virtualFiles[0].path, 'main.dc');
    assert.equal(doc.sections[0].bodyLines.length, 2);
});
