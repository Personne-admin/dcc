import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DccTestDocument, VirtualFile } from '../model/dccTestDocument';

const LF = '\n';
const CRLF = '\r\n';

test('splits FILE + EXPECT-ERRORS with CRLF and maps virtual content', () => {
    const text = [
        '=== FILE: main.dc ===',
        'module test;',
        '',
        'public void f() {}',
        '=== EXPECT-ERRORS ===',
        "main.dc:3: 'f' is not used",
    ].join(CRLF) + CRLF;

    const doc = DccTestDocument.parse(text, 'file:///cases/basic.dcc-test');

    assert.equal(doc.sections.length, 2);
    const file = doc.sections[0];
    assert.equal(file.kind, 'file');
    assert.equal(file.target, 'main.dc');
    assert.equal(file.headerLine, 0);
    assert.equal(file.bodyStartLine, 1);
    assert.deepEqual(file.bodyLines, ['module test;', '', 'public void f() {}']);
    assert.equal(file.bodyText, 'module test;\n\npublic void f() {}');

    assert.equal(doc.virtualFiles.length, 1);
    const vf = doc.virtualFiles[0];
    assert.equal(vf.content, 'module test;\n\npublic void f() {}\n');
    assert.equal(vf.path, 'main.dc');

    assert.equal(doc.entryPath, 'main.dc');

    assert.equal(doc.virtualLineToContainerLine(vf, 0), 1);
    assert.equal(doc.virtualLineToContainerLine(vf, 1), 2);
    assert.equal(doc.virtualLineToContainerLine(vf, 2), 3);

    assert.equal(doc.virtualLineToContainerLine(vf, 3), null);

    assert.equal(doc.containerLineToVirtualLine(file, 3), 2);
    assert.equal(doc.containerLineToVirtualLine(file, 0), null);
    assert.equal(doc.containerLineToVirtualLine(file, 4), null);
    assert.equal(doc.fileSectionAt(2)?.path, 'main.dc');
    assert.equal(doc.fileSectionAt(0), undefined);

    const err = doc.sections[1];
    assert.equal(err.kind, 'expect-errors');
    assert.equal(err.expectations.length, 1);
    const exp = err.expectations[0];
    assert.equal(exp.file, 'main.dc');
    assert.equal(exp.line, 3);
    assert.equal(exp.message, "'f' is not used");
    assert.equal(exp.valid, true);
    assert.equal(doc.diagnosticLineToContainer(vf, 3), 3);
    assert.equal(doc.diagnosticLineToContainer(vf, 99), null);
});

