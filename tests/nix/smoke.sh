#!/usr/bin/env bash
set -euo pipefail
prefix="$(realpath "$1")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export PATH="$prefix/bin:/usr/bin:/bin" HOME="$work"
for tool in dcc dccd dpm learndc dcdoc; do test -x "$prefix/bin/$tool"; done
dcc --version
dpm --version
learndc --version
learndc list --json > "$work/list.json"
grep -q fundamentals/hello-world "$work/list.json"
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$work"
for backend in llvm custom; do
    timeout 60 dcc -flibdcext linux -fbackend "$backend" "$root/ci/release/smoke/hello.dc" -o "hello-$backend"
    set +e
    output="$("./hello-$backend")"
    code=$?
    set -e
    [[ "$output" == 'hello from dcc' && "$code" == 42 ]]
done
