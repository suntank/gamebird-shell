#!/bin/sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this script as root" >&2
  exit 1
fi

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cmdline=/boot/firmware/cmdline.txt

[ "$(findmnt -n -o SOURCE /data)" = "/dev/mmcblk0p3" ] || {
  echo "/data is not mounted from /dev/mmcblk0p3" >&2
  exit 1
}
[ "$(findmnt -n -o FSTYPE /data)" = "ext4" ] || {
  echo "/data is not an ext4 filesystem" >&2
  exit 1
}
[ "$(blkid -s LABEL -o value /dev/mmcblk0p3)" = "GAMEBIRD-DATA" ] || {
  echo "Persistent data partition has the wrong label" >&2
  exit 1
}

install -D -m 0755 "$script_dir/gamebird-remount-fs" \
  /usr/local/sbin/gamebird-remount-fs
install -D -m 0644 "$script_dir/systemd-remount-fs-gamebird.conf" \
  /etc/systemd/system/systemd-remount-fs.service.d/gamebird-overlay.conf

cp -a /etc/fstab /etc/fstab.before-gamebird-overlay
cp -a "$cmdline" "$cmdline.before-gamebird-overlay"

# Raspberry Pi OS's supported command installs overlayroot, protects boot, and
# stages a RAM-backed root overlay. recurse=0 is essential: it leaves the
# separate GAMEBIRD-DATA filesystem and its bind mounts genuinely writable.
raspi-config nonint do_overlayfs 0
sed -i 's/overlayroot=tmpfs /overlayroot=tmpfs:recurse=0 /' "$cmdline"

update-initramfs -u -k all

grep -qw 'overlayroot=tmpfs:recurse=0' "$cmdline"
grep -E '^[^#].*[[:space:]]/boot/firmware[[:space:]].*defaults,ro' /etc/fstab \
  >/dev/null

for image in /boot/firmware/initramfs /boot/firmware/initramfs7 \
             /boot/firmware/initramfs8; do
  [ -f "$image" ] || continue
  listing="$(mktemp)"
  lsinitramfs "$image" > "$listing"
  grep -q 'scripts/init-bottom/overlayroot' "$listing"
  grep -q 'kernel/fs/overlayfs/overlay.ko' "$listing"
done

echo "OverlayFS is staged and verified. Reboot to activate protection."
