import { Range, pos } from './text';
import { DccTestDocument, VirtualFile } from './dccTestDocument';

export const VIRTUAL_SCHEME = 'dccv';

const B64_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';

export function encodeBase64Url(data: string): string {
    const bytes = new TextEncoder().encode(data);
    let out = '';
    for (let i = 0; i < bytes.length; i += 3) {
        const b0 = bytes[i];
        const b1 = i + 1 < bytes.length ? bytes[i + 1] : 0;
        const b2 = i + 2 < bytes.length ? bytes[i + 2] : 0;
        out += B64_ALPHABET[b0 >> 2];
        out += B64_ALPHABET[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += i + 1 < bytes.length ? B64_ALPHABET[((b1 & 0x0f) << 2) | (b2 >> 6)] : '';
        out += i + 2 < bytes.length ? B64_ALPHABET[b2 & 0x3f] : '';
    }

    return out;
}

export function decodeBase64Url(data: string): string {
    const map = new Map<string, number>();
    for (let i = 0; i < B64_ALPHABET.length; ++i) {
        map.set(B64_ALPHABET[i], i);
    }

    const bytes: number[] = [];
    for (let i = 0; i < data.length; i += 4) {
        const c0 = map.get(data[i]);
        const c1 = i + 1 < data.length ? map.get(data[i + 1]) : undefined;
        const c2 = i + 2 < data.length ? map.get(data[i + 2]) : undefined;
        const c3 = i + 3 < data.length ? map.get(data[i + 3]) : undefined;
        if (c0 === undefined || c1 === undefined)
            break;

        bytes.push((c0 << 2) | (c1 >> 4));
        if (c2 !== undefined)
            bytes.push(((c1 & 0x0f) << 4) | (c2 >> 2));

        if (c3 !== undefined)
            bytes.push(((c2! & 0x03) << 6) | c3);
    }

    return new TextDecoder().decode(new Uint8Array(bytes));
}

export function virtualUriFor(containerUri: string, virtualPath: string): string {
    return `${VIRTUAL_SCHEME}:${encodeBase64Url(containerUri)}/${virtualPath}`;
}

export function isVirtualUri(uri: string): boolean {
    return uri.startsWith(`${VIRTUAL_SCHEME}:`);
}

export function containerUriFromVirtual(uri: string): string | null {
    if (!isVirtualUri(uri))
        return null;

    const rest = uri.slice(VIRTUAL_SCHEME.length + 1);
    const slash = rest.indexOf('/');
    if (slash < 0)
        return null;

    try {
        return decodeBase64Url(rest.slice(0, slash));
    } catch {
        return null;
    }
}

export function virtualPathFromUri(uri: string): string | null {
    if (!isVirtualUri(uri))
        return null;

    const rest = uri.slice(VIRTUAL_SCHEME.length + 1);
    const slash = rest.indexOf('/');
    if (slash < 0)
        return null;

    return rest.slice(slash + 1);
}

export function mapVirtualPositionToContainer(vfile: VirtualFile, vline: number, vchar: number): { line: number; character: number } | null {
    const containerLine = vfile.section.bodyStartLine + vline;
    if (vline < 0 || vline >= vfile.section.bodyLines.length)
        return null;

    const bodyLine = vfile.section.bodyLines[vline];
    return { line: containerLine, character: Math.max(0, Math.min(vchar, bodyLine.length)) };
}

export function mapContainerPositionToVirtual(vfile: VirtualFile, cline: number, cchar: number): { line: number; character: number } | null {
    const rel = cline - vfile.section.bodyStartLine;
    if (rel < 0 || rel >= vfile.section.bodyLines.length)
        return null;

    const bodyLine = vfile.section.bodyLines[rel];
    return { line: rel, character: Math.max(0, Math.min(cchar, bodyLine.length)) };
}

export function mapVirtualRangeToContainer(vfile: VirtualFile, r: Range): Range | null {
    const start = mapVirtualPositionToContainer(vfile, r.start.line, r.start.character);
    const end = mapVirtualPositionToContainer(vfile, r.end.line, r.end.character);
    if (!start || !end)
        return null;

    return { start: pos(start.line, start.character), end: pos(end.line, end.character) };
}

export function mapContainerRangeToVirtual(vfile: VirtualFile, r: Range): Range | null {
    const start = mapContainerPositionToVirtual(vfile, r.start.line, r.start.character);
    const end = mapContainerPositionToVirtual(vfile, r.end.line, r.end.character);
    if (!start || !end)
        return null;

    return { start: pos(start.line, start.character), end: pos(end.line, end.character) };
}

export function virtualFileAtContainerPosition(model: DccTestDocument, cline: number): VirtualFile | undefined {
    return model.fileSectionAt(cline);
}

export function virtualDocumentRange(vfile: VirtualFile): Range {
    const lines = vfile.content.split('\n');
    const last = Math.max(0, lines.length - 1);
    return { start: pos(0, 0), end: pos(last, lines[last].length) };
}
