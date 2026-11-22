#!/usr/bin/env bash
set -euo pipefail

msi="$(realpath "$1")"
version_out="$2"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
export WINEPREFIX="$work/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
wine wineboot --init >/dev/null 2>&1
msiinfo export "$msi" Registry | tr -d '\r' > "$work/registry.idt"
python3 - "$work/registry.idt" <<'PYREG'
import csv
import sys
with open(sys.argv[1], newline='') as f:
    rows = list(csv.DictReader(f, delimiter='\t'))[2:]
expected = {'c_path_entry', 'c_installer_options', 'c_start_menu'}
assert {r['Component_'] for r in rows} == expected, rows
assert all(r['Root'] == '2' for r in rows), rows
PYREG
msiinfo export "$msi" Shortcut | tr -d '\r' > "$work/shortcut.idt"
msiinfo export "$msi" Environment | tr -d '\r' > "$work/environment.idt"
grep -q 'Learn DC' "$work/shortcut.idt" || { echo "error: no Learn DC shortcut in the MSI" >&2; cat "$work/shortcut.idt" >&2; exit 1; }
grep -q '\[#f_bin_learndc_exe\]' "$work/shortcut.idt" || { echo "error: the Learn DC shortcut does not target learndc.exe" >&2; exit 1; }
grep -q $'=-\*PATH\t\[~\];\[INSTALLDIR\]bin' "$work/environment.idt" || { echo "error: the MSI does not append [INSTALLDIR]bin to the system PATH and remove it on uninstall" >&2; cat "$work/environment.idt" >&2; exit 1; }
wine msiexec /i "$msi" /qn
wineserver -w
inst="$WINEPREFIX/drive_c/Program Files/dcc"
(cd "$inst" && find . -type f | sort) > "$work/installed.txt"
for f in ./bin/dcc.exe ./bin/dccd.exe ./bin/dpm.exe ./bin/learndc.exe ./bin/ld.lld.exe ./LICENSE ./licenses/FTXUI-LICENSE ./lib/libdcext-windows-llvm.a ./lib/libdcext-windows-custom.a ./lib/libdcext-freestanding-llvm.a ./lib/libdcext-freestanding-custom.a ./include/std.dc; do
    grep -qxF "$f" "$work/installed.txt" || { echo "error: $f not installed" >&2; exit 1; }
done
echo "installed $(wc -l < "$work/installed.txt") files under C:\\Program Files\\dcc"
if grep -E '^\./lib/' "$work/installed.txt" | grep -vE '/(libdcext-(windows|freestanding)-(llvm|custom)|lib(kernel32|ws2_32|advapi32|shell32|ntdll))\.a$'; then
    echo "error: unexpected libraries installed" >&2
    exit 1
fi

timeout 60 wine "$inst/bin/dcc.exe" --version | tr -d '\r' | tee "$version_out"
timeout 60 wine "$inst/bin/dpm.exe" --version
timeout 60 wine "$inst/bin/learndc.exe" --version | tr -d '\r'
mkdir -p "$work/learndc home"
LEARNDC_HOME="$(winepath -w "$work/learndc home")" timeout 60 wine "$inst/bin/learndc.exe" list --json | tr -d '\r' > "$work/learndc-list.json"
grep -q '"first-steps/hello-world"' "$work/learndc-list.json" || { echo "error: learndc.exe did not list its embedded exercises" >&2; exit 1; }
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
