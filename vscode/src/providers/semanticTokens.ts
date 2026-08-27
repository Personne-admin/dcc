import * as vscode from 'vscode';

export const SEMANTIC_TOKEN_TYPES = [
    'section',
    'path',
    'flag',
    'key',
    'location',
    'ssa',
    'block',
    'global',
    'instruction',
    'terminator',
    'type',
    'constant',
    'attribute',
    'keyword',
    'identifier',
    'diagnostic',
] as const;

export const SEMANTIC_TOKEN_MODIFIERS = ['declaration', 'readonly'] as const;

export const DCC_SEMANTIC_LEGEND = new vscode.SemanticTokensLegend(
    [...SEMANTIC_TOKEN_TYPES],
    [...SEMANTIC_TOKEN_MODIFIERS],
);

export type SemanticTokenType = (typeof SEMANTIC_TOKEN_TYPES)[number];

export function tokenTypeIndex(name: SemanticTokenType): number {
    return SEMANTIC_TOKEN_TYPES.indexOf(name);
}

export function tokenModifierIndex(name: (typeof SEMANTIC_TOKEN_MODIFIERS)[number]): number {
    return SEMANTIC_TOKEN_MODIFIERS.indexOf(name);
}

export class TokenBuilder {
    private readonly m_tokens: { line: number; char: number; length: number; type: number; mods: number }[] = [];

    add(line: number, char: number, length: number, type: SemanticTokenType, declaration = false): void {
        if (length <= 0)
            return;

        this.m_tokens.push({
            line,
            char,
            length,
            type: tokenTypeIndex(type),
            mods: declaration ? tokenModifierIndex('declaration') : 0,
        });
    }

    addIndexed(line: number, char: number, length: number, typeIndex: number, modMask: number): void {
        if (length <= 0)
            return;

        this.m_tokens.push({ line, char, length, type: typeIndex, mods: modMask });
    }

    build(): vscode.SemanticTokens {
        return encodeTokens(this.m_tokens);
    }

    tokens(): { line: number; char: number; length: number; type: number; mods: number }[] {
        return [...this.m_tokens].sort((a, b) => a.line - b.line || a.char - b.char);
    }

    static fromEncoded(tokens: vscode.SemanticTokens): TokenBuilder {
        const b = new TokenBuilder();
        const data = tokens.data;

        let line = 0;
        let char = 0;
        for (let i = 0; i + 4 < data.length; i += 5) {
            line += data[i];
            if (data[i] === 0)
                char += data[i + 1];
            else
                char = data[i + 1];

            b.m_tokens.push({
                line,
                char,
                length: data[i + 2],
                type: data[i + 3],
                mods: data[i + 4],
            });
        }

        return b;
    }
}

function encodeTokens(tokens: { line: number; char: number; length: number; type: number; mods: number }[]): vscode.SemanticTokens {
    const sorted = [...tokens].sort((a, b) => a.line - b.line || a.char - b.char);
    const data: number[] = [];

    let prevLine = 0;
    let prevChar = 0;

    for (const t of sorted) {
        data.push(t.line - prevLine);
        if (t.line === prevLine)
            data.push(t.char - prevChar);
        else
            data.push(t.char);

        data.push(t.length);
        data.push(t.type);
        data.push(t.mods);
        prevLine = t.line;
        prevChar = t.char;
    }

    return new vscode.SemanticTokens(new Uint32Array(data));
}

export function mergeSemanticTokens(sources: (TokenBuilder | vscode.SemanticTokens)[]): vscode.SemanticTokens {
    const all: { line: number; char: number; length: number; type: number; mods: number }[] = [];
    for (const src of sources)
        if (src instanceof TokenBuilder)
            all.push(...src.tokens());
        else
            all.push(...TokenBuilder.fromEncoded(src).tokens());

    return encodeTokens(all);
}
