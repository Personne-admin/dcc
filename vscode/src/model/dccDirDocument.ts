import { Range, range, splitHarnessLines, trimWhitespace } from './text';

export type DirRefKind = 'local' | 'global' | 'block' | 'const';

export interface DirRef {
    kind: DirRefKind;
    name: string;
    range: Range;
}

export interface DirInst {
    line: number;
    text: string;
    resultName?: string;
    resultRange?: Range;
    mnemonic?: string;
    mnemonicRange?: Range;
    refs: DirRef[];
    range: Range;
}

export interface DirBlockParam {
    name: string;
    nameRange: Range;
    type?: string;
}

export interface DirBlock {
    name: string;
    nameRange: Range;
    params: DirBlockParam[];
    headerLine: number;
    insts: DirInst[];
    range: Range;
}

export interface DirFunction {
    name: string;
    nameRange: Range;
    retType: string;
    paramTypes: string[];
    paramTypesRange: Range;
    attrs: { name: string; range: Range }[];
    linkage?: string;
    headerLine: number;
    startLine: number;
    blocks: DirBlock[];
    range: Range;
    locals: Map<string, { kind: 'local' | 'param'; range: Range }>;
}

export interface DirGlobal {
    name: string;
    nameRange: Range;
    type: string;
    linkage?: string;
    isConst: boolean;
    line: number;
    initRefs: DirRef[];
    range: Range;
}

export interface DirProblem {
    severity: 'error' | 'warning' | 'info';
    message: string;
    range: Range;
}

export interface DirDocumentData {
    moduleName?: string;
    globals: DirGlobal[];
    functions: DirFunction[];
    problems: DirProblem[];
}

const TERMINATOR_MNEMONICS = new Set(['br', 'br.cond', 'ret', 'unreachable', 'switch']);

export const DIR_TYPES = new Set([
    'void', 'bool', 'i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64', 'isize', 'usize', 'f32', 'f64',
]);

export function scanRefs(line: string, lineIndex: number, startChar = 0): DirRef[] {
    const refs: DirRef[] = [];
    const n = line.length;

    let i = startChar;
    while (i < n) {
        const ch = line[i];
        if (ch === '"') {
            ++i;
            while (i < n && line[i] !== '"') {
                if (line[i] === '\\')
                    ++i;

                ++i;
            }
            ++i;
            continue;
        }

        if (ch === 'l' && line.startsWith('label ', i)) {
            const m = /%([A-Za-z0-9_$][A-Za-z0-9_$.]*)/.exec(line.slice(i + 6));
            if (m) {
                const off = i + 6 + m.index;
                refs.push({ kind: 'block', name: m[1], range: range(lineIndex, off, lineIndex, off + m[0].length) });
                i = off + m[0].length;
                continue;
            }
            i += 6;
            continue;
        }

        if (ch === '%') {
            const m = /^%([A-Za-z0-9_$?][A-Za-z0-9_$.]*)/.exec(line.slice(i));
            if (m) {
                const name = m[1];
                if (name !== '?')
                    refs.push({ kind: 'local', name, range: range(lineIndex, i, lineIndex, i + m[0].length) });

                i += m[0].length;
                continue;
            }
            ++i;
            continue;
        }
        if (ch === '@') {
            const m = /^@([A-Za-z0-9_$.]+)/.exec(line.slice(i));
            if (m) {
                refs.push({ kind: 'global', name: m[1], range: range(lineIndex, i, lineIndex, i + m[0].length) });
                i += m[0].length;
                continue;
            }
            ++i;
            continue;
        }
        if (ch === '#') {
            const m = /^#([A-Za-z0-9_]+)/.exec(line.slice(i));
            if (m) {
                refs.push({ kind: 'const', name: m[1], range: range(lineIndex, i, lineIndex, i + m[0].length) });
                i += m[0].length;
                continue;
            }
            ++i;
            continue;
        }
        ++i;
    }
    return refs;
}

function isCommentLine(line: string): boolean {
    const trimmed = trimWhitespace(line);
    return trimmed.startsWith('//') || trimmed.startsWith(';');
}

export function parseInstLine(raw: string, lineIndex: number): DirInst {
    const text = trimWhitespace(raw);
    const inst: DirInst = { line: lineIndex, text, refs: [], range: range(lineIndex, 0, lineIndex, raw.length) };
    let startChar = 0;

    const resultMatch = /^(%([A-Za-z0-9_$][A-Za-z0-9_$.]*))\s*=\s*/.exec(text);
    if (resultMatch) {
        inst.resultName = resultMatch[2];
        const relStart = text.indexOf(resultMatch[1]);
        inst.resultRange = range(lineIndex, relStart, lineIndex, relStart + resultMatch[1].length);
        startChar = resultMatch[0].length;
    }

    const mnemonic = /^([a-z][a-z0-9_.]*)/.exec(text.slice(startChar));
    if (mnemonic) {
        inst.mnemonic = mnemonic[1];
        inst.mnemonicRange = range(lineIndex, startChar + mnemonic.index, lineIndex, startChar + mnemonic.index + mnemonic[1].length);
    }

    inst.refs = scanRefs(raw, lineIndex, startChar);
    return inst;
}

