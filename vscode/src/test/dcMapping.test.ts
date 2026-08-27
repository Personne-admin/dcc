import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DccTestDocument } from '../model/dccTestDocument';
import {
    DiagnosticsAggregator,
    intersectVirtualRange,
    mapLspLocation,
    mapLspLocationLink,
    mapWorkspaceEdit,
    orderedVirtualFilesForSync,
    VirtualOwner,
} from '../model/dcMapping';
import { LspLocation, LspLocationLink, LspWorkspaceEdit } from '../model/lspTypes';
import { virtualDocumentRange, virtualUriFor } from '../model/virtualFs';

const CONTAINER_A = 'file:///cases/a.dcc-test';
const CONTAINER_B = 'file:///cases/b.dcc-test';

function fixtureA(): DccTestDocument {
    return DccTestDocument.parse(
        [
            '=== FILE: main.dc ===',
            'module test;',
            'public void f() {}',
            'public void g() { f(); }',
            '=== FILE: util.dc ===',
            'module util;',
            'public void helper() {}',
            '=== EXPECT-ERRORS ===',
        ].join('\n'),
        CONTAINER_A,
    );
}

function fixtureB(): DccTestDocument {
    return DccTestDocument.parse(
        [
            '=== FILE: main.dc ===',
            'module other;',
            'public void f() {}',
            '=== EXPECT-ERRORS ===',
        ].join('\n'),
        CONTAINER_B,
    );
}

function makeResolver(a: DccTestDocument, b: DccTestDocument): (uri: string) => VirtualOwner | null {
    const models = new Map<string, DccTestDocument>([
        [CONTAINER_A, a],
        [CONTAINER_B, b],
    ]);

    return (uri: string) => {
        for (const [containerUri, model] of models) {
            const vf = model.virtualFiles.find((v) => virtualUriFor(containerUri, v.path) === uri);
            if (vf)
                return { containerUri, path: vf.path, vf };

        }
        return null;
    };
}

test('sync ordering puts the effective entry FILE last', () => {
    const doc = DccTestDocument.parse(
        [
            '=== FILE: a.dc ===',
            'module a;',
            '=== FILE: main.dc ===',
            'module main;',
            '=== ENTRY: main.dc ===',
            '=== FILE: c.dc ===',
            'module c;',
            '=== EXPECT-ERRORS ===',
        ].join('\n'),
        CONTAINER_A,
    );

    const ordered = orderedVirtualFilesForSync(doc).map((v) => v.path);
    assert.deepEqual(ordered, ['a.dc', 'c.dc', 'main.dc']);
});

test('sync ordering defaults the first FILE as entry and moves it last', () => {
    const doc = DccTestDocument.parse(
        ['=== FILE: main.dc ===', 'module main;', '=== FILE: util.dc ===', 'module util;'].join('\n'),
        CONTAINER_A,
    );

    assert.equal(doc.entryPath, 'main.dc');
    const ordered = orderedVirtualFilesForSync(doc).map((v) => v.path);
    assert.deepEqual(ordered, ['util.dc', 'main.dc']);
});

test('cross-file location mapping re-homes virtual targets to their own container', () => {
    const a = fixtureA();
    const b = fixtureB();
    const resolve = makeResolver(a, b);

    const sameLoc: LspLocation = {
        uri: virtualUriFor(CONTAINER_A, 'main.dc'),
        range: { start: { line: 1, character: 11 }, end: { line: 1, character: 12 } },
    };

    const mappedSame = mapLspLocation(sameLoc, resolve);
    assert.ok(mappedSame);
    assert.equal(mappedSame!.uri, CONTAINER_A);
    assert.equal(mappedSame!.rehomed, true);
    assert.deepEqual(mappedSame!.range, { start: { line: 2, character: 11 }, end: { line: 2, character: 12 } });

    const crossLoc: LspLocation = {
        uri: virtualUriFor(CONTAINER_B, 'main.dc'),
        range: { start: { line: 0, character: 11 }, end: { line: 0, character: 12 } },
    };

    const mappedCross = mapLspLocation(crossLoc, resolve);
    assert.ok(mappedCross);
    assert.equal(mappedCross!.uri, CONTAINER_B);
    assert.deepEqual(mappedCross!.range, { start: { line: 1, character: 11 }, end: { line: 1, character: 12 } });
});

test('location links use the target selection range', () => {
    const a = fixtureA();
    const resolve = makeResolver(a, fixtureB());
    const link: LspLocationLink = {
        originSelectionRange: { start: { line: 3, character: 13 }, end: { line: 3, character: 14 } },
        targetUri: virtualUriFor(CONTAINER_A, 'main.dc'),
        targetRange: { start: { line: 0, character: 0 }, end: { line: 2, character: 20 } },
        targetSelectionRange: { start: { line: 2, character: 11 }, end: { line: 2, character: 12 } },
    };

    const mapped = mapLspLocationLink(link, resolve);
    assert.ok(mapped);
    assert.equal(mapped!.uri, CONTAINER_A);
    assert.deepEqual(mapped!.range, { start: { line: 3, character: 11 }, end: { line: 3, character: 12 } });
});

test('real file locations pass through unchanged', () => {
    const resolve = makeResolver(fixtureA(), fixtureB());
    const loc: LspLocation = {
        uri: 'file:///src/real.dc',
        range: { start: { line: 0, character: 1 }, end: { line: 0, character: 2 } },
    };

    const mapped = mapLspLocation(loc, resolve);
    assert.ok(mapped);
    assert.equal(mapped!.uri, 'file:///src/real.dc');
    assert.equal(mapped!.rehomed, false);
    assert.deepEqual(mapped!.range, { start: { line: 0, character: 1 }, end: { line: 0, character: 2 } });
});

