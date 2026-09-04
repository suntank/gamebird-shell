#!/bin/sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this script as root" >&2
  exit 1
fi

data_part=/dev/mmcblk0p3
data_mount=/data
shell_root=/home/gamebird/gamebird-shell
fstab=/etc/fstab

[ -b "$data_part" ] || {
  echo "$data_part does not exist" >&2
  exit 1
}
[ "$(blkid -s LABEL -o value "$data_part")" = "GAMEBIRD-DATA" ] || {
  echo "$data_part has the wrong filesystem label" >&2
  exit 1
}

systemctl stop gbshell.service gblibd.service

mkdir -p "$data_mount"
data_uuid="$(blkid -s UUID -o value "$data_part")"
cp -a "$fstab" "$fstab.before-gamebird-data"

if ! grep -q '^[^#].*[[:space:]]/data[[:space:]]' "$fstab"; then
  printf 'UUID=%s  /data  ext4  defaults,noatime,errors=remount-ro  0  2\n' \
    "$data_uuid" >> "$fstab"
fi

systemctl daemon-reload
mount "$data_mount"

seed_path() {
  source_path="$1"
  persistent_path="$2"
  mkdir -p "$source_path" "$persistent_path"
  rsync -aHAX "$source_path/" "$persistent_path/"
}

seed_path "$shell_root/data" "$data_mount/gamebird-shell/data"
seed_path "$shell_root/config" "$data_mount/gamebird-shell/config"
seed_path "$shell_root/roms" "$data_mount/gamebird-shell/roms"
seed_path "$shell_root/apps" "$data_mount/gamebird-shell/apps"
seed_path /home/gamebird/.config/retroarch "$data_mount/retroarch/config"
seed_path /var/lib/bluetooth "$data_mount/system/bluetooth"
seed_path /etc/NetworkManager/system-connections "$data_mount/system/network-connections"
seed_path /var/lib/alsa "$data_mount/system/alsa"
seed_path /var/lib/systemd/rfkill "$data_mount/system/rfkill"

if ! grep -q '^# BEGIN GAMEBIRD PERSISTENT DATA$' "$fstab"; then
  cat >> "$fstab" <<'EOF'
# BEGIN GAMEBIRD PERSISTENT DATA
/data/gamebird-shell/data /home/gamebird/gamebird-shell/data none bind,x-systemd.requires-mounts-for=/data 0 0
/data/gamebird-shell/config /home/gamebird/gamebird-shell/config none bind,x-systemd.requires-mounts-for=/data 0 0
/data/gamebird-shell/roms /home/gamebird/gamebird-shell/roms none bind,x-systemd.requires-mounts-for=/data 0 0
/data/gamebird-shell/apps /home/gamebird/gamebird-shell/apps none bind,x-systemd.requires-mounts-for=/data 0 0
/data/retroarch/config /home/gamebird/.config/retroarch none bind,x-systemd.requires-mounts-for=/data 0 0
/data/system/bluetooth /var/lib/bluetooth none bind,x-systemd.requires-mounts-for=/data 0 0
/data/system/network-connections /etc/NetworkManager/system-connections none bind,x-systemd.requires-mounts-for=/data 0 0
/data/system/alsa /var/lib/alsa none bind,x-systemd.requires-mounts-for=/data 0 0
/data/system/rfkill /var/lib/systemd/rfkill none bind,x-systemd.requires-mounts-for=/data 0 0
# END GAMEBIRD PERSISTENT DATA
EOF
fi

systemctl daemon-reload
mount -a

sed -i 's/gamebird_repartition //g' /boot/firmware/cmdline.txt
rm -f /etc/initramfs-tools/hooks/gamebird-repartition
rm -f /etc/initramfs-tools/scripts/local-premount/gamebird-repartition
update-initramfs -u -k all

mkdir -p /etc/systemd/journald.conf.d
cat > /etc/systemd/journald.conf.d/gamebird-volatile.conf <<'EOF'
[Journal]
Storage=volatile
RuntimeMaxUse=16M
EOF

systemctl restart systemd-journald.service
systemctl start gblibd.service gbshell.service

echo "Persistent data mounts are installed. Verify them before enabling overlayroot."
