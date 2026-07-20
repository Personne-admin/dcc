#!/bin/sh

set -x
d=$(dirname "$0")
f="$1"; shift
/usr/local/bin/dcc -target x86_64-elf -c -o "$d/progl.o" "$f" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
ld.lld --static --no-dynamic-linker -e _start -o "$d/progl" "$d/progl.o" "$d/start.o" "$@"
"$d/progl"; echo "EXIT=$?"
