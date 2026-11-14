import { test } from 'node:test';
import assert from 'node:assert/strict';
import { installationQuery, MSI_UPGRADE_CODE, windowsInstallations } from '../windowsInstallation';

test('MSI query uses trusted system PowerShell, UTF-16 encoded script and UTF-8 JSON', async () => {
    const path = 'D:\\安装 Files\\dcc\\bin\\dccd.exe';
    const entries = [{ productCode: '{12345678-1234-1234-1234-123456789ABC}', upgradeCode: MSI_UPGRADE_CODE, version: '0.6.0', serverPath: path }];
    const result = await windowsInstallations({
        env: { SystemRoot: 'C:\\Windows', PATH: 'D:\\untrusted' },
        run: async (command, args) => {
            assert.equal(command, 'C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe');
            assert.deepEqual(args.slice(0, 4), ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand']);
            assert.equal(Buffer.from(args[4], 'base64').toString('utf16le'), installationQuery);
            assert.match(installationQuery, /RelatedProducts/);
            assert.match(installationQuery, /c_bin_dccd_exe/);
            assert.doesNotMatch(installationQuery, /Win32_Product|ProvideComponent|InstallProduct|Uninstall\\/);
            return '\uFEFF' + JSON.stringify(entries);
        },
    });
    assert.deepEqual(result, entries);
});

test('malformed query results fail instead of accepting arbitrary data', async () => {
    for (const text of ['not JSON', '{}', 'null'])
        await assert.rejects(windowsInstallations({ env: { SystemRoot: 'C:\\Windows' }, run: async () => text }));
    assert.deepEqual(await windowsInstallations({ env: { SystemRoot: 'C:\\Windows' }, run: async () => '[{"serverPath": "bad"}]' }), []);
});

test('query cannot use a PATH-hijacked PowerShell when SystemRoot is absent', async () => {
    await assert.rejects(windowsInstallations({ env: {}, run: async () => { throw new Error('must not run'); } }), /SystemRoot/);
});

test('native Windows MSI query returns well-formed records without elevation', { skip: process.platform !== 'win32' ? 'requires native Windows Installer API' : false }, async () => {
    const result = await windowsInstallations({ env: process.env });
    for (const entry of result) assert.equal(entry.upgradeCode, MSI_UPGRADE_CODE);
});
