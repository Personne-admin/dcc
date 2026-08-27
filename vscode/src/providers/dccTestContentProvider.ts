import * as vscode from 'vscode';
import { DccTestModelCache } from './dccTestCache';
import { containerUriFromVirtual, isVirtualUri, VIRTUAL_SCHEME, virtualPathFromUri } from '../model/virtualFs';

export function registerVirtualContentProvider(context: vscode.ExtensionContext, cache: DccTestModelCache): void {
    const provider: vscode.TextDocumentContentProvider = {
        provideTextDocumentContent(uri: vscode.Uri): string {
            const uriString = uri.toString();
            if (!isVirtualUri(uriString))
                return '';

            const containerUri = containerUriFromVirtual(uriString);
            const path = virtualPathFromUri(uriString);
            if (!containerUri || !path)
                return '';

            const model = cache.getForUri(containerUri);
            const vf = model?.fileSectionForPath(path);
            return vf ? vf.content : '';
        },
    };

    context.subscriptions.push(vscode.workspace.registerTextDocumentContentProvider(VIRTUAL_SCHEME, provider));
}
