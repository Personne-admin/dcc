import { ChildProcess, spawn } from 'node:child_process';

export function launchServer(command: string, cwd?: string, report: (status: string) => void = () => {}): Promise<ChildProcess> {
    return new Promise((resolve, reject) => {
        const child = spawn(command, [], { cwd, shell: false, windowsHide: true, stdio: 'pipe' });
        child.once('spawn', () => {
            report(`Process started (PID ${child.pid}); waiting for LSP initialize.`);
            resolve(child);
        });
        child.on('error', (error: NodeJS.ErrnoException) => {
            report(`Launch failed: ${error.code ?? error.message}`);
            reject(error);
        });
        child.once('exit', (code, signal) => {
            report(exitStatus(code, signal));
        });
    });
}

export function exitStatus(code: number | null, signal: NodeJS.Signals | null): string {
    const dependencyHint = code !== null && (code >>> 0) === 0xc0000135 ? ' A required Windows DLL is missing.' : '';
    return `Process exited (${signal ? `signal ${signal}` : `code ${code}`}).${dependencyHint}`;
}
