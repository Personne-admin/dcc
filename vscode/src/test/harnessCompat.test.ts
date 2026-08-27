import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { DccTestDocument } from '../model/dccTestDocument';
import { DirDocument } from '../model/dccDirDocument';

const casesRoot = resolve(process.cwd(), '..', 'tests', 'cases');

function collectCases(dir: string, out: string[]): string[] {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const p = join(dir, entry.name);
        if (entry.isDirectory())
            collectCases(p, out);
        else if (entry.name.endsWith('.dcc-test'))
            out.push(p);

    }
    return out;
}

const corpusPresent = existsSync(casesRoot);
const corpus = corpusPresent ? collectCases(casesRoot, []) : [];

test('real fixture corpus is parsed without crashes or data loss', { skip: !corpusPresent }, () => {
    assert.ok(corpus.length > 100, 'expected a substantial corpus');
    let totalFiles = 0;
    let totalSections = 0;

    for (const f of corpus) {
        const doc = DccTestDocument.parse(readFileSync(f, 'utf8'), 'file://' + f);
        assert.ok(doc.sections.length > 0, `no sections in ${f}`);

        for (const sec of doc.sections) {
            assert.ok(sec.headerLine >= 0, `bad headerLine in ${f}`);
            assert.ok(sec.bodyStartLine >= sec.headerLine, `bad bodyStartLine in ${f}`);
        }

        totalFiles += doc.virtualFiles.length;
        totalSections += doc.sections.length;

        if (!doc.sections.some((s) => s.kind === 'entry'))
            if (doc.virtualFiles.length > 0)
                assert.equal(doc.entryPath, doc.virtualFiles[0].path);
    }

    assert.ok(totalFiles > 1000, 'expected >1000 virtual files across the corpus');
    assert.ok(totalSections > 1000, 'expected >1000 sections across the corpus');
});

test('all EXPECT-IR bodies in the corpus parse as DIR', { skip: !corpusPresent }, () => {
    let irBodies = 0;
    let structuralProblems = 0;

    for (const f of corpus) {
        const doc = DccTestDocument.parse(readFileSync(f, 'utf8'), 'file://' + f);
        for (const sec of doc.sections) {
            if (sec.kind === 'expect-ir') {
                const dir = DirDocument.parse(sec.bodyText, `${f}#${sec.headerLine}`);
                structuralProblems += dir.problems.filter((p) => p.severity !== 'info').length;
                ++irBodies;
            }
        }
    }

    assert.ok(irBodies >= 200, 'expected >=200 EXPECT-IR sections');
    assert.equal(structuralProblems, 0, 'DIR structural warnings on real output indicate a parser bug');
});

test('known real fixtures produce harness-consistent expectations', { skip: !corpusPresent }, () => {
    const zvoid = join(casesRoot, 'sema', 'z-void-rejection.dcc-test');
    if (existsSync(zvoid)) {
        const doc = DccTestDocument.parse(readFileSync(zvoid, 'utf8'), 'file://' + zvoid);
        const errors = doc.sections.find((s) => s.kind === 'expect-errors');
        assert.ok(errors);
        assert.equal(errors.expectations.length, 7);
        assert.equal(errors.expectations[0].file, 'main.dc');
        assert.equal(errors.expectations[0].line, 6);
        assert.equal(errors.expectations[0].message, 'void type is not allowed in variable type');
        assert.equal(doc.entryPath, 'main.dc');
        const vf = doc.fileSectionForPath('main.dc');
        assert.ok(vf);
        assert.equal(doc.diagnosticLineToContainer(vf, 6), vf.section.bodyStartLine + 5);
    }

    const ir = join(casesRoot, 'ir', 'zv-pack-u16-str-slice.dcc-test');
    if (existsSync(ir)) {
        const doc = DccTestDocument.parse(readFileSync(ir, 'utf8'), 'file://' + ir);
        const irSec = doc.sections.find((s) => s.kind === 'expect-ir');
        assert.ok(irSec);
        const dir = DirDocument.parse(irSec.bodyText, 'ir');
        assert.equal(dir.moduleName, 'test');
        assert.ok(dir.functions.length >= 3);
        assert.ok(dir.globals.some((g) => g.name.startsWith('.u16str')));
        const calls = dir.functions.flatMap((f) => f.blocks).flatMap((b) => b.insts).filter((i) => i.mnemonic === 'call');
        assert.ok(calls.length > 0);
    }
});
