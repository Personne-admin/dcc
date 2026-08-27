import * as vscode from 'vscode';
import { DIR_TYPES, DirDocument, DirFunction, DirRef, isTerminatorMnemonic } from '../model/dccDirDocument';
import { Range, splitHarnessLines } from '../model/text';
import { DccTestModelCache } from './dccTestCache';
import { TokenBuilder } from './semanticTokens';

export interface DirScope {
    document: vscode.TextDocument;
    dirDoc: DirDocument;
    baseLine: number;
    bodyLines: string[];
}

export function scopeToContainerRange(scope: DirScope, r: Range): vscode.Range | null {
    const startLine = scope.baseLine + r.start.line;
    const endLine = scope.baseLine + r.end.line;
    if (startLine < scope.baseLine || endLine > scope.baseLine + scope.bodyLines.length)
        return null;

    const startChar = clampChar(scope.bodyLines, r.start.line, r.start.character);
    const endChar = clampChar(scope.bodyLines, r.end.line, r.end.character);
    return new vscode.Range(startLine, startChar, endLine, endChar);
}

export function scopeToContainerPosition(scope: DirScope, p: { line: number; character: number }): vscode.Position | null {
    if (p.line < 0 || p.line >= scope.bodyLines.length)
        return null;

    return new vscode.Position(scope.baseLine + p.line, clampChar(scope.bodyLines, p.line, p.character));
}

export function containerPositionToScope(scope: DirScope, p: vscode.Position): { line: number; character: number } | null {
    const line = p.line - scope.baseLine;
    if (line < 0 || line >= scope.bodyLines.length)
        return null;

    return { line, character: Math.max(0, Math.min(p.character, scope.bodyLines[line].length)) };
}

export function makeDirScope(document: vscode.TextDocument, cache: DccTestModelCache, kind: 'file' | 'section', sectionLine?: number): DirScope | null {
    if (kind === 'file') {
        const doc = cache.getDir(document.getText(), document.uri.toString());
        const bodyLines = splitHarnessLines(document.getText());
        return { document, dirDoc: doc, baseLine: 0, bodyLines };
    }

    const model = cache.get(document);
    const section = model.sections.find((s) => s.kind === 'expect-ir' && s.headerLine === sectionLine);
    if (!section)
        return null;

    const uri = `${document.uri.toString()}@expect-ir:${section.headerLine}`;
    const doc = cache.getDir(section.bodyText, uri);
    return { document, dirDoc: doc, baseLine: section.bodyStartLine, bodyLines: section.bodyLines };
}

function clampChar(lines: string[], line: number, character: number): number {
    const text = lines[line];
    if (text === undefined)
        return 0;

    return Math.max(0, Math.min(character, text.length));
}

const MNEMONIC_DOCS: Record<string, string> = {
    add: 'integer addition',
    sub: 'integer subtraction',
    mul: 'integer multiplication',
    udiv: 'unsigned division',
    sdiv: 'signed division',
    urem: 'unsigned remainder',
    srem: 'signed remainder',
    fdiv: 'floating-point division',
    frem: 'floating-point remainder',
    and: 'bitwise and',
    or: 'bitwise or',
    xor: 'bitwise xor',
    shl: 'shift left',
    lshr: 'logical shift right',
    ashr: 'arithmetic shift right',
    neg: 'integer negation',
    not: 'bitwise not',
    cmp: 'comparison',
    load: 'load from memory',
    'load.volatile': 'volatile load from memory',
    store: 'store to memory',
    'store.volatile': 'volatile store to memory',
    alloca: 'stack allocation',
    gep: 'get element pointer (address arithmetic)',
    zext: 'zero extension',
    sext: 'sign extension',
    trunc: 'integer truncation',
    fpext: 'floating-point extension',
    fptrunc: 'floating-point truncation',
    fptoi: 'float to int conversion',
    itofp: 'int to float conversion',
    ptrtoi: 'pointer to int conversion',
    itoptr: 'int to pointer conversion',
    bitcast: 'bit-preserving cast',
    segcast: 'segment cast',
    extract: 'extract aggregate field',
    insert: 'insert aggregate field',
    aggregate: 'build aggregate value',
    br: 'unconditional branch',
    'br.cond': 'conditional branch',
    ret: 'return from function',
    unreachable: 'unreachable terminator',
    switch: 'switch terminator',
    phi: 'phi node',
    call: 'function call',
    'call.tail': 'tail call',
    atomic_load: 'atomic load',
    atomic_store: 'atomic store',
    atomic_rmw: 'atomic read-modify-write',
    fence: 'memory fence',
    inline_asm: 'inline assembly',
};

