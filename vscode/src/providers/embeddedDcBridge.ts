import * as vscode from 'vscode';
import { LanguageClient } from 'vscode-languageclient/node';
import {
    DiagnosticsAggregator,
    intersectVirtualRange,
    MappedLocation,
    mapLspLocation,
    mapLspLocationLink,
    mapWorkspaceEdit,
    orderedVirtualFilesForSync,
    VirtualOwner,
} from '../model/dcMapping';
import { DccTestDocument, VirtualFile } from '../model/dccTestDocument';
import {
    LspCodeAction,
    LspCompletionItem,
    LspCompletionList,
    LspDiagnostic,
    LspDocumentHighlight,
    LspHover,
    LspInlayHint,
    LspLocation,
    LspLocationLink,
    LspPrepareRenameResult,
    LspSemanticTokens,
    LspSignatureHelp,
    LspSymbol,
    LspTextEdit,
    LspWorkspaceEdit,
} from '../model/lspTypes';
import { serverTokenModifiersToDccMask, serverTokenTypeToDccName } from '../model/serverSemanticTokens';
import { Range as ModelRange } from '../model/text';
import {
    containerUriFromVirtual,
    isVirtualUri,
    mapContainerPositionToVirtual,
    mapContainerRangeToVirtual,
    mapVirtualPositionToContainer,
    mapVirtualRangeToContainer,
    virtualDocumentRange,
    virtualPathFromUri,
    virtualUriFor,
} from '../model/virtualFs';
import { DccTestModelCache } from './dccTestCache';
import { SemanticTokenType, TokenBuilder, tokenTypeIndex } from './semanticTokens';

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

export class EmbeddedDcBridge implements vscode.Disposable {
    private readonly m_cache: DccTestModelCache;
    private readonly m_trees = new Map<string, VirtualTree>();
    private readonly m_diagnostics = vscode.languages.createDiagnosticCollection('dcc-dc-embedded');
    private readonly m_diagAggregator = new DiagnosticsAggregator<vscode.Diagnostic>();
    private readonly m_unsupportedMethods = new Set<string>();
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
        for (const vf of orderedVirtualFilesForSync(model))
            desired.set(vf.path, { uri: virtualUriFor(containerUri, vf.path), text: vf.content });

        for (const [path, state] of tree.files) {
            if (!desired.has(path)) {
                this.sendNotification('textDocument/didClose', {
                    textDocument: { uri: state.uri },
                });
                tree.files.delete(path);
                this.m_diagAggregator.drop(containerUri, path);
                this.emitContainerDiagnostics(containerUri);
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

        if (tree.files.size === 0) {
            this.m_trees.delete(containerUri);
        }
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
        this.m_diagAggregator.dropContainer(containerUri);
        this.m_diagnostics.delete(vscode.Uri.parse(containerUri));
    }

    handleServerDiagnostics(uri: string, diagnostics: readonly vscode.Diagnostic[]): void {
        const owner = this.findOwner(uri);
        if (!owner) {
            const container = containerUriFromVirtual(uri);
            const path = virtualPathFromUri(uri);
            if (container && path) {
                this.m_diagAggregator.drop(container, path);
                this.emitContainerDiagnostics(container);
            }

            return;
        }

        this.m_diagAggregator.set(owner.containerUri, owner.path, this.mapDiagnostics(owner, diagnostics));
        this.emitContainerDiagnostics(owner.containerUri);
    }

    private mapDiagnostics(owner: VirtualOwner, diagnostics: readonly vscode.Diagnostic[]): vscode.Diagnostic[] {
        const out: vscode.Diagnostic[] = [];
        for (const d of diagnostics) {
            const r = mapVirtualRangeToContainer(owner.vf, d.range as unknown as ModelRange);
            if (!r) {
                continue;
            }
            const mapped = new vscode.Diagnostic(
                new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character),
                d.message,
                d.severity,
            );
            mapped.source = d.source ? `${d.source} (${owner.vf.path})` : `dcc (${owner.vf.path})`;
            if (d.code !== undefined) {
                mapped.code = d.code;
            }
            out.push(mapped);
        }
        return out;
    }

