import * as vscode from 'vscode';
import { DccTestDocument, Section, SectionKind, VirtualFile } from '../model/dccTestDocument';
import { Range } from '../model/text';
import { TokenBuilder } from './semanticTokens';

const SECTION_DESCRIPTIONS: Record<SectionKind, string> = {
    file: 'Virtual source file.',
    entry: 'Entry file for the fixture; defaults to the first FILE section.',
    mode: 'Parse mode for the fixture.',
    inject: 'Declaration injected into the compilation before analysis.',
    'expect-ast': 'Expected AST dump for a target file (defaults to the entry file).',
    'expect-scope': 'Expected scope dump for a module.',
    'expect-types': 'Expected type dump for a module.',
    'expect-body': 'Expected lowered body dump for a module.',
    'expect-inst-body': 'Expected instantiated body dump for a module.',
    'expect-ir': 'Expected IR (DIR) dump.',
    'expect-llvm': 'Expected LLVM IR text emitted by the LLVM backend.',
    'expect-em64t-asm': 'Expected em64t assembly output (CONTAINS:/TARGET: keys).',
    'expect-em64t-object': 'Expected em64t object bytes (RUN-EXIT:/PIC:/REQUIRE-* keys).',
    'expect-executable': 'Expected executable bytes emitted by the LLVM backend.',
    'expect-registry': 'Expected specification registry dump.',
    'expect-warnings': 'Expected compiler warnings (file:line[:col]: message).',
    'expect-error-count': 'Expected number of emitted errors.',
    'expect-errors': 'Expected compiler errors (file:line[:col]: message).',
    unknown: 'Unknown section header.',
};

const KIND_SYMBOL: Record<SectionKind, vscode.SymbolKind> = {
    file: vscode.SymbolKind.File,
    entry: vscode.SymbolKind.Constant,
    mode: vscode.SymbolKind.Constant,
    inject: vscode.SymbolKind.Variable,
    'expect-ast': vscode.SymbolKind.Object,
    'expect-scope': vscode.SymbolKind.Object,
    'expect-types': vscode.SymbolKind.Object,
    'expect-body': vscode.SymbolKind.Object,
    'expect-inst-body': vscode.SymbolKind.Object,
    'expect-ir': vscode.SymbolKind.Object,
    'expect-llvm': vscode.SymbolKind.Object,
    'expect-em64t-asm': vscode.SymbolKind.Object,
    'expect-em64t-object': vscode.SymbolKind.Object,
    'expect-executable': vscode.SymbolKind.Object,
    'expect-registry': vscode.SymbolKind.Object,
    'expect-warnings': vscode.SymbolKind.Object,
    'expect-error-count': vscode.SymbolKind.Constant,
    'expect-errors': vscode.SymbolKind.Object,
    unknown: vscode.SymbolKind.Object,
};

function toVscodeRange(r: Range): vscode.Range {
    return new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
}

export function sectionSymbol(sec: Section): vscode.DocumentSymbol {
    const name = sec.headerText.length > 0 ? sec.headerText : `(empty header)`;

    const sym = new vscode.DocumentSymbol(
        name,
        sectionDetail(sec),
        KIND_SYMBOL[sec.kind],
        toVscodeRange(sec.fullRange),
        toVscodeRange(sec.headerRange),
    );

    sym.children = [];
    return sym;
}

export function dccTestSectionSymbols(model: DccTestDocument): vscode.DocumentSymbol[] {
    return model.sections.map((sec) => sectionSymbol(sec));
}

function sectionDetail(sec: Section): string {
    const base = SECTION_DESCRIPTIONS[sec.kind] ?? '';
    const extras: string[] = [];
    if (sec.kind === 'file' && sec.target)
        extras.push(`path: ${sec.target}`);

    if (sec.kind === 'file' || sec.kind === 'expect-errors' || sec.kind === 'expect-warnings')
        extras.push(`${sec.bodyLines.length} body line(s)`);

    if (sec.flags)
        extras.push(`flags: ${sec.flags}`);

    if (sec.exact)
        extras.push('exact match');

    if (sec.count !== undefined)
        extras.push(`count: ${sec.count}`);

    if (sec.interactive)
        extras.push('interactive');

    if (sec.isError)
        extras.push('expects backend error');

    return [base, ...extras].join(' ').trim();
}

export function dccTestFoldingRanges(model: DccTestDocument): vscode.FoldingRange[] {
    const out: vscode.FoldingRange[] = [];
    for (const sec of model.sections) {
        if (sec.bodyLines.length === 0)
            continue;

        const end = sec.bodyStartLine + sec.bodyLines.length - 1;
        if (end > sec.headerLine)
            out.push(new vscode.FoldingRange(sec.headerLine, end));

    }

    return out;
}

