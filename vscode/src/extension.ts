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
let stopServer: (() => Promise<void>) | undefined;

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
    let stateSubscription: vscode.Disposable | undefined;
    let coreFs: vscode.Disposable | undefined;

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
        const reason = error instanceof Error ? error.message.slice(0, 600) : 'Unknown server error';
        if (failure !== reason) output.appendLine(`Startup failed (${status}): ${reason}`);
        failure = reason;
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
        vscode.workspace.createFileSystemWatcher('**/project.dcon'),
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

    const createClient = () => {
        const next = new LanguageClient('dccd', 'DCC Language Server', serverOptions, clientOptions);
        bridge.attachClient(next);
        stateSubscription = next.onDidChangeState(event => {
            if (client !== next) return;
            if (event.newState === State.Running) {
                status = 'Running; LSP initialize succeeded';
                failure = '';
                output.appendLine(status);
            } else if (event.newState === State.Stopped) {
                serverProcess?.kill();
            }
        });
        coreFs = vscode.workspace.registerFileSystemProvider('dcc-core', new DccCoreFileSystemProvider(next),
            { isReadonly: true, isCaseSensitive: true });
        return next;
    };
    client = createClient();
    const initialClient = client;
    initialClient.start().catch(error => { if (client === initialClient) reportFailure(error); });
    let retry: Promise<void> | undefined;
    context.subscriptions.push(vscode.commands.registerCommand('dcc.showServerDiagnostics', showDiagnostics));
    context.subscriptions.push(vscode.commands.registerCommand('dcc.restartServer', () => {
        if (retry) return retry;
        retry = (async () => {
            if (disposed) return;
            if (client?.isRunning()) {
                await client.restart();
            } else {
                const previous = client;
                client = undefined;
                ++generation;
                stateSubscription?.dispose();
                coreFs?.dispose();
                serverProcess?.kill();
                serverProcess = undefined;
                // Client v9 cannot stop a failed/starting client; dispose still marks it disposed.
                await previous?.dispose().catch(() => {});
                previous?.diagnostics?.dispose();
                if (disposed) return;
                client = createClient();
                await client.start();
            }
        })().catch(reportFailure).finally(() => { retry = undefined; });
        return retry;
    }));
    stopServer = async () => {
        if (disposed) return;
        disposed = true;
        ++generation;
        stateSubscription?.dispose();
        coreFs?.dispose();
        try {
            if (client?.isRunning()) await client.stop();
        } finally {
            await client?.dispose().catch(() => {});
            client?.diagnostics?.dispose();
            serverProcess?.kill();
            serverProcess = undefined;
        }
    };
    const stop = stopServer;
    context.subscriptions.push({ dispose: () => { void stop().catch(() => {}); } });

    registerVirtualContentProvider(context, cache);
    registerDccTestProviders(context, cache, bridge);
    registerDccDirProviders(context, cache);
}

export function deactivate(): Thenable<void> | undefined {
    return stopServer?.();
}
