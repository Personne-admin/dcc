import { constants, promises as fs } from 'node:fs';
import { posix, win32 } from 'node:path';
import { MSI_UPGRADE_CODE, windowsInstallations } from './windowsInstallation';
export { MSI_UPGRADE_CODE } from './windowsInstallation';

export interface ServerResolutionOptions {
    platform: NodeJS.Platform;
    env: NodeJS.ProcessEnv;
    serverPath?: string;
    compilerPath?: string;
    workspaceFolder?: string;
    isExecutable?: (path: string) => Promise<boolean>;
    installations?: () => Promise<Installation[]>;
}

export interface Installation {
    productCode: string;
    upgradeCode: string;
    version: string;
    serverPath: string;
}

export interface ResolvedServer {
    command: string;
    source: string;
    notes: string[];
}

export async function resolveServer(options: ServerResolutionOptions): Promise<ResolvedServer> {
    const windows = options.platform === 'win32';
    const paths = windows ? win32 : posix;
    const env = (name: string): string | undefined => windows
        ? Object.entries(options.env).find(([key]) => key.toLowerCase() === name.toLowerCase())?.[1]
        : options.env[name];
    const notes: string[] = [];
    const executable = options.isExecutable ?? (async (file: string) => {
        try {
            await fs.access(file, windows ? constants.F_OK : constants.X_OK);
            return (await fs.stat(file)).isFile();
        } catch { return false; }
    });
    const check = async (file: string): Promise<string | undefined> => {
        if (windows && !paths.extname(file)) file += '.exe';
        if (windows && paths.extname(file).toLowerCase() !== '.exe') return undefined;
        return await executable(file) ? file : undefined;
    };
    const absolute = (file: string): string => paths.resolve(options.workspaceFolder ?? process.cwd(), file);
    const inPath = async (name: string): Promise<string | undefined> => {
        const search = env('PATH') ?? (windows ? '' : '/usr/bin:/bin');
        for (let directory of search.split(windows ? ';' : ':')) {
            if (windows && directory.startsWith('"') && directory.endsWith('"')) directory = directory.slice(1, -1);
            if (windows && !paths.isAbsolute(directory)) continue;
            const hit = await check(paths.join(absolute(directory || '.'), name));
            if (hit) return hit;
        }
        return undefined;
    };
    const configured = async (file: string): Promise<string | undefined> =>
        paths.isAbsolute(file) || /[\\/]/.test(file) ? check(absolute(file)) : inPath(file);
    const result = (command: string, source: string): ResolvedServer => ({ command, source, notes });
    if (options.serverPath !== undefined) {
        const hit = typeof options.serverPath === 'string' && options.serverPath.length ? await configured(options.serverPath) : undefined;
        if (!hit) throw new Error('Configured dcc.serverPath is missing or not executable. Set it to the unquoted executable path, or remove the override to enable discovery.');
        return result(hit, 'explicit dcc.serverPath');
    }
    const compiler = options.compilerPath ? await configured(options.compilerPath) : windows ? await inPath('dcc') : undefined;
    if (compiler) {
        const sibling = await check(paths.join(paths.dirname(compiler), windows ? 'dccd.exe' : 'dccd'));
        if (sibling) return result(sibling, options.compilerPath ? 'sibling of dcc.compilerPath' : 'sibling of PATH dcc');
        notes.push('Selected compiler has no executable dccd sibling.');
    } else if (options.compilerPath) {
        notes.push('Configured dcc.compilerPath was not found.');
    }
    const pathServer = await inPath('dccd');
    if (pathServer) return result(pathServer, 'PATH dccd');
    notes.push('dccd was not found on the extension host PATH.');
    if (windows) {
        try {
            const installations = await (options.installations ?? (() => windowsInstallations({ env: options.env })))();
            installations.sort((a, b) => {
                const av = a.version.split('.').map(Number), bv = b.version.split('.').map(Number);
                for (let i = 0; i < 3; i++) {
                    const difference = (bv[i] || 0) - (av[i] || 0);
                    if (difference) return difference;
                }
                return a.serverPath < b.serverPath ? -1 : a.serverPath > b.serverPath ? 1 : 0;
            });
            for (const installation of installations) {
                if (installation.upgradeCode.toUpperCase() !== MSI_UPGRADE_CODE
                    || !/^\{[0-9a-f-]{36}\}$/i.test(installation.productCode)
                    || !paths.isAbsolute(installation.serverPath)
                    || paths.basename(installation.serverPath).toLowerCase() !== 'dccd.exe') continue;
                const hit = await check(installation.serverPath);
                if (hit) return result(hit, `dcc MSI ${installation.version} (${installation.productCode})`);
            }
            notes.push('No usable dccd was found in registered dcc MSI products.');
        } catch (error) {
            notes.push(error instanceof Error ? error.message : 'MSI discovery was unavailable.');
        }
        const roots = new Set(['ProgramW6432', 'ProgramFiles', 'ProgramFiles(x86)'].map(env).filter((root): root is string => !!root && paths.isAbsolute(root)));
        for (const root of roots) {
            const hit = await check(paths.join(root, 'dcc', 'bin', 'dccd.exe'));
            if (hit) return result(hit, 'known MSI installation default');
        }
    }
    throw new Error([...notes, 'Cannot find dccd. Install dcc on the extension host or set dcc.serverPath to its executable.'].join(' '));
}
