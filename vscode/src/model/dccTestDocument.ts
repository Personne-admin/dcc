import { Position, Range, pos, range, splitHarnessLines, trimWhitespace } from './text';

export type SectionKind =
    | 'file'
    | 'entry'
    | 'mode'
    | 'inject'
    | 'expect-ast'
    | 'expect-scope'
    | 'expect-types'
    | 'expect-body'
    | 'expect-inst-body'
    | 'expect-ir'
    | 'expect-llvm'
    | 'expect-em64t-asm'
    | 'expect-em64t-object'
    | 'expect-executable'
    | 'expect-registry'
    | 'expect-warnings'
    | 'expect-error-count'
    | 'expect-errors'
    | 'unknown';

export type ProblemSeverity = 'error' | 'warning' | 'info';

export interface ModelProblem {
    severity: ProblemSeverity;
    message: string;
    range: Range;
}

export interface ErrorExpectation {
    file: string;
    line: number;
    column?: number;
    message: string;
    lineIndex: number;
    range: Range;
    valid: boolean;
}

export interface BodyKey {
    key: string;
    value: string;
    range: Range;
    valueRange: Range;
}

export interface Section {
    index: number;
    headerText: string;
    rawHeader: string;
    kind: SectionKind;
    headerLine: number;
    headerRange: Range;
    bodyStartLine: number;
    bodyRange: Range;
    fullRange: Range;
    bodyLines: string[];
    bodyText: string;

    target?: string;
    flags?: string;
    isError?: boolean;
    exact?: boolean;
    count?: number;
    interactive?: boolean;

    expectations: ErrorExpectation[];
    bodyKeys: BodyKey[];
    problems: ModelProblem[];
}

export interface VirtualFile {
    path: string;
    section: Section;
    content: string;
}

export interface DccTestDiagnostic {
    severity: ProblemSeverity;
    message: string;
    range: Range;
}

const SECTION_SOURCE = 'dcc-test';

const BODY_KEY_NAMES = new Set([
    'TARGET',
    'CONTAINS',
    'RUN-EXIT',
    'PIC',
    'SHARED-LINK',
    'REQUIRE-RELA',
    'REQUIRE-SECTION-ALIGN',
    'REQUIRE-COFF-RELOC',
    'REQUIRE-COFF-UNDEFINED',
    'FORBID-COFF-DEFINED',
]);

function isHeaderLine(stripped: string): boolean {
    return stripped.startsWith('=== ') && stripped.length >= 8 && stripped.endsWith(' ===');
}

interface Classified {
    kind: SectionKind;
    target?: string;
    isError?: boolean;
    exact?: boolean;
    flags?: string;
    count?: number;
    interactive?: boolean;
}

function extractFlags(headerText: string): string | undefined {
    const idx = headerText.indexOf('FLAGS:');
    if (idx < 0)
        return undefined;

    return trimWhitespace(headerText.slice(idx + 'FLAGS:'.length));
}

