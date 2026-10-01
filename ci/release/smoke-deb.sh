#!/usr/bin/env bash
set -euo pipefail

deb="$(realpath "$1")"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
status=0
for image in ubuntu:24.04 debian:trixie; do
    echo "==> $image"
    if ! docker run --rm --pull always \
        -v "$deb:/pkg/$(basename "$deb"):ro" -v "$here/smoke:/smoke:ro" -v "$here/smoke-deb-inner.sh:/smoke-deb-inner.sh:ro" \
        "$image" bash /smoke-deb-inner.sh "/pkg/$(basename "$deb")"; then
        echo "==> $image FAILED" >&2
        status=1
    fi
done
exit $status
