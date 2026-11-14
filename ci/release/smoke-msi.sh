#!/usr/bin/env bash
set -euo pipefail

msi="$(realpath "$1")"
version_out="$2"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
export WINEPREFIX="$work/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
wine wineboot --init >/dev/null 2>&1
wine msiexec /i "$msi" /qn
wineserver -w
inst="$WINEPREFIX/drive_c/Program Files/dcc"
(cd "$inst" && find . -type f | sort) > "$work/installed.txt"
for f in ./bin/dcc.exe ./bin/dccd.exe ./bin/dpm.exe ./bin/ld.lld.exe ./LICENSE ./lib/libdcext-windows-llvm.a ./lib/libdcext-windows-custom.a ./lib/libdcext-freestanding-llvm.a ./lib/libdcext-freestanding-custom.a ./include/std.dc; do
    grep -qxF "$f" "$work/installed.txt" || { echo "error: $f not installed" >&2; exit 1; }
done
echo "installed $(wc -l < "$work/installed.txt") files under C:\\Program Files\\dcc"
if grep -E '^\./lib/' "$work/installed.txt" | grep -vE '/(libdcext-(windows|freestanding)-(llvm|custom)|lib(kernel32|ws2_32|advapi32|shell32))\.a$'; then
    echo "error: unexpected libraries installed" >&2
    exit 1
fi

timeout 60 wine "$inst/bin/dcc.exe" --version | tr -d '\r' | tee "$version_out"
timeout 60 wine "$inst/bin/dpm.exe" --version
python3 "$here/../../tests/lsp_stdio.py" "$inst/bin/dccd.exe" --wine wine
cp "$here/smoke/hello.dc" "$work/"
cd "$work"
for backend in llvm custom; do
    timeout 60 wine "$inst/bin/dcc.exe" "-ffile-prefix-map=$(winepath -w "$work")=dcc-smoke" -flibdcext windows -target x86_64-coff -fbackend "$backend" -c -o "hello-$backend.obj" hello.dc
    timeout 60 wine "$inst/bin/dcc.exe" -flibdcext windows -target x86_64-coff -fbackend "$backend" \
        "hello-$backend.obj" -o "hello-$backend.exe"
    set +e
    output="$(wine "./hello-$backend.exe" | tr -d '\r')"
    rc=$?
    set -e
    echo "$backend: output=$output exit=$rc"
    [[ "$output" == "hello from dcc" && $rc -eq 42 ]]
done
wineserver -k || true
echo "msi smoke ok"