function classify(headerText: string): Classified {
    if (headerText.startsWith('FILE:'))
        return { kind: 'file', target: trimWhitespace(headerText.slice('FILE:'.length)) };

    if (headerText.startsWith('ENTRY:'))
        return { kind: 'entry', target: trimWhitespace(headerText.slice('ENTRY:'.length)) };

    if (headerText.startsWith('MODE:')) {
        const value = trimWhitespace(headerText.slice('MODE:'.length));
        return { kind: 'mode', interactive: value === 'interactive' };
    }
    if (headerText.startsWith('INJECT:'))
        return { kind: 'inject', target: trimWhitespace(headerText.slice('INJECT:'.length)) };

    if (headerText.startsWith('EXPECT-AST FOR:'))
        return { kind: 'expect-ast', target: trimWhitespace(headerText.slice('EXPECT-AST FOR:'.length)) };

    if (headerText.startsWith('EXPECT-AST'))
        return { kind: 'expect-ast' };

    if (headerText.startsWith('EXPECT-SCOPE FOR:'))
        return { kind: 'expect-scope', target: trimWhitespace(headerText.slice('EXPECT-SCOPE FOR:'.length)) };

    if (headerText.startsWith('EXPECT-TYPES FOR:'))
        return { kind: 'expect-types', target: trimWhitespace(headerText.slice('EXPECT-TYPES FOR:'.length)) };

    if (headerText.startsWith('EXPECT-BODY FOR:'))
        return { kind: 'expect-body', target: trimWhitespace(headerText.slice('EXPECT-BODY FOR:'.length)) };

    if (headerText.startsWith('EXPECT-BODY'))
        return { kind: 'expect-body' };

    if (headerText.startsWith('EXPECT-INSTANTIATED-BODY FOR:'))
        return {
            kind: 'expect-inst-body',
            target: trimWhitespace(headerText.slice('EXPECT-INSTANTIATED-BODY FOR:'.length)),
        };

    if (headerText.startsWith('EXPECT-IR'))
        return { kind: 'expect-ir', flags: extractFlags(headerText) };

    if (headerText.startsWith('EXPECT-LLVM-ERRORS'))
        return { kind: 'expect-llvm', isError: true, flags: extractFlags(headerText) };

    if (headerText.startsWith('EXPECT-LLVM'))
        return { kind: 'expect-llvm', isError: false, flags: extractFlags(headerText) };

    if (headerText.startsWith('EXPECT-EM64T-ASM'))
        return { kind: 'expect-em64t-asm', flags: extractFlags(headerText) };

    if (headerText.startsWith('EXPECT-EM64T-OBJECT'))
        return { kind: 'expect-em64t-object', flags: extractFlags(headerText) };

    if (headerText.startsWith('EXPECT-EXECUTABLE-ERRORS'))
        return { kind: 'expect-executable', isError: true, flags: extractFlags(headerText) };

    if (headerText.startsWith('EXPECT-EXECUTABLE'))
        return { kind: 'expect-executable', isError: false, flags: extractFlags(headerText) };

    if (headerText.startsWith('EXPECT-REGISTRY'))
        return { kind: 'expect-registry' };

    if (headerText.startsWith('EXPECT-WARNINGS'))
        return { kind: 'expect-warnings' };

    if (headerText.startsWith('EXPECT-ERROR-COUNT:')) {
        const raw = trimWhitespace(headerText.slice('EXPECT-ERROR-COUNT:'.length));
        const value = /^[0-9]+$/.test(raw) ? Number(raw) : undefined;
        return { kind: 'expect-error-count', count: value };
    }

    if (headerText.startsWith('EXPECT-ERRORS'))
        return { kind: 'expect-errors', exact: headerText === 'EXPECT-ERRORS EXACT' };

    return { kind: 'unknown' };
}

function parseExpectations(bodyLines: string[], bodyStartLine: number, source: string): ErrorExpectation[] {
    const out: ErrorExpectation[] = [];
    for (let i = 0; i < bodyLines.length; ++i) {
        const trimmed = trimWhitespace(bodyLines[i]);
        if (trimmed.length === 0)
            continue;

        const lineRange = range(bodyStartLine + i, 0, bodyStartLine + i, bodyLines[i].length);
        const firstColon = trimmed.indexOf(':');
        const secondColon = firstColon < 0 ? -1 : trimmed.indexOf(':', firstColon + 1);
        if (secondColon < 0) {
            out.push({
                file: '',
                line: 0,
                message: '',
                lineIndex: i,
                range: lineRange,
                valid: false,
            });
            continue;
        }

        const file = trimmed.slice(0, firstColon);
        const lineStr = trimmed.slice(firstColon + 1, secondColon);
        const line = /^[0-9]+$/.test(lineStr) ? Number(lineStr) : 0;
        let messageStart = secondColon + 1;
        let column: number | undefined;

        const thirdColon = trimmed.indexOf(':', messageStart);
        if (thirdColon >= 0) {
            const candidate = trimWhitespace(trimmed.slice(messageStart, thirdColon));
            if (/^[0-9]+$/.test(candidate)) {
                column = Number(candidate);
                messageStart = thirdColon + 1;
            }
        }

        out.push({
            file,
            line,
            column,
            message: trimWhitespace(trimmed.slice(messageStart)),
            lineIndex: i,
            range: lineRange,
            valid: line > 0,
        });
    }

    return out;
}

function parseBodyKeys(bodyLines: string[], bodyStartLine: number): BodyKey[] {
    const out: BodyKey[] = [];
    for (let i = 0; i < bodyLines.length; ++i) {
        const trimmed = trimWhitespace(bodyLines[i]);
        const colon = trimmed.indexOf(':');
        if (colon <= 0)
            continue;

        const key = trimmed.slice(0, colon);
        if (!BODY_KEY_NAMES.has(key))
            continue;

        const raw = bodyLines[i];
        const keyStart = raw.indexOf(key);
        out.push({
            key,
            value: trimWhitespace(trimmed.slice(colon + 1)),
            range: range(bodyStartLine + i, Math.max(keyStart, 0), bodyStartLine + i, Math.max(keyStart, 0) + key.length + 1),
            valueRange: range(bodyStartLine + i, Math.max(keyStart, 0) + key.length + 1, bodyStartLine + i, raw.length),
        });
    }

    return out;
}

export class DccTestDocument {
    readonly uri: string;
    readonly text: string;
    readonly sections: Section[];
    readonly virtualFiles: VirtualFile[];
    readonly entryPath: string;
    readonly diagnostics: DccTestDiagnostic[];

