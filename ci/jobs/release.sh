#!/usr/bin/env bash
source ci/lib/container.sh
export WINEDEBUG=-all WINEPREFIX="$HOME/wine"

out="${DCC_CI_OUT:?}"
tag="${DCC_RELEASE_TAG:-}"
if [[ -z "$tag" && "${DCC_RELEASE_DRY_RUN:-0}" != 1 ]]; then
    tag="$(git tag --points-at HEAD --list 'v*' | head -1)"
fi

ci_section version
if [[ "${DCC_RELEASE_DRY_RUN:-0}" == 1 ]]; then
    python3 ci/release/version.py resolve --dry-run > "$out/versions.env"
else
    python3 ci/release/version.py resolve --tag "$tag" > "$out/versions.env"
fi
cat "$out/versions.env"
set -a
source "$out/versions.env"
set +a
python3 ci/release/version.py inject "$VSIX_VERSION"

MAKE_ARGS+=(PREFIX=/usr DCC_VERSION="$DCC_VERSION")

ci_section build-linux
ci_make all
ci_make libdcext TARGET=x86_64-linux BACKEND=custom
ci/lib/check-static.sh build/bin/dcc build/bin/dccd build/bin/dcdoc

ci_section deb
ci/release/build-deb.sh "$DEB_VERSION" "$out" "${MAKE_ARGS[@]}"

ci_section msi
msi_args=("FILE_PREFIX_MAP_FLAGS=-ffile-prefix-map=$TOPLEVEL=dcc" BUILD_TYPE=release STATIC_LINK=1 PREFIX=/usr ENABLE_LLVM=1 "DCC_WRAPPER=timeout $DCC_TIMEOUT" DCC_VERSION="$DCC_VERSION" MSI_VERSION="$MSI_VERSION")
make -j"$JOBS" "${msi_args[@]}" tools-windows
make "${msi_args[@]}" msi
cp "build-windows/dcc-$MSI_VERSION-x86_64.msi" "$out/dcc-$DCC_VERSION-x86_64.msi"

ci_section vsix
(
    cd vscode
    npm ci --no-audit --no-fund
    npm test
    npm run validate
    npx --no-install vsce package --no-update-package-json --out "$out/dcc-vscode-$VSIX_VERSION.vsix"
)
python3 ci/release/check_vsix.py "$out/dcc-vscode-$VSIX_VERSION.vsix" "$VSIX_VERSION"

ci_section msi-smoke
ci/release/smoke-msi.sh "$out/dcc-$DCC_VERSION-x86_64.msi" "$out/dcc-exe-version.txt"

ci_section verify
python3 ci/release/version.py verify --versions "$out/versions.env" --out "$out" --windows-version "$out/dcc-exe-version.txt"

ci_section notes
python3 ci/release/notes.py --version "$DCC_VERSION" --tag "$tag" > "$out/RELEASE_NOTES.md"
cat "$out/RELEASE_NOTES.md"

ci_section checksums
(
    cd "$out"
    sha256sum "dcc_${DEB_VERSION}_amd64.deb" "dcc-$DCC_VERSION-x86_64.msi" "dcc-vscode-$VSIX_VERSION.vsix" > SHA256SUMS
    sha256sum -c SHA256SUMS
)
cat "$out/SHA256SUMS"