    private emitContainerDiagnostics(containerUri: string): void {
        const all = this.m_diagAggregator.forContainer(containerUri);
        const container = vscode.Uri.parse(containerUri);
        if (all.length > 0) {
            this.m_diagnostics.set(container, all);
        } else {
            this.m_diagnostics.delete(container);
        }
    }

    private findOwner(virtualUri: string): VirtualOwner | null {
        for (const tree of this.m_trees.values()) {
            for (const state of tree.files.values()) {
                if (state.uri === virtualUri) {
                    const model = this.m_cache.getForUri(tree.containerUri);
                    const vf = model?.fileSectionForPath(state.path);
                    if (vf) {
                        return { containerUri: tree.containerUri, path: state.path, vf };
                    }
                    return null;
                }
            }
        }
        return null;
    }

    private canProxy(): boolean {
        return !!this.m_client && this.m_client.isRunning();
    }

    private async request<T>(method: string, params: unknown): Promise<T | undefined> {
        const client = this.m_client;
        if (!client || !client.isRunning() || this.m_unsupportedMethods.has(method))
            return undefined;

        try {
            return (await client.sendRequest(method, params)) as T;
        } catch (err) {
            if ((err as { code?: number })?.code === -32601)
                this.m_unsupportedMethods.add(method);

            return undefined;
        }
    }

    private virtualFileAt(model: DccTestDocument, line: number): { vf: VirtualFile; uri: string } | null {
        const vf = model.fileSectionAt(line);
        if (!vf) {
            return null;
        }
        return { vf, uri: virtualUriFor(model.uri, vf.path) };
    }

