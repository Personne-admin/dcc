import { DccTestDocument, VirtualFile } from './dccTestDocument';
import { LspLocation, LspLocationLink, LspRange, LspTextEdit, LspWorkspaceEdit } from './lspTypes';
import { Range, pos } from './text';
import { isVirtualUri, mapVirtualRangeToContainer } from './virtualFs';

export interface VirtualOwner {
    containerUri: string;
    path: string;
    vf: VirtualFile;
}

export type OwnerResolver = (virtualUri: string) => VirtualOwner | null;

export function orderedVirtualFilesForSync(model: DccTestDocument): VirtualFile[] {
    const files = model.virtualFiles;
    if (files.length <= 1)
        return [...files];

    const entry = files.find((v) => v.path === model.entryPath);
    if (!entry)
        return [...files];

    const rest = files.filter((v) => v !== entry);
    return [...rest, entry];
}

export interface MappedLocation {
    uri: string;
    range: Range;
    rehomed: boolean;
}

function mapRangeToOwner(owner: VirtualOwner, range: LspRange): Range | null {
    return mapVirtualRangeToContainer(owner.vf, range);
}

export function mapLspLocation(loc: LspLocation, resolveOwner: OwnerResolver): MappedLocation | null {
    if (!loc || typeof loc.range !== 'object' || typeof loc.uri !== 'string')
        return null;

    if (isVirtualUri(loc.uri)) {
        const owner = resolveOwner(loc.uri);
        if (!owner)
            return null;

        const range = mapRangeToOwner(owner, loc.range);
        if (!range)
            return null;

        return { uri: owner.containerUri, range, rehomed: true };
    }

    if (loc.uri.startsWith('file:'))
        return { uri: loc.uri, range: toModelRange(loc.range), rehomed: false };

    return null;
}

export function mapLspLocationLink(link: LspLocationLink, resolveOwner: OwnerResolver): MappedLocation | null {
    if (!link || typeof link.targetUri !== 'string')
        return null;

    const range = link.targetSelectionRange ?? link.targetRange;
    return mapLspLocation({ uri: link.targetUri, range }, resolveOwner);
}

export function mapWorkspaceEdit(edit: LspWorkspaceEdit, resolveOwner: OwnerResolver): LspWorkspaceEdit | null {
    if (!edit || !edit.changes)
        return null;

    const out: Record<string, LspTextEdit[]> = {};
    for (const [uri, edits] of Object.entries(edit.changes)) {
        if (!Array.isArray(edits))
            continue;

        if (isVirtualUri(uri)) {
            const owner = resolveOwner(uri);
            if (!owner)
                continue;

            const mappedEdits: LspTextEdit[] = [];
            for (const e of edits) {
                if (!e || typeof e.newText !== 'string' || typeof e.range !== 'object')
                    continue;

                const range = mapRangeToOwner(owner, e.range);
                if (!range)
                    continue;

                mappedEdits.push({ range, newText: e.newText });
            }
            if (mappedEdits.length > 0) {
                const existing = out[owner.containerUri];
                if (existing)
                    existing.push(...mappedEdits);
                else
                    out[owner.containerUri] = mappedEdits;
            }
        } else if (uri.startsWith('file:'))
            out[uri] = [...edits];
    }

    if (Object.keys(out).length === 0)
        return null;

    return { changes: out };
}

export function intersectVirtualRange(vf: VirtualFile, containerRange: Range): Range | null {
    const sec = vf.section;
    const bodyStart = sec.bodyStartLine;
    const bodyEndExclusive = bodyStart + sec.bodyLines.length;
    const startLine = Math.max(containerRange.start.line, bodyStart);
    const endLine = Math.min(containerRange.end.line, bodyEndExclusive);
    if (startLine > endLine || startLine >= bodyEndExclusive)
        return null;

    const startChar = startLine === containerRange.start.line ? containerRange.start.character : 0;
    if (endLine === bodyEndExclusive) {
        const lastLine = bodyEndExclusive - 1;
        const lastText = sec.bodyLines[lastLine - bodyStart];
        return { start: pos(startLine - bodyStart, startChar), end: pos(lastLine - bodyStart, lastText.length) };
    }

    const endChar = endLine === containerRange.end.line ? containerRange.end.character : 0;
    return { start: pos(startLine - bodyStart, startChar), end: pos(endLine - bodyStart, endChar) };
}

function toModelRange(r: LspRange): Range {
    return { start: pos(r.start.line, r.start.character), end: pos(r.end.line, r.end.character) };
}

export class DiagnosticsAggregator<T> {
    private readonly m_byContainer = new Map<string, Map<string, T[]>>();

    set(containerUri: string, path: string, diagnostics: readonly T[]): void {
        let byPath = this.m_byContainer.get(containerUri);
        if (!byPath) {
            byPath = new Map();
            this.m_byContainer.set(containerUri, byPath);
        }

        if (diagnostics.length === 0)
            byPath.delete(path);
        else
            byPath.set(path, [...diagnostics]);

        if (byPath.size === 0)
            this.m_byContainer.delete(containerUri);
    }

    drop(containerUri: string, path: string): void {
        this.set(containerUri, path, []);
    }

    dropContainer(containerUri: string): void {
        this.m_byContainer.delete(containerUri);
    }

    forContainer(containerUri: string): T[] {
        const byPath = this.m_byContainer.get(containerUri);
        if (!byPath)
            return [];

        const out: T[] = [];
        for (const diags of byPath.values())
            out.push(...diags);

        return out;
    }

    hasContainer(containerUri: string): boolean {
        return this.m_byContainer.has(containerUri);
    }

    clear(): void {
        this.m_byContainer.clear();
    }
}
