import * as vscode from 'vscode';
import { LanguageClient } from 'vscode-languageclient/node';
import { DccTestDocument, VirtualFile } from '../model/dccTestDocument';
import {
    isVirtualUri,
    mapContainerPositionToVirtual,
    mapVirtualRangeToContainer,
    virtualUriFor,
} from '../model/virtualFs';
import { DccTestModelCache } from './dccTestCache';
import { TokenBuilder, SemanticTokenType } from './semanticTokens';

interface VirtualDocState {
    path: string;
    uri: string;
    version: number;
    text: string;
}

interface VirtualTree {
    containerUri: string;
    files: Map<string, VirtualDocState>;
}

interface LspPosition {
    line: number;
    character: number;
}

interface LspRange {
    start: LspPosition;
    end: LspPosition;
}

interface LspLocation {
    uri: string;
    range: LspRange;
}

interface LspLocationLink {
    originSelectionRange?: LspRange;
    targetUri: string;
    targetRange: LspRange;
    targetSelectionRange?: LspRange;
}

interface LspSymbol {
    name: string;
    detail?: string;
    kind: number;
    range: LspRange;
    selectionRange?: LspRange;
    children?: LspSymbol[];
}

interface LspSymbolInfo {
    name: string;
    kind: number;
    location: LspLocation;
    containerName?: string;
}

interface HoverResult {
    contents?: vscode.Hover['contents'] | null;
    range?: LspRange;
}

interface SemanticTokensResult {
    data?: number[];
}

interface CompletionResult {
    items?: unknown[];
    isIncomplete?: boolean;
}

export class EmbeddedDcBridge implements vscode.Disposable {
    private readonly m_cache: DccTestModelCache;
    private readonly m_trees = new Map<string, VirtualTree>();
    private readonly m_diagnostics = vscode.languages.createDiagnosticCollection('dcc-dc-embedded');
    private m_client: LanguageClient | undefined;

    constructor(cache: DccTestModelCache) {
        this.m_cache = cache;
    }

    attachClient(client: LanguageClient): void {
        this.m_client = client;
    }

    isVirtualUri(uri: string): boolean {
        return isVirtualUri(uri);
    }

    sync(document: vscode.TextDocument): void {
        if (document.languageId !== 'dcc-test')
            return;

        const model = this.m_cache.get(document);
        const containerUri = document.uri.toString();
        let tree = this.m_trees.get(containerUri);
        if (!tree) {
            tree = { containerUri, files: new Map() };
            this.m_trees.set(containerUri, tree);
        }

        const desired = new Map<string, { uri: string; text: string }>();
        for (const vf of model.virtualFiles)
            desired.set(vf.path, { uri: virtualUriFor(containerUri, vf.path), text: vf.content });


        for (const [path, state] of tree.files) {
            if (!desired.has(path)) {
                this.sendNotification('textDocument/didClose', {
                    textDocument: { uri: state.uri },
                });
                tree.files.delete(path);
                this.m_diagnostics.delete(vscode.Uri.parse(containerUri));
            }
        }

        for (const [path, want] of desired) {
            const existing = tree.files.get(path);
            if (!existing) {
                this.sendNotification('textDocument/didOpen', {
                    textDocument: { uri: want.uri, languageId: 'dc', version: 1, text: want.text },
                });
                tree.files.set(path, { path, uri: want.uri, version: 1, text: want.text });
            } else if (existing.text !== want.text) {
                const version = existing.version + 1;
                this.sendNotification('textDocument/didChange', {
                    textDocument: { uri: want.uri, version },
                    contentChanges: [{ text: want.text }],
                });
                existing.version = version;
                existing.text = want.text;
            }
        }

        if (tree.files.size === 0)
            this.m_trees.delete(containerUri);
    }

    closeContainer(containerUri: string): void {
        const tree = this.m_trees.get(containerUri);
        if (tree) {
            for (const state of tree.files.values()) {
                this.sendNotification('textDocument/didClose', {
                    textDocument: { uri: state.uri },
                });
            }
            this.m_trees.delete(containerUri);
        }
        this.m_diagnostics.delete(vscode.Uri.parse(containerUri));
    }

    handleServerDiagnostics(uri: string, diagnostics: readonly vscode.Diagnostic[]): void {
        const mapped = this.mapDiagnostics(uri, diagnostics);
        if (mapped)
            this.m_diagnostics.set(vscode.Uri.parse(mapped.containerUri), mapped.diagnostics);
        else
            this.m_diagnostics.delete(vscode.Uri.parse(uri));

    }