    async hover(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.Hover | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit) {
            return null;
        }
        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos) {
            return null;
        }
        const result = await this.request<LspHover>('textDocument/hover', {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
        });
        if (!result || result.contents === undefined || result.contents === null) {
            return null;
        }
        let range: vscode.Range | undefined;
        if (result.range) {
            const mapped = mapVirtualRangeToContainer(hit.vf, result.range);
            if (mapped) {
                range = new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character);
            }
        }
        return new vscode.Hover(this.hoverContents(result.contents), range);
    }

    private hoverContents(raw: unknown): vscode.MarkdownString | string | vscode.MarkdownString[] {
        if (raw === undefined || raw === null) {
            return '';
        }
        if (typeof raw === 'string') {
            return raw;
        }
        if (Array.isArray(raw)) {
            const parts: vscode.MarkdownString[] = [];
            for (const item of raw) {
                const c = this.hoverContents(item);
                if (typeof c === 'string') {
                    parts.push(new vscode.MarkdownString(c));
                } else if (Array.isArray(c)) {
                    parts.push(...c);
                } else if (c) {
                    parts.push(c);
                }
            }
            return parts;
        }
        if (typeof raw === 'object') {
            const value = (raw as { value?: unknown }).value;
            if (typeof value === 'string') {
                return new vscode.MarkdownString(value);
            }
        }
        return '';
    }

    async definition(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.Location[] | null> {
        return this.proxyLocations('textDocument/definition', document, position, true);
    }

    async references(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.Location[] | null> {
        return this.proxyLocations('textDocument/references', document, position, false);
    }

    async declaration(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.Location[] | null> {
        return this.definition(document, position);
    }

    async typeDefinition(_document: vscode.TextDocument, _position: vscode.Position): Promise<vscode.Location[] | null> {
        return null;
    }

    private async proxyLocations(
        method: string,
        document: vscode.TextDocument,
        position: vscode.Position,
        includeDeclaration: boolean,
    ): Promise<vscode.Location[] | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit) {
            return null;
        }
        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos) {
            return null;
        }
        const params: Record<string, unknown> = {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
        };
        if (method === 'textDocument/references') {
            params.context = { includeDeclaration };
        }
        const result = await this.request<LspLocation | LspLocation[] | LspLocationLink[] | null>(method, params);
        if (result === undefined || result === null)
            return [];

        const items = Array.isArray(result) ? result : [result];
        const out: vscode.Location[] = [];
        for (const item of items) {
            const mapped = this.mapLocationResultItem(item);
            if (mapped) {
                out.push(mapped);
            }
        }
        return out;
    }

    private mapLocationResultItem(item: unknown): vscode.Location | null {
        if (!item || typeof item !== 'object')
            return null;

        const obj = item as Record<string, unknown>;
        let mapped: MappedLocation | null = null;
        if (typeof obj.targetUri === 'string' && obj.targetRange) {
            mapped = mapLspLocationLink(item as LspLocationLink, (uri) => this.findOwner(uri));
        } else if (typeof obj.uri === 'string' && obj.range) {
            mapped = mapLspLocation(item as LspLocation, (uri) => this.findOwner(uri));
        }
        if (!mapped) {
            return null;
        }
        return new vscode.Location(
            vscode.Uri.parse(mapped.uri),
            new vscode.Range(mapped.range.start.line, mapped.range.start.character, mapped.range.end.line, mapped.range.end.character),
        );
    }

    async documentSymbols(document: vscode.TextDocument): Promise<Map<string, vscode.DocumentSymbol[]>> {
        const out = new Map<string, vscode.DocumentSymbol[]>();
        if (!this.canProxy()) {
            return out;
        }
        const model = this.m_cache.get(document);
        for (const vf of model.virtualFiles) {
            const uri = virtualUriFor(model.uri, vf.path);
            const result = await this.request<LspSymbol[] | null>('textDocument/documentSymbol', { textDocument: { uri } });
            if (!result || !Array.isArray(result)) {
                continue;
            }
            const symbols: vscode.DocumentSymbol[] = [];
            for (const item of result) {
                const mapped = this.translateSymbol(vf, item);
                if (mapped) {
                    symbols.push(mapped);
                }
            }
            if (symbols.length > 0) {
                out.set(vf.path, symbols);
            }
        }
        return out;
    }

    private translateSymbol(vf: VirtualFile, sym: LspSymbol): vscode.DocumentSymbol | null {
        const r = mapVirtualRangeToContainer(vf, sym.range);
        if (!r) {
            return null;
        }
        const selectionRange = sym.selectionRange !== undefined ? mapVirtualRangeToContainer(vf, sym.selectionRange) ?? r : r;
        const children: vscode.DocumentSymbol[] = [];
        for (const child of sym.children ?? []) {
            const mapped = this.translateSymbol(vf, child);
            if (mapped) {
                children.push(mapped);
            }
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
        if (!this.canProxy()) {
            return builder;
        }
        const model = this.m_cache.get(document);
        for (const vf of model.virtualFiles) {
            const uri = virtualUriFor(model.uri, vf.path);
            const result = await this.request<LspSemanticTokens>('textDocument/semanticTokens/full', { textDocument: { uri } });
            if (!result || !result.data) {
                continue;
            }
            const data = result.data;
            let line = 0;
            let char = 0;
            for (let i = 0; i + 4 < data.length; i += 5) {
                line += data[i];
                if (data[i] === 0) {
                    char += data[i + 1];
                } else {
                    char = data[i + 1];
                }
                const length = data[i + 2];
                const mapped = mapVirtualRangeToContainer(vf, {
                    start: { line, character: char },
                    end: { line, character: char + length },
                });
                if (!mapped) {
                    continue;
                }
                const typeName = serverTokenTypeToDccName(data[i + 3]);
                builder.addIndexed(
                    mapped.start.line,
                    mapped.start.character,
                    Math.max(0, mapped.end.character - mapped.start.character),
                    tokenTypeIndex(typeName as SemanticTokenType),
                    serverTokenModifiersToDccMask(data[i + 4]),
                );
            }
        }
        return builder;
    }

    async completion(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.CompletionItem[] | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit) {
            return null;
        }
        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos) {
            return null;
        }
        const result = await this.request<LspCompletionList | LspCompletionItem[]>('textDocument/completion', {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
        });
        if (result === undefined || result === null) {
            return [];
        }
        const list = Array.isArray(result) ? result : result.items;
        if (!Array.isArray(list)) {
            return [];
        }
        const items: vscode.CompletionItem[] = [];
        for (const raw of list) {
            const item = this.translateCompletionItem(raw, hit.vf);
            if (item) {
                items.push(item);
            }
        }
        return items;
    }

    private translateCompletionItem(raw: LspCompletionItem, vf: VirtualFile): vscode.CompletionItem | null {
        const item = new vscode.CompletionItem(raw.label ?? '', raw.kind as vscode.CompletionItemKind);
        if (raw.detail !== undefined) {
            item.detail = raw.detail;
        }
        if (raw.documentation !== undefined) {
            item.documentation = typeof raw.documentation === 'string' ? raw.documentation : (raw.documentation as { value?: string })?.value;
        }
        if (raw.sortText !== undefined) {
            item.sortText = raw.sortText;
        }
        if (raw.preselect !== undefined) {
            item.preselect = raw.preselect;
        }
        const isSnippet = raw.insertTextFormat === 2;
        if (raw.textEdit) {
            const mapped = mapVirtualRangeToContainer(vf, raw.textEdit.range);
            if (!mapped)
                return null;

            const range = new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character);
            const textEdit = new vscode.TextEdit(range, raw.textEdit.newText);
            if (isSnippet) {
                (textEdit as unknown as { newText: vscode.SnippetString }).newText = new vscode.SnippetString(raw.textEdit.newText);
            }
            item.textEdit = textEdit;
        }
        if (raw.insertText !== undefined && !raw.textEdit) {
            item.insertText = isSnippet ? new vscode.SnippetString(raw.insertText) : raw.insertText;
        }
        if (raw.additionalTextEdits) {
            const edits: vscode.TextEdit[] = [];
            for (const e of raw.additionalTextEdits) {
                const mapped = mapVirtualRangeToContainer(vf, e.range);
                if (!mapped) {
                    return null;
                }
                edits.push(
                    new vscode.TextEdit(
                        new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character),
                        e.newText,
                    ),
                );
            }
            if (edits.length > 0) {
                item.additionalTextEdits = edits;
            }
        }
        if (raw.command) {
            item.command = raw.command as unknown as vscode.Command;
        }
        return item;
    }

    async signatureHelp(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.SignatureHelp | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit) {
            return null;
        }
        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos) {
            return null;
        }
        const result = await this.request<LspSignatureHelp>('textDocument/signatureHelp', {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
        });
        if (!result || !Array.isArray(result.signatures)) {
            return null;
        }
        const help = new vscode.SignatureHelp();
        help.activeSignature = result.activeSignature ?? 0;
        help.activeParameter = result.activeParameter ?? 0;
        help.signatures = result.signatures.map((sig) => {
            const info = new vscode.SignatureInformation(sig.label, sig.documentation);
            info.parameters = (sig.parameters ?? []).map((p) => new vscode.ParameterInformation(p.label, p.documentation));
            return info;
        });
        return help;
    }

    async documentHighlights(document: vscode.TextDocument, position: vscode.Position): Promise<vscode.DocumentHighlight[] | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit) {
            return null;
        }
        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos) {
            return null;
        }
        const result = await this.request<LspDocumentHighlight[]>('textDocument/documentHighlight', {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
        });
        if (!result || !Array.isArray(result)) {
            return [];
        }
        const out: vscode.DocumentHighlight[] = [];
        for (const h of result) {
            const mapped = mapVirtualRangeToContainer(hit.vf, h.range);
            if (!mapped)
                continue;

            const kind =
                h.kind === 2 ? vscode.DocumentHighlightKind.Read : h.kind === 3 ? vscode.DocumentHighlightKind.Write : vscode.DocumentHighlightKind.Text;
            out.push(
                new vscode.DocumentHighlight(
                    new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character),
                    kind,
                ),
            );
        }
        return out;
    }

    async inlayHints(document: vscode.TextDocument, range: vscode.Range): Promise<vscode.InlayHint[] | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const containerRange: ModelRange = {
            start: { line: range.start.line, character: range.start.character },
            end: { line: range.end.line, character: range.end.character },
        };
        const out: vscode.InlayHint[] = [];
        for (const vf of model.virtualFiles) {
            const vrange = intersectVirtualRange(vf, containerRange);
            if (!vrange) {
                continue;
            }
            const uri = virtualUriFor(model.uri, vf.path);
            const result = await this.request<LspInlayHint[]>('textDocument/inlayHint', {
                textDocument: { uri },
                range: vrange,
            });
            if (!result || !Array.isArray(result)) {
                continue;
            }
            for (const h of result) {
                const mapped = mapVirtualPositionToContainer(vf, h.position.line, h.position.character);
                if (!mapped) {
                    continue;
                }
                const hint = new vscode.InlayHint(
                    new vscode.Position(mapped.line, mapped.character),
                    h.label ?? '',
                    h.kind as vscode.InlayHintKind,
                );
                if (h.paddingLeft !== undefined) {
                    hint.paddingLeft = h.paddingLeft;
                }
                if (h.paddingRight !== undefined) {
                    hint.paddingRight = h.paddingRight;
                }
                out.push(hint);
            }
        }
        return out;
    }

    async codeActions(document: vscode.TextDocument, range: vscode.Range, context: vscode.CodeActionContext): Promise<vscode.CodeAction[] | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, range.start.line);
        if (!hit) {
            return null;
        }
        const containerRange: ModelRange = {
            start: { line: range.start.line, character: range.start.character },
            end: { line: range.end.line, character: range.end.character },
        };
        const vrange = mapContainerRangeToVirtual(hit.vf, containerRange) ?? virtualDocumentRange(hit.vf);
        const vDiags: LspDiagnostic[] = [];
        for (const d of context.diagnostics) {
            const vr = mapContainerRangeToVirtual(hit.vf, {
                start: { line: d.range.start.line, character: d.range.start.character },
                end: { line: d.range.end.line, character: d.range.end.character },
            });
            if (!vr) {
                continue;
            }
            vDiags.push({ range: vr, message: d.message, severity: d.severity });
        }
        if (vDiags.length === 0) {
            return [];
        }
        const result = await this.request<LspCodeAction[]>('textDocument/codeAction', {
            textDocument: { uri: hit.uri },
            range: vrange,
            context: { diagnostics: vDiags },
        });
        if (!result || !Array.isArray(result)) {
            return [];
        }
        const out: vscode.CodeAction[] = [];
        for (const raw of result) {
            const action = this.translateCodeAction(raw);
            if (action) {
                out.push(action);
            }
        }
        return out;
    }

    private translateCodeAction(raw: LspCodeAction): vscode.CodeAction | null {
        if (!raw.edit) {
            return null;
        }
        const mapped = mapWorkspaceEdit(raw.edit, (uri) => this.findOwner(uri));
        if (!mapped) {
            return null;
        }
        const action = new vscode.CodeAction(raw.title, raw.kind as unknown as vscode.CodeActionKind);
        const edit = new vscode.WorkspaceEdit();
        for (const [uri, edits] of Object.entries(mapped.changes ?? {})) {
            edit.set(
                vscode.Uri.parse(uri),
                edits.map(
                    (e) =>
                        new vscode.TextEdit(
                            new vscode.Range(e.range.start.line, e.range.start.character, e.range.end.line, e.range.end.character),
                            e.newText,
                        ),
                ),
            );
        }
        action.edit = edit;
        return action;
    }

    async prepareRename(document: vscode.TextDocument, position: vscode.Position): Promise<{ range: vscode.Range; placeholder: string } | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit) {
            return null;
        }
        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos) {
            return null;
        }
        const result = await this.request<LspPrepareRenameResult>('textDocument/prepareRename', {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
        });
        if (!result || !result.range) {
            return null;
        }
        const mapped = mapVirtualRangeToContainer(hit.vf, result.range);
        if (!mapped) {
            return null;
        }
        return {
            range: new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character),
            placeholder: result.placeholder ?? '',
        };
    }

    async rename(document: vscode.TextDocument, position: vscode.Position, newName: string): Promise<vscode.WorkspaceEdit | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit) {
            return null;
        }
        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos) {
            return null;
        }
        const result = await this.request<LspWorkspaceEdit>('textDocument/rename', {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
            newName,
        });
        if (!result) {
            return null;
        }
        return this.translateWorkspaceEdit(result);
    }

    private translateWorkspaceEdit(raw: LspWorkspaceEdit): vscode.WorkspaceEdit | null {
        const mapped = mapWorkspaceEdit(raw, (uri) => this.findOwner(uri));
        if (!mapped) {
            return null;
        }
        const out = new vscode.WorkspaceEdit();
        for (const [uri, edits] of Object.entries(mapped.changes ?? {})) {
            out.set(
                vscode.Uri.parse(uri),
                edits.map(
                    (e) =>
                        new vscode.TextEdit(
                            new vscode.Range(e.range.start.line, e.range.start.character, e.range.end.line, e.range.end.character),
                            e.newText,
                        ),
                ),
            );
        }
        return out;
    }

    async formatting(document: vscode.TextDocument, options: vscode.FormattingOptions): Promise<vscode.TextEdit[] | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const lspOptions = { tabSize: options.tabSize, insertSpaces: options.insertSpaces };
        const out: vscode.TextEdit[] = [];
        for (const vf of model.virtualFiles) {
            const uri = virtualUriFor(model.uri, vf.path);
            const result = await this.request<LspTextEdit[]>('textDocument/formatting', {
                textDocument: { uri },
                options: lspOptions,
            });
            if (!result || !Array.isArray(result)) {
                continue;
            }
            out.push(...this.mapTextEdits(vf, result));
        }
        return out;
    }

    async rangeFormatting(document: vscode.TextDocument, range: vscode.Range, options: vscode.FormattingOptions): Promise<vscode.TextEdit[] | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const containerRange: ModelRange = {
            start: { line: range.start.line, character: range.start.character },
            end: { line: range.end.line, character: range.end.character },
        };
        const lspOptions = { tabSize: options.tabSize, insertSpaces: options.insertSpaces };
        const out: vscode.TextEdit[] = [];
        for (const vf of model.virtualFiles) {
            const vrange = intersectVirtualRange(vf, containerRange);
            if (!vrange) {
                continue;
            }
            const uri = virtualUriFor(model.uri, vf.path);
            const result = await this.request<LspTextEdit[]>('textDocument/rangeFormatting', {
                textDocument: { uri },
                range: vrange,
                options: lspOptions,
            });
            if (!result || !Array.isArray(result)) {
                continue;
            }
            out.push(...this.mapTextEdits(vf, result));
        }
        return out;
    }

    async onTypeFormatting(
        document: vscode.TextDocument,
        position: vscode.Position,
        ch: string,
        options: vscode.FormattingOptions,
    ): Promise<vscode.TextEdit[] | null> {
        if (!this.canProxy()) {
            return null;
        }
        const model = this.m_cache.get(document);
        const hit = this.virtualFileAt(model, position.line);
        if (!hit) {
            return null;
        }
        const vpos = mapContainerPositionToVirtual(hit.vf, position.line, position.character);
        if (!vpos) {
            return null;
        }
        const result = await this.request<LspTextEdit[]>('textDocument/onTypeFormatting', {
            textDocument: { uri: hit.uri },
            position: { line: vpos.line, character: vpos.character },
            ch,
            options: { tabSize: options.tabSize, insertSpaces: options.insertSpaces },
        });
        if (!result || !Array.isArray(result)) {
            return [];
        }
        return this.mapTextEdits(hit.vf, result);
    }

    private mapTextEdits(vf: VirtualFile, edits: readonly LspTextEdit[]): vscode.TextEdit[] {
        const out: vscode.TextEdit[] = [];
        for (const e of edits) {
            if (!e || typeof e.newText !== 'string' || typeof e.range !== 'object') {
                continue;
            }
            const mapped = mapVirtualRangeToContainer(vf, e.range);
            if (!mapped) {
                continue;
            }
            out.push(
                new vscode.TextEdit(
                    new vscode.Range(mapped.start.line, mapped.start.character, mapped.end.line, mapped.end.character),
                    e.newText,
                ),
            );
        }
        return out;
    }

    private sendNotification(method: string, params: unknown): void {
        const client = this.m_client;
        if (!client || !client.isRunning()) {
            return;
        }
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
        this.m_diagAggregator.clear();
        this.m_diagnostics.dispose();
    }
}
