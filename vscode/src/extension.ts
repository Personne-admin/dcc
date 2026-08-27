import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Executable,
} from 'vscode-languageclient/node';
import { DccCoreFileSystemProvider } from './dccCoreFileSystemProvider';
import { DccTestModelCache } from './providers/dccTestCache';
import { EmbeddedDcBridge } from './providers/embeddedDcBridge';
import { registerDccTestProviders } from './providers/dccTestProviders';
import { registerDccDirProviders } from './providers/dccDirProviders';
import { registerVirtualContentProvider } from './providers/dccTestContentProvider';

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext): void {
    const cache = new DccTestModelCache();
    context.subscriptions.push(cache);

    const bridge = new EmbeddedDcBridge(cache);
    context.subscriptions.push(bridge);

    const serverPath: string = vscode.workspace
        .getConfiguration('dcc')
        .get<string>('serverPath', 'dccd');

    const run: Executable = {
        command: serverPath,
        args: [],
    };

    const debug: Executable = {
        command: serverPath,
        args: [],
    };

    const serverOptions: ServerOptions = {
        run,
        debug,
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'dc' },
        ],
        synchronize: {
            configurationSection: 'dcc',
        },
        middleware: {
            handleDiagnostics: (uri, diagnostics, next) => {
                if (bridge.isVirtualUri(uri.toString())) {
                    bridge.handleServerDiagnostics(uri.toString(), diagnostics);
                    return;
                }

                next(uri, diagnostics);
            },
        },
    };

    client = new LanguageClient(
        'dccd',
        'DCC Language Server',
        serverOptions,
        clientOptions,
    );

    bridge.attachClient(client);
    context.subscriptions.push(client);

    client.start().catch((err: unknown) => {
        console.error('[dcc-vscode] failed to start dccd language client:', err);
    });

    const dccCoreProvider = new DccCoreFileSystemProvider(client);
    const dccCoreFs = vscode.workspace.registerFileSystemProvider(
        'dcc-core',
        dccCoreProvider,
        { isReadonly: true, isCaseSensitive: true },
    );
    context.subscriptions.push(dccCoreFs);

    registerVirtualContentProvider(context, cache);
    registerDccTestProviders(context, cache, bridge);
    registerDccDirProviders(context, cache);
}

export function deactivate(): Thenable<void> | undefined {
    if (!client)
        return undefined;

    return client.stop();
}
