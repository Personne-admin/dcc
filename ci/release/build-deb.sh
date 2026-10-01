#!/usr/bin/env bash
set -euo pipefail

version="$1"
out="$2"
shift 2
root="$(pwd)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
pkg="$stage/dcc"

make install DESTDIR="$pkg" PREFIX=/usr "$@" >/dev/null
find "$pkg/usr/lib" -name 'libdcext-*.a' ! -name 'libdcext-linux-llvm.a' ! -name 'libdcext-linux-custom.a' -delete
install -m 644 LICENSE "$pkg/usr/share/doc/dcc/copyright"
strip --strip-unneeded "$pkg/usr/bin/dcc" "$pkg/usr/bin/dccd" "$pkg/usr/bin/dcdoc"

libs="$(cd "$pkg/usr/lib" && ls)"
if [[ "$libs" != $'libdcext-linux-custom.a\nlibdcext-linux-llvm.a' ]]; then
    echo "error: unexpected libraries in package: $libs" >&2
    exit 1
fi

mkdir -p "$stage/shlibs/debian"
printf 'Source: dcc\n\nPackage: dcc\nArchitecture: any\n' > "$stage/shlibs/debian/control"
shlibs="$(cd "$stage/shlibs" && dpkg-shlibdeps -O "$pkg/usr/bin/dcc" "$pkg/usr/bin/dccd" "$pkg/usr/bin/dcdoc" | sed -n 's/^shlibs:Depends=//p')"

mkdir -p "$pkg/DEBIAN"
sed -e "s/@VERSION@/$version/" -e "s/@DEPENDS@/$shlibs, lld/" \
    -e "s/@INSTALLED_SIZE@/$(du -sk --exclude=DEBIAN "$pkg" | cut -f1)/" \
    "$root/ci/release/deb-control.in" > "$pkg/DEBIAN/control"

find "$pkg" -type d -exec chmod 755 {} +
mkdir -p "$out"
dpkg-deb --root-owner-group --build -Zxz "$pkg" "$out/dcc_${version}_amd64.deb"
dpkg-deb -I "$out/dcc_${version}_amd64.deb"
dpkg-deb -c "$out/dcc_${version}_amd64.deb" | awk '{print $6}' | grep -v '/$' | sort > "$out/deb-contents.txt"
grep -c . "$out/deb-contents.txt"
