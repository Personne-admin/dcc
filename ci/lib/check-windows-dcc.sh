#!/usr/bin/env bash
set -euo pipefail

dcc="$(realpath "$1")"
wine="${WINE:-wine}"
export WINEDEBUG="${WINEDEBUG:--all}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

version="$("$wine" "$dcc" --version | tr -d '\r')"
echo "$version"
[[ "$version" == "dcc ${EXPECT_VERSION:-}"* ]]

printf 'module smoke;\n@nomangle public i32 dcc_main() { return 7; }\n' > smoke.dc
for backend in llvm custom; do
    "$wine" "$dcc" -fbackend "$backend" -target x86_64-coff -c -o "smoke-$backend.obj" smoke.dc
    test -s "smoke-$backend.obj"
    head -c 2 "smoke-$backend.obj" | od -An -tx1 | grep -q '64 86'
    echo "dcc.exe -fbackend $backend: ok"
done
