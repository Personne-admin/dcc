#!/usr/bin/env bash
set -euo pipefail

PRISM_VERSION="1.29.0"
PRISM_SHA256="e7b88bddc6c757b2fc8cb113e2469801ab14a78ec1a8fada4d6391e3573f5f9f"
PRIMARY_URL="https://cdnjs.cloudflare.com/ajax/libs/prism/$PRISM_VERSION/prism.min.js"
FALLBACK_URL="https://cdn.jsdelivr.net/npm/prismjs@$PRISM_VERSION/prism.js"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="$SCRIPT_DIR/vendor"
mkdir -p "$VENDOR_DIR"
OUT="$VENDOR_DIR/prism-core.min.js"
TMP="$(mktemp)"

cleanup() { rm -f "$TMP"; }
trap cleanup EXIT

downloaded=""
for url in "$PRIMARY_URL" "$FALLBACK_URL"; do
	echo "==> trying $url"
	if curl -fsSL --max-time 120 -o "$TMP" "$url"; then
		downloaded="$url"
		break
	fi

	echo "    failed, trying next mirror..." >&2
done

if [ -z "$downloaded" ]; then
	echo "ERROR: could not download Prism $PRISM_VERSION from any mirror." >&2
	exit 1
fi

actual="$(sha256sum "$TMP" | awk "{print \$1}")"
if [ "$actual" != "$PRISM_SHA256" ]; then
	echo "ERROR: SHA256 mismatch for $downloaded" >&2
	echo "  expected: $PRISM_SHA256" >&2
	echo "  actual:   $actual" >&2
	echo "Upstream may have re-cut v$PRISM_VERSION" >&2
	echo "Refusing to vendor." >&2
	exit 1
fi

mv "$TMP" "$OUT"
trap - EXIT
echo "==> vendored $downloaded"
echo "    $OUT ($(wc -c <"$OUT") bytes, sha256 $actual)"