export function dirDocumentSymbols(scope: DirScope): vscode.DocumentSymbol[] {
    const symbols: vscode.DocumentSymbol[] = [];
    for (const g of scope.dirDoc.globals) {
        const r = scopeToContainerRange(scope, g.range);
        const sr = scopeToContainerRange(scope, g.nameRange);
        if (!r || !sr)
            continue;

        const gsym = new vscode.DocumentSymbol(
            g.name,
            `${g.isConst ? 'const ' : ''}global : ${g.type}`,
            g.isConst ? vscode.SymbolKind.Constant : vscode.SymbolKind.Variable,
            r,
            sr,
        );

        gsym.children = [];
        symbols.push(gsym);
    }
    for (const fn of scope.dirDoc.functions) {
        const r = scopeToContainerRange(scope, fn.range);
        const sr = scopeToContainerRange(scope, fn.nameRange);
        if (!r || !sr)
            continue;

        const children: vscode.DocumentSymbol[] = [];
        for (const block of fn.blocks) {
            const br = scopeToContainerRange(scope, block.range);
            const bsr = scopeToContainerRange(scope, block.nameRange);
            if (!br || !bsr)
                continue;

            const bsym = new vscode.DocumentSymbol(
                `%${block.name}`,
                block.params.map((p) => `${p.type ?? ''} %${p.name}`).join(', '),
                vscode.SymbolKind.Struct,
                br,
                bsr,
            );

            bsym.children = [];
            children.push(bsym);
        }

        const fsym = new vscode.DocumentSymbol(
            fn.name,
            `${fn.retType}(${fn.paramTypes.join(', ')})`,
            vscode.SymbolKind.Function,
            r,
            sr,
        );

        fsym.children = children;
        symbols.push(fsym);
    }

    return symbols;
}

export function dirFoldingRanges(scope: DirScope): vscode.FoldingRange[] {
    const out: vscode.FoldingRange[] = [];
    for (const fn of scope.dirDoc.functions) {
        if (fn.blocks.length > 0) {
            const start = scope.baseLine + fn.startLine;
            const end = scope.baseLine + fn.range.end.line - 1;
            if (end > start)
                out.push(new vscode.FoldingRange(start, end));
        }
    }

    return out;
}

function refAt(scope: DirScope, p: { line: number; character: number }): { ref: DirRef; fn: DirFunction | undefined } | null {
    const fn = scope.dirDoc.functionAt(p.line);
    if (fn) {
        const block = scope.dirDoc.blockAt(fn, p.line);
        if (block) {
            for (const inst of block.insts) {
                for (const ref of inst.refs)
                    if (ref.range.start.line === p.line && p.character >= ref.range.start.character && p.character <= ref.range.end.character)
                        return { ref, fn };

                if (inst.resultRange && inst.resultRange.start.line === p.line && p.character >= inst.resultRange.start.character && p.character <= inst.resultRange.end.character)
                    return { ref: { kind: 'local', name: inst.resultName!, range: inst.resultRange }, fn };
            }

            for (const param of block.params)
                if (param.nameRange.start.line === p.line && p.character >= param.nameRange.start.character && p.character <= param.nameRange.end.character)
                    return { ref: { kind: 'local', name: param.name, range: param.nameRange }, fn };

            if (block.nameRange.start.line === p.line && p.character >= block.nameRange.start.character && p.character <= block.nameRange.end.character)
                return { ref: { kind: 'block', name: block.name, range: block.nameRange }, fn };
        }

        for (const attr of fn.attrs)
            if (attr.range.start.line === p.line && p.character >= attr.range.start.character && p.character <= attr.range.end.character)
                return { ref: { kind: 'global', name: attr.name, range: attr.range }, fn };

        if (fn.nameRange.start.line === p.line && p.character >= fn.nameRange.start.character && p.character <= fn.nameRange.end.character)
            return { ref: { kind: 'global', name: fn.name, range: fn.nameRange }, fn };
    }

    for (const g of scope.dirDoc.globals)
        if (g.nameRange.start.line === p.line && p.character >= g.nameRange.start.character && p.character <= g.nameRange.end.character)
            return { ref: { kind: 'global', name: g.name, range: g.nameRange }, fn: undefined };

    return null;
}