test('content before the first header is ignored', () => {
    const text = [
        'preamble line, not a section',
        '',
        '=== FILE: main.dc ===',
        'module test;',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/preamble.dcc-test');
    assert.equal(doc.sections.length, 1);
    assert.equal(doc.sections[0].kind, 'file');
    assert.equal(doc.sections[0].headerLine, 2);
});

test('multifile with explicit ENTRY', () => {
    const text = [
        '=== FILE: a.dc ===',
        'module a;',
        '=== FILE: sub/b.dc ===',
        'module b;',
        '=== ENTRY: sub/b.dc ===',
        '=== EXPECT-AST FOR: a.dc ===',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/multi.dcc-test');
    assert.equal(doc.virtualFiles.length, 2);
    assert.equal(doc.virtualFiles[0].path, 'a.dc');
    assert.equal(doc.virtualFiles[1].path, 'sub/b.dc');
    assert.equal(doc.entryPath, 'sub/b.dc');
    const ast = doc.sections.find((s) => s.kind === 'expect-ast');
    assert.ok(ast);
    assert.equal(ast.target, 'a.dc');
    assert.equal(ast.kind, 'expect-ast');
});

test('EXPECT-AST without FOR defaults to entry', () => {
    const text = ['=== FILE: main.dc ===', 'x', '=== EXPECT-AST ===', 'dump'].join(LF);
    const doc = DccTestDocument.parse(text, 'file:///cases/ast.dcc-test');
    const ast = doc.sections.find((s) => s.kind === 'expect-ast');
    assert.ok(ast);
    assert.equal(ast.target, undefined);
    assert.ok(ast.problems.some((p) => p.severity === 'info' && /defaults to the entry/.test(p.message)));
});

test('duplicate FILE paths produce an error diagnostic', () => {
    const text = [
        '=== FILE: a.dc ===',
        'x',
        '=== FILE: a.dc ===',
        'y',
        '=== ENTRY: a.dc ===',
    ].join(LF);
    const doc = DccTestDocument.parse(text, 'file:///cases/dup.dcc-test');
    assert.ok(doc.diagnostics.some((d) => d.severity === 'error' && /duplicate FILE path 'a\.dc'/.test(d.message)));
    assert.equal(doc.virtualFiles.length, 2);
    assert.equal(doc.entryPath, 'a.dc');
});

test('invalid ENTRY and invalid FOR targets are diagnosed locally', () => {
    const text = [
        '=== FILE: main.dc ===',
        'x',
        '=== ENTRY: nope.dc ===',
        '=== EXPECT-AST FOR: missing.dc ===',
    ].join(LF);
    const doc = DccTestDocument.parse(text, 'file:///cases/badref.dcc-test');
    assert.ok(doc.diagnostics.some((d) => d.severity === 'error' && /ENTRY: 'nope\.dc'/.test(d.message)));
    assert.ok(doc.diagnostics.some((d) => d.severity === 'warning' && /EXPECT-AST FOR: 'missing\.dc'/.test(d.message)));
});

test('unknown headers are preserved and reported without breaking splitting', () => {
    const text = [
        '=== FILE: main.dc ===',
        'module test;',
        '=== EXPECT-SEMA ===',
        'some legacy dump',
        '=== EXPECT-ERRORS ===',
        'main.dc:1: boom',
    ].join(LF);
    const doc = DccTestDocument.parse(text, 'file:///cases/unknown.dcc-test');
    const unknown = doc.sections[1];
    assert.equal(unknown.kind, 'unknown');
    assert.equal(unknown.headerText, 'EXPECT-SEMA');
    assert.deepEqual(unknown.bodyLines, ['some legacy dump']);
    assert.equal(doc.sections[2].kind, 'expect-errors');
    assert.equal(doc.sections[2].expectations[0].message, 'boom');
    assert.ok(doc.diagnostics.some((d) => /unknown section header 'EXPECT-SEMA'/.test(d.message)));
});

test('header-like text is only detected by the exact delimiter rule', () => {
    const text = [
        '=== FILE: main.dc ===',
        '// === FILE: fake.dc ===   <- comment, not a header',
        'x === y === z',
        '=== EXPECT-ERRORS ===',
    ].join(LF);
    const doc = DccTestDocument.parse(text, 'file:///cases/exact.dcc-test');

    assert.equal(doc.sections.length, 2);
    assert.equal(doc.sections[0].kind, 'file');
    assert.deepEqual(doc.sections[0].bodyLines, ['// === FILE: fake.dc ===   <- comment, not a header XD', 'x === y === z']);
    assert.equal(doc.virtualFiles[0].path, 'main.dc');
    assert.equal(doc.virtualFiles.length, 1);
});

test('a body line that exactly matches the header rule does split (harness parity)', () => {
    const text = [
        '=== FILE: main.dc ===',
        '=== x ===',
        '=== EXPECT-ERRORS ===',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/exact2.dcc-test');
    assert.equal(doc.sections.length, 3);
    assert.equal(doc.sections[1].headerText, 'x');
    assert.equal(doc.sections[1].kind, 'unknown');
});

test('EXPECT-ERRORS EXACT and EXPECT-ERROR-COUNT parsing', () => {
    const text = [
        '=== FILE: main.dc ===',
        'x',
        '=== EXPECT-ERROR-COUNT: 6 ===',
        '=== EXPECT-ERRORS EXACT ===',
        'main.dc:1: boom',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/count.dcc-test');
    const count = doc.sections.find((s) => s.kind === 'expect-error-count');
    assert.ok(count);
    assert.equal(count.count, 6);
    const errors = doc.sections.find((s) => s.kind === 'expect-errors');
    assert.ok(errors);
    assert.equal(errors.exact, true);
});

test('malformed EXPECT-ERROR-COUNT is diagnosed', () => {
    const text = '=== EXPECT-ERROR-COUNT: xyz ===';
    const doc = DccTestDocument.parse(text, 'file:///cases/badcount.dcc-test');
    const sec = doc.sections[0];
    assert.equal(sec.count, undefined);
    assert.ok(doc.diagnostics.some((d) => d.severity === 'error' && /malformed EXPECT-ERROR-COUNT/.test(d.message)));
});

test('malformed expected-error locations are diagnosed but recoverable', () => {
    const text = [
        '=== FILE: main.dc ===',
        'x',
        '=== EXPECT-ERRORS ===',
        'this line has no colons',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/malformed.dcc-test');
    const errors = doc.sections.find((s) => s.kind === 'expect-errors');
    assert.ok(errors);
    assert.equal(errors.expectations.length, 1);
    assert.equal(errors.expectations[0].valid, false);
    assert.ok(doc.diagnostics.some((d) => /malformed expected-error location/.test(d.message)));
});

test('expected-error line outside the file is diagnosed (clamped mapping)', () => {
    const text = [
        '=== FILE: main.dc ===',
        'module test;',
        '=== EXPECT-ERRORS ===',
        'main.dc:99: boom',
        'other.dc:1: nope',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/outofrange.dcc-test');
    assert.ok(doc.diagnostics.some((d) => /expected error line 99 is outside 'main\.dc' \(1\.\.2\)/.test(d.message)));
    assert.ok(doc.diagnostics.some((d) => /expected-error file 'other\.dc' does not match any FILE section/.test(d.message)));
});

test('expected-error with column parses', () => {
    const text = [
        '=== FILE: main.dc ===',
        'x',
        '=== EXPECT-ERRORS ===',
        'main.dc:1:7: boom',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/col.dcc-test');
    const exp = doc.sections[1].expectations[0];
    assert.equal(exp.column, 7);
    assert.equal(exp.message, 'boom');
});

test('MODE interactive and INJECT are classified', () => {
    const text = [
        '=== FILE: main.dc ===',
        'x',
        '=== MODE: interactive ===',
        '=== INJECT: const bool A = true; ===',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/mode.dcc-test');
    const mode = doc.sections.find((s) => s.kind === 'mode');
    assert.ok(mode);
    assert.equal(mode.interactive, true);
    const inject = doc.sections.find((s) => s.kind === 'inject');
    assert.ok(inject);
    assert.equal(inject.target, 'const bool A = true;');
});

test('EXPECT-IR FLAGS are captured', () => {
    const text = [
        '=== FILE: main.dc ===',
        'x',
        '=== EXPECT-IR FLAGS: -fbounds-check ===',
        'module "test"',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/ir.dcc-test');
    const ir = doc.sections.find((s) => s.kind === 'expect-ir');
    assert.ok(ir);
    assert.equal(ir.flags, '-fbounds-check');
    assert.deepEqual(ir.bodyLines, ['module "test"']);
});

test('em64t object body keys are parsed', () => {
    const text = [
        '=== FILE: test.dc ===',
        'x',
        '=== EXPECT-EM64T-OBJECT ===',
        'RUN-EXIT: 0',
        'PIC: true',
        'CONTAINS: some-bytes',
        'REQUIRE-RELA: .text R_X86_64_PC32',
        'REQUIRE-SECTION-ALIGN: .rodata 16',
        'REQUIRE-COFF-RELOC: .text IMAGE_REL_AMD64_REL32 printf',
        'REQUIRE-COFF-UNDEFINED: printf',
        'FORBID-COFF-DEFINED: main',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/em64t.dcc-test');
    const obj = doc.sections.find((s) => s.kind === 'expect-em64t-object');
    assert.ok(obj);

    const keys = Object.fromEntries(obj.bodyKeys.map((k) => [k.key, k.value]));
    assert.equal(keys['RUN-EXIT'], '0');
    assert.equal(keys['PIC'], 'true');
    assert.equal(keys['CONTAINS'], 'some-bytes');
    assert.equal(keys['REQUIRE-RELA'], '.text R_X86_64_PC32');
    assert.equal(keys['REQUIRE-SECTION-ALIGN'], '.rodata 16');
    assert.equal(keys['REQUIRE-COFF-RELOC'], '.text IMAGE_REL_AMD64_REL32 printf');
    assert.equal(keys['REQUIRE-COFF-UNDEFINED'], 'printf');
    assert.equal(keys['FORBID-COFF-DEFINED'], 'main');
});

test('section ranges are exact and shift after edits/reparse', () => {
    const text = [
        '=== FILE: main.dc ===',
        'a',
        'b',
        '=== EXPECT-ERRORS ===',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/ranges.dcc-test');
    const file = doc.sections[0];
    assert.deepEqual(
        { s: file.bodyRange.start.line, e: file.bodyRange.end.line },
        { s: 1, e: 3 },
    );

    assert.deepEqual(
        { s: file.fullRange.start.line, e: file.fullRange.end.line },
        { s: 0, e: 3 },
    );

    assert.deepEqual(
        { s: file.headerRange.start.line, e: file.headerRange.start.character },
        { s: 0, e: 0 },
    );

    const edited = ['// inserted', '', text].join(LF);
    const doc2 = DccTestDocument.parse(edited, 'file:///cases/ranges.dcc-test');
    const file2 = doc2.sections[0];
    assert.equal(file2.headerLine, 2);
    assert.equal(file2.bodyStartLine, 3);
    assert.deepEqual(file2.bodyLines, ['a', 'b']);
    assert.deepEqual(
        { s: file2.bodyRange.start.line, e: file2.bodyRange.end.line },
        { s: 3, e: 5 },
    );

    const vf2 = doc2.virtualFiles[0];
    assert.equal(doc2.virtualLineToContainerLine(vf2, 0), 3);
    assert.equal(doc2.virtualLineToContainerLine(vf2, 1), 4);
});

test('empty FILE body is reported as info and virtual content is empty', () => {
    const text = ['=== FILE: empty.dc ===', '=== EXPECT-ERRORS ==='].join(LF);
    const doc = DccTestDocument.parse(text, 'file:///cases/empty.dcc-test');
    const vf = doc.virtualFiles[0];
    assert.equal(vf.content, '');
    assert.ok(doc.diagnostics.some((d) => d.severity === 'info' && /FILE section has no body/.test(d.message)));
});

test('EXPECT-WARNINGS entries parse like errors', () => {
    const text = [
        '=== FILE: main.dc ===',
        'x',
        '=== EXPECT-WARNINGS ===',
        'main.dc:1: unused variable',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/warnings.dcc-test');
    const warn = doc.sections.find((s) => s.kind === 'expect-warnings');
    assert.ok(warn);
    assert.equal(warn.expectations[0].message, 'unused variable');
});

test('EXPECT-LLVM-ERRORS is flagged as error-expected', () => {
    const text = ['=== FILE: test.dc ===', 'x', '=== EXPECT-LLVM-ERRORS FLAGS: -verify ==='].join(LF);
    const doc = DccTestDocument.parse(text, 'file:///cases/llvm.dcc-test');
    const llvm = doc.sections.find((s) => s.kind === 'expect-llvm');
    assert.ok(llvm);

    assert.equal(llvm.isError, true);
    assert.equal(llvm.flags, '-verify');
});

test('EXPECT-EXECUTABLE-VALID matches EXPECT-EXECUTABLE (harness parity)', () => {
    const text = ['=== FILE: test.dc ===', 'x', '=== EXPECT-EXECUTABLE-VALID ==='].join(LF);
    const doc = DccTestDocument.parse(text, 'file:///cases/exe.dcc-test');
    const exe = doc.sections[1];
    assert.equal(exe.kind, 'expect-executable');
    assert.equal(exe.isError, false);
});

test('CR-only trailing whitespace handling and body line preservation', () => {
    const text = ['=== FILE: main.dc ===', '  indented  ', 'tail   '].join('\r\n') + '\r\n';
    const doc = DccTestDocument.parse(text, 'file:///cases/cr.dcc-test');

    assert.deepEqual(doc.sections[0].bodyLines, ['  indented  ', 'tail   ', '']);
    assert.equal(doc.virtualFiles[0].content, '  indented  \ntail   \n');
});

function expectNoFileDiags(doc: DccTestDocument): void {
    assert.equal(doc.diagnostics.filter((d) => d.severity === 'error').length, 0);
}

test('well-formed fixture has no error diagnostics', () => {
    const text = [
        '=== FILE: main.dc ===',
        'module test;',
        'public void f() {}',
        '=== EXPECT-IR ===',
        'module "test"',
        'void@f() {',
        '%entry:',
        '  ret void',
        '}',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/clean.dcc-test');
    expectNoFileDiags(doc);
    assert.equal(doc.sections.length, 2);
});

test('virtual file helpers find sections by path and position', () => {
    const text = [
        '=== FILE: one.dc ===',
        'a',
        'b',
        '=== FILE: two.dc ===',
        'c',
        '=== ENTRY: two.dc ===',
    ].join(LF);

    const doc = DccTestDocument.parse(text, 'file:///cases/find.dcc-test');
    const vf: VirtualFile | undefined = doc.fileSectionForPath('two.dc');
    assert.ok(vf);
    assert.equal(vf.section.bodyStartLine, 4);
    assert.equal(doc.fileSectionAt(1)?.path, 'one.dc');
    assert.equal(doc.fileSectionAt(2)?.path, 'one.dc');
    assert.equal(doc.fileSectionAt(4)?.path, 'two.dc');
    assert.equal(doc.virtualLineToContainerLine(vf, 0), 4);

    assert.equal(doc.fileSectionAt(0), undefined);
    assert.equal(doc.fileSectionAt(3), undefined);
    assert.equal(doc.fileSectionAt(5), undefined);
});
