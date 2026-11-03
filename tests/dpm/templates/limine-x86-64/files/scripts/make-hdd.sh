#!/bin/sh
set -eu
kernel=$1 limine=$2 tool=$3 hdd=$4 conf=$5 size=$6
heads=64
sectors=32
cylinder=$((heads * sectors))
start=$cylinder
count=$(((size - 2) * cylinder))
end=$((start + count - 1))
offset=$((start * 512))
rm -f "$hdd"
dd if=/dev/zero bs=1024k count=0 seek="$size" of="$hdd"
PATH=$PATH:/usr/sbin:/sbin sgdisk "$hdd" -n "1:$start:$end" -t 1:ef00 -m 1
"$tool" bios-install "$hdd"
image="$hdd@@$offset"
mformat -i "$image" -T "$count" -h "$heads" -s "$sectors" ::
mmd -i "$image" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
mcopy -i "$image" "$kernel" ::/boot/kernel
mcopy -i "$image" "$conf" "$limine/limine-bios.sys" ::/boot/limine
mcopy -i "$image" "$limine/BOOTX64.EFI" "$limine/BOOTIA32.EFI" ::/EFI/BOOT