export function dirHover(scope: DirScope, position: vscode.Position): vscode.Hover | null {
    const p = containerPositionToScope(scope, position);
    if (!p)
        return null;

    const hit = refAt(scope, p);
    if (hit) {
        const { ref, fn } = hit;
        let text = '';
        switch (ref.kind) {
            case 'global': {
                const g = scope.dirDoc.globalByName(ref.name);
                if (g)
                    text = `${g.isConst ? 'const ' : ''}global @${g.name} : ${g.type}`;
                else {
                    const f = scope.dirDoc.functionByName(ref.name);
                    text = f ? `function @${f.name} (${f.retType}(${f.paramTypes.join(', ')}))` : `symbol @${ref.name}`;
                }

                break;
            }
            case 'block': {
                const fn = scope.dirDoc.functionAt(p.line);
                const block = fn?.blocks.find((b) => b.name === ref.name);
                text = block ? `block %${block.name}${block.params.length > 0 ? `(${block.params.map((q) => `${q.type ?? ''} %${q.name}`).join(', ')})` : ''}` : `block %${ref.name}`;
                break;
            }
            case 'local': {
                const f = fn ?? scope.dirDoc.functionAt(p.line);
                const def = f?.locals.get(ref.name);
                text = def ? (def.kind === 'param' ? `parameter %${ref.name}` : `local %${ref.name}`) : `value %${ref.name}`;
                break;
            }
            default:
                break;
        }

        if (text)
            return new vscode.Hover(new vscode.MarkdownString(`\`${text}\``), scopeToContainerRange(scope, ref.range) ?? undefined);
    }

    const fn = scope.dirDoc.functionAt(p.line);
    const block = fn ? scope.dirDoc.blockAt(fn, p.line) : undefined;
    const inst = block?.insts.find((i) => i.line === p.line);

    if (inst?.mnemonic) {
        const doc = MNEMONIC_DOCS[inst.mnemonic] ?? 'instruction';
        const range = scopeToContainerRange(scope, inst.mnemonicRange ?? inst.range);
        return new vscode.Hover(new vscode.MarkdownString(`**${inst.mnemonic}** — ${doc}`), range ?? undefined);
    }

    return null;
}

function localDefLocation(scope: DirScope, fn: DirFunction, name: string): vscode.Location | null {
    const def = fn.locals.get(name);
    if (def) {
        const r = scopeToContainerRange(scope, def.range);
        return r ? new vscode.Location(scope.document.uri, r) : null;
    }

    return null;
}

export function dirDefinition(scope: DirScope, position: vscode.Position): vscode.Location[] {
    const p = containerPositionToScope(scope, position);
    if (!p)
        return [];

    const hit = refAt(scope, p);
    if (!hit)
        return [];

    const { ref, fn } = hit;
    if (ref.kind === 'local' && fn) {
        const loc = localDefLocation(scope, fn, ref.name);
        return loc ? [loc] : [];
    }

    if (ref.kind === 'block') {
        const targetFn = fn ?? scope.dirDoc.functionAt(p.line);
        const block = targetFn?.blocks.find((b) => b.name === ref.name);
        if (block) {
            const r = scopeToContainerRange(scope, block.nameRange);
            return r ? [new vscode.Location(scope.document.uri, r)] : [];
        }

        return [];
    }

    if (ref.kind === 'global') {
        const g = scope.dirDoc.globalByName(ref.name);
        if (g) {
            const r = scopeToContainerRange(scope, g.nameRange);
            return r ? [new vscode.Location(scope.document.uri, r)] : [];
        }

        const f = scope.dirDoc.functionByName(ref.name);
        if (f) {
            const r = scopeToContainerRange(scope, f.nameRange);
            return r ? [new vscode.Location(scope.document.uri, r)] : [];
        }

        if (fn?.attrs.some((a) => a.name === ref.name))
            return [];
    }

    return [];
}

