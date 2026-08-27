import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DirDocument, parseInstLine, scanRefs } from '../model/dccDirDocument';

const IR = [
    'module "test"',
    '',
    'const global @.u16str.0 : [2 x u16] = aggregate [2 x u16], #104, #105',
    '',
    'void@_DC0F1.4.test8.take_u161SCqi16uv([]u16) {',
    '%entry([]u16 %v):',
    '  %0 = alloca []u16',
    '  store []u16 %v, ptr<[]u16> %0',
    '  ret void',
    '}',
    '',
    'i32@_DC0F1.4.test9.test_char0i32s() {',
    '%entry:',
    '  %0 = call fn(@_DC0S1.4.test8.fmt_char1SCqci32sX1tSCqc)([]u8 aggregate []u8, @.str.0, #5) -> i32',
    '  br.cond %0, label %taken, label %done',
    '%taken:',
    '  %1 = add i32 %0, #1',
    '  br label %done',
    '%done:',
    '  ret i32 %1',
    '}',
    '',
].join('\n');

test('parses module, globals and functions', () => {
    const doc = DirDocument.parse(IR, 'file:///cases/out.dir');
    assert.equal(doc.moduleName, 'test');
    assert.equal(doc.globals.length, 1);
    const g = doc.globals[0];
    assert.equal(g.name, '.u16str.0');
    assert.equal(g.isConst, true);
    assert.equal(g.type, '[2 x u16]');
    assert.equal(g.line, 2);
    assert.equal(doc.functions.length, 2);

    const f0 = doc.functions[0];
    assert.equal(f0.name, '_DC0F1.4.test8.take_u161SCqi16uv');
    assert.equal(f0.retType, 'void');
    assert.deepEqual(f0.paramTypes, ['[]u16']);
    assert.equal(f0.blocks.length, 1);

    const f1 = doc.functions[1];
    assert.equal(f1.name, '_DC0F1.4.test9.test_char0i32s');
    assert.equal(f1.retType, 'i32');
    assert.equal(f1.blocks.length, 3);
});

test('blocks, params and instructions', () => {
    const doc = DirDocument.parse(IR, 'file:///cases/out.dir');
    const f0 = doc.functions[0];
    const entry = f0.blocks[0];
    assert.equal(entry.name, 'entry');
    assert.equal(entry.params.length, 1);
    assert.equal(entry.params[0].name, 'v');
    assert.equal(entry.params[0].type, '[]u16');
    assert.equal(entry.insts.length, 3);

    const alloca = entry.insts[0];
    assert.equal(alloca.resultName, '0');
    assert.equal(alloca.mnemonic, 'alloca');
    const store = entry.insts[1];
    assert.equal(store.mnemonic, 'store');
    const ret = entry.insts[2];
    assert.equal(ret.mnemonic, 'ret');

    assert.ok(f0.locals.has('v'));
    assert.equal(f0.locals.get('v')?.kind, 'param');
    assert.ok(f0.locals.has('0'));
    assert.equal(f0.locals.get('0')?.kind, 'local');
});

test('local refs, global refs, block labels and calls', () => {
    const doc = DirDocument.parse(IR, 'file:///cases/out.dir');
    const f1 = doc.functions[1];
    const entry = f1.blocks[0];
    const call = entry.insts[0];
    assert.equal(call.mnemonic, 'call');
    assert.ok(call.refs.some((r) => r.kind === 'global' && r.name === '_DC0S1.4.test8.fmt_char1SCqci32sX1tSCqc'));
    assert.ok(call.refs.some((r) => r.kind === 'global' && r.name === '.str.0'));
    assert.ok(call.refs.some((r) => r.kind === 'const' && r.name === '5'));

    const brcond = entry.insts[1];
    assert.equal(brcond.mnemonic, 'br.cond');
    assert.ok(brcond.refs.some((r) => r.kind === 'local' && r.name === '0'));
    assert.ok(brcond.refs.some((r) => r.kind === 'block' && r.name === 'taken'));
    assert.ok(brcond.refs.some((r) => r.kind === 'block' && r.name === 'done'));

    const taken = f1.blocks[1];
    assert.equal(taken.name, 'taken');
    assert.ok(taken.insts.some((i) => i.refs.some((r) => r.kind === 'block' && r.name === 'done')));

    const g = doc.globals[0];
    assert.ok(g.initRefs.some((r) => r.kind === 'const' && r.name === '104'));
    assert.ok(g.initRefs.some((r) => r.kind === 'const' && r.name === '105'));
});

test('duplicate local definitions are diagnosed', () => {
    const text = [
        'void@f() {',
        '%entry:',
        '  %0 = add i32 %0, #1',
        '  %0 = sub i32 %0, #2',
        '  ret i32 %0',
        '}',
    ].join('\n');

    const doc = DirDocument.parse(text, 'file:///cases/dup.dir');
    assert.ok(doc.problems.some((p) => p.severity === 'warning' && /duplicate local '%0' in function 'f'/.test(p.message)));
});

