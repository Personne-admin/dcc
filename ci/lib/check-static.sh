#!/usr/bin/env bash
set -euo pipefail

allowed='^(libc\.so\.6|libm\.so\.6|ld-linux-x86-64\.so\.2)$'
max_glibc="${MAX_GLIBC:-2.39}"
status=0

for bin in "$@"; do
    echo "== $bin"
    ldd "$bin"
    needed="$(readelf -d "$bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"
    while read -r lib; do
        [[ -z "$lib" ]] && continue
        if ! [[ "$lib" =~ $allowed ]]; then
            echo "error: $bin needs $lib" >&2
            status=1
        fi
    done <<< "$needed"
    newest="$(objdump -T "$bin" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
    echo "newest glibc symbol version: $newest"
    if [[ "$(printf '%s\n%s\n' "$newest" "$max_glibc" | sort -V | tail -1)" != "$max_glibc" ]]; then
        echo "error: $bin needs glibc $newest, newer than $max_glibc" >&2
        status=1
    fi
done
exit $status