const HEADER_COMPLETIONS: { label: string; insert: string; detail: string }[] = [
    { label: 'FILE', insert: 'FILE: ${1:main.dc}', detail: 'virtual source file' },
    { label: 'ENTRY', insert: 'ENTRY: ${1:main.dc}', detail: 'entry file (defaults to first FILE)' },
    { label: 'MODE', insert: 'MODE: interactive', detail: 'parse mode' },
    { label: 'INJECT', insert: 'INJECT: ${1:const bool X = true;}', detail: 'injected declaration' },
    { label: 'EXPECT-AST', insert: 'EXPECT-AST${1| FOR: ,|}', detail: 'expected AST dump' },
    { label: 'EXPECT-SCOPE', insert: 'EXPECT-SCOPE FOR: ${1:module}', detail: 'expected scope dump' },
    { label: 'EXPECT-TYPES', insert: 'EXPECT-TYPES FOR: ${1:module}', detail: 'expected type dump' },
    { label: 'EXPECT-BODY', insert: 'EXPECT-BODY${1| FOR: ,|}', detail: 'expected lowered body dump' },
    { label: 'EXPECT-INSTANTIATED-BODY', insert: 'EXPECT-INSTANTIATED-BODY FOR: ${1:module}', detail: 'expected instantiated body dump' },
    { label: 'EXPECT-IR', insert: 'EXPECT-IR${1| , FLAGS: -fbounds-check|}', detail: 'expected IR (DIR) dump' },
    { label: 'EXPECT-LLVM', insert: 'EXPECT-LLVM${1| , FLAGS: -verify|}', detail: 'expected LLVM IR text' },
    { label: 'EXPECT-LLVM-ERRORS', insert: 'EXPECT-LLVM-ERRORS${1| , FLAGS: -verify|}', detail: 'expected LLVM backend error' },
    { label: 'EXPECT-EM64T-ASM', insert: 'EXPECT-EM64T-ASM', detail: 'expected em64t assembly' },
    { label: 'EXPECT-EM64T-OBJECT', insert: 'EXPECT-EM64T-OBJECT${1| , FLAGS: -no-section-check|}', detail: 'expected em64t object bytes' },
    { label: 'EXPECT-EXECUTABLE', insert: 'EXPECT-EXECUTABLE${1| , FLAGS: -verify|}', detail: 'expected executable bytes' },
    { label: 'EXPECT-EXECUTABLE-ERRORS', insert: 'EXPECT-EXECUTABLE-ERRORS', detail: 'expected executable emission error' },
    { label: 'EXPECT-REGISTRY', insert: 'EXPECT-REGISTRY', detail: 'expected spec registry dump' },
    { label: 'EXPECT-WARNINGS', insert: 'EXPECT-WARNINGS', detail: 'expected compiler warnings' },
    { label: 'EXPECT-ERROR-COUNT', insert: 'EXPECT-ERROR-COUNT: ${1:0}', detail: 'expected number of errors' },
    { label: 'EXPECT-ERRORS', insert: 'EXPECT-ERRORS${1| , EXACT|}', detail: 'expected compiler errors' },
];

const FLAG_COMPLETIONS: string[] = [
    '-verify',
    '-fbounds-check',
    '-gdwarf',
    '-gpdb',
    '-g',
    '-g0',
    '-gnone',
    '-fno-red-zone',
    '-fno-simd',
    '-fno-x87',
    '-fPIC',
    '-fPIE',
    '-fno-omit-frame-pointer',
    '-fomit-frame-pointer',
    '-O1',
    '-target x86_64-unknown-linux-gnu',
    '-mcmodel=small',
    '-no-section-check',
];

const BODY_KEY_COMPLETIONS: string[] = [
    'TARGET:',
    'CONTAINS:',
    'RUN-EXIT:',
    'PIC:',
    'SHARED-LINK:',
    'REQUIRE-RELA:',
    'REQUIRE-SECTION-ALIGN:',
    'REQUIRE-COFF-RELOC:',
    'REQUIRE-COFF-UNDEFINED:',
    'FORBID-COFF-DEFINED:',
];

