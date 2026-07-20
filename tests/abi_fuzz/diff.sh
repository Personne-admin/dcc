#!/bin/sh

d=$(dirname "$0"); f="$1"
rm -f "$d/de.o" "$d/dl.o" "$d/deprog" "$d/dlprog"
/usr/local/bin/dcc -fbackend em64t -target x86_64-elf -c -o "$d/de.o" "$f" >"$d/e.log" 2>&1 || { echo "EM64T-COMPILE-FAIL"; exit 1; }
/usr/local/bin/dcc -target x86_64-elf -c -o "$d/dl.o" "$f" >"$d/l.log" 2>&1 || { echo "LLVM-COMPILE-FAIL"; exit 1; }
ld.lld --static --no-dynamic-linker -e _start -o "$d/deprog" "$d/de.o" "$d/start.o" >/dev/null 2>&1 || { echo "EM64T-LINK-FAIL"; exit 1; }
ld.lld --static --no-dynamic-linker -e _start -o "$d/dlprog" "$d/dl.o" "$d/start.o" >/dev/null 2>&1 || { echo "LLVM-LINK-FAIL"; exit 1; }
"$d/deprog" >/dev/null 2>&1; e=$?
"$d/dlprog" >/dev/null 2>&1; l=$?
if [ "$e" = "$l" ]; then echo "ok($e)"; else echo "MISMATCH em64t=$e llvm=$l"; fi
