#!/bin/sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this script as root" >&2
  exit 1
fi

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cmdline=/boot/firmware/cmdline.txt

[ "$(findmnt -n -o SOURCE /)" = "/dev/mmcblk0p2" ] || {
  echo "Unexpected root device; refusing to repartition" >&2
  exit 1
}
[ "$(cat /sys/block/mmcblk0/size)" = "122191872" ] || {
  echo "Unexpected SD-card size; refusing to repartition" >&2
  exit 1
}

install -m 0755 "$script_dir/initramfs-hook" \
  /etc/initramfs-tools/hooks/gamebird-repartition
install -m 0755 "$script_dir/initramfs-premount" \
  /etc/initramfs-tools/scripts/local-premount/gamebird-repartition

if [ ! -f "$cmdline.before-gamebird-repartition" ]; then
  cp -a "$cmdline" "$cmdline.before-gamebird-repartition"
fi
if ! grep -qw gamebird_repartition "$cmdline"; then
  sed -i 's/^/gamebird_repartition /' "$cmdline"
fi

update-initramfs -u -k all

for image in /boot/firmware/initramfs /boot/firmware/initramfs7 \
             /boot/firmware/initramfs8; do
  [ -f "$image" ] || continue
  lsinitramfs "$image" | grep -q 'scripts/local-premount/gamebird-repartition'
  lsinitramfs "$image" | grep -q 'usr/sbin/resize2fs'
  lsinitramfs "$image" | grep -q 'usr/sbin/mke2fs'
done

echo "Stage 1 is installed and verified. Reboot to create /dev/mmcblk0p3."