export function dccTestCompletion(model: DccTestDocument, document: vscode.TextDocument, position: vscode.Position): vscode.CompletionItem[] {
    const line = document.lineAt(position.line);
    const lineText = line.text;
    const trimmedStart = lineText.trimStart();

    if (trimmedStart.startsWith('===')) {
        const items: vscode.CompletionItem[] = [];
        const headerPrefix = lineText.slice(lineText.indexOf('===') + 4, position.character).trimStart();
        for (const h of HEADER_COMPLETIONS) {
            const item = new vscode.CompletionItem(h.label, vscode.CompletionItemKind.Snippet);
            item.detail = h.detail;
            item.insertText = new vscode.SnippetString(`=== ${h.insert} ===`);
            item.range = line.range;
            item.filterText = `=== ${h.label}`;
            items.push(item);
        }

        if (/FLAGS:/.test(headerPrefix)) {
            for (const flag of FLAG_COMPLETIONS) {
                const item = new vscode.CompletionItem(flag, vscode.CompletionItemKind.Value);
                item.detail = 'flag';
                item.insertText = new vscode.SnippetString(flag);
                items.push(item);
            }
        }
        return items;
    }

    const modelSection = model.sections.find((s) => position.line >= s.bodyStartLine && position.line < s.bodyStartLine + s.bodyLines.length);
    if (modelSection && (modelSection.kind === 'expect-em64t-asm' || modelSection.kind === 'expect-em64t-object')) {
        const indent = lineText.length - lineText.trimStart().length;
        return BODY_KEY_COMPLETIONS.map((key) => {
            const item = new vscode.CompletionItem(key, vscode.CompletionItemKind.Keyword);
            item.insertText = new vscode.SnippetString(`${' '.repeat(indent)}${key} `);
            return item;
        });
    }

    return [];
}

export function dccTestHover(model: DccTestDocument, document: vscode.TextDocument, position: vscode.Position): vscode.Hover | null {
    const sec = model.sections.find((s) => position.line === s.headerLine);
    if (sec) {
        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**${sec.headerText}**\n\n${SECTION_DESCRIPTIONS[sec.kind] ?? ''}`);

        const extras: string[] = [];

        if (sec.kind === 'file' && sec.target) {
            const vf = model.fileSectionForPath(sec.target);
            extras.push(`Path: \`${sec.target}\``);
            if (vf)
                extras.push(`Body: ${sec.bodyLines.length} line(s), ${vf.content.length} byte(s) virtual content`);
        }

        if (sec.flags)
            extras.push(`Flags: \`${sec.flags}\``);

        if (sec.exact)
            extras.push('Mode: exact error set');

        if (sec.count !== undefined)
            extras.push(`Expected error count: ${sec.count}`);

        if (sec.interactive)
            extras.push('Parse mode: interactive');

        if (sec.isError)
            extras.push('Expects a backend error.');

        if (sec.expectations.length > 0)
            extras.push(`${sec.expectations.length} expected ${sec.kind === 'expect-errors' ? 'error' : 'warning'}(s)`);

        if (extras.length > 0)
            md.appendMarkdown(`\n\n${extras.join('\n')}`);

        return new vscode.Hover(md, toVscodeRange(sec.headerRange));
    }

    const errSection = model.sections.find(
        (s) =>
            (s.kind === 'expect-errors' || s.kind === 'expect-warnings') &&
            position.line >= s.bodyStartLine &&
            position.line < s.bodyStartLine + s.bodyLines.length,
    );

    if (errSection) {
        const exp = errSection.expectations.find((e) => e.range.start.line === position.line);
        if (exp) {
            const what = errSection.kind === 'expect-errors' ? 'error' : 'warning';
            if (exp.valid) {
                const vf = model.fileSectionForPath(exp.file);
                const target = vf ? ` (maps to \`${exp.file}\` line ${exp.line})` : ' (no matching FILE section)';
                const md = new vscode.MarkdownString();
                md.appendMarkdown(`**expected ${what}** ${exp.column !== undefined ? `at ${exp.file}:${exp.line}:${exp.column}` : `at ${exp.file}:${exp.line}`}${target}`);
                md.appendCodeblock(exp.message, 'text');
                return new vscode.Hover(md, toVscodeRange(exp.range));
            }

            return new vscode.Hover('Malformed expected-' + what + ' location (expected `file:line[:column]: message`)', toVscodeRange(exp.range));
        }
    }

    return null;
}

function pathOccurrenceInHeader(sec: Section, path: string): vscode.Range | null {
    if (!path)
        return null;

    const rawHeader = sec.rawHeader;
    let idx = rawHeader.indexOf(path);
    while (idx >= 0) {
        const before = idx > 0 ? rawHeader[idx - 1] : '';
        const after = idx + path.length < rawHeader.length ? rawHeader[idx + path.length] : '';
        if (!/[\w.\-/]/.test(before) && !/[\w.\-/]/.test(after))
            return new vscode.Range(sec.headerRange.start.line, 4 + idx, sec.headerRange.start.line, 4 + idx + path.length);

        idx = rawHeader.indexOf(path, idx + 1);
    }

    return null;
}