    private constructor(uri: string, text: string) {
        this.uri = uri;
        this.text = text;
        const parsed = DccTestDocument.parseText(text, uri);
        this.sections = parsed.sections;
        this.virtualFiles = parsed.virtualFiles;
        this.entryPath = parsed.entryPath;
        this.diagnostics = parsed.diagnostics;
    }

    static parse(text: string, uri: string): DccTestDocument {
        return new DccTestDocument(uri, text);
    }

    private static parseText(text: string, uri: string): {
        sections: Section[];
        virtualFiles: VirtualFile[];
        entryPath: string;
        diagnostics: DccTestDiagnostic[];
    } {
        const lines = splitHarnessLines(text);
        const sections: Section[] = [];
        const diagnostics: DccTestDiagnostic[] = [];

        let current: Section | null = null;
        let buffer: string[] = [];

        const finalize = (sec: Section, bodyLines: string[], nextLine: number): void => {
            sec.bodyLines = bodyLines;
            sec.bodyText = bodyLines.join('\n');
            sec.bodyRange = range(sec.bodyStartLine, 0, nextLine, 0);
            sec.fullRange = range(sec.headerLine, 0, nextLine, 0);
        };

        for (let i = 0; i < lines.length; ++i) {
            const stripped = lines[i];
            if (isHeaderLine(stripped)) {
                if (current)
                    finalize(current, buffer, i);

                const content = stripped.slice(4, stripped.length - 4);
                const headerText = trimWhitespace(content);
                const cls = classify(headerText);
                const sec: Section = {
                    index: sections.length,
                    headerText,
                    rawHeader: content,
                    kind: cls.kind,
                    headerLine: i,
                    headerRange: range(i, 0, i, stripped.length),
                    bodyStartLine: i + 1,
                    bodyRange: range(i + 1, 0, i + 1, 0),
                    fullRange: range(i, 0, i + 1, 0),
                    bodyLines: [],
                    bodyText: '',
                    target: cls.target,
                    flags: cls.flags,
                    isError: cls.isError,
                    exact: cls.exact,
                    count: cls.count,
                    interactive: cls.interactive,
                    expectations: [],
                    bodyKeys: [],
                    problems: [],
                };
                sections.push(sec);
                current = sec;
                buffer = [];
            } else if (current)
                buffer.push(stripped);
        }

        if (current)
            finalize(current, buffer, lines.length);

        const filePaths = new Set<string>();
        let entryPath = '';
        for (const sec of sections) {
            switch (sec.kind) {
                case 'file':
                    if (!sec.target) {
                        sec.problems.push({
                            severity: 'warning',
                            message: 'FILE section has no path',
                            range: sec.headerRange,
                        });
                    } else if (filePaths.has(sec.target)) {
                        sec.problems.push({
                            severity: 'error',
                            message: `duplicate FILE path '${sec.target}'`,
                            range: sec.headerRange,
                        });
                    } else {
                        filePaths.add(sec.target);
                    }
                    if (sec.bodyLines.length === 0) {
                        sec.problems.push({
                            severity: 'info',
                            message: 'FILE section has no body',
                            range: sec.headerRange,
                        });
                    }
                    break;
                case 'entry':
                    if (!sec.target) {
                        sec.problems.push({
                            severity: 'warning',
                            message: 'ENTRY section has no path',
                            range: sec.headerRange,
                        });
                    } else {
                        entryPath = sec.target;
                    }
                    break;
                case 'mode':
                    if (!sec.interactive) {
                        sec.problems.push({
                            severity: 'info',
                            message: `unsupported MODE value '${trimWhitespace(sec.rawHeader.slice('MODE:'.length))}' (only 'interactive' is recognized)`,
                            range: sec.headerRange,
                        });
                    }
                    break;
                case 'expect-ast':
                    if (!sec.target) {
                        sec.problems.push({
                            severity: 'info',
                            message: 'EXPECT-AST without FOR: defaults to the entry file',
                            range: sec.headerRange,
                        });
                    }
                    break;
                case 'expect-scope':
                case 'expect-types':
                case 'expect-body':
                case 'expect-inst-body':
                    if (sec.headerText.includes('FOR:') && !sec.target) {
                        sec.problems.push({
                            severity: 'warning',
                            message: `${sec.headerText.split(' ')[0]} FOR: has no target module`,
                            range: sec.headerRange,
                        });
                    }
                    break;
                case 'expect-error-count':
                    if (sec.count === undefined) {
                        sec.problems.push({
                            severity: 'error',
                            message: 'malformed EXPECT-ERROR-COUNT (expected a non-negative integer)',
                            range: sec.headerRange,
                        });
                    }
                    break;
                case 'expect-errors':
                case 'expect-warnings': {
                    sec.expectations = parseExpectations(sec.bodyLines, sec.bodyStartLine, uri);
                    for (const exp of sec.expectations) {
                        if (!exp.valid) {
                            sec.problems.push({
                                severity: 'warning',
                                message: `malformed expected-${sec.kind === 'expect-errors' ? 'error' : 'warning'} location (expected file:line[:column]: message)`,
                                range: exp.range,
                            });
                        }
                    }
                    break;
                }
                case 'expect-em64t-asm':
                case 'expect-em64t-object':
                    sec.bodyKeys = parseBodyKeys(sec.bodyLines, sec.bodyStartLine);
                    break;
                default:
                    break;
            }
        }

        if (!entryPath && sections.some((s) => s.kind === 'file'))
            entryPath = sections.find((s) => s.kind === 'file')!.target ?? '';

        const virtualFiles: VirtualFile[] = [];
        for (const sec of sections) {
            if (sec.kind !== 'file' || !sec.target)
                continue;

            let content = sec.bodyText;
            if (content.length > 0 && !content.endsWith('\n'))
                content += '\n';

            virtualFiles.push({ path: sec.target, section: sec, content });
        }

        for (const sec of sections) {
            if (sec.kind === 'entry' && sec.target && virtualFiles.length > 0 && !filePaths.has(sec.target)) {
                sec.problems.push({
                    severity: 'error',
                    message: `ENTRY: '${sec.target}' does not match any FILE section`,
                    range: sec.headerRange,
                });
            }
            if (sec.kind === 'expect-ast' && sec.target && !filePaths.has(sec.target)) {
                sec.problems.push({
                    severity: 'warning',
                    message: `EXPECT-AST FOR: '${sec.target}' does not match any FILE section`,
                    range: sec.headerRange,
                });
            }
            if (sec.kind === 'expect-errors' || sec.kind === 'expect-warnings') {
                for (const exp of sec.expectations) {
                    if (!exp.valid)
                        continue;

                    if (exp.file.startsWith('<') && exp.file.endsWith('>'))
                        continue;

                    if (!filePaths.has(exp.file)) {
                        sec.problems.push({
                            severity: 'warning',
                            message: `expected-${sec.kind === 'expect-errors' ? 'error' : 'warning'} file '${exp.file}' does not match any FILE section`,
                            range: exp.range,
                        });
                        continue;
                    }
                    const vf = virtualFiles.find((v) => v.path === exp.file)!;
                    const virtualLineCount = vf.content.split('\n').length;
                    if (exp.line < 1 || exp.line > virtualLineCount) {
                        sec.problems.push({
                            severity: 'warning',
                            message: `expected ${sec.kind === 'expect-errors' ? 'error' : 'warning'} line ${exp.line} is outside '${exp.file}' (1..${virtualLineCount})`,
                            range: exp.range,
                        });
                    }
                }
            }
        }

        for (const sec of sections) {
            if (sec.kind === 'unknown')
                sec.problems.push({
                    severity: 'warning',
                    message: `unknown section header '${sec.headerText}' (section preserved, not interpreted)`,
                    range: sec.headerRange,
                });

            for (const p of sec.problems)
                diagnostics.push({ severity: p.severity, message: p.message, range: p.range })
        }

        return { sections, virtualFiles, entryPath, diagnostics };
    }

