#!/bin/sh
set -eu
kernel=$1 limine=$2 tool=$3 iso=$4 conf=$5
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/boot/limine" "$stage/EFI/BOOT"
cp "$kernel" "$stage/boot/kernel"
cp "$conf" "$limine/limine-bios.sys" "$limine/limine-bios-cd.bin" "$limine/limine-uefi-cd.bin" "$stage/boot/limine/"
cp "$limine/BOOTX64.EFI" "$limine/BOOTIA32.EFI" "$stage/EFI/BOOT/"
rm -f "$iso"
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label "$stage" -o "$iso"
"$tool" bios-install "$iso"