function pathOccurrencesInExpectations(sec: Section, path: string): vscode.Range[] {
    const out: vscode.Range[] = [];
    if (sec.kind !== 'expect-errors' && sec.kind !== 'expect-warnings')
        return out;

    for (const exp of sec.expectations)
        if (exp.valid && exp.file === path)
            out.push(new vscode.Range(exp.range.start.line, exp.range.start.character, exp.range.start.line, exp.range.start.character + path.length));

    return out;
}

function pathOccurrences(model: DccTestDocument, path: string): vscode.Range[] {
    const out: vscode.Range[] = [];
    for (const sec of model.sections) {
        const inHeader = pathOccurrenceInHeader(sec, path);
        if (inHeader)
            out.push(inHeader);

        out.push(...pathOccurrencesInExpectations(sec, path));
    }

    return out;
}

export function dccTestDefinition(model: DccTestDocument, document: vscode.TextDocument, position: vscode.Position): vscode.Location[] {
    const sec = model.sections.find((s) => position.line === s.headerLine);
    if (sec) {
        if (sec.target && (sec.kind === 'file' || sec.kind === 'entry' || sec.kind === 'expect-ast')) {
            const vf = model.fileSectionForPath(sec.target);
            if (vf) {
                const occ = pathOccurrenceInHeader(sec, sec.target);
                if (occ)
                    return [new vscode.Location(document.uri, occ)];
            }
        }

        for (const path of model.virtualFiles.map((v) => v.path)) {
            if (sec.target === path) {
                const vf = model.fileSectionForPath(path);
                if (vf) {
                    const occ = pathOccurrenceInHeader(sec, path);
                    if (occ)
                        return [new vscode.Location(document.uri, occ)];
                }
            }
        }

        return [];
    }

    const errSection = model.sections.find(
        (s) =>
            (s.kind === 'expect-errors' || s.kind === 'expect-warnings') &&
            position.line >= s.bodyStartLine &&
            position.line < s.bodyStartLine + s.bodyLines.length,
    );

    if (errSection) {
        const exp = errSection.expectations.find((e) => e.range.start.line === position.line);
        if (exp && exp.valid) {
            const vf = model.fileSectionForPath(exp.file);
            if (vf) {
                const containerLine = model.diagnosticLineToContainer(vf, exp.line) ?? vf.section.bodyStartLine + vf.section.bodyLines.length - 1;
                const bodyLine = vf.section.bodyLines[containerLine - vf.section.bodyStartLine] ?? '';
                return [
                    new vscode.Location(
                        document.uri,
                        new vscode.Range(
                            containerLine,
                            0,
                            containerLine,
                            Math.min(exp.column !== undefined ? exp.column - 1 : 0, bodyLine.length),
                        ),
                    ),
                ];
            }
        }

        return [];
    }

    return [];
}

export function dccTestReferences(model: DccTestDocument, document: vscode.TextDocument, position: vscode.Position): vscode.Location[] {
    const sec = model.sections.find((s) => position.line === s.headerLine);
    if (sec) {
        const candidates = [sec.target, ...model.virtualFiles.map((v) => v.path)];
        for (const path of candidates) {
            if (!path)
                continue;

            const occ = pathOccurrenceInHeader(sec, path);
            if (occ && occ.contains(position))
                return pathOccurrences(model, path).map((r) => new vscode.Location(document.uri, r));
        }
        return [];
    }

    const errSection = model.sections.find(
        (s) =>
            (s.kind === 'expect-errors' || s.kind === 'expect-warnings') &&
            position.line >= s.bodyStartLine &&
            position.line < s.bodyStartLine + s.bodyLines.length,
    );

    if (errSection) {
        const exp = errSection.expectations.find((e) => e.range.start.line === position.line);
        if (exp && exp.valid)
            return pathOccurrences(model, exp.file).map((r) => new vscode.Location(document.uri, r));

        return [];
    }
    return [];
}

export function dccTestDiagnostics(model: DccTestDocument): vscode.Diagnostic[] {
    const out: vscode.Diagnostic[] = [];
    for (const d of model.diagnostics) {
        const severity =
            d.severity === 'error'
                ? vscode.DiagnosticSeverity.Error
                : d.severity === 'warning'
                    ? vscode.DiagnosticSeverity.Warning
                    : vscode.DiagnosticSeverity.Information;

        const diag = new vscode.Diagnostic(toVscodeRange(d.range), d.message, severity);
        diag.source = 'dcc-test';
        out.push(diag);
    }

    return out;
}