export function dirReferences(scope: DirScope, position: vscode.Position): vscode.Location[] {
    const p = containerPositionToScope(scope, position);
    if (!p)
        return [];

    const hit = refAt(scope, p);
    if (!hit)
        return [];

    const { ref, fn } = hit;
    const out: vscode.Location[] = [];
    const push = (r: Range | undefined): void => {
        if (!r)
            return;

        const mapped = scopeToContainerRange(scope, r);
        if (mapped)
            out.push(new vscode.Location(scope.document.uri, mapped));

    };

    if (ref.kind === 'local' && fn) {
        const def = fn.locals.get(ref.name);
        push(def?.range);
        for (const b of fn.blocks) {
            for (const p2 of b.params)
                if (p2.name === ref.name)
                    push(p2.nameRange);

            for (const inst of b.insts)
                for (const r2 of inst.refs)
                    if (r2.kind === 'local' && r2.name === ref.name)
                        push(r2.range);
        }
    } else if (ref.kind === 'block') {
        const targetFn = fn ?? scope.dirDoc.functionAt(p.line);
        for (const f of targetFn ? [targetFn] : scope.dirDoc.functions) {
            for (const b of f.blocks) {
                if (b.name === ref.name)
                    push(b.nameRange);

                for (const inst of b.insts)
                    for (const r2 of inst.refs)
                        if (r2.kind === 'block' && r2.name === ref.name)
                            push(r2.range);
            }
        }
    } else if (ref.kind === 'global') {
        const g = scope.dirDoc.globalByName(ref.name);
        if (g)
            push(g.nameRange);

        const f = scope.dirDoc.functionByName(ref.name);
        if (f)
            push(f.nameRange);

        const scanFn = (all: boolean): void => {
            for (const f2 of all ? scope.dirDoc.functions : fn ? [fn] : [])
                for (const b of f2.blocks)
                    for (const inst of b.insts)
                        for (const r2 of inst.refs)
                            if (r2.kind === 'global' && r2.name === ref.name)
                                push(r2.range);

            for (const g2 of scope.dirDoc.globals)
                for (const r2 of g2.initRefs)
                    if (r2.kind === 'global' && r2.name === ref.name)
                        push(r2.range);
        };

        scanFn(!fn || (fn.name === ref.name));
    }
    return out;
}

const DIR_TYPE_RE = /\b(?:void|bool|i8|u8|i16|u16|i32|u32|i64|u64|isize|usize|f32|f64)\b/g;
const DIR_WORD_RE = /\b(?:ptr|slice|aggregate|fn|label|to|field|global|const|module|linkonce_odr|weak_odr|import|export|align|section)\b/g;

