#!/usr/bin/env bash
set -euo pipefail

deb="$1"
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
echo "== $PRETTY_NAME"
if grep -rqs 'apt.llvm.org' /etc/apt/sources.list /etc/apt/sources.list.d; then
    echo "error: apt.llvm.org is configured" >&2
    exit 1
fi
apt-get update -qq
apt-get install -y -qq --no-install-recommends "$deb" >/dev/null
dpkg -s dcc | grep -E '^(Version|Depends):'
for bin in dcc dccd dcdoc; do
    echo "-- ldd $bin"
    ldd "/usr/bin/$bin"
done
dcc --version

work="$(mktemp -d)"
cp /smoke/hello.dc "$work/"
cd "$work"
for backend in llvm custom; do
    dcc -flibdcext linux -fbackend "$backend" hello.dc -o "hello-$backend"
    set +e
    output="$("./hello-$backend")"
    rc=$?
    set -e
    echo "$backend: output=$output exit=$rc"
    [[ "$output" == "hello from dcc" && $rc -eq 42 ]]
done
echo "smoke ok on $PRETTY_NAME"
