#!/usr/bin/env bash
set -euo pipefail

dir="$1"
tag="$2"
cd "$dir"
sha256sum -c SHA256SUMS
mapfile -t assets < <(awk '{print $2}' SHA256SUMS)
if gh release view "$tag" >/dev/null 2>&1; then
    echo "error: release $tag already exists" >&2
    exit 1
fi
gh release create "$tag" --verify-tag --draft --title "dcc ${tag#v}" --notes-file RELEASE_NOTES.md "${assets[@]}" SHA256SUMS