test('duplicate block labels are diagnosed', () => {
    const text = [
        'void@f() {',
        '%entry:',
        '  br label %other',
        '%other:',
        '  br label %other',
        '%other:',
        '  ret void',
        '}',
    ].join('\n');

    const doc = DirDocument.parse(text, 'file:///cases/dupblock.dir');
    assert.ok(doc.problems.some((p) => p.severity === 'warning' && /duplicate block label '%other'/.test(p.message)));
});

test('unresolved locals are conservatively diagnosed', () => {
    const text = [
        'void@f() {',
        '%entry:',
        '  ret i32 %ghost',
        '}',
    ].join('\n');

    const doc = DirDocument.parse(text, 'file:///cases/unresolved.dir');
    assert.ok(doc.problems.some((p) => p.severity === 'info' && /unresolved local '%ghost' in function 'f'/.test(p.message)));
});

test('function attributes are captured', () => {
    const text = ['@nomangle', '@section("foo")', 'i64@f(i32) {', '%entry:', '  ret i64 #0', '}'].join('\n');
    const doc = DirDocument.parse(text, 'file:///cases/attrs.dir');
    const f = doc.functions[0];
    assert.equal(f.attrs.length, 2);
    assert.equal(f.attrs[0].name, 'nomangle');
    assert.equal(f.attrs[1].name, 'section');
    assert.equal(f.retType, 'i64');
    assert.deepEqual(f.paramTypes, ['i32']);

    assert.equal(f.startLine, 0);
});

test('linkonce_odr linkage on globals and functions', () => {
    const text = [
        'linkonce_odr const global @g : i32 = #7',
        'linkonce_odr void@h() {',
        '%entry:',
        '  ret void',
        '}',
    ].join('\n');

    const doc = DirDocument.parse(text, 'file:///cases/linkage.dir');
    assert.equal(doc.globals[0].linkage, 'linkonce_odr');
    assert.equal(doc.functions[0].name, 'h');
});

test('switch terminator with brace region does not leak across functions', () => {
    const text = [
        'void@a() {',
        '%entry:',
        '  switch i32 %x {',
        '  #0 => label %l0',
        '  default => label %l1',
        '  }',
        '%l0:',
        '  ret void',
        '%l1:',
        '  ret void',
        '}',
        'void@b() {',
        '%entry:',
        '  ret void',
        '}',
    ].join('\n');

    const doc = DirDocument.parse(text, 'file:///cases/switch.dir');
    assert.equal(doc.functions.length, 2);
    const a = doc.functions[0];
    assert.equal(a.blocks.length, 3);
    const b = doc.functions[1];
    assert.equal(b.blocks.length, 1);

    const l1 = a.blocks[2];
    assert.equal(l1.name, 'l1');
});

test('comments are ignored', () => {
    const text = ['// header comment', '; asm-ish comment', 'void@f() {', '%entry:', '  ret void', '}'].join('\n');
    const doc = DirDocument.parse(text, 'file:///cases/comments.dir');
    assert.equal(doc.functions.length, 1);
    assert.equal(doc.problems.filter((p) => p.severity !== 'info').length, 0);
});

test('scanRefs skips string literals', () => {
    const refs = scanRefs('  %0 = inline_asm "mov %rax, %rbx" clobbers=[%rcx], label %after', 0);

    assert.ok(!refs.some((r) => r.name === 'rax'));
    assert.ok(!refs.some((r) => r.name === 'rbx'));

    assert.ok(refs.some((r) => r.kind === 'local' && r.name === 'rcx'));
    assert.ok(refs.some((r) => r.kind === 'block' && r.name === 'after'));
});

test('parseInstLine extracts result, mnemonic and refs with ranges', () => {
    const inst = parseInstLine('  %2 = gep ptr<[]i32> %1, field #0, #1', 3);
    assert.equal(inst.resultName, '2');
    assert.equal(inst.mnemonic, 'gep');
    assert.ok(inst.resultRange);
    assert.equal(inst.resultRange.start.line, 3);
    assert.ok(inst.refs.some((r) => r.kind === 'local' && r.name === '1'));
    assert.ok(inst.refs.some((r) => r.kind === 'const' && r.name === '0'));
    assert.ok(inst.refs.some((r) => r.kind === 'const' && r.name === '1'));

    assert.ok(!inst.refs.some((r) => r.name === '2'));
});

test('block/function lookup by line', () => {
    const doc = DirDocument.parse(IR, 'file:///cases/out.dir');
    const fn = doc.functionAt(13);
    assert.ok(fn);
    assert.equal(fn.name, '_DC0F1.4.test9.test_char0i32s');
    const block = doc.blockAt(fn!, 17);
    assert.ok(block);
    assert.equal(block.name, 'taken');
});

test('CRLF input is handled', () => {
    const text = ['void@f() {', '%entry:', '  ret void', '}'].join('\r\n') + '\r\n';
    const doc = DirDocument.parse(text, 'file:///cases/crlf.dir');
    assert.equal(doc.functions.length, 1);
    assert.equal(doc.functions[0].blocks[0].name, 'entry');
});

test('function/global by name lookup', () => {
    const doc = DirDocument.parse(IR, 'file:///cases/out.dir');
    assert.ok(doc.globalByName('.u16str.0'));
    assert.ok(doc.functionByName('_DC0F1.4.test9.test_char0i32s'));
    assert.equal(doc.globalByName('nope'), undefined);
});