test('unmappable virtual locations and unknown schemes are rejected', () => {
    const a = fixtureA();
    const resolve = makeResolver(a, fixtureB());

    const bad: LspLocation = {
        uri: virtualUriFor(CONTAINER_A, 'main.dc'),
        range: { start: { line: 0, character: 0 }, end: { line: 3, character: 0 } },
    };
    assert.equal(mapLspLocation(bad, resolve), null);

    assert.equal(mapLspLocation({ uri: virtualUriFor('file:///unknown.dcc-test', 'x.dc'), range: bad.range }, resolve), null);
    assert.equal(mapLspLocation({ uri: 'dcc-core:///generated.dc', range: bad.range }, resolve), null);
});

test('workspace edits: virtual edits re-home, real file edits pass through, unmappable dropped', () => {
    const a = fixtureA();
    const resolve = makeResolver(a, fixtureB());

    const edit: LspWorkspaceEdit = {
        changes: {
            [virtualUriFor(CONTAINER_A, 'main.dc')]: [
                { range: { start: { line: 0, character: 7 }, end: { line: 0, character: 11 } }, newText: 'test2' },
            ],
            [virtualUriFor(CONTAINER_B, 'main.dc')]: [
                { range: { start: { line: 1, character: 11 }, end: { line: 1, character: 12 } }, newText: 'g' },
            ],
            'file:///src/real.dc': [{ range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } }, newText: '// x\n' }],
            [virtualUriFor(CONTAINER_A, 'util.dc')]: [
                { range: { start: { line: 0, character: 0 }, end: { line: 99, character: 0 } }, newText: 'x' },
            ],
            [virtualUriFor('file:///gone.dcc-test', 'x.dc')]: [
                { range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } }, newText: 'y' },
            ],
            'dcc-core:///gen.dc': [{ range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } }, newText: 'z' }],
        },
    };

    const mapped = mapWorkspaceEdit(edit, resolve);
    assert.ok(mapped);
    const changes = mapped!.changes!;
    assert.deepEqual(Object.keys(changes).sort(), [CONTAINER_A, CONTAINER_B, 'file:///src/real.dc'].sort());
    assert.deepEqual(changes[CONTAINER_A], [{ range: { start: { line: 1, character: 7 }, end: { line: 1, character: 11 } }, newText: 'test2' }]);
    assert.deepEqual(changes[CONTAINER_B], [{ range: { start: { line: 2, character: 11 }, end: { line: 2, character: 12 } }, newText: 'g' }]);
    assert.deepEqual(changes['file:///src/real.dc'], [{ range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } }, newText: '// x\n' }]);
});

test('workspace edits with only unmappable/unknown targets produce null', () => {
    const a = fixtureA();
    const resolve = makeResolver(a, fixtureB());
    const edit: LspWorkspaceEdit = {
        changes: {
            [virtualUriFor(CONTAINER_A, 'util.dc')]: [
                { range: { start: { line: 0, character: 0 }, end: { line: 99, character: 0 } }, newText: 'x' },
            ],
        },
    };
    assert.equal(mapWorkspaceEdit(edit, resolve), null);
});

test('diagnostics aggregator accumulates per container and per path', () => {
    const agg = new DiagnosticsAggregator<string>();

    agg.set(CONTAINER_A, 'main.dc', ['m1', 'm2']);
    agg.set(CONTAINER_A, 'util.dc', ['u1']);
    assert.deepEqual(agg.forContainer(CONTAINER_A), ['m1', 'm2', 'u1']);

    agg.set(CONTAINER_A, 'main.dc', ['m3']);
    assert.deepEqual(agg.forContainer(CONTAINER_A), ['m3', 'u1']);

    agg.set(CONTAINER_A, 'util.dc', []);
    assert.deepEqual(agg.forContainer(CONTAINER_A), ['m3']);
    assert.ok(agg.hasContainer(CONTAINER_A));

    agg.dropContainer(CONTAINER_A);
    assert.deepEqual(agg.forContainer(CONTAINER_A), []);
    assert.ok(!agg.hasContainer(CONTAINER_A));

    agg.drop(CONTAINER_A, 'util.dc');
    assert.deepEqual(agg.forContainer(CONTAINER_A), []);
});

test('intersectVirtualRange maps overlapping container ranges into virtual coordinates', () => {
    const a = fixtureA();
    const vf = a.fileSectionForPath('main.dc')!;
    assert.ok(vf);

    const inter = intersectVirtualRange(vf, { start: { line: 2, character: 2 }, end: { line: 3, character: 6 } });
    assert.deepEqual(inter, { start: { line: 1, character: 2 }, end: { line: 2, character: 6 } });

    const clamped = intersectVirtualRange(vf, { start: { line: 1, character: 0 }, end: { line: 4, character: 0 } });
    assert.deepEqual(clamped, { start: { line: 0, character: 0 }, end: { line: 2, character: 'public void g() { f(); }'.length } });

    assert.equal(intersectVirtualRange(vf, { start: { line: 0, character: 0 }, end: { line: 0, character: 4 } }), null);
    assert.equal(intersectVirtualRange(vf, { start: { line: 10, character: 0 }, end: { line: 11, character: 0 } }), null);
});

test('virtualDocumentRange covers the whole virtual document', () => {
    const a = fixtureA();
    const vf = a.fileSectionForPath('main.dc')!;
    const r = virtualDocumentRange(vf);

    assert.deepEqual(r, { start: { line: 0, character: 0 }, end: { line: 3, character: 0 } });
});
