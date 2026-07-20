#!/bin/sh

d=$(dirname "$0"); f="$1"; be="$2"
rm -f "$d/prog.o" "$d/prog"
if [ "$be" = "em64t" ]; then BE="-fbackend em64t"; else BE=""; fi
/usr/local/bin/dcc $BE -target x86_64-elf -c -o "$d/prog.o" "$f" >"$d/cc.log" 2>&1 || { echo "COMPILE-FAIL"; exit 1; }
ld.lld --static --no-dynamic-linker -e _start -o "$d/prog" "$d/prog.o" "$d/start.o" >/dev/null 2>&1 || { echo "LINK-FAIL"; exit 1; }
"$d/prog"; echo "EXIT=$?"
