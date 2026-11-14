import * as vscode from 'vscode';
import { ChildProcess } from 'node:child_process';
import { promises as fs } from 'node:fs';
import { resolveServer, ResolvedServer } from './serverDiscovery';
import { launchServer } from './serverLaunch';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    State,
} from 'vscode-languageclient/node';
import { DccCoreFileSystemProvider } from './dccCoreFileSystemProvider';
import { DccTestModelCache } from './providers/dccTestCache';
import { EmbeddedDcBridge } from './providers/embeddedDcBridge';
import { registerDccTestProviders } from './providers/dccTestProviders';
import { registerDccDirProviders } from './providers/dccDirProviders';
import { registerVirtualContentProvider } from './providers/dccTestContentProvider';

let client: LanguageClient | undefined;
let serverProcess: ChildProcess | undefined;

class ServerStartCancelled extends Error {}

export function activate(context: vscode.ExtensionContext): void {
    const cache = new DccTestModelCache();
    context.subscriptions.push(cache);

    const bridge = new EmbeddedDcBridge(cache);
    context.subscriptions.push(bridge);

    const output = vscode.window.createOutputChannel('DCC Language Server');
    context.subscriptions.push(output);
    let selected: ResolvedServer | undefined;
    let status = 'Not started';
    let failure = '';
    let generation = 0;
    let disposed = false;
    context.subscriptions.push({ dispose: () => { disposed = true; serverProcess?.kill(); } });

    const showDiagnostics = () => {
        output.appendLine(`Selected dccd: ${selected?.command ?? '(none)'}`);
        output.appendLine(`Discovery source: ${selected?.source ?? '(unresolved)'}`);
        output.appendLine(`Server status: ${status}`);
        if (failure) output.appendLine(`Failure: ${failure}`);
        for (const note of selected?.notes ?? []) output.appendLine(note);
        output.appendLine('Set dcc.serverPath to an unquoted executable path, then run DCC: Restart Language Server.');
        output.show(true);
    };
    const reportFailure = (error: unknown) => {
        if (disposed || error instanceof ServerStartCancelled) return;
        failure = error instanceof Error ? error.message.slice(0, 600) : 'Unknown server error';
        output.appendLine(`Startup failed (${status}): ${failure}`);
        serverProcess?.kill();
        void vscode.window.showErrorMessage('DCC language server could not start. Check dcc.serverPath or open server diagnostics.',
            'Show Diagnostics', 'Settings').then(action => {
                if (action === 'Show Diagnostics') showDiagnostics();
                if (action === 'Settings') void vscode.commands.executeCommand('workbench.action.openSettings', 'dcc.serverPath');
            });
    };
    const serverOptions: ServerOptions = async () => {
        const current = ++generation;
        serverProcess?.kill();
        serverProcess = undefined;
        selected = undefined;
        failure = '';
        status = 'Resolving executable';
        try {
            const configuration = vscode.workspace.getConfiguration('dcc');
            const setting = configuration.inspect<string>('serverPath');
            const overridden = setting && [setting.globalValue, setting.workspaceValue, setting.workspaceFolderValue,
                setting.globalLanguageValue, setting.workspaceLanguageValue, setting.workspaceFolderLanguageValue].some(value => value !== undefined);
            const folder = vscode.workspace.workspaceFolders?.[0]?.uri;
            let workspaceFolder: string | undefined;
            if (folder?.scheme === 'file') {
                try {
                    if ((await fs.stat(folder.fsPath)).isDirectory()) workspaceFolder = folder.fsPath;
                } catch { }
            }
            const candidate = await resolveServer({
                platform: process.platform, env: process.env,
                serverPath: overridden ? configuration.get<string>('serverPath', '') : undefined,
                compilerPath: configuration.get<string>('compilerPath'), workspaceFolder,
            });
            if (disposed || current !== generation) throw new ServerStartCancelled();
            selected = candidate;
            output.appendLine(`Selected dccd: ${selected.command} (${selected.source})`);
            status = 'Launching executable';
            const child = await launchServer(selected.command, workspaceFolder, message => {
                if (current !== generation) return;
                status = message;
                output.appendLine(message);
            });
            if (disposed || current !== generation) { child.kill(); throw new ServerStartCancelled(); }
            serverProcess = child;
            return { process: child, detached: false };
        } catch (error) {
            if (!disposed && current === generation && !(error instanceof ServerStartCancelled)) {
                failure = error instanceof Error ? error.message.slice(0, 600) : 'Unknown server error';
                output.appendLine(`Startup failed (${status}): ${failure}`);
            }
            throw error;
        }
    };

    const watchers: vscode.FileSystemWatcher[] = [
        vscode.workspace.createFileSystemWatcher('**/dcc.json'),
        vscode.workspace.createFileSystemWatcher('**/compile_commands.json'),
        vscode.workspace.createFileSystemWatcher('**/project.lock.dcon'),
    ];
    context.subscriptions.push(...watchers);

    const clientOptions: LanguageClientOptions = {
        outputChannelName: 'DCC Language Server',
        outputChannel: output,
        documentSelector: [
            { scheme: 'file', language: 'dc' },
            { scheme: 'file', language: 'dcon' },
        ],
        synchronize: {
            configurationSection: 'dcc',
            fileEvents: watchers,
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

    context.subscriptions.push(client.onDidChangeState(event => {
        if (event.newState === State.Running) {
            status = 'Running; LSP initialize succeeded';
            failure = '';
            output.appendLine(status);
        } else if (event.newState === State.Stopped) {
            serverProcess?.kill();
        }
    }));
    context.subscriptions.push(vscode.commands.registerCommand('dcc.showServerDiagnostics', showDiagnostics));
    context.subscriptions.push(vscode.commands.registerCommand('dcc.restartServer', () => client?.restart().catch(reportFailure)));
    client.start().catch(reportFailure);

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

    return client.stop().finally(() => { serverProcess?.kill(); serverProcess = undefined; });
}
