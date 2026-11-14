import { execFile } from 'node:child_process';
import { win32 } from 'node:path';
import type { Installation } from './serverDiscovery';

export const MSI_UPGRADE_CODE = 'BC06D996-A013-4321-9967-7BCEDBBD011B';

export const installationQuery = `
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
function Property($object, $name, [object[]]$arguments) {
    $object.GetType().InvokeMember($name, [Reflection.BindingFlags]::GetProperty, $null, $object, $arguments)
}
function Method($object, $name, [object[]]$arguments) {
    $object.GetType().InvokeMember($name, [Reflection.BindingFlags]::InvokeMethod, $null, $object, $arguments)
}
$installer = New-Object -ComObject WindowsInstaller.Installer
$products = Property $installer 'RelatedProducts' @('{${MSI_UPGRADE_CODE}}')
$results = @()
foreach ($product in $products) {
    try {
        if ((Property $installer 'ProductState' @($product)) -ne 5) { continue }
        $version = Property $installer 'ProductInfo' @($product, 'VersionString')
        $location = $null
        try { $location = Property $installer 'ProductInfo' @($product, 'InstallLocation') } catch { }
        $server = $null
        if ($location) { $server = [IO.Path]::Combine($location, 'bin', 'dccd.exe') }
        if (-not $server -or -not [IO.File]::Exists($server)) {
            $package = Property $installer 'ProductInfo' @($product, 'LocalPackage')
            $database = Method $installer 'OpenDatabase' @($package, 0)
            $view = Method $database 'OpenView' @('SELECT \`ComponentId\` FROM \`Component\` WHERE \`Component\` = ''c_bin_dccd_exe''')
            Method $view 'Execute' @() | Out-Null
            $record = Method $view 'Fetch' @()
            if ($record) {
                $component = Property $record 'StringData' @(1)
                $server = Property $installer 'ComponentPath' @($product, $component)
            }
            Method $view 'Close' @() | Out-Null
        }
        if ($server) {
            $results += @{ productCode = $product; upgradeCode = '${MSI_UPGRADE_CODE}'; version = $version; serverPath = $server }
        }
    } catch { }
}
ConvertTo-Json -InputObject @($results) -Compress
`;

export interface QueryOptions {
    env: NodeJS.ProcessEnv;
    run?: (command: string, args: string[]) => Promise<string>;
}

export async function windowsInstallations(options: QueryOptions): Promise<Installation[]> {
    const systemRoot = Object.entries(options.env).find(([key]) => key.toLowerCase() === 'systemroot')?.[1];
    if (!systemRoot || !win32.isAbsolute(systemRoot))
        throw new Error('Windows SystemRoot is unavailable; MSI lookup cannot run.');
    const powershell = win32.join(systemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
    const args = ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(installationQuery, 'utf16le').toString('base64')];
    const run = options.run ?? ((command, arguments_) => new Promise<string>((resolve, reject) => {
        execFile(command, arguments_, { encoding: 'utf8', timeout: 5000, maxBuffer: 1024 * 1024, windowsHide: true, shell: false }, (error, stdout) => {
            if (error)
                reject(new Error(`Windows Installer query failed (${error.code ?? 'process error'}).`));
            else
                resolve(stdout);
        });
    }));
    const value: unknown = JSON.parse((await run(powershell, args)).replace(/^\uFEFF/, ''));
    if (!Array.isArray(value))
        throw new Error('Windows Installer query returned an invalid result.');
    return value.filter((entry): entry is Installation => !!entry && typeof entry === 'object'
        && typeof entry.productCode === 'string' && typeof entry.upgradeCode === 'string'
        && typeof entry.version === 'string' && typeof entry.serverPath === 'string');
}
