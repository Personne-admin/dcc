import * as vscode from 'vscode';
import { DccTestModelCache } from './dccTestCache';
import { EmbeddedDcBridge } from './embeddedDcBridge';
import {
    dccTestCompletion,
    dccTestDefinition,
    dccTestDiagnostics,
    dccTestFoldingRanges,
    dccTestHover,
    dccTestReferences,
    dccTestSemanticTokens,
    fileSectionAt,
    sectionSymbol,
} from './dccTestFeatures';
import {
    dirCompletion,
    dirDefinition,
    dirDiagnostics,
    dirDocumentSymbols,
    dirFoldingRanges,
    dirHover,
    dirReferences,
    dirScopeAtPosition,
    dirSemanticTokens,
    makeDirScope,
} from './dirFeatures';
import { DCC_SEMANTIC_LEGEND, mergeSemanticTokens } from './semanticTokens';

export function registerDccTestProviders(
    context: vscode.ExtensionContext,
    cache: DccTestModelCache,
    bridge: EmbeddedDcBridge,
): void {
    const collection = vscode.languages.createDiagnosticCollection('dcc-test');
    context.subscriptions.push(collection);

    const publishDiagnostics = (document: vscode.TextDocument): void => {
        if (document.languageId !== 'dcc-test')
            return;

        const model = cache.get(document);
        const diags = dccTestDiagnostics(model);
        for (const sec of model.sections) {
            if (sec.kind === 'expect-ir') {
                const scope = makeDirScope(document, cache, 'section', sec.headerLine);
                if (scope)
                    diags.push(...dirDiagnostics(scope));
            }
        }

        collection.set(document.uri, diags);
        bridge.sync(document);
    };

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(publishDiagnostics),
        vscode.workspace.onDidChangeTextDocument((e) => publishDiagnostics(e.document)),
        vscode.workspace.onDidCloseTextDocument((doc) => {
            collection.delete(doc.uri);
            bridge.closeContainer(doc.uri.toString());
        }),
    );
    for (const doc of vscode.workspace.textDocuments)
        publishDiagnostics(doc);

    context.subscriptions.push(
        vscode.languages.registerDocumentSymbolProvider('dcc-test', {
            async provideDocumentSymbols(document): Promise<vscode.DocumentSymbol[]> {
                const model = cache.get(document);
                const embedded = await bridge.documentSymbols(document);
                const out: vscode.DocumentSymbol[] = [];
                for (const sec of model.sections) {
                    const sym = sectionSymbol(sec);
                    if (sec.kind === 'file' && sec.target) {
                        const children = embedded.get(sec.target);
                        if (children)
                            sym.children.push(...children);
                    } else if (sec.kind === 'expect-ir') {
                        const scope = makeDirScope(document, cache, 'section', sec.headerLine);
                        if (scope)
                            sym.children.push(...dirDocumentSymbols(scope));
                    }

                    out.push(sym);
                }

                return out;
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerFoldingRangeProvider('dcc-test', {
            provideFoldingRanges(document): vscode.FoldingRange[] {
                const model = cache.get(document);
                return dccTestFoldingRanges(model);
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            'dcc-test',
            {
                async provideCompletionItems(document, position): Promise<vscode.CompletionItem[]> {
                    const model = cache.get(document);
                    const scope = dirScopeAtPosition(document, cache, position);
                    if (scope)
                        return dirCompletion(scope, position);

                    if (fileSectionAt(model, position)) {
                        const proxied = await bridge.completion(document, position);
                        return proxied ?? [];
                    }

                    return dccTestCompletion(model, document, position);
                },
            },
            ':',
            '=',
            ' ',
            '-',
            '.',
        ),
    );

    context.subscriptions.push(
        vscode.languages.registerHoverProvider('dcc-test', {
            async provideHover(document, position): Promise<vscode.Hover | null> {
                const model = cache.get(document);
                const scope = dirScopeAtPosition(document, cache, position);
                if (scope)
                    return dirHover(scope, position);

                if (fileSectionAt(model, position))
                    return bridge.hover(document, position);

                return dccTestHover(model, document, position);
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerDefinitionProvider('dcc-test', {
            async provideDefinition(document, position): Promise<vscode.Definition | null> {
                const model = cache.get(document);
                const scope = dirScopeAtPosition(document, cache, position);
                if (scope) {
                    const locs = dirDefinition(scope, position);
                    return locs.length > 0 ? locs : null;
                }

                if (fileSectionAt(model, position)) {
                    const proxied = await bridge.definition(document, position);
                    return proxied && proxied.length > 0 ? proxied : null;
                }

                const locs = dccTestDefinition(model, document, position);
                return locs.length > 0 ? locs : null;
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerDeclarationProvider('dcc-test', {
            async provideDeclaration(document, position): Promise<vscode.Declaration | null> {
                const model = cache.get(document);
                const scope = dirScopeAtPosition(document, cache, position);
                if (scope) {
                    const locs = dirDefinition(scope, position);
                    return locs.length > 0 ? locs : null;
                }
                if (fileSectionAt(model, position)) {
                    const proxied = await bridge.declaration(document, position);
                    return proxied && proxied.length > 0 ? proxied : null;
                }
                return null;
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerTypeDefinitionProvider('dcc-test', {
            async provideTypeDefinition(document, position): Promise<vscode.Definition | null> {
                const model = cache.get(document);
                if (fileSectionAt(model, position)) {
                    const proxied = await bridge.typeDefinition(document, position);
                    return proxied && proxied.length > 0 ? proxied : null;
                }
                return null;
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerReferenceProvider('dcc-test', {
            async provideReferences(document, position, _context): Promise<vscode.Location[] | null> {
                const model = cache.get(document);
                const scope = dirScopeAtPosition(document, cache, position);
                if (scope) {
                    const locs = dirReferences(scope, position);
                    return locs.length > 0 ? locs : null;
                }

                if (fileSectionAt(model, position)) {
                    const proxied = await bridge.references(document, position);
                    return proxied && proxied.length > 0 ? proxied : null;
                }

                const locs = dccTestReferences(model, document, position);
                return locs.length > 0 ? locs : null;
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerDocumentHighlightProvider('dcc-test', {
            async provideDocumentHighlights(document, position): Promise<vscode.DocumentHighlight[] | null> {
                const model = cache.get(document);
                if (fileSectionAt(model, position)) {
                    return bridge.documentHighlights(document, position);
                }
                return null;
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerSignatureHelpProvider(
            'dcc-test',
            {
                async provideSignatureHelp(document, position): Promise<vscode.SignatureHelp | null> {
                    const model = cache.get(document);
                    if (fileSectionAt(model, position)) {
                        return bridge.signatureHelp(document, position);
                    }
                    return null;
                },
            },
            '(',
            ',',
        ),
    );

    context.subscriptions.push(
        vscode.languages.registerInlayHintsProvider('dcc-test', {
            async provideInlayHints(document, range): Promise<vscode.InlayHint[] | null> {
                const model = cache.get(document);
                if (fileSectionAt(model, range.start)) {
                    return bridge.inlayHints(document, range);
                }
                return [];
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerCodeActionsProvider('dcc-test', {
            async provideCodeActions(document, range, context): Promise<vscode.CodeAction[]> {
                const model = cache.get(document);
                if (fileSectionAt(model, range.start)) {
                    const proxied = await bridge.codeActions(document, range, context);
                    return proxied ?? [];
                }
                return [];
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerRenameProvider('dcc-test', {
            async provideRenameEdits(document, position, newName): Promise<vscode.WorkspaceEdit | null> {
                const model = cache.get(document);
                if (fileSectionAt(model, position)) {
                    return bridge.rename(document, position, newName);
                }
                return null;
            },
            async prepareRename(document, position): Promise<vscode.Range | { range: vscode.Range; placeholder: string } | null> {
                const model = cache.get(document);
                if (fileSectionAt(model, position)) {
                    return bridge.prepareRename(document, position);
                }
                return null;
            },
        }),
    );

    context.subscriptions.push(
        vscode.languages.registerDocumentFormattingEditProvider('dcc-test', {
            async provideDocumentFormattingEdits(document, options): Promise<vscode.TextEdit[]> {
                const model = cache.get(document);
                if (model.virtualFiles.length === 0) {
                    return [];
                }
                const proxied = await bridge.formatting(document, options);
                return proxied ?? [];
            },
        }),
        vscode.languages.registerDocumentRangeFormattingEditProvider('dcc-test', {
            async provideDocumentRangeFormattingEdits(document, range, options): Promise<vscode.TextEdit[]> {
                const model = cache.get(document);
                if (fileSectionAt(model, range.start) || fileSectionAt(model, range.end)) {
                    const proxied = await bridge.rangeFormatting(document, range, options);
                    return proxied ?? [];
                }
                return [];
            },
        }),
        vscode.languages.registerOnTypeFormattingEditProvider(
            'dcc-test',
            {
                async provideOnTypeFormattingEdits(document, position, ch, options): Promise<vscode.TextEdit[]> {
                    const model = cache.get(document);
                    if (fileSectionAt(model, position)) {
                        const proxied = await bridge.onTypeFormatting(document, position, ch, options);
                        return proxied ?? [];
                    }
                    return [];
                },
            },
            '}',
            ';',
        ),
    );

    const semanticTokenProvider = vscode.languages.registerDocumentSemanticTokensProvider(
        'dcc-test',
        {
            async provideDocumentSemanticTokens(document): Promise<vscode.SemanticTokens> {
                const model = cache.get(document);
                const local = dccTestSemanticTokens(model);
                const sources: (vscode.SemanticTokens)[] = [local.build()];
                for (const sec of model.sections) {
                    if (sec.kind === 'expect-ir') {
                        const scope = makeDirScope(document, cache, 'section', sec.headerLine);
                        if (scope)
                            sources.push(dirSemanticTokens(scope));
                    }
                }

                const embedded = await bridge.semanticTokens(document);
                if (embedded)
                    sources.push(embedded.build());

                return mergeSemanticTokens(sources);
            },
        },
        DCC_SEMANTIC_LEGEND,
    );

    context.subscriptions.push(semanticTokenProvider);
}
