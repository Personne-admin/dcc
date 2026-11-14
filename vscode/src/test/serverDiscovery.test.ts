import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveServer, MSI_UPGRADE_CODE } from '../serverDiscovery';

test('MSI install is discovered with a stale PATH and spaces in Program Files', async () => {
    const server = 'C:\\Program Files\\dcc\\bin\\dccd.exe';
    const result = await resolveServer({
        platform: 'win32', env: { Path: 'C:\\Windows\\System32' },
        isExecutable: async path => path === server,
        installations: async () => [{
            productCode: '{12345678-1234-1234-1234-123456789ABC}',
            upgradeCode: MSI_UPGRADE_CODE, version: '0.6.0', serverPath: server,
        }],
    });
    assert.equal(result.command, server);
    assert.match(result.source, /MSI/);
});

function windows(files: string[], overrides: Partial<Parameters<typeof resolveServer>[0]> = {}) {
    return resolveServer({
        platform: 'win32', env: { Path: 'C:\\one;C:\\two', ProgramFiles: 'C:\\Program Files' },
        isExecutable: async path => files.some(file => file.toLowerCase() === path.toLowerCase()),
        installations: async () => [], ...overrides,
    });
}

const installed = (serverPath: string, version = '0.6.0') => ({
    productCode: '{12345678-1234-1234-1234-123456789ABC}', upgradeCode: MSI_UPGRADE_CODE, version, serverPath,
});

test('explicit override wins over compiler, PATH and MSI and remains unquoted', async () => {
    const file = 'D:\\Tools 安装\\language server.exe';
    const result = await windows([file, 'C:\\one\\dcc.exe', 'C:\\one\\dccd.exe'], {
        serverPath: file, installations: async () => { throw new Error('must not query MSI'); },
    });
    assert.equal(result.command, file);
    assert.match(result.source, /explicit/);
});

test('invalid, empty, quoted and non-exe explicit overrides never fall back', async () => {
    for (const serverPath of ['D:\\missing.exe', '', '"C:\\one\\dccd.exe"', 'C:\\one\\dccd.cmd'])
        await assert.rejects(windows(['C:\\one\\dccd.exe'], { serverPath }), /dcc.serverPath/);
});

test('explicit bare command searches PATH only and appends .exe', async () => {
    const result = await windows(['C:\\two\\custom.exe'], { serverPath: 'custom' });
    assert.equal(result.command, 'C:\\two\\custom.exe');
});

test('compiler setting selects matching server before other installations', async () => {
    const result = await windows(['D:\\custom\\dcc.exe', 'D:\\custom\\dccd.exe', 'C:\\one\\dccd.exe'], { compilerPath: 'D:\\custom\\dcc.exe' });
    assert.equal(result.command, 'D:\\custom\\dccd.exe');
    assert.match(result.source, /dcc.compilerPath/);
});

test('first PATH compiler sibling precedes an unrelated PATH server', async () => {
    const result = await windows(['C:\\one\\dccd.exe', 'C:\\two\\dcc.exe', 'C:\\two\\dccd.exe']);
    assert.equal(result.command, 'C:\\two\\dccd.exe');
    assert.match(result.source, /sibling/);
});

test('missing compiler sibling falls through to PATH server', async () => {
    const result = await windows(['C:\\one\\dcc.exe', 'C:\\two\\dccd.exe']);
    assert.equal(result.command, 'C:\\two\\dccd.exe');
    assert.match(result.source, /PATH dccd/);
});

test('PATH server precedes MSI without reading registry', async () => {
    assert.equal((await windows(['C:\\one\\dccd.exe'], { installations: async () => { throw new Error('should not query'); } })).source, 'PATH dccd');
});

test('custom per-user or machine MSI paths with Unicode do not need PATH changes', async () => {
    for (const path of ['D:\\工具\\My dcc\\bin\\dccd.exe', 'C:\\Users\\Ada\\AppData\\Local\\dcc\\bin\\dccd.exe']) {
        const result = await windows([path], { installations: async () => [installed(path)] });
        assert.equal(result.command, path);
        assert.match(result.source, /MSI/);
    }
});

test('MSI products are sorted newest first with deterministic ties', async () => {
    const a = installed('D:\\a\\bin\\dccd.exe', '0.6.0');
    const b = installed('D:\\b\\bin\\dccd.exe', '0.10.0');
    const c = installed('D:\\c\\bin\\dccd.exe', '0.10.0');
    for (const entries of [[a, c, b], [b, a, c]])
        assert.equal((await windows([a.serverPath, b.serverPath, c.serverPath], { installations: async () => entries })).command, b.serverPath);
});

test('stale MSI records and unrelated product identities cannot select a binary', async () => {
    const good = installed('D:\\dcc\\bin\\dccd.exe');
    const unrelated = { ...installed('D:\\other\\bin\\dccd.exe'), upgradeCode: 'unrelated' };
    const notServer = installed('D:\\dcc\\bin\\dcc.exe');
    const relative = installed('bin\\dccd.exe');
    const stale = installed('D:\\removed\\bin\\dccd.exe', '9.0.0');
    const result = await windows([good.serverPath, unrelated.serverPath, notServer.serverPath], { installations: async () => [unrelated, notServer, relative, stale, good] });
    assert.equal(result.command, good.serverPath);
});

test('known Program Files x86 default is only used after MSI discovery', async () => {
    let queried = false;
    const file = 'C:\\Program Files (x86)\\dcc\\bin\\dccd.exe';
    const result = await windows([file], {
        env: { 'ProgramFiles(x86)': 'C:\\Program Files (x86)' },
        installations: async () => { queried = true; return []; },
    });
    assert.ok(queried);
    assert.equal(result.source, 'known MSI installation default');
});

test('registry failure is reported concisely without exposing environment', async () => {
    await assert.rejects(windows([], { env: { SECRET: 'do-not-log' }, installations: async () => { throw new Error('MSI unavailable'); } }), error => {
        assert.match(String(error), /MSI unavailable/);
        assert.doesNotMatch(String(error), /do-not-log/);
        return true;
    });
});

test('missing dccd gives a configuration fix', async () => {
    await assert.rejects(windows([]), /set dcc.serverPath/);
});

test('quoted PATH directories with spaces are handled as single directories', async () => {
    const file = 'D:\\My tools\\dccd.exe';
    const result = await windows([file], { env: { PATH: '"D:\\My tools";C:\\Windows' } });
    assert.equal(result.command, file);
});

test('Linux executable and relative PATH behavior is preserved without querying MSI', async () => {
    const result = await resolveServer({
        platform: 'linux', env: { PATH: ':bin:/usr/bin' }, workspaceFolder: '/project',
        isExecutable: async file => file === '/project/bin/dccd',
        installations: async () => { throw new Error('must not query on Linux'); },
    });
    assert.equal(result.command, '/project/bin/dccd');
    const explicit = await resolveServer({ platform: 'linux', env: {}, serverPath: './server with spaces', workspaceFolder: '/project', isExecutable: async file => file === '/project/server with spaces' });
    assert.equal(explicit.command, '/project/server with spaces');
});

test('Linux PATH dccd retains priority over a discovered compiler sibling', async () => {
    const files = ['/one/dccd', '/two/dcc', '/two/dccd'];
    const result = await resolveServer({ platform: 'linux', env: { PATH: '/one:/two' }, isExecutable: async file => files.includes(file) });
    assert.equal(result.command, '/one/dccd');
});