export function isTerminatorMnemonic(m: string | undefined): boolean {
    return m !== undefined && TERMINATOR_MNEMONICS.has(m);
}

export function parseDirText(text: string): DirDocumentData {
    const lines = splitHarnessLines(text);
    const problems: DirProblem[] = [];
    const globals: DirGlobal[] = [];
    const functions: DirFunction[] = [];
    let moduleName: string | undefined;

    let currentFn: DirFunction | null = null;
    let currentBlock: DirBlock | null = null;
    let pendingAttrs: { name: string; range: Range }[] = [];
    let braceDepth = 0;

    const problem = (severity: DirProblem['severity'], message: string, r: Range): void => {
        problems.push({ severity, message, range: r });
    };

    const closeBlock = (): void => {
        currentBlock = null;
    };

    const closeFunction = (closingLine: number): void => {
        if (currentFn) {
            if (currentBlock)
                currentBlock.range = range(currentBlock.headerLine, 0, closingLine, 0);

            currentFn.range = range(currentFn.startLine, 0, closingLine + 1, 0);
        }

        currentFn = null;
        currentBlock = null;
        pendingAttrs = [];
        braceDepth = 0;
    };

    for (let i = 0; i < lines.length; ++i) {
        const raw = lines[i];
        const trimmed = trimWhitespace(raw);
        if (trimmed.length === 0 || isCommentLine(raw))
            continue;

        if (!currentFn) {
            const attrMatch = /^@([A-Za-z_$][A-Za-z0-9_$]*)(?:\([^)]*\))?\s*$/.exec(trimmed);
            if (attrMatch) {
                pendingAttrs.push({ name: attrMatch[1], range: range(i, 0, i, trimmed.length) });
                continue;
            }
            const modMatch = /^module\s+"(.*)"\s*$/.exec(trimmed);
            if (modMatch) {
                moduleName = modMatch[1];
                continue;
            }
            const globalMatch =
                /^(?:(linkonce_odr|weak_odr)\s+)?(?:(import|export)\s+)*(?:const\s+)?global\s+@([A-Za-z0-9_$.]+)\s*:\s*(.+?)(?:\s+align=\d+)?(?:\s+section="[^"]*")?\s*(?:=\s*(.*))?$/.exec(
                    trimmed,
                );
            if (globalMatch) {
                const name = globalMatch[3];
                const nameStart = trimmed.indexOf('@' + name);
                globals.push({
                    name,
                    nameRange: range(i, nameStart, i, nameStart + name.length + 1),
                    type: globalMatch[4] ?? '',
                    linkage: globalMatch[1],
                    isConst: /(^|\s)const\s+global\b/.test(trimmed),
                    line: i,
                    initRefs: scanRefs(raw, i),
                    range: range(i, 0, i, raw.length),
                });
                continue;
            }
            if (trimmed.endsWith('{')) {
                const fnMatch = /(@([A-Za-z0-9_$.]+)\s*\(([^)]*)\))\s*(?:align=\d+\s*)?\{$/.exec(trimmed);
                if (fnMatch) {
                    const name = fnMatch[2];
                    const nameStart = trimmed.indexOf('@' + name);

                    const paramTypesText = fnMatch[3];
                    const paramTypes = paramTypesText
                        .split(',')
                        .map((p) => trimWhitespace(p))
                        .filter((p) => p.length > 0);

                    const startLine = pendingAttrs.length > 0 ? Math.min(...pendingAttrs.map((a) => a.range.start.line)) : i;
                    const fn: DirFunction = {
                        name,
                        nameRange: range(i, nameStart, i, nameStart + name.length),
                        retType: trimWhitespace(trimmed.slice(0, nameStart)),
                        paramTypes,
                        paramTypesRange: range(i, nameStart + name.length + 1, i, nameStart + name.length + 1 + paramTypesText.length),
                        attrs: pendingAttrs,
                        headerLine: i,
                        startLine,
                        blocks: [],
                        range: range(startLine, 0, i + 1, 0),
                        locals: new Map(),
                    };

                    functions.push(fn);
                    currentFn = fn;
                    currentBlock = null;
                    pendingAttrs = [];
                    braceDepth = 1;
                    continue;
                }
            }
            continue;
        }

        if (trimmed === '}') {
            if (braceDepth > 0) {
                --braceDepth;
                if (braceDepth === 0)
                    closeFunction(i);
            }
            continue;
        }

        const blockMatch = /^\s*(%([A-Za-z0-9_$][A-Za-z0-9_$.]*))(\s*\(([^)]*)\))?:\s*$/.exec(raw);
        if (blockMatch) {
            if (currentBlock) {
                currentBlock.range = range(currentBlock.headerLine, 0, i, 0);
                closeBlock();
            }

            const name = blockMatch[2];
            const nameStart = blockMatch.index + blockMatch[1].indexOf('%');
            const paramText = blockMatch[4] ?? '';
            if (currentFn.blocks.some((b) => b.name === name))
                problem('warning', `duplicate block label '%${name}' in function '${currentFn.name}'`, range(i, nameStart, i, nameStart + name.length));

            const params: DirBlockParam[] = [];
            if (paramText.trim().length > 0) {
                for (const part of paramText.split(',')) {
                    const trimmedPart = trimWhitespace(part);
                    const pm = /^(.*?)\s*%([A-Za-z0-9_$][A-Za-z0-9_$.]*)$/.exec(trimmedPart);
                    if (pm) {
                        const pName = pm[2];
                        const pNameOffset = trimmedPart.lastIndexOf('%' + pName);

                        const absStart = nameStart + name.length + 1 + paramText.indexOf(trimmedPart) + pNameOffset;
                        params.push({
                            name: pName,
                            nameRange: range(i, absStart, i, absStart + pName.length),
                            type: trimWhitespace(pm[1]),
                        });

                        const existing = currentFn.locals.get(pName);
                        if (!existing)
                            currentFn.locals.set(pName, { kind: 'param', range: params[params.length - 1].nameRange });
                        else
                            problem('warning', `duplicate local '%${pName}' in function '${currentFn.name}'`, params[params.length - 1].nameRange);

                    }
                }
            }

            const block: DirBlock = {
                name,
                nameRange: range(i, nameStart, i, nameStart + name.length),
                params,
                headerLine: i,
                insts: [],
                range: range(i, 0, i + 1, 0),
            };

            currentFn.blocks.push(block);
            currentBlock = block;
            continue;
        }

        if (trimmed.endsWith('{')) {
            const inst = parseInstLine(raw, i);
            if (currentBlock)
                currentBlock.insts.push(inst);

            ++braceDepth;
            continue;
        }

        const inst = parseInstLine(raw, i);
        if (currentBlock)
            currentBlock.insts.push(inst);

        if (inst.resultName && currentFn) {
            const existing = currentFn.locals.get(inst.resultName);
            if (existing)
                problem('warning', `duplicate local '%${inst.resultName}' in function '${currentFn.name}'`, inst.resultRange!);
            else
                currentFn.locals.set(inst.resultName, { kind: 'local', range: inst.resultRange! });
        }
    }

    if (currentFn)
        closeFunction(lines.length - 1);

    for (const fn of functions) {
        const defNames = new Set<string>();
        for (const b of fn.blocks) {
            defNames.add(b.name);
            for (const p of b.params)
                defNames.add(p.name);
        }

        for (const name of fn.locals.keys())
            defNames.add(name);

        for (const block of fn.blocks)
            for (const inst of block.insts)
                for (const ref of inst.refs)
                    if (ref.kind === 'local' && !defNames.has(ref.name))
                        problem('info', `unresolved local '%${ref.name}' in function '${fn.name}'`, ref.range);
    }

    return { moduleName, globals, functions, problems };
}

export class DirDocument {
    readonly uri: string;
    readonly text: string;
    readonly moduleName?: string;
    readonly globals: DirGlobal[];
    readonly functions: DirFunction[];
    readonly problems: DirProblem[];

    private constructor(uri: string, text: string, data: DirDocumentData) {
        this.uri = uri;
        this.text = text;
        this.moduleName = data.moduleName;
        this.globals = data.globals;
        this.functions = data.functions;
        this.problems = data.problems;
    }

    static parse(text: string, uri: string): DirDocument {
        return new DirDocument(uri, text, parseDirText(text));
    }

    globalByName(name: string): DirGlobal | undefined {
        return this.globals.find((g) => g.name === name);
    }

    functionByName(name: string): DirFunction | undefined {
        return this.functions.find((f) => f.name === name);
    }

    functionAt(line: number): DirFunction | undefined {
        return this.functions.find((f) => line >= f.startLine && line <= f.range.end.line);
    }

    blockAt(fn: DirFunction, line: number): DirBlock | undefined {
        return fn.blocks.find((b) => line >= b.headerLine && line <= b.range.end.line);
    }
}