export function dirSemanticTokens(scope: DirScope): vscode.SemanticTokens {
    const b = new TokenBuilder();
    const doc = scope.dirDoc;

    if (doc.moduleName !== undefined) {
        const line = 0;
        const moduleIdx = scope.bodyLines[line]?.indexOf('module');
        if (moduleIdx !== undefined && moduleIdx >= 0)
            b.add(scope.baseLine + line, moduleIdx, 'module'.length, 'keyword');
    }

    for (const g of doc.globals) {
        b.add(scope.baseLine + g.nameRange.start.line, g.nameRange.start.character, g.nameRange.end.character - g.nameRange.start.character, 'global', true);
        for (const ref of g.initRefs) {
            if (ref.kind === 'global')
                b.add(scope.baseLine + ref.range.start.line, ref.range.start.character, ref.range.end.character - ref.range.start.character, 'global');
            else if (ref.kind === 'const')
                b.add(scope.baseLine + ref.range.start.line, ref.range.start.character, ref.range.end.character - ref.range.start.character, 'constant');

        }
    }

    for (const fn of doc.functions) {
        b.add(scope.baseLine + fn.nameRange.start.line, fn.nameRange.start.character, fn.nameRange.end.character - fn.nameRange.start.character, 'global', true);
        for (const attr of fn.attrs)
            b.add(scope.baseLine + attr.range.start.line, 0, attr.range.end.character, 'attribute');

        for (const block of fn.blocks) {
            b.add(scope.baseLine + block.nameRange.start.line, block.nameRange.start.character, block.nameRange.end.character - block.nameRange.start.character, 'block', true);
            for (const param of block.params) {
                b.add(scope.baseLine + param.nameRange.start.line, param.nameRange.start.character, param.nameRange.end.character - param.nameRange.start.character, 'ssa', true);
                if (param.type) {
                    const typeIdx = param.nameRange.start.character - param.type.length;
                    b.add(scope.baseLine + param.nameRange.start.line, Math.max(0, typeIdx), param.type.length, 'type');
                }
            }

            for (const inst of block.insts) {
                if (inst.resultRange)
                    b.add(scope.baseLine + inst.resultRange.start.line, inst.resultRange.start.character, inst.resultRange.end.character - inst.resultRange.start.character, 'ssa', true);

                if (inst.mnemonicRange)
                    b.add(scope.baseLine + inst.mnemonicRange.start.line, inst.mnemonicRange.start.character, inst.mnemonicRange.end.character - inst.mnemonicRange.start.character, isTerminatorMnemonic(inst.mnemonic) ? 'terminator' : 'instruction');

                for (const ref of inst.refs)
                    if (ref.kind === 'local')
                        b.add(scope.baseLine + ref.range.start.line, ref.range.start.character, ref.range.end.character - ref.range.start.character, 'ssa');
                    else if (ref.kind === 'global')
                        b.add(scope.baseLine + ref.range.start.line, ref.range.start.character, ref.range.end.character - ref.range.start.character, 'global');
                    else if (ref.kind === 'block')
                        b.add(scope.baseLine + ref.range.start.line, ref.range.start.character, ref.range.end.character - ref.range.start.character, 'block');
                    else
                        b.add(scope.baseLine + ref.range.start.line, ref.range.start.character, ref.range.end.character - ref.range.start.character, 'constant');
            }
        }
    }

    for (let line = 0; line < scope.bodyLines.length; ++line) {
        const text = scope.bodyLines[line];
        DIR_TYPE_RE.lastIndex = 0;
        let m: RegExpExecArray | null;
        while ((m = DIR_TYPE_RE.exec(text)) !== null)
            if (!tokenOverlapsRefs(scope, line, m.index, m[0].length))
                b.add(scope.baseLine + line, m.index, m[0].length, 'type');

        DIR_WORD_RE.lastIndex = 0;
        while ((m = DIR_WORD_RE.exec(text)) !== null)
            if (!tokenOverlapsRefs(scope, line, m.index, m[0].length))
                b.add(scope.baseLine + line, m.index, m[0].length, 'keyword');
    }

    return b.build();
}

function tokenOverlapsRefs(scope: DirScope, line: number, start: number, length: number): boolean {
    const fn = scope.dirDoc.functionAt(line);
    if (!fn) {
        const g = scope.dirDoc.globals.find((x) => x.line === line);
        if (g)
            return g.initRefs.some((r) => r.range.start.line === line && r.range.start.character >= start && r.range.start.character < start + length);

        return false;
    }

    const block = scope.dirDoc.blockAt(fn, line);
    const insts = block?.insts ?? [];
    for (const inst of insts) {
        const ranges = [
            inst.resultRange,
            inst.mnemonicRange,
            ...inst.refs.map((r) => r.range),
        ].filter((r): r is Range => !!r);

        for (const r of ranges)
            if (r.start.line === line && r.start.character >= start && r.start.character < start + length)
                return true;
    }

    return false;
}

