import { test } from 'node:test';
import assert from 'node:assert/strict';
import Module from 'node:module';

test('activation wires discovery, diagnostics, restart and startup failure phases', async () => {
    const commands = new Map<string, () => unknown>();
    const lines: string[] = [];
    const notices: string[] = [];
    const resolutions: { serverPath?: string }[] = [];
    const launches: string[] = [];
    let override: string | undefined;
    let launchFailure = false;
    let protocolFailure = false;
    let kills = 0;
    const noop: any = new Proxy(function () {}, {
        get: (_target, key) => key === Symbol.iterator ? [][Symbol.iterator].bind([]) : noop,
        apply: () => noop, construct: () => noop,
    });
    const api: any = new Proxy({
        workspace: new Proxy({
            workspaceFolders: [], textDocuments: [],
            getConfiguration: () => ({
                inspect: () => ({ defaultValue: 'dccd', globalValue: override }),
                get: (name: string, fallback?: unknown) => name === 'serverPath' ? override ?? 'dccd' : fallback,
            }),
        }, { get: (target, key) => (target as any)[key] ?? noop }),
        window: new Proxy({
            createOutputChannel: () => ({ appendLine: (line: string) => lines.push(line), show: () => {}, dispose: () => {} }),
            showErrorMessage: async (message: string) => { notices.push(message); },
        }, { get: (target, key) => (target as any)[key] ?? noop }),
        commands: {
            registerCommand: (name: string, callback: () => unknown) => { commands.set(name, callback); return noop; },
            executeCommand: () => Promise.resolve(),
        },
    }, { get: (target, key) => (target as any)[key] ?? noop });
    class Client {
        private listener: (event: { newState: number }) => void = () => {};
        constructor(_id: string, _name: string, private server: () => Promise<unknown>) {}
        onDidChangeState(listener: typeof this.listener) { this.listener = listener; return noop; }
        async start() {
            await this.server();
            if (protocolFailure) throw new Error('Invalid LSP initialize response');
            this.listener({ newState: 2 });
        }
        async stop() { this.listener({ newState: 1 }); }
        async restart() { await this.stop(); await this.start(); }
        isRunning() { return true; }
        dispose() {}
    }
    const loader = Module as unknown as { _load: (name: string, parent: unknown, main: boolean) => unknown };
    const original = loader._load;
    loader._load = (name, parent, main) => {
        if (name === 'vscode') return api;
        if (name === 'vscode-languageclient/node') return { LanguageClient: Client, State: { Running: 2, Stopped: 1 } };
        if (name === './serverDiscovery') return {
            resolveServer: async (options: { serverPath?: string }) => {
                resolutions.push(options);
                if (options.serverPath === 'missing') throw new Error('Configured dcc.serverPath is missing');
                return { command: options.serverPath ?? 'C:\\Program Files\\dcc\\bin\\dccd.exe', source: 'test MSI', notes: [] };
            },
        };
        if (name === './serverLaunch') return {
            launchServer: async (path: string, _cwd: unknown, report: (message: string) => void) => {
                launches.push(path);
                if (launchFailure) { report('Launch failed: ENOENT'); throw new Error('ENOENT'); }
                report('Process started; waiting for LSP initialize');
                return { kill: () => { kills++; } };
            },
        };
        return original(name, parent, main);
    };
    let extension: typeof import('../extension');
    try { extension = require('../extension'); }
    finally { loader._load = original; }
    const context = { subscriptions: [] } as unknown as import('vscode').ExtensionContext;
    const settle = () => new Promise(resolve => setImmediate(resolve));
    extension.activate(context);
    await settle();
    assert.equal(resolutions[0].serverPath, undefined, 'manifest default must not disable MSI discovery');
    assert.equal(launches[0], 'C:\\Program Files\\dcc\\bin\\dccd.exe');
    commands.get('dcc.showServerDiagnostics')!();
    assert.ok(lines.includes('Server status: Running; LSP initialize succeeded'));
    override = 'D:\\安装 tools\\dccd.exe';
    await commands.get('dcc.restartServer')!();
    assert.equal(launches.at(-1), override);
    const count = launches.length;
    override = 'missing';
    await commands.get('dcc.restartServer')!();
    commands.get('dcc.showServerDiagnostics')!();
    assert.equal(launches.length, count, 'invalid override must not launch a fallback');
    assert.ok(lines.some(line => line.includes('Configured dcc.serverPath is missing')));
    assert.ok(notices.some(notice => notice.includes('dcc.serverPath')));
    override = undefined;
    launchFailure = true;
    await commands.get('dcc.restartServer')!();
    assert.ok(lines.some(line => line.includes('Startup failed (Launch failed: ENOENT)')));
    launchFailure = false;
    protocolFailure = true;
    await commands.get('dcc.restartServer')!();
    assert.ok(lines.some(line => line.includes('Invalid LSP initialize response')));
    assert.ok(lines.some(line => line.includes('Startup failed (Process started; waiting for LSP initialize)')));
    await extension.deactivate();
    assert.ok(kills > 0, 'owned server processes must be cleaned up');
});
