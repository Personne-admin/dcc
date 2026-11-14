import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, rmSync, symlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { once } from 'node:events';
import { createMessageConnection, StreamMessageReader, StreamMessageWriter } from 'vscode-jsonrpc/node';
import { launchServer, exitStatus } from '../serverLaunch';

test('launch errors are distinct from discovery and initialize errors', async () => {
    const status: string[] = [];
    await assert.rejects(launchServer(join(tmpdir(), 'dcc-missing-executable', 'dccd'), undefined, value => status.push(value)), /ENOENT/);
    assert.ok(status.some(value => value.includes('Launch failed: ENOENT')));
});

const binary = process.env.DCC_TEST_DCCD ?? resolve(process.cwd(), '../build/bin/dccd');
test('real dccd launches directly from a Unicode spaced path and completes LSP initialize', {
    skip: process.platform === 'win32' || !existsSync(binary), timeout: 10000,
}, async t => {
    const directory = mkdtempSync(join(tmpdir(), 'dcc Program Files 安装 '));
    const executable = join(directory, 'dccd;no-shell');
    symlinkSync(binary, executable);
    t.after(() => rmSync(directory, { recursive: true, force: true }));
    const statuses: string[] = [];
    const child = await launchServer(executable, directory, value => statuses.push(value));
    t.after(() => child.kill());
    child.stderr!.resume();
    const exited = once(child, 'exit');
    const connection = createMessageConnection(new StreamMessageReader(child.stdout!), new StreamMessageWriter(child.stdin!));
    t.after(() => connection.dispose());
    connection.listen();
    const result = await connection.sendRequest<{ capabilities: object }>('initialize', {
        processId: process.pid, rootUri: pathToFileURL(directory).toString(), capabilities: {},
    });
    assert.ok(result.capabilities);
    await connection.sendNotification('initialized', {});
    await connection.sendRequest('shutdown', null);
    await connection.sendNotification('exit', null);
    assert.equal((await exited)[0], 0);
    assert.ok(statuses.some(value => value.startsWith('Process started')));
});

test('Windows missing-DLL exits have an actionable launch diagnostic', () => {
    for (const code of [0xc0000135, -1073741515])
        assert.match(exitStatus(code, null), /required Windows DLL is missing/);
    assert.equal(exitStatus(null, 'SIGTERM'), 'Process exited (signal SIGTERM).');
    assert.equal(exitStatus(1, null), 'Process exited (code 1).');
});