    private mapDiagnostics(
        uri: string,
        diagnostics: readonly vscode.Diagnostic[],
    ): { containerUri: string; diagnostics: vscode.Diagnostic[] } | null {
        const owner = this.findOwner(uri);
        if (!owner)
            return null;

        const out: vscode.Diagnostic[] = [];
        for (const d of diagnostics) {
            const r = mapVirtualRangeToContainer(owner.vf, d.range as unknown as LspRange);
            if (!r)
                continue;

            const mapped = new vscode.Diagnostic(
                new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character),
                d.message,
                d.severity,
            );
            mapped.source = d.source ? `${d.source} (${owner.vf.path})` : `dcc (${owner.vf.path})`;
            if (d.code !== undefined)
                mapped.code = d.code;

            out.push(mapped);
        }
        return { containerUri: owner.containerUri, diagnostics: out };
    }

    private findOwner(virtualUri: string): { containerUri: string; vf: VirtualFile } | null {
        for (const tree of this.m_trees.values()) {
            for (const state of tree.files.values()) {
                if (state.uri === virtualUri) {
                    const model = this.m_cache.getForUri(tree.containerUri);
                    const vf = model?.fileSectionForPath(state.path);
                    if (vf)
                        return { containerUri: tree.containerUri, vf };

                    return null;
                }
            }
        }
        return null;
    }

    private canProxy(): boolean {
        return !!this.m_client && this.m_client.isRunning();
    }

    private virtualFileAt(model: DccTestDocument, line: number): { vf: VirtualFile; uri: string } | null {
        const vf = model.fileSectionAt(line);
        if (!vf)
            return null;

        return { vf, uri: virtualUriFor(model.uri, vf.path) };
    }

    async hover(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.Hover | null> {
        if (!this.canProxy())
            return null;

        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit)
            return null;

        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos)
            return null;

        try {
            const result = (await this.m_client!.sendRequest('textDocument/hover', {
                textDocument: { uri: hit.uri },
                position: { line: vpos.line, character: vpos.character },
            })) as HoverResult | undefined;

            if (!result || result.contents === undefined || result.contents === null)
                return null;

            let range: vscode.Range | undefined;
            if (result.range) {
                const mapped = mapVirtualRangeToContainer(hit.vf, result.range as LspRange);
                if (mapped)
                    range = new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character);
            }

            return new vscode.Hover(result.contents, range);
        } catch {
            return null;
        }
    }

    async definition(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.Location[] | null> {
        return this.proxyLocations('textDocument/definition', document, position, true);
    }

    async references(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.Location[] | null> {
        return this.proxyLocations('textDocument/references', document, position, false);
    }

    private async proxyLocations(
        method: string,
        document: vscode.TextDocument,
        position: vscode.Position,
        includeDeclaration: boolean,
    ): Promise<vscode.Location[] | null> {
        if (!this.canProxy())
            return null;

        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit)
            return null;

        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos)
            return null;

        const params: Record<string, unknown> = {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
        };

        if (method === 'textDocument/references')
            params.context = { includeDeclaration };

        try {
            const result = (await this.m_client!.sendRequest(method, params)) as LspLocation | LspLocation[] | LspLocationLink[] | null;
            if (!result)
                return [];

            const items = Array.isArray(result) ? result : [result];
            const out: vscode.Location[] = [];
            for (const item of items) {
                if (item && typeof item === 'object' && 'uri' in item && 'range' in item) {
                    const loc = item as LspLocation;
                    if (loc.uri === hit.uri) {
                        const mapped = mapVirtualRangeToContainer(hit.vf, loc.range);
                        if (mapped) {
                            out.push(
                                new vscode.Location(
                                    document.uri,
                                    new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character),
                                ),
                            );
                        }
                    }
                } else if (item && typeof item === 'object' && 'targetUri' in item) {
                    const link = item as LspLocationLink;
                    if (link.targetUri === hit.uri && link.targetSelectionRange) {
                        const mapped = mapVirtualRangeToContainer(hit.vf, link.targetSelectionRange);
                        if (mapped) {
                            out.push(
                                new vscode.Location(
                                    document.uri,
                                    new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character),
                                ),
                            );
                        }
                    }
                }
            }
            return out;
        } catch {
            return [];
        }
    }

    async documentSymbols(document: vscode.TextDocument): Promise<Map<string, vscode.DocumentSymbol[]>> {
        const out = new Map<string, vscode.DocumentSymbol[]>();
        if (!this.canProxy())
            return out;

        const model = this.m_cache.get(document);
        for (const vf of model.virtualFiles) {
            const uri = virtualUriFor(model.uri, vf.path);
            try {
                const result = (await this.m_client!.sendRequest('textDocument/documentSymbol', {
                    textDocument: { uri },
                })) as LspSymbol[] | null;

                if (!result)
                    continue;

                const symbols: vscode.DocumentSymbol[] = [];
                for (const item of result) {
                    const mapped = this.translateSymbol(vf, item);
                    if (mapped)
                        symbols.push(mapped);
                }
                if (symbols.length > 0)
                    out.set(vf.path, symbols);
            } catch { }
        }

        return out;
    }

    private translateSymbol(vf: VirtualFile, sym: LspSymbol): vscode.DocumentSymbol | null {
        const r = mapVirtualRangeToContainer(vf, sym.range);
        if (!r)
            return null;

        const selectionRange =
            sym.selectionRange !== undefined
                ? mapVirtualRangeToContainer(vf, sym.selectionRange) ?? r
                : r;

        const children: vscode.DocumentSymbol[] = [];
        for (const child of sym.children ?? []) {
            const mapped = this.translateSymbol(vf, child);
            if (mapped)
                children.push(mapped);
        }

        const out = new vscode.DocumentSymbol(
            sym.name,
            sym.detail ?? '',
            sym.kind as vscode.SymbolKind,
            new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character),
            new vscode.Range(selectionRange.start.line, selectionRange.start.character, selectionRange.end.line, selectionRange.end.character),
        );

        out.children = children;
        return out;
    }

    async semanticTokens(document: vscode.TextDocument): Promise<TokenBuilder | null> {
        const builder = new TokenBuilder();
        if (!this.canProxy())
            return builder;

        const model = this.m_cache.get(document);
        for (const vf of model.virtualFiles) {
            const uri = virtualUriFor(model.uri, vf.path);
            try {
                const result = (await this.m_client!.sendRequest('textDocument/semanticTokens/full', {
                    textDocument: { uri },
                })) as SemanticTokensResult | null;

                if (!result || !result.data)
                    continue;

                const data = result.data;
                let line = 0;
                let char = 0;
                for (let i = 0; i + 4 < data.length; i += 5) {
                    line += data[i];
                    if (data[i] === 0)
                        char += data[i + 1];
                    else
                        char = data[i + 1];

                    const length = data[i + 2];
                    const mapped = mapVirtualRangeToContainer(vf, {
                        start: { line, character: char },
                        end: { line, character: char + length },
                    });

                    if (!mapped)
                        continue;

                    builder.add(
                        mapped.start.line,
                        mapped.start.character,
                        Math.max(0, mapped.end.character - mapped.start.character),
                        'identifier' as SemanticTokenType,
                        false,
                    );
                }
            } catch { }
        }

        return builder;
    }

    async completion(
        document: vscode.TextDocument,
        position: vscode.Position,
    ): Promise<vscode.CompletionItem[] | null> {
        if (!this.canProxy())
            return null;

        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit)
            return null;

        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos)
            return null;

        try {
            const result = (await this.m_client!.sendRequest('textDocument/completion', {
                textDocument: { uri: hit.uri },
                position: { line: vpos.line, character: vpos.character },
            })) as CompletionResult | null;

            const list = Array.isArray(result) ? result : result?.items;
            if (!Array.isArray(list))
                return [];

            const items: vscode.CompletionItem[] = [];
            for (const raw of list) {
                const item = raw as vscode.CompletionItem;

                const textEdit = (item as unknown as { textEdit?: { range?: LspRange } }).textEdit;
                if (textEdit && textEdit.range) {
                    const mapped = mapVirtualRangeToContainer(hit.vf, textEdit.range);
                    if (!mapped)
                        continue;

                    textEdit.range = {
                        start: { line: mapped.start.line, character: mapped.start.character },
                        end: { line: mapped.end.line, character: mapped.end.character },
                    };

                    (item as unknown as { textEdit: unknown }).textEdit = textEdit;
                }

                items.push(item);
            }

            return items;
        } catch {
            return [];
        }
    }

    private sendNotification(method: string, params: unknown): void {
        const client = this.m_client;
        if (!client || !client.isRunning())
            return;

        void client.sendNotification(method, params);
    }

    dispose(): void {
        for (const tree of this.m_trees.values()) {
            for (const state of tree.files.values()) {
                this.sendNotification('textDocument/didClose', {
                    textDocument: { uri: state.uri },
                });
            }
        }
        this.m_trees.clear();
        this.m_diagnostics.dispose();
    }
}