const HEADER_KEYWORD_RE = /\b(?:FILE|ENTRY|MODE|INJECT|FOR|FLAGS|EXACT|TARGET|CONTAINS|RUN-EXIT|PIC|SHARED-LINK|REQUIRE-RELA|REQUIRE-SECTION-ALIGN|REQUIRE-COFF-RELOC|REQUIRE-COFF-UNDEFINED|FORBID-COFF-DEFINED)\b/g;
const LOCATION_RE = /\b[A-Za-z0-9_./-]+\.(?:dc|dcc|dcc-test)(?::\d+){1,2}\b/g;
const DIAGNOSTIC_MARK_RE = /^\s*(?:error|warning|note|help|fix):/;
const IDENTIFIER_RE = /\b[a-zA-Z_][a-zA-Z0-9_]*\b/g;
const NUMBER_RE = /\b[0-9][0-9a-fA-FxX_]*\b/g;

const DUMP_IDENTIFIER_SECTIONS: SectionKind[] = [
    'expect-ast',
    'expect-scope',
    'expect-types',
    'expect-body',
    'expect-inst-body',
    'expect-registry',
    'expect-llvm',
    'expect-em64t-asm',
    'expect-executable',
];

const SECTION_NAME_RE = /^([A-Z][A-Z0-9-]*)/;

export function dccTestSemanticTokens(model: DccTestDocument): TokenBuilder {
    const b = new TokenBuilder();

    for (const sec of model.sections) {
        const headerLine = sec.headerLine;
        const headerText = sec.headerText;
        const headerOffset = 4;
        b.add(headerLine, 0, 4, 'keyword');
        b.add(headerLine, headerOffset + headerText.length, 4, 'keyword');

        const nameMatch = SECTION_NAME_RE.exec(headerText);
        if (nameMatch)
            b.add(headerLine, headerOffset, nameMatch[1].length, 'section');

        HEADER_KEYWORD_RE.lastIndex = 0;
        let m: RegExpExecArray | null;
        while ((m = HEADER_KEYWORD_RE.exec(headerText)) !== null) {
            const isPath = m[1] === 'FILE' || m[1] === 'ENTRY' || m[1] === 'FOR';
            b.add(headerLine, headerOffset + m.index, m[0].length, isPath ? 'path' : 'keyword');
        }

        if (sec.target && (sec.kind === 'file' || sec.kind === 'entry' || sec.kind === 'expect-ast')) {
            const idx = headerText.indexOf(sec.target);
            if (idx >= 0)
                b.add(headerLine, headerOffset + idx, sec.target.length, 'path');
        }

        if (sec.flags) {
            const idx = headerText.indexOf(sec.flags);
            if (idx >= 0)
                b.add(headerLine, headerOffset + idx, sec.flags.length, 'flag');
        }

        if (sec.kind === 'expect-error-count' && sec.count !== undefined) {
            const idx = headerText.lastIndexOf(String(sec.count));
            if (idx >= 0)
                b.add(headerLine, headerOffset + idx, String(sec.count).length, 'constant');
        }

        if (sec.kind === 'file' || sec.kind === 'expect-ir')
            continue;

        for (let li = 0; li < sec.bodyLines.length; ++li) {
            const lineNo = sec.bodyStartLine + li;
            const text = sec.bodyLines[li];

            for (const key of sec.bodyKeys)
                if (key.range.start.line === lineNo)
                    b.add(lineNo, key.range.start.character, key.key.length + 1, 'key');

            LOCATION_RE.lastIndex = 0;
            while ((m = LOCATION_RE.exec(text)) !== null)
                b.add(lineNo, m.index, m[0].length, 'location');

            if (DIAGNOSTIC_MARK_RE.test(text)) {
                const colon = text.indexOf(':');
                b.add(lineNo, text.length - text.trimStart().length, colon - (text.length - text.trimStart().length), 'diagnostic');
            }

            if (DUMP_IDENTIFIER_SECTIONS.includes(sec.kind)) {
                IDENTIFIER_RE.lastIndex = 0;
                while ((m = IDENTIFIER_RE.exec(text)) !== null)
                    b.add(lineNo, m.index, m[0].length, 'identifier');
            } else if (sec.kind === 'expect-em64t-object') {
                NUMBER_RE.lastIndex = 0;
                while ((m = NUMBER_RE.exec(text)) !== null)
                    b.add(lineNo, m.index, m[0].length, 'constant');
            }
        }
    }

    return b;
}

export function fileSectionAt(model: DccTestDocument, position: vscode.Position): VirtualFile | undefined {
    return model.fileSectionAt(position.line);
}
