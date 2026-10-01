#!/usr/bin/env bash
source ci/lib/container.sh
cd vscode
ci_section npm-ci
npm ci --no-audit --no-fund
ci_section npm-test
npm test
ci_section validate
npm run validate
ci_section package
version="$(node -p 'require("./package.json").version')"
npx --no-install vsce package --no-update-package-json --out "/tmp/dcc-vscode-$version.vsix"
python3 ../ci/release/check_vsix.py "/tmp/dcc-vscode-$version.vsix" "$version"
