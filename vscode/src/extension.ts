import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Executable,
} from 'vscode-languageclient/node';
import { DccCoreFileSystemProvider } from './dccCoreFileSystemProvider';

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext): void {
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
    };

    client = new LanguageClient(
        'dccd',
        'DCC Language Server',
        serverOptions,
        clientOptions
    );

    client.start();

    context.subscriptions.push(client);

    const dccCoreProvider = new DccCoreFileSystemProvider(client);
    const dccCoreFs = vscode.workspace.registerFileSystemProvider(
        'dcc-core',
        dccCoreProvider,
        { isReadonly: true, isCaseSensitive: true },
    );
    context.subscriptions.push(dccCoreFs);
}

export function deactivate(): Thenable<void> | undefined {
    if (!client)
        return undefined;

    return client.stop();
}