export function dirCompletion(scope: DirScope, position: vscode.Position): vscode.CompletionItem[] {
    const p = containerPositionToScope(scope, position);
    if (!p)
        return [];

    const items: vscode.CompletionItem[] = [];
    const fn = scope.dirDoc.functionAt(p.line);
    const mnemonics = [
        'add', 'sub', 'mul', 'udiv', 'sdiv', 'urem', 'srem', 'fdiv', 'frem', 'and', 'or', 'xor', 'shl', 'lshr', 'ashr',
        'neg', 'not',
        'cmp.eq', 'cmp.ne', 'cmp.lt', 'cmp.le', 'cmp.gt', 'cmp.ge',
        'cmp.olt', 'cmp.ole', 'cmp.ogt', 'cmp.oge', 'cmp.ult', 'cmp.ule', 'cmp.ugt', 'cmp.uge',
        'load', 'load.volatile', 'store', 'store.volatile', 'alloca', 'gep',
        'zext', 'sext', 'trunc', 'fpext', 'fptrunc', 'fptoi', 'itofp', 'ptrtoi', 'itoptr', 'bitcast', 'segcast',
        'extract', 'insert', 'aggregate',
        'br', 'br.cond', 'ret', 'unreachable', 'switch',
        'phi', 'call', 'call.tail', 'atomic_load', 'atomic_store', 'atomic_rmw', 'fence', 'inline_asm',
    ];

    const seen = new Set<string>();
    const add = (item: vscode.CompletionItem): void => {
        const key = item.label.toString();
        if (seen.has(key))
            return;

        seen.add(key);
        items.push(item);
    };

    if (fn) {
        for (const name of fn.locals.keys()) {
            const item = new vscode.CompletionItem(`%${name}`, vscode.CompletionItemKind.Variable);
            item.insertText = new vscode.SnippetString(`%${name}`);
            add(item);
        }
        for (const block of fn.blocks) {
            const item = new vscode.CompletionItem(`%${block.name}`, vscode.CompletionItemKind.Struct);
            item.insertText = new vscode.SnippetString(`label %${block.name}`);
            item.detail = 'block label';
            add(item);
        }
    }

    for (const g of scope.dirDoc.globals) {
        const item = new vscode.CompletionItem(`@${g.name}`, vscode.CompletionItemKind.Variable);
        item.insertText = new vscode.SnippetString(`@${g.name}`);
        item.detail = g.type;
        add(item);
    }

    for (const f of scope.dirDoc.functions) {
        const item = new vscode.CompletionItem(`@${f.name}`, vscode.CompletionItemKind.Function);
        item.insertText = new vscode.SnippetString(`@${f.name}`);
        item.detail = `${f.retType}(${f.paramTypes.join(', ')})`;
        add(item);
    }

    for (const m of mnemonics) {
        const item = new vscode.CompletionItem(m, vscode.CompletionItemKind.Keyword);
        item.insertText = new vscode.SnippetString(m);
        item.detail = MNEMONIC_DOCS[m];
        add(item);
    }

    for (const t of [...DIR_TYPES]) {
        const item = new vscode.CompletionItem(t, vscode.CompletionItemKind.TypeParameter);
        item.insertText = new vscode.SnippetString(t);
        add(item);
    }

    return items;
}

export function dirDiagnostics(scope: DirScope): vscode.Diagnostic[] {
    const out: vscode.Diagnostic[] = [];
    for (const problem of scope.dirDoc.problems) {
        const r = scopeToContainerRange(scope, problem.range);
        if (!r)
            continue;

        const severity =
            problem.severity === 'error'
                ? vscode.DiagnosticSeverity.Error
                : problem.severity === 'warning'
                    ? vscode.DiagnosticSeverity.Warning
                    : vscode.DiagnosticSeverity.Information;

        out.push(new vscode.Diagnostic(r, problem.message, severity));
    }
    return out;
}

export function dirScopeAtPosition(document: vscode.TextDocument, cache: DccTestModelCache, position: vscode.Position): DirScope | null {
    if (document.languageId !== 'dcc-test')
        return null;

    const model = cache.get(document);
    const section = model.sections.find(
        (s) => s.kind === 'expect-ir' && position.line >= s.bodyStartLine && position.line < s.bodyStartLine + s.bodyLines.length,
    );

    if (!section)
        return null;

    return makeDirScope(document, cache, 'section', section.headerLine);
}
