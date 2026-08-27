#!/usr/bin/env node
import { readFileSync, existsSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const pkgPath = resolve(root, 'package.json');

let failures = 0;
const fail = (msg) => {
    failures += 1;
    console.error(`  FAIL  ${msg}`);
};

const ok = (msg) => {
    console.log(`  ok    ${msg}`);
};

function readJson(path) {
    if (!existsSync(path)) {
        fail(`missing file: ${path}`);
        return null;
    }

    try {
        return JSON.parse(readFileSync(path, 'utf8'));
    } catch (e) {
        fail(`invalid JSON in ${path}: ${e.message}`);
        return null;
    }
}

const pkg = readJson(pkgPath);
if (!pkg)
    process.exit(1);

console.log(`validating ${pkg.name}@${pkg.version}`);

const languages = pkg.contributes?.languages ?? [];
const grammars = pkg.contributes?.grammars ?? [];
const activationEvents = pkg.activationEvents ?? [];
const languageIds = new Set(languages.map((l) => l.id));
const grammarScopeNames = new Set(grammars.map((g) => g.scopeName));

for (const lang of languages) {
    if (!activationEvents.includes(`onLanguage:${lang.id}`))
        fail(`language '${lang.id}' has no onLanguage activation event`);
    else
        ok(`activation event onLanguage:${lang.id}`);

}

for (const lang of languages) {
    if (lang.configuration) {
        const cfgPath = resolve(root, lang.configuration);
        const cfg = readJson(cfgPath);
        if (cfg)
            ok(`language configuration ${lang.configuration} (${lang.id})`);
    } else
        fail(`language '${lang.id}' has no configuration file`);
}

const grammarFiles = new Map();
for (const g of grammars) {
    if (!languageIds.has(g.language))
        fail(`grammar scope '${g.scopeName}' references unregistered language '${g.language}'`);
    else
        ok(`grammar ${g.scopeName} -> language ${g.language}`);

    const gPath = resolve(root, g.path);
    const parsed = readJson(gPath);
    if (parsed) {
        grammarFiles.set(gPath, parsed);
        if (parsed.scopeName !== g.scopeName)
            fail(`grammar file ${g.path} declares scopeName '${parsed.scopeName}' but contributes says '${g.scopeName}'`);

        const lang = languages.find((l) => l.id === g.language);
        if (lang && parsed.fileTypes) {
            const expected = (lang.extensions ?? []).map((e) => e.replace(/^\./, ''));
            const actual = parsed.fileTypes;
            for (const ft of actual)
                if (!expected.includes(ft))
                    fail(`grammar ${g.path} fileType '${ft}' not in language '${lang.id}' extensions`);

            ok(`grammar ${g.path} fileTypes match language ${g.language}`);
        }
    }
}

const dccTestGrammar = grammars.find((g) => g.scopeName === 'source.dcc-test');
if (dccTestGrammar) {
    const gPath = resolve(root, dccTestGrammar.path);

    const parsed = grammarFiles.get(gPath);
    if (parsed) {
        const text = JSON.stringify(parsed);
        for (const embedded of ['source.dc', 'source.dcc-dir'])
            if (text.includes(embedded) && !grammarScopeNames.has(embedded))
                fail(`dcc-test grammar embeds '${embedded}' but no grammar contributes that scope`);
            else if (text.includes(embedded))
                ok(`dcc-test grammar embeds ${embedded}`);
    }
}

if (grammarScopeNames.size !== grammars.length)
    fail('duplicate grammar scopeNames');
else
    ok('grammar scopeNames unique');


if (failures > 0) {
    console.error(`\nvalidation failed with ${failures} error(s)`);
    process.exit(1);
}

console.log('\nvalidation passed');