    diagnosticLineToContainer(vfile: VirtualFile, expectedLine1Based: number): number | null {
        const bodyLineCount = vfile.section.bodyLines.length;
        const containerLine = vfile.section.bodyStartLine + (expectedLine1Based - 1);
        if (expectedLine1Based < 1 || expectedLine1Based > bodyLineCount)
            return null;

        return containerLine;
    }

    virtualLineToContainerLine(vfile: VirtualFile, virtualLine0Based: number): number | null {
        const bodyLineCount = vfile.section.bodyLines.length;
        if (virtualLine0Based < 0 || virtualLine0Based >= bodyLineCount)
            return null;

        return vfile.section.bodyStartLine + virtualLine0Based;
    }

    containerLineToVirtualLine(section: Section, containerLine0Based: number): number | null {
        const rel = containerLine0Based - section.bodyStartLine;
        if (rel < 0 || rel >= section.bodyLines.length)
            return null;

        return rel;
    }

    fileSectionAt(containerLine: number): VirtualFile | undefined {
        return this.virtualFiles.find((v) => {
            const s = v.section;
            return containerLine >= s.bodyStartLine && containerLine < s.bodyStartLine + s.bodyLines.length;
        });
    }

    fileSectionForPath(path: string): VirtualFile | undefined {
        return this.virtualFiles.find((v) => v.path === path);
    }
}
