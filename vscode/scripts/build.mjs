import { build } from 'esbuild';
import { copyFileSync, readFileSync, writeFileSync, chmodSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const production = process.argv.includes('--production');
const result = await build({
    entryPoints: ['src/extension.ts'],
    bundle: true,
    outfile: 'out/extension.js',
    external: ['vscode'],
    format: 'cjs',
    platform: 'node',
    target: 'node18',
    minify: production,
    sourcemap: !production,
    sourcesContent: false,
    metafile: true,
});

const clientRoot = dirname(fileURLToPath(import.meta.resolve('vscode-languageclient/package.json')));
copyFileSync(join(clientRoot, 'lib/node/terminateProcess.sh'), 'out/terminateProcess.sh');
chmodSync('out/terminateProcess.sh', 0o755);

const packages = new Set();
for (const input of Object.keys(result.metafile.inputs)) {
    const match = input.match(/^node_modules\/((?:@[^/]+\/)?[^/]+)\//);
    if (match)
        packages.add(match[1]);
}
const notices = [];
for (const name of [...packages].sort()) {
    const root = join('node_modules', name);
    const license = ['License.txt', 'LICENSE', 'LICENSE.md', 'LICENSE.txt'].find(file => existsSync(join(root, file)));
    if (!license)
        throw new Error(`Missing bundled dependency license: ${name}`);
    notices.push(`${name}\n\n${readFileSync(join(root, license), 'utf8')}`);
    if (existsSync(join(root, 'thirdpartynotices.txt')))
        notices.push(readFileSync(join(root, 'thirdpartynotices.txt'), 'utf8'));
}
writeFileSync('out/THIRD_PARTY_NOTICES.txt', notices.join('\n\n--------------------\n\n'));
