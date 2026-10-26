#!/usr/bin/env bash
set -euo pipefail

msi="$(realpath "$1")"
version_out="$2"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mingw="${MINGW_SYSROOT:-/opt/llvm-mingw}"
work="$(mktemp -d)"
export WINEPREFIX="$work/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
wine wineboot --init >/dev/null 2>&1
wine msiexec /i "$msi" /qn
wineserver -w
inst="$WINEPREFIX/drive_c/Program Files/dcc"
(cd "$inst" && find . -type f | sort) > "$work/installed.txt"
for f in ./bin/dcc.exe ./bin/dccd.exe ./LICENSE ./lib/libdcext-windows-llvm.a ./lib/libdcext-windows-custom.a ./lib/libdcext-freestanding-llvm.a ./lib/libdcext-freestanding-custom.a ./include/std.dc; do
    grep -qxF "$f" "$work/installed.txt" || { echo "error: $f not installed" >&2; exit 1; }
done
echo "installed $(wc -l < "$work/installed.txt") files under C:\\Program Files\\dcc"
if grep -E '^\./lib/' "$work/installed.txt" | grep -vE '/libdcext-(windows|freestanding)-(llvm|custom)\.a$'; then
    echo "error: unexpected libraries installed" >&2
    exit 1
fi

timeout 60 wine "$inst/bin/dcc.exe" --version | tr -d '\r' | tee "$version_out"
cp "$here/smoke/hello.dc" "$work/"
cd "$work"
for backend in llvm custom; do
    timeout 60 wine "$inst/bin/dcc.exe" "-ffile-prefix-map=$(winepath -w "$work")=dcc-smoke" -flibdcext windows -target x86_64-coff -fbackend "$backend" -c -o "hello-$backend.obj" hello.dc
    timeout 60 "${DCC_HOST:-$here/../../build/bin/dcc}" -flibdcext windows -target x86_64-coff -fbackend "$backend" \
        -I "$inst/include" -L "$inst/lib" -L "$mingw/x86_64-w64-mingw32/lib" \
        "hello-$backend.obj" -lkernel32 -lws2_32 -ladvapi32 -lshell32 -o "hello-$backend.exe"
    set +e
    output="$(wine "./hello-$backend.exe" | tr -d '\r')"
    rc=$?
    set -e
    echo "$backend: output=$output exit=$rc"
    [[ "$output" == "hello from dcc" && $rc -eq 42 ]]
done
wineserver -k || true
echo "msi smoke ok"
