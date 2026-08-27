import * as vscode from 'vscode';
import { DccTestModelCache } from './dccTestCache';
import {
    dirCompletion,
    dirDefinition,
    dirDiagnostics,
    dirDocumentSymbols,
    dirFoldingRanges,
    dirHover,
    dirReferences,
    dirSemanticTokens,
    makeDirScope,
} from './dirFeatures';
import { DCC_SEMANTIC_LEGEND } from './semanticTokens';

export function registerDccDirProviders(context: vscode.ExtensionContext, cache: DccTestModelCache): void {
    const collection = vscode.languages.createDiagnosticCollection('dcc-dir');
    context.subscriptions.push(collection);

    const publishDiagnostics = (document: vscode.TextDocument): void => {
        if (document.languageId !== 'dcc-dir')
            return;

        const scope = makeDirScope(document, cache, 'file');
        if (!scope)
            return;

        collection.set(document.uri, dirDiagnostics(scope));
    };

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(publishDiagnostics),
        vscode.workspace.onDidChangeTextDocument((e) => publishDiagnostics(e.document)),
        vscode.workspace.onDidCloseTextDocument((doc) => collection.delete(doc.uri)),
    );

    for (const doc of vscode.workspace.textDocuments)
        publishDiagnostics(doc);

    context.subscriptions.push(
        vscode.languages.registerDocumentSymbolProvider('dcc-dir', {
            provideDocumentSymbols(document): vscode.DocumentSymbol[] {
                const scope = makeDirScope(document, cache, 'file');
                return scope ? dirDocumentSymbols(scope) : [];
            },
        }),
        vscode.languages.registerFoldingRangeProvider('dcc-dir', {
            provideFoldingRanges(document): vscode.FoldingRange[] {
                const scope = makeDirScope(document, cache, 'file');
                return scope ? dirFoldingRanges(scope) : [];
            },
        }),
        vscode.languages.registerCompletionItemProvider(
            'dcc-dir',
            {
                provideCompletionItems(document, position): vscode.CompletionItem[] {
                    const scope = makeDirScope(document, cache, 'file');
                    return scope ? dirCompletion(scope, position) : [];
                },
            },
            '%',
            '@',
            '#',
        ),
        vscode.languages.registerHoverProvider('dcc-dir', {
            provideHover(document, position): vscode.Hover | null {
                const scope = makeDirScope(document, cache, 'file');
                return scope ? dirHover(scope, position) : null;
            },
        }),
        vscode.languages.registerDefinitionProvider('dcc-dir', {
            provideDefinition(document, position): vscode.Definition | null {
                const scope = makeDirScope(document, cache, 'file');
                if (!scope) {
                    return null;
                }
                const locs = dirDefinition(scope, position);
                return locs.length > 0 ? locs : null;
            },
        }),
        vscode.languages.registerReferenceProvider('dcc-dir', {
            provideReferences(document, position): vscode.Location[] | null {
                const scope = makeDirScope(document, cache, 'file');
                if (!scope) {
                    return null;
                }
                const locs = dirReferences(scope, position);
                return locs.length > 0 ? locs : null;
            },
        }),
        vscode.languages.registerDocumentSemanticTokensProvider(
            'dcc-dir',
            {
                provideDocumentSemanticTokens(document): vscode.SemanticTokens {
                    const scope = makeDirScope(document, cache, 'file');
                    return scope ? dirSemanticTokens(scope) : new vscode.SemanticTokens(new Uint32Array(0));
                },
            },
            DCC_SEMANTIC_LEGEND,
        ),
    );
}
