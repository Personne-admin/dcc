#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/dist/debian"

mkdir -p "$OUT"
rm -f "$OUT"/*.deb

echo "==> Building Debian package"

docker build \
    --progress=plain \
    --tag dcc-debian-builder \
    --file "$ROOT/packaging/debian/Dockerfile" \
    "$ROOT"

echo "==> Extracting package"

CONTAINER="$(docker create dcc-debian-builder)"

trap 'docker rm -f "$CONTAINER" >/dev/null 2>&1 || true' EXIT

docker cp "$CONTAINER:/artifacts/." "$OUT/"

echo
echo "Built:"
ls -lh "$OUT"/*.deb
