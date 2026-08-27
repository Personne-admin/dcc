export interface Position {
    readonly line: number;
    readonly character: number;
}

export interface Range {
    readonly start: Position;
    readonly end: Position;
}

export function pos(line: number, character: number): Position {
    return { line, character };
}

export function range(startLine: number, startChar: number, endLine: number, endChar: number): Range {
    return { start: pos(startLine, startChar), end: pos(endLine, endChar) };
}

export function containsPosition(r: Range, p: Position): boolean {
    if (p.line < r.start.line || p.line > r.end.line)
        return false;

    if (p.line === r.start.line && p.character < r.start.character)
        return false;

    if (p.line === r.end.line && p.character > r.end.character)
        return false;

    return true;
}

export function clampLine(lines: readonly string[], line: number): number {
    if (lines.length === 0)
        return 0;

    if (line < 0)
        return 0;

    if (line >= lines.length)
        return lines.length - 1;

    return line;
}

export function clampChar(lines: readonly string[], line: number, character: number): number {
    const text = lines[line];
    if (text === undefined)
        return 0;

    if (character < 0)
        return 0;

    if (character > text.length)
        return text.length;

    return character;
}

export function splitHarnessLines(text: string): string[] {
    const out: string[] = [];
    const parts = text.split('\n');
    for (const part of parts)
        out.push(part.endsWith('\r') ? part.slice(0, -1) : part);

    return out;
}

export function trimWhitespace(s: string): string {
    let a = 0;
    while (a < s.length && (s[a] === ' ' || s[a] === '\t'))
        ++a;

    let b = s.length;
    while (b > a && (s[b - 1] === ' ' || s[b - 1] === '\t'))
        --b;

    return s.slice(a, b);
}
